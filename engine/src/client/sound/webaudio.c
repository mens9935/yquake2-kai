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
 * Web Audio backend for KaiOS/Emscripten. See the big comment in
 * header/local.h (right above the WA_* declarations) for why this
 * exists instead of just tuning the SDL backend further -- short
 * version: real-device profiling showed our own software mixing
 * (S_Update/SDL_Update) was never the expensive part, so this hands
 * playback to the browser's native audio graph instead, which runs on
 * its own real-time thread and needs no continuous main-thread callback
 * at all.
 *
 * =======================================================================
 */

#ifdef __EMSCRIPTEN__

#include <stdint.h>

#include <emscripten/em_asm.h>

#include "../header/client.h"
#include "header/local.h"
#include "header/vorbis.h"

/* Same tuned falloff OpenAL uses for ambient loop sounds
 * (SOUND_LOOPATTENUATE in openal.c) -- sdl.c has its own copy
 * (SDL_LOOPATTENUATE) as a private macro, not exported via local.h. */
#define WA_LOOPATTENUATE 0.003f

/*
 * One persistent slot per channel array index (ch - channels), matching
 * how AL_PlayChannel binds s_srcnums[ch - channels] -- the JS side keeps
 * one GainNode+StereoPannerNode pair alive per slot forever, and just
 * swaps which (ephemeral, single-use per the Web Audio spec)
 * AudioBufferSourceNode is plugged into it on each play.
 */
static qboolean wa_inited = false;

/* Shared between WA_Update and WA_AddLoopSounds -- a loop channel's
 * wa_autoframe gets stamped with this each frame it's still audible
 * from some entity; WA_Update compares against it to notice when a
 * loop sound has dropped out and needs an explicit stop. Must be one
 * counter both functions see, not a per-function local. */
static int wa_framecount = 0;

/*
 * Sets up Module.kaiosAudio: one AudioContext, a master gain node (wired
 * through a shared lowpass filter for the underwater effect), and
 * MAX_CHANNELS persistent gain/pan slots. Returns false (without
 * touching sound_started) if the browser has no Web Audio API at all --
 * S_Init() falls back to OpenAL/SDL in that case, same as any other
 * backend init failure.
 */
