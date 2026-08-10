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
