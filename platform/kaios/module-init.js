// Must exist before quake2-kaios.js runs (it checks
// "typeof Module !== 'undefined'" at the very top). Split out into its
// own file rather than an inline <script> block in index.html: privileged
// KaiOS apps get a mandatory CSP (script-src 'self', no 'unsafe-inline')
// that silently drops inline scripts entirely -- confirmed on a real
// device via EM_ASM diagnostics showing Module.canvas as undefined at
// runtime despite this exact assignment "being there" in index.html. An
// external, same-origin file like this one is unaffected by that CSP.
//
// noInitialRun holds off main() until app.js has copied baseq2 into the
// FS; quake2-kaios.js itself is loaded dynamically by app.js, only once a
// baseq2 folder is confirmed, so this alone was likely never the whole
// story for the double-main()-entry issue from earlier testing -- but a
// Module object that silently never existed at all (same CSP cause)
// fully explains it: Emscripten's own auto-run defaults to true unless
// this object's noInitialRun is seen as true by quake2-kaios.js's own
// top-level code, same as it does for canvas below.
// ---------------------------------------------------------------------
// Crash-log ring buffer
// ---------------------------------------------------------------------
// KaiOS WebIDE's remote-debugging connection dies along with the app
// on a crash (same as closing the tab it's attached to), taking the
// live console with it before there's ever a chance to read the final
// lines that would explain what happened. A bounded ring buffer of
// recent output, periodically flushed to localStorage (survives
// independently of the debugger connection, unlike anything only kept
// in memory), means the tail of a crashed session's log is still
// readable on the *next* launch even though it was lost live.
//
// Deliberately NOT flushed on every line -- localStorage.setItem() is
// synchronous with a real, measurable cost, and a9f5870's own finding
// was that plain console.log() calls alone caused 700ms+ frame spikes
// on this hardware (see cl_main.c/frame.c's KAIOS_FRAMESPIKE
// comments). A throttled timer instead keeps the worst case bounded
// to one small write every couple of seconds regardless of how bursty
// the actual logging gets. This can't catch everything -- a true
// instant process kill leaves whatever happened since the last tick
// unrecovered -- but it turns "nothing at all" into "everything up to
// a couple of seconds before the crash."
var KAIOS_LOG_RING_KEY = 'kaios_crash_log';
var KAIOS_LOG_RING_MAX_LINES = 300;
var kaiosLogRingLines = [];
var kaiosLogRingDirty = false;

function kaiosLogRingPush(line) {
	kaiosLogRingLines.push(line);
	if (kaiosLogRingLines.length > KAIOS_LOG_RING_MAX_LINES) {
		kaiosLogRingLines.shift();
	}
	kaiosLogRingDirty = true;
}

function kaiosLogRingFlush() {
	if (!kaiosLogRingDirty) {
		return;
	}
	kaiosLogRingDirty = false;
	try {
		localStorage.setItem(KAIOS_LOG_RING_KEY, JSON.stringify(kaiosLogRingLines));
	} catch (e) {
		// Storage full/unavailable -- losing the crash-recovery log is
		// a much smaller problem than taking the whole app down over it.
	}
}

setInterval(kaiosLogRingFlush, 2000);

// A genuine process kill (OOM, engine abort) gives no JS event at all
// -- only the timer above can catch that class of death. A JS-level
// exception or an Emscripten-side abort() DO give a chance to run a
// handler first, so those trigger an immediate flush on top of the
// timer, catching lines right up to the moment of failure instead of
// however stale the last periodic tick happened to be.
window.addEventListener('error', kaiosLogRingFlush);
window.addEventListener('unhandledrejection', kaiosLogRingFlush);