qboolean
WA_Init(void)
{
	int ok = EM_ASM_INT({
		try {
			var AC = window.AudioContext || window.webkitAudioContext;
			if (!AC) {
				console.log('[kaios] webaudio: no AudioContext available');
				return 0;
			}

			var ka = {};
			ka.ctx = new AC();
			ka.master = ka.ctx.createGain();
			ka.master.gain.value = $0;

			/* Shared underwater lowpass -- one filter for the whole mix
			 * instead of per-source filters, much cheaper and close
			 * enough for a feature-phone game. Frequency gets pulled
			 * down by WA_Update only while snd_is_underwater is true. */
			ka.underwaterFilter = ka.ctx.createBiquadFilter();
			ka.underwaterFilter.type = 'lowpass';
			ka.underwaterFilter.frequency.value = 22050;
			ka.master.connect(ka.underwaterFilter);
			ka.underwaterFilter.connect(ka.ctx.destination);

			ka.buffers = [null]; /* index 0 reserved as "no buffer" */
			ka.slots = [];

			for (var i = 0; i < $1; i++) {
				var g = ka.ctx.createGain();
				var p = ka.ctx.createStereoPanner ? ka.ctx.createStereoPanner() : null;
				if (p) {
					g.connect(p);
					p.connect(ka.master);
				} else {
					g.connect(ka.master);
				}
				ka.slots.push({gain: g, pan: p, src: null});
			}

			ka.rawNextTime = 0;

			/* AudioContext starts (or resumes after a tab switch)
			 * suspended until a user gesture on most engines -- this is
			 * a KaiOS phone, every session starts with a keypress or
			 * touch, so just resume opportunistically on the first few
			 * of either instead of gating actual gameplay input on it.
			 * Logging the rejection reason (instead of swallowing it)
			 * and guarding resume's existence -- this engine (GL_VENDOR
			 * "Mozilla", an old Gecko/B2G build per this session's other
			 * findings) may predate resume() being in the spec at all,
			 * in which case calling it would throw a TypeError instead
			 * of returning a rejectable promise. */
			var resume = function() {
				if (ka.ctx.state !== 'running' && typeof ka.ctx.resume === 'function') {
					try {
						ka.ctx.resume().catch(function(e) {
							console.log('[kaios] webaudio: resume() rejected: ' + e);
						});
					} catch (e) {
						console.log('[kaios] webaudio: resume() threw: ' + e);
					}
				}
			};
			document.addEventListener('keydown', resume);
			document.addEventListener('touchstart', resume);
			resume();

			console.log('[kaios] webaudio: init ok, sampleRate=' + ka.ctx.sampleRate +
				', state=' + ka.ctx.state + ', hasResume=' + (typeof ka.ctx.resume === 'function') +
				', hasStereoPanner=' + (typeof ka.ctx.createStereoPanner === 'function'));

			Module.kaiosAudio = ka;
			return ka.ctx.sampleRate;
		} catch (e) {
			console.log('[kaios] webaudio: init threw: ' + e);
			return 0;
		}
	}, s_volume->value, MAX_CHANNELS);

	wa_inited = (ok != 0);

	if (wa_inited)
	{
		/* Purely informational (S_SoundInfo_f's "Sound sampling rate"
		 * print reads sound.speed regardless of backend) -- this
		 * backend has no software ring buffer, so none of sound_t's
		 * other fields (samples/submission_chunk/buffer/...) apply.
		 * sound.channels matters beyond logging though: SDL_Spatialize/
		 * SDL_SpatializeOrigin (sdl.c), reused as-is here, special-case
		 * sound.channels == 1 to skip stereo panning -- a real browser
		 * AudioContext is always genuinely stereo output. */
		sound.speed = ok;
		sound.channels = 2;

		/* THE bug that caused total, error-free silence: s_numchannels
		 * gates S_PickChannel's entire scan loop (sound.c) -- SDL and
		 * OpenAL both set it themselves (sdl.c: s_numchannels =
		 * MAX_CHANNELS; openal.c: s_numchannels = i after allocating AL
		 * sources), but this backend never did. Left at its
		 * zero-initialized default, S_PickChannel's for-loop over
		 * s_numchannels never ran even once, so it always fell through
		 * to "return NULL" -- and S_IssuePlaysound's (!ch) branch drops
		 * the sound and returns with NO error message at all. Every
		 * other part of the pipeline (decode, upload, spatialize,
		 * S_StartSound queuing, WA_IssuePlaysounds draining the pending
		 * list) genuinely worked the whole time; WA_PlayChannel itself
		 * was simply never reached. */
		s_numchannels = MAX_CHANNELS;
	}

	return wa_inited;
}

void
WA_Shutdown(void)
{
	if (!wa_inited)
	{
		return;
	}

	EM_ASM({
		if (Module.kaiosAudio && Module.kaiosAudio.ctx) {
			try { Module.kaiosAudio.ctx.close(); } catch (e) {}
		}
		Module.kaiosAudio = null;
	});

	wa_inited = false;
}

void
WA_SoundInfo(void)
{
	EM_ASM({
		var ka = Module.kaiosAudio;
		if (!ka) {
			console.log('webaudio backend not initialized');
			return;
		}
		console.log('Web Audio backend, sampleRate=' + ka.ctx.sampleRate +
			', state=' + ka.ctx.state + ', buffers=' + (ka.buffers.length - 1) +
			', slots=' + ka.slots.length);
	});
}

/*
 * Decodes raw PCM (already parsed out of the .wav by S_LoadSound, same
 * "data" that SDL_Cache/AL_UploadSfx also receive) into a native
 * AudioBuffer. AudioContext resamples to its own output rate on
 * playback automatically, so unlike SDL_Cache there's no manual
 * stepscale resampling needed here.
 */
