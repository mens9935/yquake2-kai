/*
 * Copyright (C) 1997-2001 Id Software, Inc.
 * Copyright (C) 2010, 2013 Yamagi Burmeister
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307,
 * USA.
 *
 * =======================================================================
 *
 * Background music playback for the KaiOS/Emscripten webaudio backend.
 * Deliberately its own file/pipeline, not part of webaudio.c's
 * WebAudio-graph-based sfx/cinematics path (WA_RawSamples() et al) --
 * music needs nothing that path provides (panning, per-channel gain
 * slots, precise scheduling) and mixing the two together isn't worth
 * it for a single, long-lived, always-looping stream.
 *
 * The obvious approach -- decode the whole OGG via
 * AudioContext.decodeAudioData() into raw PCM, same as WA_RawSamples()
 * does per-chunk in webaudio.c -- was tried first and made things
 * *worse* on real hardware -- this exact device's decodeAudioData()
 * apparently doesn't handle a several-MB Vorbis file at all, and never
 * rejects the promise either, so every map's full-track decode attempt
 * just pinned another few MB in memory forever with no error and no
 * sound.
 *
 * ClassiCube's KaiOS port (a separate, unrelated project) hit the
 * identical failure mode for the identical reason and fixed it by
 * routing music through a plain <audio> element instead -- the
 * browser's normal media pipeline (the same one behind <video>/<audio>
 * tags anywhere else on the web) streams and decodes incrementally
 * instead of eagerly materializing the whole track as PCM. Doing the
 * same here: hand the browser the raw (still Vorbis-compressed) bytes
 * as a Blob, point a real <audio> element at it via a blob: URL, let
 * the browser do everything else.
 *
 * Requires the "audio-channel-content" permission in manifest.webapp
 * for a privileged app's <audio> element to make sound at all (see
 * that file) -- webaudio.c's AudioContext-based output doesn't need it
 * since it apparently isn't gated by the same content-channel policy.
 *
 * =======================================================================
 */

#ifdef __EMSCRIPTEN__

#include <emscripten/em_asm.h>

#include "../header/client.h"
#include "header/local.h"

void
WA_PlayMusic(const byte *data, int len)
{
	EM_ASM({
		var ka = Module.kaiosAudio;
		if (!ka) {
			return;
		}

		if (!ka.musicEl) {
			ka.musicEl = new Audio();
			ka.musicEl.loop = true;
			ka.musicEl.addEventListener('error', function () {
				console.log('[kaios] music: <audio> error, code=' +
					(ka.musicEl.error ? ka.musicEl.error.code : '?'));
			});
		}

		var ptr = $0;
		var len = $1;

		try {
			/* Copy out of HEAPU8 into a fresh, independent buffer before
			 * building the Blob -- the C-side buffer this data came from
			 * is freed by the caller as soon as this EM_ASM call
			 * returns. */
			var bytes = new Uint8Array(len);
			bytes.set(HEAPU8.subarray(ptr, ptr + len));

			if (ka.musicUrl) {
				URL.revokeObjectURL(ka.musicUrl);
				ka.musicUrl = null;
			}

			var blob = new Blob([bytes], {type: 'audio/ogg'});
			ka.musicUrl = URL.createObjectURL(blob);
			ka.musicBytesUsed = len;
			ka.musicEl.src = ka.musicUrl;
			ka.musicEl.volume = (ka.musicVolume !== undefined) ? ka.musicVolume : 1.0;

			var p = ka.musicEl.play();
			if (p && typeof p.catch === 'function') {
				p.catch(function (e) {
					console.log('[kaios] music: play() failed: ' + e);
				});
			}
		} catch (e) {
			console.log('[kaios] music: WA_PlayMusic threw: ' + e);
		}
	}, data, len);
}

void
WA_StopMusic(void)
{
	EM_ASM({
		var ka = Module.kaiosAudio;
		if (!ka || !ka.musicEl) {
			return;
		}

		ka.musicEl.pause();
		ka.musicEl.removeAttribute('src');
		ka.musicEl.load();

		if (ka.musicUrl) {
			URL.revokeObjectURL(ka.musicUrl);
			ka.musicUrl = null;
		}
		ka.musicBytesUsed = 0;
	});
}

/*
 * Unlike AudioBufferSourceNode (webaudio.c's WA_RawSamples()), a real
 * <audio> element has genuine pause()/play() that preserve position --
 * no volume-mute workaround needed here.
 */
void
WA_PauseMusic(qboolean pause)
{
	EM_ASM({
		var ka = Module.kaiosAudio;
		if (!ka || !ka.musicEl) {
			return;
		}

		if ($0) {
			ka.musicEl.pause();
		} else {
			var p = ka.musicEl.play();
			if (p && typeof p.catch === 'function') {
				p.catch(function () {});
			}
		}
	}, pause);
}

void
WA_SetMusicVolume(float vol)
{
	EM_ASM({
		var ka = Module.kaiosAudio;
		if (!ka) {
			return;
		}

		ka.musicVolume = $0;

		if (ka.musicEl) {
			ka.musicEl.volume = $0;
		}
	}, vol);
}

#endif /* __EMSCRIPTEN__ */