// Recover and print whatever the *previous* session managed to flush
// before dying, then clear it -- printed as plain console.log() (not
// kaiosLogRingPush()) since this session's own ring buffer starts
// empty and a fresh copy of the last one's tail doesn't belong in it.
(function () {
	try {
		var prev = localStorage.getItem(KAIOS_LOG_RING_KEY);
		if (prev) {
			var lines = JSON.parse(prev);
			console.log('[kaios] --- recovered log tail from previous session (' +
				lines.length + ' lines) ---');
			for (var i = 0; i < lines.length; i++) {
				console.log(lines[i]);
			}
			console.log('[kaios] --- end of recovered log ---');
		}
		localStorage.removeItem(KAIOS_LOG_RING_KEY);
	} catch (e) {
		console.log('[kaios] crash-log recovery failed: ' + e);
	}
})();

// ---------------------------------------------------------------------
// Outer JS tick timing
// ---------------------------------------------------------------------
// Texture-upload, VBO, and audio-play timing (gl3_image.c/gl3_main.c/
// webaudio.c) have all been measured directly against real
// KAIOS_FRAMESPIKE events and ruled out -- none of them account for
// more than a few ms while the spike's renderdelta-minus-breakdown gap
// runs into the hundreds or low thousands. Whatever's eating that time
// is not inside any C-side timer this engine has, which leaves exactly
// two possibilities, and only JS can tell them apart: either it's
// happening *inside* this frame's own callback (something our C code
// calls into JS for that nothing wraps -- GC pressure from our own
// allocations, Emscripten's own glue overhead) or *between* callbacks
// entirely (the browser simply not running requestAnimationFrame
// promptly -- OS scheduling, tab throttling) -- neither is visible
// from inside the engine at all, no matter how many more Com_Printf
// timers get added there.
//
// Browser.requestAnimationFrame (Emscripten's own runtime, baked into
// quake2-kaios.js) looks up the bare `requestAnimationFrame` global
// fresh on every single call rather than caching a reference once --
// confirmed by reading the generated glue -- so patching
// window.requestAnimationFrame here, before quake2-kaios.js is even
// loaded, reliably wraps every engine frame's callback for the whole
// session, with nothing on the emscripten side needing to change.
//
// gap = wall time since the *previous* callback started -- the JS-side
// equivalent of cl_main.c's renderdelta, but measured with
// performance.now() outside the engine entirely, so it can't miss
// anything the C side's own clock might. duration = how long *this*
// callback's own synchronous execution took, start to finish -- if
// this comes back close to renderdelta while KAIOS_FRAMESPIKE_
// BREAKDOWN's `all` stays small, the missing time is inside our own
// code's execution somewhere unwrapped; if duration stays small while
// gap is huge, the engine was never even running during the gap and
// it's genuinely a browser/OS-level stall no amount of internal timing
// could ever have caught. Threshold matches KAIOS_FRAMESPIKE's own
// 400ms exactly so every line here lines up 1:1 with one there.
(function () {
	var lastTickStart = null;
	var origRAF = window.requestAnimationFrame.bind(window);

	window.requestAnimationFrame = function (cb) {
		return origRAF(function (ts) {
			var callStart = performance.now();
			var gap = (lastTickStart !== null) ? (callStart - lastTickStart) : 0;
			lastTickStart = callStart;

			cb(ts);

			var duration = performance.now() - callStart;

			if (gap > 400 || duration > 400) {
				var line = '[kaios] KAIOS_JS_TICK: gap=' + gap.toFixed(1) +
					'ms duration=' + duration.toFixed(1) + 'ms wall=' + Date.now();
				console.log(line);
				kaiosLogRingPush(line);
			}
		});
	};
})();