sfxcache_t *
WA_UploadSfx(sfx_t *s, wavinfo_t *s_info, byte *data, short volume,
			 int begin_length, int end_length,
			 int attack_length, int fade_length)
{
	sfxcache_t *sc;
	int bufnum;

	if (!s_info->samples)
	{
		return NULL;
	}

	bufnum = EM_ASM_INT({
		var ka = Module.kaiosAudio;
		var ptr = $0;
		var frames = $1;
		var width = $2;
		var chans = $3;
		var rate = $4;

		try {
			var buf = ka.ctx.createBuffer(chans, frames, rate);

			var peak = 0;

			for (var c = 0; c < chans; c++) {
				var out = buf.getChannelData(c);

				if (width === 1) {
					for (var i = 0; i < frames; i++) {
						out[i] = (HEAPU8[ptr + i * chans + c] - 128) / 128.0;
					}
				} else {
					var base = ptr >> 1;
					for (var i = 0; i < frames; i++) {
						out[i] = HEAP16[base + i * chans + c] / 32768.0;
					}
				}

				for (var i = 0; i < frames; i++) {
					var a = Math.abs(out[i]);
					if (a > peak) {
						peak = a;
					}
				}
			}

			ka.uploadLogCount = (ka.uploadLogCount || 0);
			if (ka.uploadLogCount < 8) {
				ka.uploadLogCount++;
				console.log('[kaios] webaudio: upload#' + ka.uploadLogCount +
					' frames=' + frames + ' width=' + width + ' chans=' + chans +
					' rate=' + rate + ' peak=' + peak.toFixed(4) +
					' duration=' + buf.duration.toFixed(3));
			}

			ka.buffers.push(buf);
			return ka.buffers.length - 1;
		} catch (e) {
			console.log('[kaios] webaudio: upload threw: ' + e);
			return 0;
		}
	}, data, s_info->samples, s_info->width, s_info->channels, s_info->rate);

	if (!bufnum)
	{
		return NULL;
	}

	/* Placeholder cache entry -- same idea as AL_UploadSfx, just enough
	 * metadata for sound.c's channel bookkeeping, no PCM data of our own
	 * to keep around since the real audio now lives in the JS buffer. */
	sc = s->cache = Z_TagMalloc(sizeof(*sc), 0);
	sc->length = ((uint64_t)s_info->samples * 1000) / s_info->rate;
	sc->loopstart = s_info->loopstart;
	sc->width = s_info->width;
	sc->wa_bufnum = bufnum;
	sc->stereo = s_info->channels - 1;
	sc->volume = volume;
	sc->begin = begin_length * 1000 / s_info->rate;
	sc->end = end_length * 1000 / s_info->rate;
	sc->fade = fade_length * 1000 / s_info->rate;
	sc->attack = attack_length * 1000 / s_info->rate;

	return sc;
}

void
WA_DeleteSfx(sfx_t *s)
{
	sfxcache_t *sc = s->cache;

	if (!sc || !sc->wa_bufnum)
	{
		return;
	}

	EM_ASM({
		var ka = Module.kaiosAudio;
		if (ka && $0 > 0 && $0 < ka.buffers.length) {
			ka.buffers[$0] = null;
		}
	}, sc->wa_bufnum);
}

/*
 * Converts SDL_Spatialize's already-computed leftvol/rightvol (0-255
 * each) into a gain+pan pair and pushes them to the channel's slot.
 * Reused both right after starting playback and every frame afterwards
 * from WA_Update -- no audio processing happens here, just AudioParam
 * writes, so this is cheap regardless of how often it's called.
 */
static void
WA_PushParams(int slot, int leftvol, int rightvol)
{
	float gain = (leftvol + rightvol) / (2.0f * 255.0f);
	float total = (float)(leftvol + rightvol);
	float pan = (total > 0.0f) ? ((rightvol - leftvol) / total) : 0.0f;

	EM_ASM({
		var ka = Module.kaiosAudio;
		var s = ka && ka.slots[$0];
		if (!s) {
			return;
		}
		s.gain.gain.value = $1;
		if (s.pan) {
			s.pan.pan.value = $2;
		}
	}, slot, gain, pan);
}

