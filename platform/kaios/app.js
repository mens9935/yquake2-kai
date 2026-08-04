/*
 * KaiOS shell for the asm.js Quake II build.
 *
 * Two jobs:
 *   1. Let the user pick the folder on the SD card that holds "baseq2"
 *      (via the Device Storage API -- there's no <input type=file> on
 *      KaiOS) and copy its contents into the Emscripten virtual FS.
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
function readFileAsArrayBuffer(file) {
	return new Promise(function (resolve, reject) {
		var reader = new FileReader();
		reader.onload = function () { resolve(reader.result); };
		reader.onerror = function () { reject(reader.error); };
		reader.readAsArrayBuffer(file);
	});
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
	var toCopy = files.filter(function (f) {
		return ('/' + f.name.replace(/^\/+/, '')).indexOf(root + '/') === 0;
	});

	var i = 0;
	function next() {
		if (i >= toCopy.length) {
			return Promise.resolve();
		}
		var file = toCopy[i];
		var rel = ('/' + file.name.replace(/^\/+/, '')).slice(root.length + 1);
		var dest = '/' + GAMEDIR + '/' + rel;

		setStatus('Copying ' + rel, i / toCopy.length);

		return readFileAsArrayBuffer(file).then(function (buf) {
			mkdirTreeFor(dest);
			FS.writeFile(dest, new Uint8Array(buf));
			i++;
			return next();
		});
	}
	return next();
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

function bootEngine() {
	setStatus('Starting...', 1);
	writeAutoexec();

	statusEl.style.display = 'none';
	canvasEl.style.display = 'block';

	engineStarted = true;
	Module.callMain(['+set', 'vid_width', '240', '+set', 'vid_height', '320']);
}

// ---------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------

function start() {
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
			setStatus('Copying baseq2...', 0);
			copyBaseq2(files, choice.root).then(bootEngine, function (err) {
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

if (document.readyState === 'loading') {
	document.addEventListener('DOMContentLoaded', start);
} else {
	start();
}

})();