// ---------------------------------------------------------------------
// setInterval heartbeat -- independent of requestAnimationFrame
// ---------------------------------------------------------------------
// Every render-cost hypothesis this engine's own timers could reach
// (texture upload, VBO, WA_PLAY, entity/particle/dlight counts, camera
// movement) has been checked against real KAIOS_FRAMESPIKE events and
// ruled out one by one, including with sound entirely off -- the gap
// itself (KAIOS_JS_TICK: small duration, huge gap) is real but nothing
// inside the engine or the render path explains it. What's left is
// genuinely outside anything content-dependent: either the whole JS
// thread stalls (GC, OS-level preemption -- something unrelated to
// rendering at all) or specifically requestAnimationFrame gets
// deprioritized while the thread itself stays free (a
// rendering/compositor-side throttle). Only a timer that has nothing
// to do with rendering can tell those apart. setInterval is scheduled
// by the browser's ordinary timer queue, not tied to paint/compositing
// the way requestAnimationFrame is -- if IT stalls in lockstep with
// KAIOS_JS_TICK's gaps, the whole thread was blocked and this has
// nothing to do with GL3/WebGL at all; if it stays smooth while
// KAIOS_JS_TICK still shows huge gaps, the browser is specifically
// deprioritizing frame callbacks and the thread itself was free the
// whole time. Same 400ms threshold as the others so every line lines
// up 1:1 across all three.
(function () {
	var lastBeat = null;
	var nominalInterval = 200;

	setInterval(function () {
		var now = performance.now();
		var gap = (lastBeat !== null) ? (now - lastBeat) : nominalInterval;
		lastBeat = now;

		if (gap > 400) {
			var line = '[kaios] KAIOS_HEARTBEAT_GAP: gap=' + gap.toFixed(1) + 'ms wall=' + Date.now();
			console.log(line);
			kaiosLogRingPush(line);
		}
	}, nominalInterval);
})();

// ---------------------------------------------------------------------
// Tab visibility changes
// ---------------------------------------------------------------------
// A stutter that lines up with the app losing the foreground (an
// incoming call, a KaiOS system notification, the user pressing the
// home/end key) would explain "works fine here, terrible there" as
// session-specific interruptions rather than anything about the scene
// being rendered -- exactly the kind of randomness that's otherwise
// impossible to distinguish from GPU/compositor backpressure using
// only the timers above. Logged immediately (a discrete, rare event,
// not a per-frame cost like the two timers above), so this can be
// cross-referenced against KAIOS_JS_TICK/KAIOS_HEARTBEAT_GAP lines
// with real timestamps on both sides.
document.addEventListener('visibilitychange', function () {
	var line = '[kaios] KAIOS_VISIBILITY: state=' + document.visibilityState +
		' t=' + performance.now().toFixed(1) + ' wall=' + Date.now();
	console.log(line);
	kaiosLogRingPush(line);
});

// ---------------------------------------------------------------------
// Long Tasks API (best-effort)
// ---------------------------------------------------------------------
// Standard way modern browsers self-report "the main thread was blocked
// for this long, starting here" without needing our own polling --
// strictly more precise than KAIOS_HEARTBEAT_GAP when it's available.
// KaiOS 2.5's Gecko-48-class engine predates this API in most Firefox
// releases, so this is feature-detected and silently skipped if
// PerformanceObserver or the 'longtask' entry type isn't there --
// never assume support, just take it if offered.
(function () {
	if (typeof PerformanceObserver === 'undefined') {
		return;
	}

	try {
		var obs = new PerformanceObserver(function (list) {
			var entries = list.getEntries();
			for (var i = 0; i < entries.length; i++) {
				var e = entries[i];
				var line = '[kaios] KAIOS_LONGTASK: duration=' + e.duration.toFixed(1) +
					'ms start=' + e.startTime.toFixed(1) + ' wall=' + Date.now();
				console.log(line);
				kaiosLogRingPush(line);
			}
		});
		obs.observe({ entryTypes: ['longtask'] });
	} catch (e) {
		// 'longtask' entry type not supported on this engine -- expected
		// on KaiOS 2.5, not worth logging as a real error.
	}
})();

var Module = {
	canvas: document.getElementById('canvas'),
	noInitialRun: true,
	print: function (text) { console.log('[q2] ' + text); kaiosLogRingPush('[q2] ' + text); },
	printErr: function (text) { console.log('[q2!] ' + text); kaiosLogRingPush('[q2!] ' + text); },
	// Emscripten-side abort() (OOM, a fatal internal assertion) -- the
	// class of crash this buffer exists for most. Flushes immediately
	// and stamps the reason as the last line so it's not just implied
	// by the log simply stopping.
	onAbort: function (what) {
		kaiosLogRingPush('[kaios] Module.onAbort: ' + what);
		kaiosLogRingFlush();
	}
};