void
WA_PlayChannel(channel_t *ch)
{
	sfxcache_t *sc = ch->sfx->cache;
	int slot = ch - channels;

	if (!sc || !sc->wa_bufnum)
	{
		memset(ch, 0, sizeof(*ch));
		return;
	}

	/* Spatialize before starting playback so it doesn't audibly begin
	 * at whatever stale gain/pan the slot last had. */
	SDL_Spatialize(ch);

	EM_ASM({
		var ka = Module.kaiosAudio;
		var slot = $0;
		var bufnum = $1;
		var gain = $2;
		var pan = $3;
		var loop = $4;

		try {
			var s = ka && ka.slots[slot];
			var buf = ka && ka.buffers[bufnum];

			ka.playLogCount = (ka.playLogCount || 0);
			if (ka.playLogCount < 8) {
				ka.playLogCount++;
				console.log('[kaios] webaudio: play#' + ka.playLogCount +
					' slot=' + slot + ' bufnum=' + bufnum + ' hasSlot=' + !!s +
					' hasBuf=' + !!buf + ' gain=' + gain.toFixed(3) + ' pan=' + pan.toFixed(3) +
					' loop=' + loop + ' ctxState=' + ka.ctx.state +
					' masterGain=' + ka.master.gain.value);
			}

			if (!s || !buf) {
				return;
			}
			if (s.src) {
				try { s.src.stop(); } catch (e) {}
				s.src.disconnect();
				s.src = null;
			}
			var src = ka.ctx.createBufferSource();
			src.buffer = buf;
			src.loop = !!loop;
			src.connect(s.gain);
			s.gain.gain.value = gain;
			if (s.pan) {
				s.pan.pan.value = pan;
			}
			src.start(0);
			s.src = src;
			src.onended = function() {
				if (s.src === src) {
					s.src = null;
				}
			};
		} catch (e) {
			console.log('[kaios] webaudio: WA_PlayChannel threw: ' + e);
		}
	}, slot, sc->wa_bufnum,
		(ch->leftvol + ch->rightvol) / (2.0f * 255.0f),
		((ch->leftvol + ch->rightvol) > 0) ?
			((float)(ch->rightvol - ch->leftvol) / (float)(ch->leftvol + ch->rightvol)) : 0.0f,
		ch->autosound ? 1 : 0);

	ch->end = paintedtime + sc->length;
}

void
WA_StopChannel(channel_t *ch)
{
	int slot = ch - channels;

	EM_ASM({
		var ka = Module.kaiosAudio;
		var s = ka && ka.slots[$0];
		if (!s || !s.src) {
			return;
		}
		try { s.src.stop(); } catch (e) {}
		s.src.disconnect();
		s.src = null;
	}, slot);

	memset(ch, 0, sizeof(*ch));
}

void
WA_StopAllChannels(void)
{
	int i;
	channel_t *ch = channels;

	for (i = 0; i < s_numchannels; i++, ch++)
	{
		if (!ch->sfx)
		{
			continue;
		}

		WA_StopChannel(ch);
	}

	s_rawend = 0;

	EM_ASM({
		var ka = Module.kaiosAudio;
		if (ka) {
			ka.rawNextTime = 0;
		}
	});
}

/*
 * Returns the channel already playing sfx's looping instance for
 * entnum, if any -- mirrors AL_FindLoopingSound so an ambient sound
 * that's still audible this frame gets its lifetime refreshed instead
 * of being torn down and restarted (which would click/pop every frame).
 */
static channel_t *
WA_FindLoopingSound(int entnum, sfx_t *sfx)
{
	int i;
	channel_t *ch = channels;

	for (i = 0; i < s_numchannels; i++, ch++)
	{
		if (!ch->sfx || !ch->autosound)
		{
			continue;
		}

		if ((ch->entnum == entnum) && (ch->sfx == sfx))
		{
			return ch;
		}
	}

	return NULL;
}

/*
 * Entities with a "sound" field generate looped ambient sounds. Modeled
 * on AL_AddLoopSounds (event-driven start/stop), not SDL_AddLoopSounds
 * (which regenerates every channel's volume from scratch every frame --
 * fine for software mixing, but would restart our native source nodes
 * audibly every single frame).
 */
