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
var PAK_MARKER = '/' + GAMEDIR + '/pak0.pak';

var pickerEl = document.getElementById('picker');
var pickerListEl = document.getElementById('picker-list');
var statusEl = document.getElementById('status');
var statusTextEl = document.getElementById('status-text');
var statusBarEl = document.getElementById('status-bar');
var canvasEl = document.getElementById('canvas');

// Used to be an inline oncontextmenu="..." attribute on the <canvas> tag
// in index.html -- moved here for the same reason module-init.js exists:
// privileged KaiOS apps' mandatory CSP silently drops inline scripts
// *and* inline event handler attributes, so it never actually ran.
canvasEl.addEventListener('contextmenu', function (ev) {
	ev.preventDefault();
});

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

function installKeyHandlers() {
	function handle(down) {
		return function (ev) {
			if (PREVENT_DEFAULT[ev.key]) {
				ev.preventDefault();
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

	window.addEventListener('keydown', handle(true));
	window.addEventListener('keyup', handle(false));
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
	node.contents = null;

	node.stream_ops = {
		llseek: MEMFS.stream_ops.llseek,
		read: function (stream, buffer, offset, length, position) {
			if (position >= file.size) {
				return 0;
			}
			var end = Math.min(position + length, file.size);
			var size = end - position;
			var slice = file.slice(position, end);

			// One retry on failure: a real device was seen to have this
			// read fail on its *second* use in a session while the very
			// first use (moments earlier) succeeded -- suggestive of a
			// one-off cold-start hiccup in the browser's sync-XHR/blob-URL
			// machinery rather than a real, repeatable fault. A single
			// retry is cheap and turns a transient hiccup into a stall
			// instead of a fatal engine error; a genuinely broken read
			// still fails after the retry same as before.
			var bytes, lastErr;
			for (var attempt = 0; attempt < 2; attempt++) {
				var url = URL.createObjectURL(slice);
				try {
					var xhr = new XMLHttpRequest();
					xhr.overrideMimeType('text/plain; charset=x-user-defined');
					xhr.open('GET', url, false);
					xhr.send(null);
					bytes = xhr.responseText;
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

			var n = Math.min(bytes.length, size);
			for (var i = 0; i < n; i++) {
				buffer[offset + i] = bytes.charCodeAt(i) & 0xff;
			}
			return n;
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

function enumerateStorage(storage, path) {
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
			cursor.continue();
		};
		cursor.onerror = function () {
			reject(cursor.error);
		};
	});
}

function findBaseq2Candidates(files) {
	var candidates = [];
	for (var i = 0; i < files.length; i++) {
		var name = files[i].name || '';
		var normalized = '/' + name.replace(/^\/+/, '');
		if (normalized.toLowerCase().indexOf(PAK_MARKER.toLowerCase()) !== -1) {
			var root = normalized.slice(0,
				normalized.toLowerCase().lastIndexOf(PAK_MARKER.toLowerCase()));
			candidates.push({ root: root, file: files[i] });
		}
	}
	return candidates;
}

// ---------------------------------------------------------------------
// Picker UI (DPAD up/down, OK to select)
// ---------------------------------------------------------------------

function showPicker(candidates, onChosen) {
	var selected = 0;

	function render() {
		pickerListEl.innerHTML = '';
		for (var i = 0; i < candidates.length; i++) {
			var li = document.createElement('li');
			li.textContent = candidates[i].root || '/';
			if (i === selected) {
				li.className = 'selected';
			}
			pickerListEl.appendChild(li);
		}
	}

	function onKeyDown(ev) {
		if (ev.key === 'ArrowUp') {
			selected = Math.max(0, selected - 1);
			render();
		} else if (ev.key === 'ArrowDown') {
			selected = Math.min(candidates.length - 1, selected + 1);
			render();
		} else if (ev.key === 'Enter') {
			window.removeEventListener('keydown', onKeyDown);
			onChosen(candidates[selected]);
		}
	}

	render();
	window.addEventListener('keydown', onKeyDown);
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
function copyBaseq2(files, root) {
	// `root` is baseq2's *parent* directory, and is "" when baseq2 sits
	// right at the SD card root -- root + '/' would then be just "/",
	// which every single path on the card starts with, copying the
	// whole card instead of just the baseq2 folder. Scope explicitly to
	// ".../baseq2/" instead, which is correct whether root is empty or not.
	var srcPrefix = root + '/' + GAMEDIR + '/';

	// save/ and scrnshot/ are pure engine *output* -- the engine
	// creates them itself under its writable config dir, they're never
	// something that needs to be supplied as input data. Skipping them
	// if present on the SD card (e.g. copied back from a previous,
	// different session) avoids wasting memory on data that isn't
	// used for anything.
	var SKIP_PREFIXES = ['save/', 'scrnshot/'];

	var toCopy = files.filter(function (f) {
		var norm = '/' + f.name.replace(/^\/+/, '');
		if (norm.indexOf(srcPrefix) !== 0) {
			return false;
		}
		var rel = norm.slice(srcPrefix.length);
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
		var rel = ('/' + file.name.replace(/^\/+/, '')).slice(srcPrefix.length);
		var dest = '/' + GAMEDIR + '/' + rel;

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

function loadEngineScripts() {
	if (engineScriptsLoaded) {
		return Promise.resolve();
	}
	return loadScript('quake2-kaios.js').then(function () {
		return loadScript('autoexec.cfg.js');
	}).then(function () {
		engineScriptsLoaded = true;

		// Physically disarm Module.callMain() (only just defined by the
		// script that just loaded) until bootEngine() explicitly arms it,
		// after copyBaseq2() confirms the FS is actually populated. Any
		// call that sneaks in before that logs a stack trace instead of
		// quietly running main() over an empty filesystem -- which, if it
		// still happens even with scripts loaded this late, finally pins
		// down where it's coming from.
		var realCallMain = Module.callMain;

		// Temporary diagnostic: chasing a report that this call stopped
		// producing any output at all (not even the C side's first-line
		// "main() entered" fprintf) once noInitialRun started actually
		// working (see module-init.js) -- confirm realCallMain is a real
		// function before wrapping it.
		console.log('[kaios] loadEngineScripts: typeof Module.callMain (pre-wrap) = ' + (typeof realCallMain));

		Module.callMain = function (args) {
			console.log('[kaios] Module.callMain wrapper invoked, callMainArmed=' + callMainArmed);
			if (!callMainArmed) {
				console.log('[kaios] BLOCKED premature Module.callMain() -- ' +
					'baseq2 is not mounted yet.\n' + (new Error().stack || '(no stack)'));
				return;
			}
			console.log('[kaios] about to call realCallMain, typeof=' + (typeof realCallMain));
			try {
				var ret = realCallMain.call(Module, args);
				console.log('[kaios] realCallMain returned: ' + ret);
				return ret;
			} catch (e) {
				console.log('[kaios] realCallMain THREW: ' + e + '\n' + (e && e.stack));
				throw e;
			}
		};
	});
}

function writeAutoexec() {
	var text = window.KAIOS_AUTOEXEC_CFG || '';
	if (!text) {
		return;
	}
	// Write to the gamedir (read search path) and to the engine's
	// writable config dir (~/.yq2/baseq2 under Emscripten's synthesized
	// HOME) so it's found regardless of which one FS_LoadFile checks.
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
}

var bootedOnce = false;

function bootEngine() {
	// Temporary diagnostic, see also the matching one at the top of
	// main() (backends/unix/main.c) -- chasing a report of the engine
	// appearing to boot twice in one session.
	console.log('[kaios] bootEngine() called, bootedOnce=' + bootedOnce);

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
	writeAutoexec();

	statusEl.style.display = 'none';
	canvasEl.style.display = 'block';

	engineStarted = true;
	// Only past this point is Module.callMain() allowed to actually do
	// anything -- see the guard installed at the top of this file.
	callMainArmed = true;
	// NOTE: this emsdk version's callMain() ignores its args entirely
	// (hardcodes argc=0) -- and "vid_width"/"vid_height" were never
	// real cvars here anyway (r_customwidth/r_customheight are, see
	// autoexec.cfg, which is what actually sets the resolution).
	console.log('[kaios] bootEngine: about to call Module.callMain([]), typeof=' + (typeof Module.callMain));
	Module.callMain([]);
	console.log('[kaios] bootEngine: Module.callMain([]) call returned');
}

// ---------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------

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
function start() {
	// Temporary diagnostic, see the note by bootEngine().
	console.log('[kaios] start() called, document.readyState=' + document.readyState);

	installKeyHandlers();

	if (!navigator.getDeviceStorage) {
		setStatus('No Device Storage API on this browser.\n' +
			'This shell needs to run as a packaged KaiOS app.');
		return;
	}

	var storage = navigator.getDeviceStorage('sdcard');
	setStatus('Scanning SD card for baseq2...');

	enumerateStorage(storage, '').then(function (files) {
		var candidates = findBaseq2Candidates(files);

		if (candidates.length === 0) {
			setStatus('No baseq2/pak0.pak found on the SD card.\n' +
				'Copy your baseq2 folder to the SD card and reopen the app.');
			return;
		}

		function proceed(choice) {
			setStatus('Loading engine...', 0);
			loadEngineScripts().then(function () {
				setStatus('Copying baseq2...', 0);
				return copyBaseq2(files, choice.root);
			}).then(bootEngine, function (err) {
				setStatus('Copy failed: ' + describeError(err));
			});
		}

		if (candidates.length === 1) {
			proceed(candidates[0]);
		} else {
			pickerEl.style.display = 'block';
			statusEl.style.display = 'none';
			showPicker(candidates, proceed);
		}
	}, function (err) {
		setStatus('Could not scan SD card: ' + describeError(err));
	});
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
