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
var Module = {
	canvas: document.getElementById('canvas'),
	noInitialRun: true,
	print: function (text) { console.log('[q2] ' + text); },
	printErr: function (text) { console.log('[q2!] ' + text); }
};