static void
WA_AddLoopSounds(void)
{
	int i;
	int sounds[MAX_EDICTS];

	if ((cls.state != ca_active) || (cl_paused->value && cl_audiopaused->value) ||
		!cl.sound_prepped || !s_ambient->value)
	{
		return;
	}

	memset(&sounds, 0, sizeof(int) * MAX_EDICTS);
	S_BuildSoundList(sounds);

	for (i = 0; i < cl.frame.num_entities; i++)
	{
		channel_t *ch;
		sfx_t *sfx;
		sfxcache_t *sc;
		int num;
		entity_state_t *ent;

		if (!sounds[i])
		{
			continue;
		}

		sfx = cl.sound_precache[sounds[i]];

		if (!sfx)
		{
			continue;
		}

		sc = sfx->cache;

		if (!sc)
		{
			continue;
		}

		num = (cl.frame.parse_entities + i) & (MAX_PARSE_ENTITIES - 1);
		ent = &cl_parse_entities[num];

		ch = WA_FindLoopingSound(ent->number, sfx);

		if (ch)
		{
			ch->wa_autoframe = wa_framecount;
			ch->end = paintedtime + sc->length;
			continue;
		}

		ch = S_PickChannel(0, 0);

		if (!ch)
		{
			continue;
		}

		ch->autosound = true;
		ch->wa_autoframe = wa_framecount;
		ch->sfx = sfx;
		ch->entnum = ent->number;
		ch->master_vol = 255;
		ch->dist_mult = WA_LOOPATTENUATE;
		ch->end = paintedtime + sc->length;

		WA_PlayChannel(ch);
	}
}

/*
 * Starts any pending playsounds whose begin time has arrived. Without
 * this, S_StartSound's entries in s_pendingplays (built up by
 * S_IssuePlaysound's caller in sound.c) would sit there forever and no
 * sound would ever actually start -- mirrors AL_IssuePlaysounds exactly.
 */
static void
WA_IssuePlaysounds(void)
{
	playsound_t *ps;

	while (1)
	{
		ps = s_pendingplays.next;

		if (ps == &s_pendingplays)
		{
			break;
		}

		if (ps->begin > paintedtime)
		{
			break;
		}

		S_IssuePlaysound(ps);
	}
}

void
WA_Update(void)
{
	int i;
	channel_t *ch;

	paintedtime = cls.realtime;

	/* Browsers keep a fresh AudioContext 'suspended' until a user
	 * gesture -- WA_Init's own resume() attempt doesn't count (it's not
	 * itself running inside a gesture handler), and the keydown/
	 * touchstart listeners it attaches only help if the player actually
	 * presses something. A build that auto-launches straight into a
	 * demo (kaios_startcmd "demomap ...") can run through the whole
	 * thing without a single real keypress, leaving the context
	 * suspended -- and every sound plays completely silently the entire
	 * time, with nothing in the C-side state (sounds: counters, sndms)
	 * showing anything wrong, since AudioBufferSourceNode.start() works
	 * fine on a suspended context, it just produces no audible output.
	 * Retrying resume() every frame is cheap (a no-op once actually
	 * running) and self-heals regardless of which DOM event, if any,
	 * the input layer actually lets through to document. */
	EM_ASM({
		var ka = Module.kaiosAudio;
		if (ka && ka.ctx.state !== 'running' && typeof ka.ctx.resume === 'function') {
			try {
				ka.ctx.resume().catch(function(e) {
					console.log('[kaios] webaudio: resume() rejected: ' + e);
				});
			} catch (e) {
				console.log('[kaios] webaudio: resume() threw: ' + e);
			}
		}
	});

	/* Once-a-second diagnostic (same cadence as cl_screen.c's
	 * KAIOS_STATS) -- ctx.currentTime advancing confirms the graph is
	 * genuinely producing audio output, not just reporting a 'running'
	 * state while actually muted at some OS/browser level outside the
	 * Web Audio spec's own visibility (KaiOS's B2G/Gecko heritage has
	 * its own audio-channel/focus concepts that a plain AudioContext
	 * might not automatically satisfy). */
	if ((wa_framecount % 30) == 0)
	{
		EM_ASM({
			var ka = Module.kaiosAudio;
			if (ka) {
				console.log('KAIOS_WA_STATE: ctxState=' + ka.ctx.state +
					' currentTime=' + ka.ctx.currentTime.toFixed(2) +
					' destChannels=' + ka.ctx.destination.channelCount +
					' baseLatency=' + (ka.ctx.baseLatency !== undefined ? ka.ctx.baseLatency : 'n/a'));
			}
		});
	}

	if (s_underwater->modified || (wa_framecount == 0))
	{
		s_underwater->modified = false;

		EM_ASM({
			var ka = Module.kaiosAudio;
			if (ka) {
				ka.underwaterFilter.frequency.value = $0;
				console.log('[kaios] webaudio: underwaterFilter.frequency=' + $0 +
					' filterType=' + ka.underwaterFilter.type);
			}
		}, (snd_is_underwater && s_underwater->value) ?
			(200.0f + s_underwater_gain_hf->value * 2000.0f) : 22050.0f);
	}

	if (s_volume->modified)
	{
		s_volume->modified = false;

		EM_ASM({
			var ka = Module.kaiosAudio;
			if (ka) {
				ka.master.gain.value = $0;
			}
		}, s_volume->value);
	}

	ch = channels;

	for (i = 0; i < s_numchannels; i++, ch++)
	{
		if (!ch->sfx)
		{
			continue;
		}

		if (ch->autosound)
		{
			/* ambient loop sounds are refreshed every frame by
			 * WA_AddLoopSounds below -- one that didn't get refreshed
			 * is no longer audible from any entity, stop it for real
			 * (unlike SDL's memset-and-forget, this one is an actual
			 * playing native source that needs an explicit stop). */
			if (ch->wa_autoframe != wa_framecount)
			{
				WA_StopChannel(ch);
				continue;
			}
		}

		if (s_show->value)
		{
			Com_Printf("%3i %3i %s\n", ch->leftvol, ch->rightvol, ch->sfx->name);
		}

		SDL_Spatialize(ch);
		WA_PushParams(ch - channels, ch->leftvol, ch->rightvol);

		/* One-shot sounds past their natural end are done -- no need to
		 * round-trip into JS to ask, ch->end already tracks this the
		 * same way S_PickChannel's channel-stealing heuristic does. */
		if (!ch->autosound && (paintedtime >= ch->end))
		{
			WA_StopChannel(ch);
		}
	}

	wa_framecount++;

	WA_AddLoopSounds();

	/* stream music, if any -- routes through S_RawSamples/WA_RawSamples
	 * above like everything else; this baseq2 install has none (see
	 * "No Ogg Vorbis music tracks have been found" at boot) but keeping
	 * the call for parity with sdl.c/openal.c in case that ever changes. */
	OGG_Stream();

	WA_IssuePlaysounds();
}

