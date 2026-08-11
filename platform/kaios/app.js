/*
 * KaiOS shell for the asm.js Quake II build.
 *
 * Two jobs:
 *   1. Let the user pick the folder on the SD card that holds "baseq2"
 *      (via the Device Storage API -- there's no <input type=file> on
 *      KaiOS) and expose its contents through the Emscripten virtual FS.
 *      pak0.pak (and any other top-level pakN.pak) is NOT copied in --
 *      see installLazyPakFile() below -- only loose files are.
 *   2. Translate KaiOS's keypad into the engine's key names and feed
 *      them straight into Key_Event() through the KaiOS_KeyEvent()
 *      bridge added in src/client/cl_keyboard.c, bypassing SDL2 input
 *      (which has no scancode mapping for SoftLeft/SoftRight/Call/...).
 *
 * NOTE: the Device Storage calls below follow the documented KaiOS/B2G
 * API shape, but there is no way to exercise navigator.getDeviceStorage
 * outside of an actual KaiOS device/simulator -- this half is unverified.
 * Everything from "FS populated" onward (autoexec.cfg, callMain, the
 * KaiOS_KeyEvent bridge) *has* been exercised against the real compiled
 * engine in headless Chromium, see platform/kaios/README.md.
 */
(function () {
'use strict';

var GAMEDIR = 'baseq2';

var menuEl = document.getElementById('menu');
var menuListEl = document.getElementById('menu-list');
var mainMenuCaptionEl = document.getElementById('menu-caption');
var pickerEl = document.getElementById('picker');
var pickerListEl = document.getElementById('picker-list');
var settingsEl = document.getElementById('settings');
var settingsListEl = document.getElementById('settings-list');
var statusEl = document.getElementById('status');
var statusTextEl = document.getElementById('status-text');
var statusBarEl = document.getElementById('status-bar');
var statusHintRightEl = document.getElementById('status-hint-right');
var canvasEl = document.getElementById('canvas');

// Shared by every keyboard-navigable list screen (main menu, baseq2
// picker, settings groups, settings items) -- builds the <li> rows,
// marks the selected one (styled in index.html's <style> block, a
// cursor glyph + text color rather than a solid highlight block, to
// read closer to Quake II's own menu convention), and scrolls it into
// view by hand. That's what actually makes a list longer than the
// screen (the Debug settings screen has 13+ rows) navigable at all --
// index.html's CSS makes the <ul> scrollable, but without this,
// ArrowDown past the visible rows would move the selection somewhere
// the player can't see or confirm.
//
// Deliberately NOT Element.scrollIntoView({block: 'nearest'}) -- that
// options-object form is a newer addition to the DOM spec than this
// device's Gecko-48-class engine (every call site here runs on every
// keypress, and every show*() function below calls render() once
// *before* assigning activeMenuKeyHandler, so a single throw here
// during that first call would silently leave the whole screen
// keyboard-dead, matching a real-device report of exactly that after
// this scrolling behavior was added). Plain scrollTop arithmetic has
// no API-version risk at all.
function renderMenuItems(listEl, labels, selectedIndex, disabledFlags) {
	// Captured before the innerHTML clear below and re-applied right
	// after -- on this device's Gecko-48-class engine, replacing every
	// child of a scrolled list has been seen to drop scrollTop back to 0
	// along with them instead of preserving it the way current browsers
	// do, which (since every render() rebuilds the whole list on every
	// keypress) made a scrolled-down settings list snap back to the top
	// on every toggle, stranding the actually-selected row off-screen
	// with no way to tell it was still selected. Restoring the old value
	// explicitly before the scroll-into-view correction below removes
	// that dependency on however this particular engine happens to
	// handle it.
	var oldScrollTop = listEl.scrollTop;

	listEl.innerHTML = '';
	for (var i = 0; i < labels.length; i++) {
		var li = document.createElement('li');
		li.textContent = labels[i];
		var classes = [];
		if (i === selectedIndex) {
			classes.push('selected');
		}
		if (disabledFlags && disabledFlags[i]) {
			classes.push('disabled');
		}
		if (classes.length) {
			li.className = classes.join(' ');
		}
		listEl.appendChild(li);
	}

	try {
		listEl.scrollTop = oldScrollTop;

		var selectedEl = listEl.children[selectedIndex];
		if (selectedEl) {
			// Real-device reports kept showing the selection scrolling
			// off the *top* of a scrolled-down list and staying there --
			// hand-rolled scrollTop = selectedEl.offsetTop arithmetic
			// (the previous approach here) depends on offsetTop/
			// clientHeight being exactly as trustworthy on this engine as
			// on any other, which real-device evidence says isn't holding
			// up. scrollIntoView(alignToTop) -- the plain boolean-argument
			// form, not the newer {block:'nearest'} options-object form
			// this file already avoids elsewhere -- hands the actual
			// scroll math to the browser's own layout code instead,
			// wherever selectedEl's ancestor chain actually needs it
			// scrolled from. Still only called when the row is actually
			// out of view (not on every render), so normal in-view
			// navigation never fights a touch/manual scroll position.
			if (selectedEl.offsetTop < listEl.scrollTop) {
				selectedEl.scrollIntoView(true);
			} else {
				var bottom = selectedEl.offsetTop + selectedEl.offsetHeight;
				if (bottom > listEl.scrollTop + listEl.clientHeight) {
					selectedEl.scrollIntoView(false);
				}
			}
		}
	} catch (e) {
		// Scrolling is a nicety, not worth ever taking the whole menu
		// down with it -- see this function's comment.
		console.log('[kaios] renderMenuItems: scroll-into-view failed: ' + e);
	}
}

// Used to be an inline oncontextmenu="..." attribute on the <canvas> tag
// in index.html -- moved here for the same reason module-init.js exists:
// privileged KaiOS apps' mandatory CSP silently drops inline scripts
// *and* inline event handler attributes, so it never actually ran.
canvasEl.addEventListener('contextmenu', function (ev) {
	ev.preventDefault();
});

// A long-press of the OK/center key (DOM "Enter") is KaiOS's system-wide
// gesture for launching the Smart/Assistant search, and it can fire
// mid-game if the keydown isn't clearly consumed. Whether that gesture
// is recognized from a bare keydown or surfaces as a synthesized
// contextmenu event depends on the exact Gecko build, so cover both:
// listen on the capture phase (runs before any other handler gets a
// chance to leave the event unconsumed) and prevent window-level
// contextmenu too, not just the canvas's own.
window.addEventListener('contextmenu', function (ev) {
	ev.preventDefault();
}, true);

// ---------------------------------------------------------------------
// Suppress the OK-button long-press -> Google Assistant gesture
// ---------------------------------------------------------------------
// The previous attempt here only ever watched the plain "Enter" key and
// still reached Assistant on a real device -- because KaiOS reports the
// long-press-Enter voice-assistant gesture as its own distinct named
// key, "MicrophoneToggle" (per kaios.dev's "Special Keys" list), a
// completely separate keydown from the normal short-press "Enter" one.
// Nothing in KEY_MAP/PREVENT_DEFAULT below was ever looking for that
// key at all, so every prior preventDefault()/stopImmediatePropagation()
// on "Enter" was defending against an event that was never the one
// actually firing. Swallow "MicrophoneToggle" outright.
//
// As a defensive fallback for firmware that instead just keeps re-firing
// "Enter" past a hold threshold rather than emitting a distinct key,
// also swallow Enter itself once it's been held past the OS's own
// long-press window -- just below where Assistant would normally
// trigger. Short-press Enter (fire) is deliberately left alone here:
// only later repeat keydowns past the threshold get swallowed, so the
// initial keydown still reaches installKeyHandlers()'s own listener
// below and starts +attack normally; the matching keyup is never
// swallowed either, so a held shot still releases correctly.
//
// Registered here, at the top of the file (so before installKeyHandlers()
// runs, later, from start()) specifically so this listener's capture-phase
// stopImmediatePropagation() -- when it fires -- runs before
// installKeyHandlers()'s own capture-phase listener on the same events;
// capture-phase listeners on the same target run in registration order.
var ENTER_HOLD_SWALLOW_MS = 400;
var enterDownAt = 0;

window.addEventListener('keydown', function (ev) {
	if (ev.key === 'MicrophoneToggle') {
		ev.preventDefault();
		ev.stopImmediatePropagation();
		return;
	}
	if (ev.key === 'Enter') {
		if (!enterDownAt) {
			enterDownAt = Date.now();
		}
		if (Date.now() - enterDownAt >= ENTER_HOLD_SWALLOW_MS) {
			ev.preventDefault();
			ev.stopImmediatePropagation();
		}
	}
}, true);

window.addEventListener('keyup', function (ev) {
	if (ev.key === 'Enter') {
		enterDownAt = 0;
	}
}, true);

// ---------------------------------------------------------------------
// KaiOS keypad -> Quake II key names
// (names match keynames[] in src/client/cl_keyboard.c / bind syntax)
// ---------------------------------------------------------------------

var KEY_MAP = {
	'ArrowUp':    'UPARROW',
	'ArrowDown':  'DOWNARROW',
	'ArrowLeft':  'LEFTARROW',
	'ArrowRight': 'RIGHTARROW',
	'Enter':      'ENTER',       // OK / center key
	'SoftRight':  'ESCAPE',      // Right Soft Key
	'SoftLeft':   'F1',          // Left Soft Key -> bound to +lookup
	'Call':       'F2',          // Call key      -> bound to +lookdown
	'#':          'F3',          // #             -> bound to +moveup (jump)
	'*':          'F4',          // *             -> bound to +movedown (crouch)
	'0': '0', '1': '1', '2': '2', '3': '3', '4': '4',
	'5': '5', '6': '6', '7': '7', '8': '8', '9': '9'
};

// Keys that must not fall through to the OS (SoftRight/Backspace would
// otherwise close the app; arrows/Enter can scroll/submit the page).
var PREVENT_DEFAULT = {
	'ArrowUp': 1, 'ArrowDown': 1, 'ArrowLeft': 1, 'ArrowRight': 1,
	'Enter': 1, 'SoftRight': 1, 'SoftLeft': 1, 'Call': 1, 'Backspace': 1,
	'#': 1, '*': 1
};

var engineStarted = false;

// The pre-boot menu/picker/settings screens (see the "Main menu" section
// below) need their own keydown handling for navigation, but they can't
// just add a second, independent window keydown listener the way a
// normal page could: the stopImmediatePropagation() below (needed to
// keep a short Enter press from also reaching the OS's own long-press
// Assistant gesture recognizer) stops *every* other keydown listener on
// window from ever firing for that event, capture or bubble, including
// ones registered later -- which silently ate Enter/arrow presses aimed
// at those screens (discovered via the main menu never responding to OK
// at all; showPicker() had the exact same latent bug, just never
// exercised before since every real test/build so far only ever hit its
// single-candidate auto-proceed path). Routing pre-boot navigation
// through this one already-tuned capture-phase pipeline instead of a
// second listener sidesteps that entirely.
var activeMenuKeyHandler = null;

function installKeyHandlers() {
	function handle(down) {
		return function (ev) {
			if (PREVENT_DEFAULT[ev.key]) {
				ev.preventDefault();
				// stopImmediatePropagation (not just preventDefault) for
				// the OK/center key specifically: it's the one KaiOS also
				// treats as a system-wide long-press gesture (Smart/
				// Assistant search), and the capture-phase listener below
				// needs to be the one place that gesture can be stopped
				// before anything else -- including this same handler's
				// own bubble-phase twin -- gets a chance to let it through.
				if (ev.key === 'Enter') {
					ev.stopImmediatePropagation();
				}
			}

			if (down && activeMenuKeyHandler) {
				activeMenuKeyHandler(ev);
			}

			if (!engineStarted) {
				return;
			}

			var q2key = KEY_MAP[ev.key];
			if (!q2key) {
				return;
			}

			Module.ccall('KaiOS_KeyEvent', null,
				['string', 'number'], [q2key, down ? 1 : 0]);
		};
	}

	// Capture phase: runs before the OS's own long-press/Assistant
	// gesture recognizer gets a look at the event, on builds where that
	// recognizer is itself just another content-side listener rather
	// than something below the DOM entirely. The MicrophoneToggle/
	// held-Enter suppression above is registered earlier (at file load,
	// before this function ever runs) specifically so it gets first
	// crack at the same events on this same capture phase.
	window.addEventListener('keydown', handle(true), true);
	window.addEventListener('keyup', handle(false), true);
}

// ---------------------------------------------------------------------
// Keep the screen (and CPU) awake -- KaiOS's own idle-timeout power
// saving otherwise dims/locks the screen mid-game, and this app has no
// use for it (there's no "idle" state in an FPS someone is actively
// playing). navigator.requestWakeLock is the B2G/KaiOS power API (not
// the newer W3C Screen Wake Lock API, which this Gecko-48-class engine
// predates); it returns a lock object with .unlock(), not a promise.
// ---------------------------------------------------------------------

var wakeLocks = [];

function releaseWakeLocks() {
	for (var i = 0; i < wakeLocks.length; i++) {
		try { wakeLocks[i].unlock(); } catch (e) { /* already gone */ }
	}
	wakeLocks = [];
}

function acquireWakeLocks() {
	if (!navigator.requestWakeLock) {
		console.log('[kaios] navigator.requestWakeLock not available -- ' +
			'cannot prevent the screen from sleeping.');
		return;
	}

	releaseWakeLocks();

	// 'screen' keeps the display on; 'cpu' keeps the whole device out of
	// deep sleep (needed too -- an asleep CPU stops running this page's
	// JS entirely, screen lock or not). Request both independently so a
	// topic this particular build/device doesn't support failing
	// doesn't take the other down with it.
	['screen', 'cpu'].forEach(function (topic) {
		try {
			wakeLocks.push(navigator.requestWakeLock(topic));
		} catch (e) {
			console.log('[kaios] requestWakeLock(' + topic + ') failed: ' +
				describeError(e));
		}
	});
}

function installWakeLock() {
	acquireWakeLocks();

	// B2G has been seen to silently drop a page's wake locks when it's
	// hidden (task-switched away, screen manually locked, ...) even
	// though the lock object itself gives no event for that -- so just
	// unconditionally re-acquire whenever this page becomes visible
	// again, whether or not the old locks were actually still held.
	// Cheap (a couple of API calls) and self-correcting either way.
	document.addEventListener('visibilitychange', function () {
		if (!document.hidden) {
			acquireWakeLocks();
		} else {
			flushConfigToPersistentStorage();
		}
	});

	// visibilitychange isn't guaranteed to fire before a page actually
	// goes away (e.g. the OS just kills it) -- pagehide is the more
	// reliable "this page may not get another turn" signal browsers
	// offer, so it's a second attempt at the same flush, not a
	// duplicate: worst case FS.syncfs() runs twice with nothing new
	// the second time, which is harmless.
	window.addEventListener('pagehide', flushConfigToPersistentStorage);
}

// ---------------------------------------------------------------------
// Persist config.cfg (options menu changes, key binds) across launches.
//
// The engine's own config.cfg write (CL_WriteConfiguration(), on quit
// or disconnect) lands on plain MEMFS, which is wiped on every reload --
// there's nothing KaiOS-specific about *that* path, it's the same file
// I/O yquake2 does everywhere. What's missing on this platform is
// somewhere durable for MEMFS to be backed by: mount IDBFS over the
// engine's writable homedir (~/.yq2, via Emscripten's synthesized HOME)
// and pull in whatever was saved last session before the engine boots.
//
// Pushing local writes back *out* to IndexedDB is JS-side (FS.syncfs())
// and doesn't happen automatically -- CL_WriteConfiguration() (cl_main.c)
// triggers one itself right after writing config.cfg, which covers the
// quit/disconnect paths. That still leaves the common real-world exit on
// a feature phone: the user just switches away or locks the screen
// without ever hitting a menu "quit". flushConfigToPersistentStorage()
// below is the backstop for that -- wired to 'visibilitychange' going
// hidden (above) and 'pagehide' (see installWakeLock()'s caller), so
// whatever's on MEMFS at that point gets pushed out regardless of
// whether the engine itself ever ran CL_WriteConfiguration.
// ---------------------------------------------------------------------

function getPersistentConfigDir() {
	var home = (typeof ENV !== 'undefined' && ENV.HOME) || '/home/web_user';
	return home + '/.yq2';
}

function mountPersistentConfig() {
	return new Promise(function (resolve) {
		var dir = getPersistentConfigDir();
		try {
			mkdirTreeFor(dir + '/x');
			FS.mount(IDBFS, {}, dir);
		} catch (e) {
			console.log('[kaios] IDBFS mount failed, settings will not persist: ' +
				describeError(e));
			resolve();
			return;
		}

		FS.syncfs(true, function (err) {
			if (err) {
				console.log('[kaios] IDBFS initial sync failed, settings will not persist: ' +
					describeError(err));
			}
			// Either way, don't block boot on this -- worst case is a
			// session that starts from defaults instead of saved settings,
			// not one that fails to start.
			resolve();
		});
	});
}

// engineStarted (not bootedOnce/callMainArmed) is the right guard here:
// CL_WriteConfiguration() itself no-ops before the client is actually
// initialized (cls.state == ca_uninitialized), same condition
// engineStarted already tracks.
function flushConfigToPersistentStorage() {
	if (!engineStarted || typeof Module === 'undefined' || !Module.ccall) {
		return;
	}
	try {
		Module.ccall('CL_WriteConfiguration', null, [], []);
	} catch (e) {
		console.log('[kaios] flushConfigToPersistentStorage failed: ' + describeError(e));
	}
}

// ---------------------------------------------------------------------
// Device Storage: find candidate baseq2 folders on the SD card
// ---------------------------------------------------------------------

// DOMError (used by the B2G/KaiOS Device Storage API for enumerate()
// failures) doesn't override toString() the way Error does, so
// '' + err on one gives the useless "[object DOMError]" -- pull out
// .name/.message by hand instead.
function describeError(err) {
	if (err && (err.name || err.message)) {
		return (err.name || 'Error') + (err.message ? ': ' + err.message : '');
	}
	return String(err);
}

// Blob.prototype.arrayBuffer() is a fairly recent addition (Firefox 69,
// 2019) that KaiOS 2.5's Gecko-48-class engine predates and almost
// certainly doesn't have -- read files the old, universally-supported
// way instead.
function readBlobAsArrayBuffer(blob) {
	return new Promise(function (resolve, reject) {
		var reader = new FileReader();
		reader.onload = function () { resolve(reader.result); };
		reader.onerror = function () { reject(reader.error); };
		reader.readAsArrayBuffer(blob);
	});
}

// pak0.pak alone is commonly 70+MB. Reading a file that size in one
// FileReader call and writing it in one FS.writeFile call means
// holding the whole thing in memory at once, and running long enough
// without yielding back to the event loop that the OS can decide the
// app has hung -- both plausible on a phone. Stream it in small
// chunks instead: bounded peak memory, and a setTimeout between
// chunks so the browser/OS sees the page as alive throughout.
var COPY_CHUNK_SIZE = 512 * 1024;

function copyFileStreaming(file, dest, onProgress) {
	return new Promise(function (resolve, reject) {
		var stream;
		try {
			mkdirTreeFor(dest);
			stream = FS.open(dest, 'w');
		} catch (e) {
			reject(e);
			return;
		}

		var offset = 0;

		function done(err) {
			try { FS.close(stream); } catch (e) { /* already broken, nothing to do */ }
			if (err) { reject(err); } else { resolve(); }
		}

		function readNextChunk() {
			if (offset >= file.size) {
				done();
				return;
			}

			var end = Math.min(offset + COPY_CHUNK_SIZE, file.size);
			readBlobAsArrayBuffer(file.slice(offset, end)).then(function (buf) {
				var chunk = new Uint8Array(buf);
				try {
					FS.write(stream, chunk, 0, chunk.length, offset);
				} catch (e) {
					done(e);
					return;
				}
				offset = end;
				if (onProgress) {
					onProgress(offset / file.size);
				}
				// Yield back to the event loop between chunks instead
				// of racing straight into the next FileReader call.
				setTimeout(readNextChunk, 0);
			}, done);
		}

		readNextChunk();
	});
}

// Matches the top-level pak archives (baseq2/pak0.pak, pak1.pak, ...) --
// these hold effectively all of baseq2's bytes (pak0.pak alone is
// commonly 70+MB uncompressed) and are exactly what installLazyPakFile()
// below keeps out of RAM. Everything else (autoexec.cfg, mod dirs, that
// sort of thing) is small and still goes through copyFileStreaming().
var PAK_FILE_RE = /^pak\d+\.pak$/i;

// Give a MEMFS node a read() that pulls bytes straight from the SD-card
// File on demand instead of pre-loading them, so pak0.pak's ~70+MB of
// decompressed data never sits resident in the page's heap -- only
// whatever byte range the engine's own fseek()/fread() calls actually
// touch (the PAK directory at startup, then each asset as it's loaded).
// installLazyPakFile() is called once per raw pak ARCHIVE (pak0.pak,
// pak1.pak, ...), not once per embedded asset -- position/length below
// are raw byte offsets into that one archive file, with the C side
// doing its own per-asset fseek()/fread() against it (see
// FS_FOpenFile()'s pak branch, filesystem.c).
//
// The tricky part is that this read() must be synchronous: the engine's
// C fread() has no concept of waiting on a promise, and this build has
// no Asyncify support to fake one (it's a plain asm.js build). Blob.slice()
// gives a cheap, synchronous *view* of just the requested range without
// touching its bytes; XMLHttpRequest against a blob: URL of that slice,
// sent synchronously, is what actually pulls the bytes in. Synchronous
// XHR forbids responseType "arraybuffer" (it throws InvalidAccessError),
// so this uses the older overrideMimeType('text/plain; charset=x-user-
// defined') trick instead -- 1 byte in, 1 char code out, no arraybuffer
// support required. That's also what makes it work on KaiOS's old
// Gecko-48-class engine, same reasoning as readBlobAsArrayBuffer() above.
//
// ---------------------------------------------------------------------
// Fixed-size block cache (ring buffer, oldest-block eviction)
// ---------------------------------------------------------------------
// Without this, every single fread() the C engine issues against a
// not-yet-cached range re-does the whole Blob.slice()+XHR+decode round
// trip, even for bytes already fetched earlier this session -- demo-
// cycling revisits the same handful of maps repeatedly, and each
// revisit redid the *entire* BSP/texture load (dozens of small lump
// reads) from scratch. Real-device evidence: KAIOS_HEARTBEAT_GAP (a
// setInterval, unrelated to rendering) stalling by the same ~20-23s as
// KAIOS_JS_TICK during a map reload -- proof the whole JS thread was
// synchronously blocked, not just requestAnimationFrame being
// deprioritized, right when a previously-visited map got reloaded.
//
// Caching every touched byte range forever (keep growing) would fix the
// re-fetch cost but trades it for unbounded growth across a long
// session -- exactly the OOM risk this device's tight RAM budget can't
// absorb (RSS has been observed well past 500MB on this hardware
// without this). Instead: one fixed-size, pre-allocated ArrayBuffer,
// carved into fixed-size blocks and reused as a ring buffer. A block
// gets fetched once on first touch; once the ring wraps back around to
// a block's slot, that block's old content is simply overwritten in
// place and its old owner's cache entry evicted -- bounded memory that
// never grows past LAZY_PAK_CACHE_BYTES no matter how long the session
// runs, while still turning every revisit of recently-touched content
// into a plain memory copy instead of a fresh sync-XHR round trip.
//
// 8MB, not the original 24MB this shipped with. This arena is a flat,
// permanent allocation from the moment app.js runs -- it doesn't exist
// in the pre-WebAudio-backend reference build (2026-08-07) at all, and
// a real-device A/B comparing that build against current HEAD (both on
// SDL audio, both software-rendered) showed HEAD still growing memory
// noticeably faster despite the recurring mid-level stutter itself
// turning out to be the WebAudio backend (see s_backend), not pak
// cache misses. That undercuts the original sizing rationale: 24MB was
// picked to survive *revisiting* a whole level's assets without a
// re-fetch, but revisits turning out not to be the dominant remaining
// cost means paying for that much headroom by default isn't earning
// its keep. The one-time first-load freeze this cache was never able
// to help with either way (nothing's cached yet on a first touch).
// 8MB still covers plenty of same-block re-touches within a single
// level; tune this constant alone if later testing wants it back up.
var LAZY_PAK_CACHE_BYTES = 8 * 1024 * 1024;
// 256KB. A real-device A/B test tried bumping this to 1MB on the
// theory that sync-XHR call overhead dominates over bytes moved, so
// 4x fewer, chunkier fetches should cost less overall -- the opposite
// happened. Same three cold map loads (Outer Base->base1,
// ->Installation/base2, ->Comm Center/base3), captured with
// KAIOS_JS_TICK/KAIOS_HEARTBEAT_GAP both sessions: 256KB blocked for
// ~16.0s/6.3s/6.4s; 1MB blocked for ~21.8s/10.8s/10.8s on the *same*
// loads -- worse across the board, not better. Root cause: sync XHR
// forbids responseType "arraybuffer" on this engine (InvalidAccess-
// Error), so every byte has to go through the char-by-char
// text.charCodeAt() decode loop below regardless of block size --
// that cost scales with total bytes moved, not call count. BSP/model
// loading does scattered, non-contiguous reads (separate lumps at
// separate offsets), so a bigger block mostly fetches+decodes bytes
// that specific read didn't need -- quadrupling the block size
// roughly quadrupled the wasted decode work per touch. Left at the
// original, actually-validated size; if anything, *smaller* blocks
// are the direction later testing should try, not bigger.
var LAZY_PAK_BLOCK_BYTES = 256 * 1024;
var LAZY_PAK_BLOCK_COUNT = LAZY_PAK_CACHE_BYTES / LAZY_PAK_BLOCK_BYTES;
var lazyPakCache = new Uint8Array(LAZY_PAK_CACHE_BYTES);
// Ring of block slots -- each holds at most one {node, blockIndex} tag,
// so evicting a slot just means forgetting that tag (the bytes
// themselves get overwritten in place by whatever claims the slot
// next, no separate free/clear step needed).
var lazyPakSlotTag = new Array(LAZY_PAK_BLOCK_COUNT).fill(null);
var lazyPakSlotCursor = 0;
// node -> {blockIndex: slotIndex}, for O(1) hit/miss lookup on read().
// A plain WeakMap-of-objects instead of a single flat map keyed by
// "node+blockIndex" string -- avoids building a string key on every
// single read() call, which runs in the hot path.
var lazyPakNodeBlocks = new WeakMap();

function lazyPakBlockMapFor(node) {
	var m = lazyPakNodeBlocks.get(node);
	if (!m) {
		m = Object.create(null);
		lazyPakNodeBlocks.set(node, m);
	}
	return m;
}

function lazyPakEvictSlot(slot) {
	var tag = lazyPakSlotTag[slot];
	if (tag) {
		delete lazyPakBlockMapFor(tag.node)[tag.blockIndex];
		lazyPakSlotTag[slot] = null;
	}
}

function lazyPakStoreBlock(node, blockIndex, bytes) {
	var slot = lazyPakSlotCursor;
	lazyPakSlotCursor = (lazyPakSlotCursor + 1) % LAZY_PAK_BLOCK_COUNT;
	lazyPakEvictSlot(slot);
	lazyPakCache.set(bytes, slot * LAZY_PAK_BLOCK_BYTES);
	lazyPakSlotTag[slot] = { node: node, blockIndex: blockIndex };
	lazyPakBlockMapFor(node)[blockIndex] = slot;
	return slot;
}

// Fetches one arbitrary byte range via the retry+sync-XHR machinery,
// decoded to a plain Uint8Array. One retry on failure: a real device
// was seen to have this read fail on its *second* use in a session
// while the very first use (moments earlier) succeeded -- suggestive of
// a one-off cold-start hiccup in the browser's sync-XHR/blob-URL
// machinery rather than a real, repeatable fault. A single retry is
// cheap and turns a transient hiccup into a stall instead of a fatal
// engine error; a genuinely broken read still fails after the retry
// same as before.
function lazyPakFetchRange(file, start, end) {
	var slice = file.slice(start, end);
	var text, lastErr;

	for (var attempt = 0; attempt < 2; attempt++) {
		var url = URL.createObjectURL(slice);
		try {
			var xhr = new XMLHttpRequest();
			xhr.overrideMimeType('text/plain; charset=x-user-defined');
			xhr.open('GET', url, false);
			xhr.send(null);
			text = xhr.responseText;
			lastErr = null;
			break;
		} catch (e) {
			lastErr = e;
		} finally {
			URL.revokeObjectURL(url);
		}
	}
	if (lastErr) {
		throw lastErr;
	}

	var n = Math.min(text.length, end - start);
	var out = new Uint8Array(n);
	for (var i = 0; i < n; i++) {
		out[i] = text.charCodeAt(i) & 0xff;
	}
	return out;
}

// MEMFS.ops_table.file.{node,stream} are shared objects reused by every
// plain-file node it creates -- mutating them in place would break reads
// for every other file, not just this one. node.stream_ops is replaced
// wholesale with a private object instead.
function installLazyPakFile(dest, file) {
	mkdirTreeFor(dest);
	// 292 = 0444 (r--r--r--). Octal literals are invalid under 'use
	// strict' unless written as 0o444 -- spelled out in decimal instead
	// to sidestep the question of whether this build's target engines
	// all parse the ES6 0o... form.
	var node = FS.create(dest, 292);
	node.usedBytes = file.size;

	node.stream_ops = {
		llseek: MEMFS.stream_ops.llseek,
		read: function (stream, buffer, offset, length, position) {
			if (position >= file.size) {
				return 0;
			}

			var end = Math.min(position + length, file.size);
			var total = end - position;
			var p = position;

			while (p < end) {
				var blockIndex = Math.floor(p / LAZY_PAK_BLOCK_BYTES);
				var blockStart = blockIndex * LAZY_PAK_BLOCK_BYTES;
				var blockEnd = Math.min(blockStart + LAZY_PAK_BLOCK_BYTES, file.size);

				var slot = lazyPakBlockMapFor(node)[blockIndex];
				if (slot === undefined) {
					var fetched = lazyPakFetchRange(file, blockStart, blockEnd);
					slot = lazyPakStoreBlock(node, blockIndex, fetched);
				}

				var copyStart = Math.max(p, blockStart);
				var copyEnd = Math.min(end, blockEnd);
				var srcOff = slot * LAZY_PAK_BLOCK_BYTES + (copyStart - blockStart);
				var dstOff = offset + (copyStart - position);
				var len = copyEnd - copyStart;

				for (var i = 0; i < len; i++) {
					buffer[dstOff + i] = lazyPakCache[srcOff + i];
				}

				p = copyEnd;
			}

			return total;
		},
		write: function () {
			// pak files are never written to -- fail loud instead of
			// silently doing nothing if that assumption ever breaks.
			// 63 is EPERM in this build's (WASI-derived) errno numbering,
			// confirmed against FS's own genericErrors[44]===ENOENT and
			// llseek's negative-position throw(28)===EINVAL above.
			throw new FS.ErrnoError(63 /* EPERM */);
		}
	};

	return node;
}

// `onFile`, when given, fires once per file as the cursor delivers it
// (DeviceStorage.enumerate() streams results one at a time, not as a
// single bulk callback) -- the only way to show any sign of life during
// a full, unscoped card enumerate() (discoverBaseq2()'s whole-card scan
// below), which can take long enough on a real, populated SD card that
// a static "Scanning..." status with no feedback at all reads as a
// frozen/hung app rather than one still working.
function enumerateStorage(storage, path, onFile) {
	return new Promise(function (resolve, reject) {
		var files = [];
		var cursor = storage.enumerate(path || '');

		cursor.onsuccess = function () {
			var file = cursor.result;
			if (!file) {
				resolve(files);
				return;
			}
			files.push(file);
			if (onFile) {
				onFile(files.length);
			}
			cursor.continue();
		};
		cursor.onerror = function () {
			reject(cursor.error);
		};
	});
}

// navigator.getDeviceStorage('sdcard') (singular) only ever returns
// ONE DeviceStorage -- the platform's default volume for that type,
// which on a device with both built-in storage and a removable SD
// slot is not guaranteed to be the actual physical SD card. The
// (plural) getDeviceStorages('sdcard') form returns one DeviceStorage
// per available volume of that type -- looping over all of them is
// what actually searches both internal storage and an inserted SD
// card, instead of silently only ever checking whichever one happens
// to be default. Falls back to the singular form for engines that
// only implement that (older devices, or this project's earlier code
// before this existed), and to an empty list if neither exists at all
// (checked by every caller before using the Device Storage API).
function getAllDeviceStorages() {
	if (navigator.getDeviceStorages) {
		return navigator.getDeviceStorages('sdcard') || [];
	}
	if (navigator.getDeviceStorage) {
		var s = navigator.getDeviceStorage('sdcard');
		return s ? [s] : [];
	}
	return [];
}

// Enumerates `path` on every storage in `storages` in parallel and
// returns each one's own {storage, files} pair (never rejects for an
// individual storage's failure -- one bad/missing volume shouldn't
// abort the whole search when others might still have baseq2).
//
// `onProgress`, when given, is called with the running total file count
// across every storage combined every time any one of them delivers
// another file -- see enumerateStorage()'s onFile for why.
function enumerateAllStorages(storages, path, onProgress) {
	var totalSoFar = 0;
	return Promise.all(storages.map(function (storage) {
		return enumerateStorage(storage, path, onProgress && function () {
			totalSoFar++;
			onProgress(totalSoFar);
		}).then(function (files) {
			return { storage: storage, files: files };
		}, function (err) {
			console.log('[kaios] enumerate failed on storage "' + storage.storageName +
				'": ' + describeError(err));
			return { storage: storage, files: [] };
		});
	}));
}

// Case-insensitive on both the folder name ("BaseQ2", "BASEQ2", ... all
// match) and the pak file name -- matched by basename instead of a fixed
// lowercase path string, so the actual on-disk casing of the baseq2
// folder (dirName below) can be recovered and reused as-is by
// copyBaseq2() instead of assuming it matches GAMEDIR's casing exactly.
var PAK0_RE = /\/pak0\.pak$/i;

// `storageName` tags every resulting candidate with which volume it
// came from -- needed to re-target the same physical storage later
// (playFast()'s scoped re-enumerate, a cache-refresh after adding
// files) instead of assuming whichever one happens to be default.
function findBaseq2Candidates(files, storageName) {
	var candidates = [];
	var seen = {};
	for (var i = 0; i < files.length; i++) {
		var name = files[i].name || '';
		var normalized = '/' + name.replace(/^\/+/, '');
		if (!PAK0_RE.test(normalized)) {
			continue;
		}
		var parts = normalized.split('/');
		var dirName = parts[parts.length - 2] || '';
		if (dirName.toLowerCase() !== GAMEDIR) {
			continue;
		}
		// parts[0] is always '' (normalized starts with a leading slash),
		// so a nested baseq2's root would otherwise come out of the
		// slice+join below as "/Games" -- a leading slash DeviceStorage's
		// own enumerate(path) does not expect on the relative path it
		// takes (see playFast()'s scopedPath, the actual consumer that
		// matters: a candidate found here but never actually reachable
		// through a scoped re-enumerate silently falls back to a full
		// rescan on every single Play press instead of booting directly).
		// Stripped here, at the source, so every consumer of `root` gets
		// the same "never a leading slash" convention -- copyBaseq2()
		// below adds its own leading slash explicitly instead of relying
		// on one already being embedded in root.
		var root = parts.slice(0, parts.length - 2).join('/').replace(/^\/+/, '');
		var key = root + '/' + dirName;
		if (seen[key]) {
			continue;
		}
		seen[key] = true;
		candidates.push({ root: root, dirName: dirName, file: files[i], storageName: storageName });
	}
	return candidates;
}

// Runs findBaseq2Candidates() across every storage's own listing (see
// enumerateAllStorages()) and returns the merged candidates plus a
// storageName -> files lookup, so a caller that ends up proceeding
// with one particular candidate can hand copyBaseq2() that SAME
// storage's file listing without having to re-enumerate it.
function discoverBaseq2(storages, path, onProgress) {
	return enumerateAllStorages(storages, path, onProgress).then(function (perStorage) {
		var candidates = [];
		var filesByStorage = {};

		for (var i = 0; i < perStorage.length; i++) {
			var entry = perStorage[i];
			filesByStorage[entry.storage.storageName] = entry.files;
			candidates = candidates.concat(findBaseq2Candidates(entry.files, entry.storage.storageName));
		}

		return { candidates: candidates, filesByStorage: filesByStorage };
	});
}

// ---------------------------------------------------------------------
// Picker UI (DPAD up/down, OK to select)
// ---------------------------------------------------------------------

function showPicker(candidates, onChosen) {
	var selected = 0;

	function render() {
		renderMenuItems(pickerListEl, candidates.map(function (c) {
			return (c.root || '') + '/' + c.dirName;
		}), selected);
	}

	function onKeyDown(ev) {
		if (ev.key === 'ArrowUp') {
			selected = Math.max(0, selected - 1);
			render();
		} else if (ev.key === 'ArrowDown') {
			selected = Math.min(candidates.length - 1, selected + 1);
			render();
		} else if (ev.key === 'Enter') {
			activeMenuKeyHandler = null;
			onChosen(candidates[selected]);
		} else if (ev.key === 'SoftRight') {
			activeMenuKeyHandler = null;
			showMainMenu();
		}
	}

	render();
	activeMenuKeyHandler = onKeyDown;
}

// ---------------------------------------------------------------------
// Copy the chosen baseq2 folder into the Emscripten virtual FS
// ---------------------------------------------------------------------

function setStatus(text, fraction) {
	pickerEl.style.display = 'none';
	statusEl.style.display = 'block';
	statusTextEl.textContent = text;
	if (typeof fraction === 'number') {
		statusBarEl.style.width = Math.round(fraction * 100) + '%';
	}
	// Cleared by default -- only performBaseq2Scan()'s cancellable scan
	// currently sets this, and every other setStatus() caller (loading
	// engine scripts, copying baseq2, ...) has no cancel handler wired
	// up at all, so leaving old hint text on screen for those would
	// promise a way out that pressing RSK wouldn't actually do anything
	// for.
	statusHintRightEl.textContent = '';
}

function mkdirTreeFor(path) {
	var parts = path.split('/').filter(Boolean);
	var cur = '';
	for (var i = 0; i < parts.length - 1; i++) {
		cur += '/' + parts[i];
		try { FS.mkdir(cur); } catch (e) { /* already exists */ }
	}
}

// `files` is the full listing already fetched by the top-level scan in
// start() -- no need to enumerate() the storage a second time, that's
// one more thing that can fail for no benefit, we already have exactly
// the data we need.
//
// `choice` is one of findBaseq2Candidates()'s entries: {root, dirName}.
// dirName is the folder's *actual* on-disk casing (e.g. "BaseQ2"), not
// GAMEDIR's fixed lowercase spelling -- matching against GAMEDIR here
// instead would silently filter toCopy down to nothing whenever the
// card's folder isn't spelled exactly "baseq2", since file.name below
// carries the real casing from the storage listing.
function copyBaseq2(files, choice) {
	var root = choice.root;
	// `root` (never leading-slashed, see findBaseq2Candidates()) is
	// baseq2's *parent* directory, "" when baseq2 sits right at the SD
	// card root -- the leading slash is added explicitly here instead of
	// via root itself, so this stays correct whether root is empty or
	// not, and matches `norm` below (built the same "always exactly one
	// leading slash" way from the raw storage listing).
	var srcPrefix = '/' + (root ? root + '/' : '') + choice.dirName + '/';

	// scrnshot/ is pure engine *output* -- the engine creates it itself
	// under its writable config dir, never something that needs to be
	// supplied as input data. Skipping it if present on the SD card
	// (e.g. copied back from a previous session) avoids wasting memory
	// on data that isn't used for anything.
	//
	// save/ is deliberately NOT skipped, unlike scrnshot/ -- baseq2/save/
	// is exactly where savegames from a real PC/other-platform Quake II
	// install live, and copying it in here is what makes the "Load Game"
	// menu pick those up as native saves on this device too, no import
	// step needed. New saves the engine itself writes this session land
	// under the IDBFS-persisted homedir (see FS_BuildGameSpecificSearchPath's
	// __EMSCRIPTEN__ branch, filesystem.c) rather than back into this
	// MEMFS copy, so there's no double-counting/staleness between the
	// two on the next launch.
	var SKIP_PREFIXES = ['scrnshot/'];

	// music/ is real input data (unlike save/scrnshot above) but a
	// large one on a slow SD-card read -- not worth the time/memory if
	// the Audio settings screen's Music toggle is off anyway, since
	// nothing will ever play it this session.
	var musicItem = AUDIO_ITEMS.filter(function (it) { return it.id === 'music'; })[0];
	if (musicItem && !getItemIndex(musicItem)) {
		SKIP_PREFIXES.push('music/');
	}

	var toCopy = files.filter(function (f) {
		var norm = '/' + f.name.replace(/^\/+/, '');
		if (norm.toLowerCase().indexOf(srcPrefix.toLowerCase()) !== 0) {
			return false;
		}
		var rel = norm.slice(srcPrefix.length).toLowerCase();
		for (var i = 0; i < SKIP_PREFIXES.length; i++) {
			if (rel.indexOf(SKIP_PREFIXES[i]) === 0) {
				return false;
			}
		}
		return true;
	});

	var i = 0;
	function next() {
		if (i >= toCopy.length) {
			return Promise.resolve();
		}
		var file = toCopy[i];
		// Lowercased on purpose: the bsp/model/sound data referenced from
		// inside pak0.pak (and the engine's own hardcoded "pak%d.pak" scan,
		// filesystem.c) is always lowercase by id Software's own
		// convention, but the SD card's actual on-disk casing can be
		// anything (some card readers/OSes normalize case, some preserve
		// whatever the source zip/archive had) -- normalizing every
		// destination path here is what makes "case insensitive to the
		// folder and files inside" (not just the top-level baseq2 folder
		// itself) actually true, and is what lets installLazyPakFile()'s
		// PAK_FILE_RE match below land pak0.pak at the exact lowercase
		// path FS_SetGamedir() (filesystem.c) goes looking for.
		var rel = (('/' + file.name.replace(/^\/+/, '')).slice(srcPrefix.length)).toLowerCase();
		var dest = '/' + GAMEDIR + '/' + rel;

		if (rel.indexOf('save/') === 0) {
			// menu.c's savegame browser (M_Menu_LoadGame_f et al) only
			// ever looks under FS_Gamedir() -- forced to the
			// IDBFS-persisted homedir on this platform (see
			// filesystem.c's FS_BuildGameSpecificSearchPath(),
			// __EMSCRIPTEN__ branch), not this plain MEMFS copy of
			// baseq2. Route save/ files there instead of GAMEDIR so an
			// existing SD-card save shows up as a native one, same
			// destination new in-game saves already land in.
			dest = getPersistentConfigDir() + '/' + GAMEDIR + '/' + rel;

			// Never clobber a save the engine itself already wrote this
			// homedir this session or a previous one -- the SD card is
			// only ever an *import* source for saves, never something
			// that should be able to overwrite further-along local
			// progress just because a folder of the same name exists on
			// the card too.
			try {
				if (FS.analyzePath(dest).exists) {
					i++;
					return next();
				}
			} catch (e) {
				/* analyzePath itself failing is not a reason to skip the
				 * import -- fall through and let copyFileStreaming's own
				 * error handling deal with anything actually wrong. */
			}
		}

		if (PAK_FILE_RE.test(rel)) {
			// The bulk of baseq2's bytes live in here -- keep it out of
			// RAM entirely instead of copying it in. See
			// installLazyPakFile() for why.
			setStatus('Indexing ' + rel, i / toCopy.length);
			try {
				installLazyPakFile(dest, file);
			} catch (e) {
				return Promise.reject(e);
			}
			i++;
			return next();
		}

		setStatus('Copying ' + rel, i / toCopy.length);

		return copyFileStreaming(file, dest, function (fileFraction) {
			// Large files can take a while on their own -- show progress
			// within the file, not just "still on this same file" with
			// no feedback.
			setStatus('Copying ' + rel, (i + fileFraction) / toCopy.length);
		}).then(function () {
			i++;
			return next();
		});
	}
	return next();
}

// The compiled engine (quake2-kaios.js, ~3MB, plus its .mem file) and its
// generated autoexec.cfg.js used to load as static <script> tags before
// app.js even started scanning the SD card -- meaning they'd start
// running, and quake2-kaios.js's own top-level run()/doRun() sequence
// would resolve, before we'd even confirmed a baseq2 folder exists, let
// alone copied anything into it. A real device showed the engine's C
// main() executing -- and failing on missing FS data -- before our own
// JS had called Module.callMain() at all, consistent with exactly that
// ordering (root mechanism still unconfirmed; see the guard this
// installs below). Loading these scripts on demand, only once a baseq2
// folder is confirmed and we're about to copy it in, closes that gap
// instead of just hoping noInitialRun and script tag order cover it.
//
// FS/MEMFS (used by copyBaseq2 above) and Module.callMain (used by
// bootEngine below) don't exist until quake2-kaios.js has actually run,
// so nothing that touches them can happen before this resolves.
var callMainArmed = false;
var engineScriptsLoaded = false;

function loadScript(src) {
	return new Promise(function (resolve, reject) {
		var el = document.createElement('script');
		el.src = src;
		el.onload = function () { resolve(); };
		el.onerror = function () { reject(new Error('Failed to load ' + src)); };
		document.body.appendChild(el);
	});
}

// Resolves once Emscripten's own runtime (including the asm.js module
// itself -- Module.asm) has finished its own async instantiation.
//
// Found the hard way: with noInitialRun actually working for the first
// time (module-init.js), Module.callMain([]) below started throwing
// "TypeError: Module.asm is undefined" instead of ever reaching main().
// In every earlier round noInitialRun had been silently inert (CSP was
// dropping the inline <script> that set it -- see module-init.js), so
// Emscripten's own run()/doRun() -- which only calls callMain() once
// runDependencies hits 0, i.e. once the asm.js module is actually ready
// -- was firing main() on its own before our explicit call ever got a
// chance to race ahead of it. Now that that accidental synchronization
// is gone, this call has to wait for the same readiness signal doRun()
// itself waits for: Module.onRuntimeInitialized, called unconditionally
// once initRuntime()/preMain() finish, regardless of noInitialRun.
var runtimeReadyResolve;
var runtimeReadyPromise = new Promise(function (resolve) {
	runtimeReadyResolve = resolve;
});

function loadEngineScripts() {
	if (engineScriptsLoaded) {
		return Promise.resolve();
	}
	// autoexec.cfg itself is no longer loaded here -- it used to be
	// pre-baked at build time into a generated autoexec.cfg.js
	// (window.KAIOS_AUTOEXEC_CFG), which meant any edit to it needed a
	// full engine rebuild to take effect. writeAutoexec() below now
	// fetches the plain text file at runtime instead, once per launch.
	return loadScript('quake2-kaios.js').then(function () {
		engineScriptsLoaded = true;

		if (Module.calledRun) {
			// Already ready by the time we got here -- no event left to wait for.
			runtimeReadyResolve();
		} else {
			Module.onRuntimeInitialized = runtimeReadyResolve;
		}

		// Physically disarm Module.callMain() (only just defined by the
		// script that just loaded) until bootEngine() explicitly arms it,
		// after copyBaseq2() confirms the FS is actually populated. Any
		// call that sneaks in before that logs a stack trace instead of
		// quietly running main() over an empty filesystem.
		var realCallMain = Module.callMain;
		Module.callMain = function (args) {
			if (!callMainArmed) {
				console.log('[kaios] BLOCKED premature Module.callMain() -- ' +
					'baseq2 is not mounted yet.\n' + (new Error().stack || '(no stack)'));
				return;
			}
			return realCallMain.call(Module, args);
		};
	});
}

// autoexec.cfg used to be embedded at build time (a generated
// autoexec.cfg.js defining window.KAIOS_AUTOEXEC_CFG, loaded as a
// <script> tag) -- editing it meant a full engine rebuild before the
// change took effect. Fetching the plain text file here instead, once
// per app launch, means autoexec.cfg can be edited and redeployed on
// its own, same as any other static asset next to index.html.
function fetchText(url) {
	return new Promise(function (resolve, reject) {
		var xhr = new XMLHttpRequest();
		xhr.open('GET', url, true);
		xhr.onload = function () {
			// status 0 covers the packaged-app (app://) origin, where
			// XHR against a local file doesn't always populate a real
			// HTTP status.
			if (xhr.status === 200 || xhr.status === 0) {
				resolve(xhr.responseText);
			} else {
				reject(new Error('HTTP ' + xhr.status));
			}
		};
		xhr.onerror = function () {
			reject(new Error('network error'));
		};
		xhr.send(null);
	});
}

function writeAutoexec() {
	return fetchText('autoexec.cfg').then(function (text) {
		if (!text) {
			return;
		}
		// Appended last so it wins over whatever the static file itself
		// sets for the same cvars -- see buildLauncherSettingsCfg()'s
		// comment for why only explicitly-toggled entries end up here.
		var overrides = buildLauncherSettingsCfg();
		if (overrides) {
			text += '\n\n// KaiOS launcher settings screen overrides\n' + overrides + '\n';
		}
		// Write to the gamedir (read search path) and to the engine's
		// writable config dir (~/.yq2/baseq2 under Emscripten's
		// synthesized HOME) so it's found regardless of which one
		// FS_LoadFile checks.
		try {
			mkdirTreeFor('/' + GAMEDIR + '/autoexec.cfg');
			FS.writeFile('/' + GAMEDIR + '/autoexec.cfg', text);
		} catch (e) { console.log('[kaios] ' + e); }

		try {
			var home = (typeof ENV !== 'undefined' && ENV.HOME) || '/home/web_user';
			var writeDir = home + '/.yq2/' + GAMEDIR;
			mkdirTreeFor(writeDir + '/autoexec.cfg');
			FS.writeFile(writeDir + '/autoexec.cfg', text);
		} catch (e) { console.log('[kaios] ' + e); }

		// See buildKaiosDefaultsCfg()'s comment -- a standing fail-safe,
		// independent of whatever the player has picked from the
		// settings screens, reachable at any time via the in-game
		// console ("exec kaiosdefaults.cfg").
		try {
			var defaults = buildKaiosDefaultsCfg();
			mkdirTreeFor('/' + GAMEDIR + '/kaiosdefaults.cfg');
			FS.writeFile('/' + GAMEDIR + '/kaiosdefaults.cfg', defaults);
		} catch (e) { console.log('[kaios] ' + e); }
	}, function (err) {
		// Not fatal -- default.cfg (inside pak0.pak) still gives a
		// playable, if unconfigured, control scheme. Missing KaiOS
		// keypad binds is a worse experience than none of this code
		// running at all, so don't block boot on it.
		console.log('[kaios] failed to fetch autoexec.cfg at runtime: ' + describeError(err));
	});
}

var bootedOnce = false;

function bootEngine() {
	// Never call Module.callMain() more than once, from wherever it
	// might get triggered -- main() isn't designed to run twice in the
	// same module instance, and it's a cheap, unconditional guard
	// against any surprise re-entry (e.g. a host runtime that keeps
	// this page's JS context alive and re-drives it instead of doing a
	// full reload) calling in with a still-in-progress copy.
	if (bootedOnce) {
		return;
	}
	bootedOnce = true;

	// Last-ditch sanity check: don't start the engine over data that
	// isn't actually there. Cheap (just a stat, not a read) and turns
	// "boots into a wall of missing-file errors" into a clear message.
	try {
		var st = FS.stat('/' + GAMEDIR + '/pak0.pak');
		if (!st || st.size <= 0) {
			throw new Error('pak0.pak is empty or missing');
		}
	} catch (e) {
		setStatus('baseq2 data is missing or incomplete (' + describeError(e) + ').\n' +
			'Reopen the app to retry.');
		bootedOnce = false;
		return;
	}

	setStatus('Starting...', 1);

	// writeAutoexec() now fetches its text at runtime (see its own
	// comment) instead of reading a build-time-baked global, so it has
	// to actually finish -- autoexec.cfg needs to already be on the FS
	// before main() gets anywhere near "exec autoexec.cfg" -- before
	// arming/calling Module.callMain() below.
	writeAutoexec().then(function () {
		// #canvas is display:block from first paint now (see index.html)
		// -- only the #status overlay covering it needs to go away here.
		statusEl.style.display = 'none';

		engineStarted = true;
		// Only past this point is Module.callMain() allowed to actually
		// do anything -- see the guard installed at the top of this file.
		callMainArmed = true;

		// Wait for Module.asm (and the rest of the runtime) to actually be
		// ready -- see runtimeReadyPromise's comment above for why this is
		// necessary now and wasn't before.
		return runtimeReadyPromise;
	}).then(function () {
		// NOTE: this emsdk version's callMain() ignores its args entirely
		// (hardcodes argc=0) -- and "vid_width"/"vid_height" were never
		// real cvars here anyway (r_customwidth/r_customheight are, see
		// autoexec.cfg, which is what actually sets the resolution).
		Module.callMain([]);
	});
}

// NOTE: baseq2's loose files are copied fresh into plain (non-persistent)
// MEMFS on every launch, not cached across launches -- but those are
// small (a few KB of configs), so redoing that copy each launch is
// cheap. pak0.pak/pak1.pak/... are the part that used to make this
// expensive, and installLazyPakFile() takes them out of this picture
// entirely: they're never copied anywhere, in or out of MEMFS, so
// there's nothing about them left to persist.
//
// An earlier attempt used IDBFS (IndexedDB-backed FS) to persist the
// *old* whole-baseq2 copy so the slow SD-card read would only happen
// once, but FS.syncfs()'s per-file write is Emscripten library code,
// not ours, and evidently had the same "hold a whole 70+MB file in
// memory as one write" problem the SD-card copy itself had before it
// was switched to chunked streaming -- and unlike that one, this one
// wasn't ours to chunk. Pulled back out at the time to prioritize a
// build that reliably boots at all; moot now that the file it choked
// on is never copied in the first place.

// ---------------------------------------------------------------------
// Main menu (shown first, on every launch): find baseq2 / settings.
// ---------------------------------------------------------------------

// Set once a baseq2 scan+copy has genuinely succeeded (see proceed()
// inside startBaseq2Scan()) -- gates both "Play" showing up on the
// main menu at all (there's nothing to play until baseq2 has been
// found at least once) and the boot-time skip timer below, per spec:
// the timer only makes sense once there's something to skip *to*.
var BASEQ2_CONFIRMED_KEY = 'kaios_baseq2_confirmed';

function isBaseq2Confirmed() {
	return localStorage.getItem(BASEQ2_CONFIRMED_KEY) === '1';
}

// Set once, the first time the app has ever launched (see
// autoScanOnFirstLaunch()) -- distinct from BASEQ2_CONFIRMED_KEY, which
// only flips on a *successful* scan. This one flips regardless of
// outcome, so a failed first-launch scan doesn't keep silently
// re-running itself on every later launch; after the first attempt the
// player drives it manually via the "Search baseq2" menu item instead.
var EVER_LAUNCHED_KEY = 'kaios_ever_launched';

function hasEverLaunched() {
	return localStorage.getItem(EVER_LAUNCHED_KEY) === '1';
}

function markEverLaunched() {
	localStorage.setItem(EVER_LAUNCHED_KEY, '1');
}

// Set by a scan (auto or manual) that comes back empty-handed or errors
// out, surfaced as mainMenuItems()'s Play caption -- the same spot that
// otherwise reads "pak0.pak not found". Cleared on a successful scan.
var mainMenuError = '';

// Set by showMainMenu() while (and only while) the main menu is the
// screen on display, so a scan that finishes after the player has
// already backed out of it (see performBaseq2Scan()'s cancel handling)
// can refresh it in place instead of forcing them back to a screen they
// deliberately left. Cleared here since every screen transition --
// including back into showMainMenu() itself, which reassigns it --
// calls hideAllScreens() first.
var refreshMainMenuIfShown = null;

function hideAllScreens() {
	menuEl.style.display = 'none';
	pickerEl.style.display = 'none';
	settingsEl.style.display = 'none';
	statusEl.style.display = 'none';
	refreshMainMenuIfShown = null;
}

function mainMenuItems() {
	var confirmed = isBaseq2Confirmed();
	var items = [];
	// Always shown, even before baseq2 has ever been found -- disabled
	// (see showMainMenu()'s render(), which also surfaces `caption`
	// underneath) instead of only appearing once confirmed, so first
	// launch shows *why* Play can't be used yet rather than just not
	// offering it. The first-launch scan (see start() below) is what
	// enables it, without the player having to manually press "Search
	// baseq2" first.
	items.push({
		label: 'Play',
		action: playFast,
		disabled: !confirmed,
		caption: confirmed ? '' : (mainMenuError || 'pak0.pak not found')
	});
	items.push({ label: 'Search baseq2', action: startBaseq2Scan });
	items.push({ label: 'Settings', action: showSettingsGroups });
	items.push({ label: 'Exit', action: exitApp });
	return items;
}

/* Same approach as the in-game "quit" command's Sys_Quit() (see
 * engine/src/backends/unix/system.c) -- window.close() is the
 * documented way for a privileged packaged KaiOS app to close itself,
 * honored from the app's own top-level document even though a plain
 * browser tab would normally refuse it. Needed here too since the
 * launcher menu (this file) runs before the engine ever boots, so
 * Sys_Quit() itself isn't reachable yet from "Exit" on the main menu. */
function exitApp() {
	try {
		window.close();
	} catch (e) {
		console.log('[kaios] window.close() failed: ' + e);
	}
}

function showMainMenu() {
	var selected = 0;

	// mainMenuItems() is recomputed on every render(), not cached once
	// at screen-entry -- a "Search baseq2" scan finishing after the
	// player cancelled back to this screen (see performBaseq2Scan()'s
	// refreshMainMenuIfShown call) needs the very next render() to pick
	// up the now-confirmed state without a full re-entry into
	// showMainMenu().
	function render() {
		var items = mainMenuItems();
		var labels = items.map(function (it) { return it.label; });
		var disabledFlags = items.map(function (it) { return !!it.disabled; });
		renderMenuItems(menuListEl, labels, selected, disabledFlags);

		var current = items[selected];
		mainMenuCaptionEl.textContent = (current && current.caption) ? current.caption : '';
	}

	function onKeyDown(ev) {
		var items = mainMenuItems();
		if (ev.key === 'ArrowUp') {
			selected = (selected - 1 + items.length) % items.length;
			render();
		} else if (ev.key === 'ArrowDown') {
			selected = (selected + 1) % items.length;
			render();
		} else if (ev.key === 'Enter') {
			if (items[selected].disabled) {
				return;
			}
			activeMenuKeyHandler = null;
			items[selected].action();
		}
	}

	hideAllScreens();
	menuEl.style.display = 'block';
	render();
	activeMenuKeyHandler = onKeyDown;
	refreshMainMenuIfShown = render;
}

// ---------------------------------------------------------------------
// Boot-time skip timer: on any launch *after* baseq2 has already been
// found successfully once, a 3-second "press any key for settings"
// window is offered before auto-proceeding straight to Play -- per
// spec, first launches (or any launch where baseq2 was never
// successfully found) skip straight to the normal main menu instead,
// there being nothing yet to fast-path into.
// ---------------------------------------------------------------------

var BOOT_SKIP_MS = 3000;

function showBootSkipTimer() {
	var remaining = Math.ceil(BOOT_SKIP_MS / 1000);
	var deadline = Date.now() + BOOT_SKIP_MS;
	var intervalId = null;

	function render() {
		menuListEl.innerHTML = '';
		var li = document.createElement('li');
		li.textContent = 'Press any key for settings (' + remaining + ')';
		menuListEl.appendChild(li);
	}

	function finish(toMenu) {
		if (intervalId !== null) {
			clearInterval(intervalId);
			intervalId = null;
		}
		activeMenuKeyHandler = null;
		if (toMenu) {
			showMainMenu();
		} else {
			playFast();
		}
	}

	function onKeyDown() {
		// Any key at all cancels the fast path -- not just OK/arrows,
		// so this matches "press any key" literally instead of only
		// the subset PREVENT_DEFAULT already recognizes.
		finish(true);
	}

	hideAllScreens();
	menuEl.style.display = 'block';
	render();
	activeMenuKeyHandler = onKeyDown;

	intervalId = setInterval(function () {
		remaining = Math.ceil((deadline - Date.now()) / 1000);
		if (remaining <= 0) {
			finish(false);
			return;
		}
		render();
	}, 250);
}

// Like setStatus(), but for a dead-end (scan/copy failure) that would
// otherwise strand the player on the status screen with no way back --
// RSK returns to the main menu instead of leaving the app inert.
function showErrorWithBack(text) {
	setStatus(text + '\n\nRSK: back to menu');

	function onKeyDown(ev) {
		if (ev.key === 'SoftRight') {
			activeMenuKeyHandler = null;
			showMainMenu();
		}
	}
	activeMenuKeyHandler = onKeyDown;
}

// Remembered after a scan+copy has genuinely succeeded once (see
// proceedWithChoice() below) so subsequent "Play" presses don't have
// to re-enumerate the *entire* SD card just to find the same folder
// again -- see playFast()'s comment for the actual fast path this
// enables.
var BASEQ2_PATH_KEY = 'kaios_baseq2_path';

function getCachedBaseq2Choice() {
	var raw = localStorage.getItem(BASEQ2_PATH_KEY);
	if (!raw) {
		return null;
	}
	try {
		var choice = JSON.parse(raw);
		if (choice && typeof choice.root === 'string' && typeof choice.dirName === 'string') {
			return choice;
		}
	} catch (e) {
		/* corrupt/foreign value -- treat as no cache */
	}
	return null;
}

function setCachedBaseq2Choice(choice) {
	localStorage.setItem(BASEQ2_PATH_KEY, JSON.stringify({
		root: choice.root,
		dirName: choice.dirName,
		// which DeviceStorage volume (internal vs. an inserted SD card,
		// see getAllDeviceStorages()) this was found on -- playFast()
		// needs to re-resolve the *same* volume, not just assume
		// whichever one happens to be default.
		storageName: choice.storageName
	}));
}

// Shared tail end of both the full scan (startBaseq2Scan) and the fast
// cached-path retry (playFast) below -- `files` only needs to cover
// the baseq2 folder's own subtree (copyBaseq2() filters it down to
// that anyway), not the whole SD card.
function proceedWithChoice(files, choice) {
	setStatus('Loading engine...', 0);
	loadEngineScripts().then(function () {
		// Pull in last session's config.cfg (if any) before baseq2 is
		// populated or the engine boots -- see mountPersistentConfig()'s
		// comment for why this needs to happen here rather than after
		// autoexec.cfg is written.
		return mountPersistentConfig();
	}).then(function () {
		setStatus('Copying baseq2...', 0);
		return copyBaseq2(files, choice);
	}).then(function () {
		// Only marked once a copy has actually completed -- see
		// mainMenuItems()/showBootSkipTimer()'s comment for why this
		// specifically gates both the "Play" menu entry and the
		// boot-time skip timer.
		localStorage.setItem(BASEQ2_CONFIRMED_KEY, '1');
		setCachedBaseq2Choice(choice);
		bootEngine();
	}, function (err) {
		showErrorWithBack('Copy failed: ' + describeError(err));
	});
}

// Shared by the manual "Search baseq2" menu item and the
// first-launch-only automatic scan (start() below) -- searches every
// storage volume (internal + any inserted SD card, see
// getAllDeviceStorages()) from scratch, taking over the screen with a
// visible "Scanning..." status (and, if more than one candidate turns
// up, the folder picker) -- silently scanning in the background on
// first launch turned out to just look like nothing was happening at
// all.
//
// `launchOnSuccess` controls what a successful scan does next: true
// (first launch only) goes straight into loading/copying/booting, same
// as pressing Play immediately after -- first-time setup should feel
// like one continuous action, not "scan, then go find Play yourself".
// false (every manual "Search baseq2" press afterward) just confirms
// the folder and returns to the main menu with Play now enabled,
// without touching anything already installed -- re-scanning to pick up
// a changed/reinserted card shouldn't also force a reload the player
// didn't ask for.
function performBaseq2Scan(launchOnSuccess) {
	var storages = getAllDeviceStorages();
	if (storages.length === 0) {
		mainMenuError = 'No Device Storage API on this browser.';
		showMainMenu();
		return;
	}

	hideAllScreens();
	setStatus('Scanning for baseq2...');
	statusHintRightEl.textContent = 'Back';

	// A real, populated SD card can take long enough to walk in full
	// (every file on it, not just baseq2's own -- there's no way to
	// scope this down before knowing where baseq2 even is) that a
	// static "Scanning..." status with no way out and no visible
	// progress reasonably reads as a hung app, not just a slow one --
	// confirmed on real hardware. RSK backs out to the main menu without
	// actually stopping the scan (there's no cancel primitive for a
	// DeviceStorage cursor) -- it keeps running quietly and updates
	// state whenever it finishes instead.
	var cancelled = false;

	function onKeyDown(ev) {
		if (ev.key === 'SoftRight') {
			cancelled = true;
			activeMenuKeyHandler = null;
			mainMenuError = 'Still searching in the background...';
			showMainMenu();
		}
	}
	activeMenuKeyHandler = onKeyDown;

	function landOnMainMenu() {
		activeMenuKeyHandler = null;
		if (cancelled) {
			// The player already backed out to (or past) the main menu
			// -- refresh it in place instead of yanking them back to it
			// a second time once this finally resolves.
			if (typeof refreshMainMenuIfShown === 'function') {
				refreshMainMenuIfShown();
			}
		} else {
			showMainMenu();
		}
	}

	// DeviceStorage.enumerate() delivers results one file at a time, and
	// this scans the whole storage volume (not just baseq2's own) -- a
	// real card can easily have thousands of entries. A DOM text update
	// on every single one of them turned out to be exactly the kind of
	// per-call cost this hardware has shown all session to be genuinely
	// expensive at high frequency (same lesson as the console.log-costs-
	// 700ms+ finding this project's whole stutter investigation started
	// from) -- doing it unthrottled made the scan itself measurably
	// slower, not just louder, which is the opposite of the point.
	// Throttled to at most 2/second by wall-clock time instead of by
	// file count, so it stays cheap regardless of how many files the
	// card actually has.
	var lastProgressUpdate = 0;

	console.log('[kaios] baseq2 scan starting on ' + storages.length + ' storage(s): ' +
		storages.map(function (s) { return s.storageName; }).join(', '));

	discoverBaseq2(storages, '', function (count) {
		var now = Date.now();
		if (now - lastProgressUpdate < 500) {
			return;
		}
		lastProgressUpdate = now;
		console.log('[kaios] baseq2 scan: ' + count + ' files seen so far' +
			(cancelled ? ' (after cancel)' : ''));

		if (cancelled) {
			// Still visible even after backing out -- same "Still
			// searching..." caption as the cancel keypress itself set,
			// just kept live instead of static, so there's some sign
			// this is still doing something without needing devtools
			// attached to see the console.log above.
			mainMenuError = 'Still searching... (' + count + ' files)';
			if (typeof refreshMainMenuIfShown === 'function') {
				refreshMainMenuIfShown();
			}
			return;
		}

		// setStatus() clears the hint by default (see its own comment)
		// -- re-set it on every one of these progress updates, not just
		// the initial setStatus() call above.
		setStatus('Scanning for baseq2... (' + count + ' files)');
		statusHintRightEl.textContent = 'Back';
	}).then(function (result) {
		console.log('[kaios] baseq2 scan finished: ' + result.candidates.length +
			' candidate(s) found' + (cancelled ? ' (after cancel)' : ''));
		var candidates = result.candidates;

		if (candidates.length === 0) {
			mainMenuError = 'baseq2/pak0.pak not found';
			landOnMainMenu();
			return;
		}

		function finish(choice) {
			mainMenuError = '';
			if (launchOnSuccess && !cancelled) {
				// The cancel-scan RSK handler above has no business
				// still being live once loading/copying/booting takes
				// over -- that phase has no cancel of its own (never
				// did, before this scan-progress screen existed at all).
				activeMenuKeyHandler = null;
				proceedWithChoice(result.filesByStorage[choice.storageName], choice);
			} else {
				// Never auto-launch into a level the player didn't
				// press Play for -- cancelling always downgrades to
				// "just confirm and land on/refresh the main menu",
				// regardless of what launchOnSuccess originally asked
				// for as the first-launch fast path.
				setCachedBaseq2Choice(choice);
				localStorage.setItem(BASEQ2_CONFIRMED_KEY, '1');
				landOnMainMenu();
			}
		}

		// A picker screen popping up after the player already backed out
		// would be exactly the same "yanked back in" problem cancelling
		// is meant to avoid -- just take the first candidate silently in
		// that case, same as the picker-less single-candidate path.
		if (candidates.length > 1 && !cancelled) {
			pickerEl.style.display = 'block';
			statusEl.style.display = 'none';
			showPicker(candidates, finish);
		} else {
			finish(candidates[0]);
		}
	}, function (err) {
		console.log('[kaios] baseq2 scan failed: ' + describeError(err) +
			(cancelled ? ' (after cancel)' : ''));
		mainMenuError = 'Scan failed: ' + describeError(err);
		landOnMainMenu();
	});
}

function startBaseq2Scan() {
	performBaseq2Scan(false);
}

// "Play" from the main menu -- baseq2 was already found and confirmed
// on some earlier launch (that's the only way "Play" is ever enabled
// at all, see mainMenuItems()), so re-enumerating the *whole* card
// again just to rediscover the exact same folder is pure waste on a
// slow SD-card read. Scope the enumerate() to the cached folder's own
// path on its own cached storage volume instead of every volume's
// root -- lists only that subtree, not every unrelated file
// elsewhere. Falls back to a full startBaseq2Scan() (which also
// refreshes the cache) if there's no cached path yet, the cached
// volume no longer exists (card removed/swapped), or the scoped
// listing comes back without a valid pak0.pak in it (folder renamed)
// -- exactly the "rescan on error" behavior asked for.
// Every fallback below goes through performBaseq2Scan(true) -- scan,
// then launch straight into the game on success -- never
// startBaseq2Scan()/performBaseq2Scan(false), which is the manual
// "Search baseq2" menu item's own function and deliberately does NOT
// launch anything, just confirms and returns to the main menu. Play and
// "Search baseq2" need to stay two different actions with two different
// outcomes; calling the same non-launching path from here was a real
// bug -- confirmed on real hardware as Play repeatedly re-scanning and
// landing back on the main menu without ever actually starting the
// engine, instead of falling back to a slower-but-still-correct full
// scan the way a "fast path failed, recover properly" fallback should.
function playFast() {
	var cached = getCachedBaseq2Choice();
	var storages = getAllDeviceStorages();

	if (!cached || storages.length === 0) {
		performBaseq2Scan(true);
		return;
	}

	var storage = null;
	for (var i = 0; i < storages.length; i++) {
		if (storages[i].storageName === cached.storageName) {
			storage = storages[i];
			break;
		}
	}

	if (!storage) {
		performBaseq2Scan(true);
		return;
	}

	hideAllScreens();

	var scopedPath = (cached.root ? cached.root + '/' : '') + cached.dirName;
	setStatus('Loading baseq2...');

	enumerateStorage(storage, scopedPath).then(function (files) {
		var candidates = findBaseq2Candidates(files, cached.storageName);
		var match = null;

		for (var i = 0; i < candidates.length; i++) {
			if (candidates[i].root === cached.root && candidates[i].dirName === cached.dirName) {
				match = candidates[i];
				break;
			}
		}

		if (!match) {
			performBaseq2Scan(true);
			return;
		}

		proceedWithChoice(files, match);
	}, function () {
		// Cached path no longer resolves (renamed/removed/card swapped)
		// -- fall back to a full scan-and-launch rather than a dead end.
		performBaseq2Scan(true);
	});
}

// ---------------------------------------------------------------------
// Settings -- pre-boot cvar choices, applied via an override appended
// to autoexec.cfg (see buildLauncherSettingsCfg()/writeAutoexec()).
//
// Every item is a "choice" (a toggle is just a 2-choice item, On/Off):
// {id, label, cvars: [...], choices: [{label, values: [...]}, ...], def}
// -- `cvars`/`values` are parallel arrays so one item can drive more
// than one cvar at once (resolution needs r_customwidth+r_customheight,
// and r_mode to actually switch to a custom size). `def` is the choice
// *index* shown/applied until the player picks this item from this
// screen at least once -- see buildLauncherSettingsCfg() for why an
// untouched item writes nothing at all (so it can't silently fight the
// in-game KaiOS Tuning options menu, menu.c, over the same cvar).
// Cvar names/defaults are kept identical to that menu's own (and to
// the matching Cvar_Get() defaults in cl_main.c/sound.c/vid.c) on
// purpose, so a value picked here and one picked in-game mean the same
// thing.
// ---------------------------------------------------------------------

function toggleItem(id, label, cvar, def) {
	return {
		id: id,
		label: label,
		cvars: [cvar],
		choices: [
			{ label: 'Off', values: ['0'] },
			{ label: 'On', values: ['1'] }
		],
		def: def ? 1 : 0
	};
}

var VIDEO_ITEMS = [
	{
		id: 'resolution',
		label: 'Resolution',
		cvars: ['r_customwidth', 'r_customheight', 'r_mode'],
		choices: [
			{ label: '480x640', values: ['480', '640', '-1'] },
			{ label: '240x320', values: ['240', '320', '-1'] },
			{ label: '176x220', values: ['176', '220', '-1'] },
			{ label: '128x160', values: ['128', '160', '-1'] }
		],
		def: 1
	},
	{
		id: 'renderer',
		label: 'Renderer',
		cvars: ['vid_renderer'],
		choices: [
			{ label: 'Software', values: ['soft'] },
			// gl1 was linked into the unified binary for a while and
			// confirmed working on real hardware, but Emscripten's
			// LEGACY_GL_EMULATION (needed for gl1's fixed-function
			// pipeline) is a whole-binary setting, not scoped to just
			// gl1's own code -- it forced every GLES3 GL call through
			// that same emulation layer too, whether gl3 was even the
			// active renderer or not, causing real overhead and
			// stutters. build.sh no longer links gl1 into the default
			// unified build for this reason (INCLUDE_GL1=1 opts back in
			// for one-off testing) -- not offered here since a default
			// build genuinely doesn't have it, and picking it would just
			// silently fall back to Software.
			{ label: 'GLES3', values: ['gles3'] }
		],
		def: 0
	},
	toggleItem('vsync', 'VSync', 'r_vsync', true),
	{
		id: 'brightness',
		label: 'Brightness',
		cvars: ['vid_gamma'],
		choices: [
			{ label: '0.8', values: ['0.8'] },
			{ label: '1.0', values: ['1.0'] },
			{ label: '1.2', values: ['1.2'] },
			{ label: '1.4', values: ['1.4'] },
			{ label: '1.6', values: ['1.6'] }
		],
		def: 1
	},
	toggleItem('lights', 'Dynamic lights', 'cl_lights', false),
	toggleItem('particles', 'Particles', 'cl_particles', false),
	toggleItem('lerp', 'Model interpolation', 'r_lerpmodels', true),
	toggleItem('fullbright', 'Disable lighting', 'r_fullbright', false),
	toggleItem('dynrelight', 'Dynamic light relighting', 'gl1_dynamic', false),
	// Three separately scaled UI layers, all stock yquake2 cvars (not
	// KaiOS-specific) -- SCR_ClampScale (cl_screen.c) already clamps
	// each one to whatever the current resolution can actually support,
	// so offering a value too large for a smaller custom resolution is
	// harmless, it just clamps down instead of doing nothing. "Auto"
	// (-1, every cvar's own engine default) leaves SCR_GetDefaultScale()
	// picking the size, same as an untouched install.
	{
		id: 'menuscale',
		label: 'Menu scale',
		cvars: ['r_menuscale'],
		choices: [
			{ label: 'Auto', values: ['-1'] },
			{ label: '0.75x', values: ['0.75'] },
			{ label: '1x', values: ['1'] },
			{ label: '1.25x', values: ['1.25'] },
			{ label: '1.5x', values: ['1.5'] },
			{ label: '2x', values: ['2'] }
		],
		def: 0
	},
	{
		id: 'hudscale',
		label: 'Ammo/HUD scale',
		cvars: ['r_hudscale'],
		choices: [
			{ label: 'Auto', values: ['-1'] },
			{ label: '0.75x', values: ['0.75'] },
			{ label: '1x', values: ['1'] },
			{ label: '1.25x', values: ['1.25'] },
			{ label: '1.5x', values: ['1.5'] },
			{ label: '2x', values: ['2'] }
		],
		def: 0
	},
	{
		id: 'crosshairscale',
		label: 'Crosshair scale',
		cvars: ['crosshair_scale'],
		choices: [
			{ label: 'Auto', values: ['-1'] },
			{ label: '1x', values: ['1'] },
			{ label: '1.5x', values: ['1.5'] },
			{ label: '2x', values: ['2'] },
			{ label: '2.5x', values: ['2.5'] },
			{ label: '3x', values: ['3'] }
		],
		def: 0
	}
];

var AUDIO_ITEMS = [
	{
		id: 'device',
		label: 'Device',
		cvars: ['s_backend'],
		choices: [
			{ label: 'OpenAL', values: ['openal'] },
			{ label: 'SDL', values: ['sdl'] },
			// Default: mixes every channel itself in C (like SDL does)
			// and only touches WebAudio to queue the already-mixed
			// result in small fixed chunks, gapless, on a schedule
			// completely decoupled from how many sounds are actually
			// playing -- gets playback off the main thread the way the
			// old "Custom 1" backend did (one AudioBufferSourceNode per
			// sound play, since removed after real-device evidence tied
			// it to constant mid-combat stutter from GC pressure),
			// without that per-sound allocation cost. Confirmed clean on
			// real hardware.
			{ label: 'Custom (Recommended)', values: ['custom2'] },
			{ label: 'Off', values: ['none'] }
		],
		def: 2
	},
	toggleItem('sound', 'Sound', 's_initsound', true),
	{
		id: 'volume',
		label: 'Volume',
		cvars: ['s_volume'],
		choices: [
			{ label: '0%', values: ['0'] },
			{ label: '25%', values: ['0.25'] },
			{ label: '50%', values: ['0.5'] },
			{ label: '75%', values: ['0.75'] },
			{ label: '100%', values: ['1'] }
		],
		def: 3
	},
	toggleItem('music', 'Music', 'ogg_enable', true),
	{
		id: 'musicvolume',
		label: 'Music volume',
		cvars: ['ogg_volume'],
		choices: [
			{ label: '0%', values: ['0'] },
			{ label: '25%', values: ['0.25'] },
			{ label: '50%', values: ['0.5'] },
			{ label: '75%', values: ['0.75'] },
			{ label: '100%', values: ['1'] }
		],
		def: 3
	},
	{
		id: 'samplerate',
		label: 'Sample rate',
		cvars: ['s_khz'],
		choices: [
			{ label: '44100', values: ['44'] },
			{ label: '22050', values: ['22'] },
			{ label: '11025', values: ['11'] }
		],
		def: 0
	},
	{
		id: 'bitdepth',
		label: 'Bit depth',
		cvars: ['s_loadas8bit'],
		choices: [
			{ label: '16-bit', values: ['0'] },
			{ label: '8-bit', values: ['1'] }
		],
		def: 0
	},
	{
		id: 'stereo',
		label: 'Stereo',
		cvars: ['sndchannels'],
		choices: [
			{ label: 'Mono', values: ['1'] },
			{ label: 'Stereo', values: ['2'] }
		],
		def: 1
	}
];

// KaiOS-only tuning added over the course of this project, not stock
// yquake2 behavior -- excluding occlusion (s_occlusion_strength), which
// stays console/in-game-menu only. This used to also carry a long run
// of "Log: ..." console-diagnostic toggles and an entity-count A/B
// switch built for the mid-combat stutter investigation -- removed now
// that the investigation is over (the stutter's root cause, the old
// "Custom 1" WebAudio backend, is gone) and they'd only ever confuse a
// player who found them, not do anything useful.
var DEBUG_ITEMS = [
	toggleItem('prewarm', 'Preload on map load', 'kaios_prewarm_cache', true),
	{
		id: 'distcull',
		label: 'Draw distance cull',
		cvars: ['r_distcull_dist'],
		choices: [
			{ label: '400', values: ['400'] },
			{ label: '1200', values: ['1200'] },
			{ label: '2048', values: ['2048'] },
			{ label: 'Off', values: ['99999'] }
		],
		def: 1
	},
	// See kaios_startcmd's own comment in frame.c/autoexec.cfg -- queued
	// once, right after SV_Init()/CL_Init() register map/demomap, so it
	// can hold a full "demomap <name>" console command. Picking a demo
	// here reruns that same mechanism the very next launch instead of
	// requiring a manual autoexec.cfg edit. demo3 and the demo1>2>3
	// cycle option (kaios_democycle, sv_send.c's SV_DemoCompleted())
	// were dropped -- demo3 doesn't play correctly. Off leaves
	// kaios_startcmd empty, same as normal play.
	{
		id: 'demoautoplay',
		label: 'Autoplay demo',
		cvars: ['kaios_startcmd'],
		choices: [
			{ label: 'Off', values: [''] },
			{ label: 'demo1', values: ['demomap demo1.dm2'] },
			{ label: 'demo2', values: ['demomap demo2.dm2'] }
		],
		def: 0
	}
];

// Applied by the engine itself, not this launcher -- these cvars are
// only ever read from G_KaiosApplyStartupCheats() (g_cmds.c), called
// once from ClientBegin() (player/client.c) every time the local
// player freshly spawns into a level. That's deliberate, not just
// convenient: god/noclip/notarget are plain toggles (a typed console
// command flips them, it doesn't set them), so applying them from here
// instead would need to know each cheat's current on/off state to
// avoid toggling an already-on effect back off mid-level. A fresh
// spawn is always known-off for all three, so the engine can just
// toggle once, unconditionally, and get a reliable "on" -- see that
// function's own comment for the rest of the reasoning (including why
// it can't simply be gi.AddCommandString("god\n") from the same spot).
var CHEATS_ITEMS = [
	toggleItem('cheatgod', 'God mode', 'kaios_cheat_god', false),
	toggleItem('cheatnoclip', 'No clip', 'kaios_cheat_noclip', false),
	toggleItem('cheatnotarget', 'No target', 'kaios_cheat_notarget', false),
	toggleItem('cheatgiveall', 'Give All', 'kaios_cheat_giveall', false)
];

var SETTINGS_GROUPS = [
	{ id: 'video', label: 'Video', items: VIDEO_ITEMS },
	{ id: 'audio', label: 'Audio', items: AUDIO_ITEMS },
	{ id: 'debug', label: 'Debug', items: DEBUG_ITEMS },
	{ id: 'cheats', label: 'Cheats', items: CHEATS_ITEMS }
];

function settingStorageKey(item) {
	return 'kaios_setting_' + item.id;
}

function getItemIndex(item) {
	var raw = localStorage.getItem(settingStorageKey(item));
	if (raw === null) {
		return item.def;
	}
	var idx = parseInt(raw, 10);
	if (isNaN(idx) || idx < 0 || idx >= item.choices.length) {
		return item.def;
	}
	return idx;
}

function setItemIndex(item, idx) {
	localStorage.setItem(settingStorageKey(item), String(idx));
}

// Only items the player actually touched from these screens are
// written out -- one nobody touched leaves config.cfg (and the
// engine's own Cvar_Get default) in charge, so this can't silently
// fight the in-game KaiOS Tuning menu over the same cvar.
function buildLauncherSettingsCfg() {
	var lines = [];
	for (var g = 0; g < SETTINGS_GROUPS.length; g++) {
		var items = SETTINGS_GROUPS[g].items;
		for (var i = 0; i < items.length; i++) {
			var item = items[i];
			if (localStorage.getItem(settingStorageKey(item)) === null) {
				continue;
			}
			var choice = item.choices[getItemIndex(item)];
			for (var c = 0; c < item.cvars.length; c++) {
				var value = choice.values[c];
				// Cvar_Set_f (cvar.c) requires exactly one token for the
				// value -- "set kaios_startcmd demomap demo1.dm2" tokenizes
				// to 4 args and demo1.dm2 gets mistaken for the trailing
				// [u/s] archive flag, silently dropped from the value.
				// Quoting keeps a multi-word value (e.g. a full console
				// command string) as the single token "set" expects.
				if (value.indexOf(' ') !== -1) {
					value = '"' + value + '"';
				}
				lines.push('set ' + item.cvars[c] + ' ' + value);
			}
		}
	}
	return lines.join('\n');
}

// A standing fail-safe independent of the localStorage-driven reset
// below: every launcher-configurable cvar's *coded* default (not
// whatever the player has picked), written to the gamedir every launch
// so "exec kaiosdefaults.cfg" from the in-game console always gets
// back to a known-sane state even if config.cfg/autoexec.cfg's saved
// state is somehow the problem.
function buildKaiosDefaultsCfg() {
	var lines = [];
	for (var g = 0; g < SETTINGS_GROUPS.length; g++) {
		var items = SETTINGS_GROUPS[g].items;
		for (var i = 0; i < items.length; i++) {
			var item = items[i];
			var choice = item.choices[item.def];
			for (var c = 0; c < item.cvars.length; c++) {
				lines.push('set ' + item.cvars[c] + ' ' + choice.values[c]);
			}
		}
	}
	return lines.join('\n') + '\n';
}

// Clears every launcher-picked override -- the *next* launch then
// applies each item's coded default again (same values
// buildKaiosDefaultsCfg() writes out), same as if the player had never
// touched any of these screens.
function resetLauncherSettings() {
	for (var g = 0; g < SETTINGS_GROUPS.length; g++) {
		var items = SETTINGS_GROUPS[g].items;
		for (var i = 0; i < items.length; i++) {
			localStorage.removeItem(settingStorageKey(items[i]));
		}
	}
}

function showSettingsItemsScreen(group) {
	var selected = 0;

	function render() {
		renderMenuItems(settingsListEl, group.items.map(function (item) {
			return item.label + ': ' + item.choices[getItemIndex(item)].label;
		}), selected);
	}

	function onKeyDown(ev) {
		if (ev.key === 'ArrowUp') {
			selected = (selected - 1 + group.items.length) % group.items.length;
			render();
		} else if (ev.key === 'ArrowDown') {
			selected = (selected + 1) % group.items.length;
			render();
		} else if (ev.key === 'Enter') {
			var item = group.items[selected];
			setItemIndex(item, (getItemIndex(item) + 1) % item.choices.length);
			render();
		} else if (ev.key === 'SoftRight') {
			activeMenuKeyHandler = null;
			showSettingsGroups();
		}
	}

	hideAllScreens();
	settingsEl.style.display = 'block';
	render();
	activeMenuKeyHandler = onKeyDown;
}

function showSettingsGroups() {
	// One extra row at the end resets every group's picked overrides --
	// see resetLauncherSettings()'s comment.
	var rows = SETTINGS_GROUPS.map(function (g) {
		return { label: g.label, action: function () { showSettingsItemsScreen(g); } };
	});
	rows.push({
		label: 'Reset settings',
		action: function () {
			resetLauncherSettings();
			showSettingsGroups();
		}
	});

	var selected = 0;

	function render() {
		renderMenuItems(settingsListEl, rows.map(function (r) { return r.label; }), selected);
	}

	function onKeyDown(ev) {
		if (ev.key === 'ArrowUp') {
			selected = (selected - 1 + rows.length) % rows.length;
			render();
		} else if (ev.key === 'ArrowDown') {
			selected = (selected + 1) % rows.length;
			render();
		} else if (ev.key === 'Enter') {
			activeMenuKeyHandler = null;
			rows[selected].action();
		} else if (ev.key === 'SoftRight') {
			activeMenuKeyHandler = null;
			showMainMenu();
		}
	}

	hideAllScreens();
	settingsEl.style.display = 'block';
	render();
	activeMenuKeyHandler = onKeyDown;
}

function start() {
	installKeyHandlers();
	installWakeLock();

	if (isBaseq2Confirmed()) {
		showBootSkipTimer();
	} else if (hasEverLaunched()) {
		// baseq2 was never confirmed, but this isn't the first launch
		// either (an earlier scan failed, or the player backed out of
		// it) -- land on the main menu with "Search baseq2" available
		// rather than auto-scanning silently again.
		showMainMenu();
	} else {
		// The very first launch ever: scan for baseq2 right away,
		// visibly, and go straight into the game on success -- see
		// performBaseq2Scan()'s own comment for why first-time setup
		// skips the "back to the main menu, press Play yourself" step
		// every later "Search baseq2" still uses.
		markEverLaunched();
		performBaseq2Scan(true, true);
	}
}

// Wait for the full 'load' event, not just DOMContentLoaded/'interactive'.
// installLazyPakFile()'s read() is the first thing in this app that does a
// synchronous XHR against a blob: URL, and on a real device it was seen to
// fail on its *second* use later in the session (see the "recursive
// shutdown" / pics/16to8.dat comment in FS_SetGamedir(), filesystem.c)
// while succeeding on the very first use moments after DOMContentLoaded --
// consistent with that machinery not being fully settled that early in a
// KaiOS webview. Waiting for 'load' costs nothing (there's nothing else on
// this page to finish loading) and removes that variable entirely.
if (document.readyState === 'complete') {
	start();
} else {
	window.addEventListener('load', start);
}

})();