/*
 * Cinematic/voiceover audio (cl_cin.c). Each chunk becomes its own tiny
 * AudioBuffer, scheduled to start exactly when the previous chunk ends
 * on the AudioContext's own clock -- gapless playback without needing a
 * continuous callback to keep a ring buffer fed.
 */
void
WA_RawSamples(int samples, int rate, int width, int channels,
		const byte *data, float volume)
{
	if (!samples)
	{
		return;
	}

	EM_ASM({
		var ka = Module.kaiosAudio;
		if (!ka) {
			return;
		}

		var ptr = $0;
		var frames = $1;
		var width = $2;
		var chans = $3;
		var rate = $4;
		var vol = $5;

		try {
			var buf = ka.ctx.createBuffer(chans, frames, rate);

			for (var c = 0; c < chans; c++) {
				var out = buf.getChannelData(c);

				if (width === 1) {
					for (var i = 0; i < frames; i++) {
						out[i] = (HEAPU8[ptr + i * chans + c] - 128) / 128.0;
					}
				} else {
					var base = ptr >> 1;
					for (var i = 0; i < frames; i++) {
						out[i] = HEAP16[base + i * chans + c] / 32768.0;
					}
				}
			}

			var src = ka.ctx.createBufferSource();
			src.buffer = buf;
			var g = ka.ctx.createGain();
			g.gain.value = Math.min(vol, 1.0);
			src.connect(g);
			g.connect(ka.master);

			var now = ka.ctx.currentTime;
			var startAt = Math.max(now, ka.rawNextTime);
			src.start(startAt);
			ka.rawNextTime = startAt + buf.duration;
		} catch (e) {
			console.log('[kaios] webaudio: raw sample queue threw: ' + e);
		}
	}, data, samples, width, channels, rate, volume);

	s_rawend += samples;
}

#endif /* __EMSCRIPTEN__ */
