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
 * Second Web Audio backend for KaiOS/Emscripten -- see the big comment
 * above the WA2_* declarations in header/local.h for the full reasoning.
 * Short version: webaudio.c's one-AudioBufferSourceNode-per-sound-play
 * design causes real, continuous GC pressure once several sounds are
 * playing at once (combat). This backend never creates a JS object per
 * game sound at all -- it mixes every active channel itself in plain C
 * (the same math sdl.c's SDL_PaintChannels does, reusing sdl.c's own
 * SDL_Cache/SDL_Spatialize/SDL_DriftBeginofs/SDL_RawSamples directly
 * rather than reimplementing them), and only touches WebAudio to queue
 * the already-mixed result in small fixed-size chunks, scheduled
 * back-to-back on the AudioContext's own clock -- exactly the gapless
 * pattern WA_RawSamples (webaudio.c) already uses for cinematics, just
 * running continuously instead of only for cinematic audio. The chunk
 * rate this produces is fixed and small, completely decoupled from how
 * many sounds the game happens to be playing.
 *
 * sound.c treats SS_WEBAUDIO2 as "just like SS_SDL" for everything
 * except init/shutdown/soundinfo/update -- channel spatialization,
 * begin-time drift, sample caching, and raw (cinematic) sample queuing
 * are all handled by the exact same sdl.c code paths SDL itself uses,
 * since paintedtime here advances in raw output-sample units the same
 * way SDL's does (not wall-clock ms, the way webaudio.c's does).
 *
 * =======================================================================
 */

#ifdef __EMSCRIPTEN__

#include <stdint.h>

#include <emscripten/em_asm.h>

#include "../header/client.h"
#include "header/local.h"

/* One AudioBuffer+AudioBufferSourceNode gets queued per chunk -- small
 * enough that the resulting audio latency is unnoticeable, large enough
 * that the queuing rate stays low regardless of how many game sounds
 * are actually playing. 2048 samples is ~46ms at 44.1kHz, the same
 * ballpark sdl.c's own s_kaios_buffer settled on for its ScriptProcessorNode
 * after real-device tuning (see that cvar's comment in sdl.c). */
#define WA2_CHUNK_SAMPLES 2048

/* How far ahead of the AudioContext's own clock to keep chunks queued.
 * This is what lets playback keep running smoothly through a slow or
 * stalled engine frame instead of being tied to how often WA2_Update()
 * happens to get called -- the same job sdl.c's s_mixahead does for its
 * own ring buffer, just measured against ctx.currentTime instead of our
 * paintedtime. */
#define WA2_MIX_AHEAD_MS 250

/* Never mix more than this many chunks in one WA2_Update() call, no
 * matter how far behind "ahead of ctx.currentTime" bookkeeping thinks
 * we are -- a safety valve, not something expected to trigger in
 * practice. ctx.currentTime itself appears to pause while the page is
 * backgrounded/suspended (matches real-device evidence: a ~23 minute
 * KAIOS_HEARTBEAT_GAP wall-clock gap with a tiny reported gap= value,
 * see this session's earlier findings), so a long real-world pause
 * shouldn't actually produce a large catch-up backlog here either --
 * this just guards the case where it somehow does. */
#define WA2_MAX_CHUNKS_PER_UPDATE 8

static qboolean wa2_inited = false;
static portable_samplepair_t wa2_mixbuf[WA2_CHUNK_SAMPLES];

qboolean
WA2_Init(void)
{
	int ok = EM_ASM_INT({
		try {
			var AC = window.AudioContext || window.webkitAudioContext;
			if (!AC) {
				console.log('[kaios] webaudio2: no AudioContext available');
				return 0;
			}

			var ka = {};
			ka.ctx = new AC();
			ka.master = ka.ctx.createGain();
			ka.master.gain.value = $0;
			ka.master.connect(ka.ctx.destination);
			ka.nextTime = 0;

			/* Same reasoning as webaudio.c's WA_Init -- a fresh/resumed
			 * AudioContext starts suspended until a user gesture, and
			 * this is a KaiOS phone where every session starts with a
			 * keypress or touch. */
			var resume = function() {
				if (ka.ctx.state !== 'running' && typeof ka.ctx.resume === 'function') {
					try {
						ka.ctx.resume().catch(function(e) {
							console.log('[kaios] webaudio2: resume() rejected: ' + e);
						});
					} catch (e) {
						console.log('[kaios] webaudio2: resume() threw: ' + e);
					}
				}
			};
			document.addEventListener('keydown', resume);
			document.addEventListener('touchstart', resume);
			resume();

			Module.kaiosAudio2 = ka;
			return ka.ctx.sampleRate;
		} catch (e) {
			console.log('[kaios] webaudio2: init threw: ' + e);
			return 0;
		}
	}, s_volume->value);

	wa2_inited = (ok != 0);

	if (wa2_inited)
	{
		sound.speed = ok;
		sound.channels = (Cvar_Get("sndchannels", "2", CVAR_ARCHIVE)->value != 1) ? 2 : 1;
		s_numchannels = MAX_CHANNELS;
	}

	return wa2_inited;
}

void
WA2_Shutdown(void)
{
	if (!wa2_inited)
	{
		return;
	}

	EM_ASM({
		if (Module.kaiosAudio2 && Module.kaiosAudio2.ctx) {
			try { Module.kaiosAudio2.ctx.close(); } catch (e) {}
		}
		Module.kaiosAudio2 = null;
	});

	wa2_inited = false;
}

void
WA2_SoundInfo(void)
{
	EM_ASM({
		var ka = Module.kaiosAudio2;
		if (!ka) {
			console.log('webaudio2 backend not initialized');
			return;
		}
		console.log('Web Audio backend #2 (C-side mix, chunked), sampleRate=' +
			ka.ctx.sampleRate + ', state=' + ka.ctx.state);
	});
}

/*
 * Starts any pending playsounds whose begin time has arrived -- same
 * list, same S_IssuePlaysound() sdl.c's own mixer drains too.
 */
static void
WA2_IssueDuePlays(void)
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

/*
 * Mixes exactly WA2_CHUNK_SAMPLES sample-pairs of every active channel
 * (plus any queued raw/cinematic samples) into wa2_mixbuf and advances
 * paintedtime by the same amount -- same responsibility as sdl.c's
 * SDL_PaintChannels, reimplemented independently rather than shared:
 * that function interleaves mixing with SDL's own ring-buffer output
 * format in small sub-blocks clipped to exact pending-play begin times,
 * which this backend's fixed-chunk-then-schedule approach doesn't need
 * -- a pending sound landing mid-chunk here just starts up to one
 * chunk (~46ms) later than requested instead, an inaudible tradeoff
 * for how much simpler it keeps this first version.
 */
static void
WA2_MixChunk(void)
{
	int i;
	channel_t *ch;

	memset(wa2_mixbuf, 0, sizeof(wa2_mixbuf));

	ch = channels;

	for (i = 0; i < s_numchannels; i++, ch++)
	{
		sfxcache_t *sc;
		int count;
		int j;

		if (!ch->sfx || (!ch->leftvol && !ch->rightvol))
		{
			continue;
		}

		sc = S_LoadSound(ch->sfx);

		if (!sc)
		{
			continue;
		}

		count = WA2_CHUNK_SAMPLES;

		if (ch->end - paintedtime < count)
		{
			count = ch->end - paintedtime;
		}

		if (count > 0)
		{
			if (sc->width == 2)
			{
				signed short *sfx = (signed short *)sc->data + ch->pos;

				for (j = 0; j < count; j++)
				{
					int data = sfx[j];

					wa2_mixbuf[j].left += (data * ch->leftvol) >> 8;
					wa2_mixbuf[j].right += (data * ch->rightvol) >> 8;
				}
			}
			else
			{
				unsigned char *sfx = sc->data + ch->pos;

				for (j = 0; j < count; j++)
				{
					int data = ((int)sfx[j] - 128) << 8;

					wa2_mixbuf[j].left += (data * ch->leftvol) >> 8;
					wa2_mixbuf[j].right += (data * ch->rightvol) >> 8;
				}
			}

			ch->pos += count;
		}

		if (paintedtime + count >= ch->end)
		{
			if (ch->autosound)
			{
				ch->pos = 0;
				ch->end = paintedtime + WA2_CHUNK_SAMPLES + sc->length;
			}
			else if (sc->loopstart >= 0)
			{
				ch->pos = sc->loopstart;
				ch->end = paintedtime + WA2_CHUNK_SAMPLES + sc->length - ch->pos;
			}
			else
			{
				ch->sfx = NULL;
			}
		}
	}

	/* cinematic/voiceover audio queued via S_RawSamples -> SDL_RawSamples
	 * (sound.c treats SS_WEBAUDIO2 as SDL for this) lands in the same
	 * s_rawsamples ring SDL's own mixer reads -- pull it in here too. */
	if (s_rawend >= paintedtime)
	{
		int stop = (paintedtime + WA2_CHUNK_SAMPLES < s_rawend) ?
			(paintedtime + WA2_CHUNK_SAMPLES) : s_rawend;

		for (i = paintedtime; i < stop; i++)
		{
			int s = i & (MAX_RAW_SAMPLES - 1);

			wa2_mixbuf[i - paintedtime].left += s_rawsamples[s].left;
			wa2_mixbuf[i - paintedtime].right += s_rawsamples[s].right;
		}
	}

	paintedtime += WA2_CHUNK_SAMPLES;
}

/*
 * Clamps wa2_mixbuf to 16-bit PCM and hands it to JS as one more
 * scheduled AudioBuffer, gapless with whatever was queued before it --
 * same technique webaudio.c's WA_RawSamples uses for cinematic chunks.
 */
static void
WA2_QueueChunk(void)
{
	static short pcm[WA2_CHUNK_SAMPLES * 2];
	int i;

	for (i = 0; i < WA2_CHUNK_SAMPLES; i++)
	{
		int l = wa2_mixbuf[i].left;
		int r = wa2_mixbuf[i].right;

		if (l > 32767)
		{
			l = 32767;
		}
		else if (l < -32768)
		{
			l = -32768;
		}

		if (r > 32767)
		{
			r = 32767;
		}
		else if (r < -32768)
		{
			r = -32768;
		}

		pcm[i * 2] = (short)l;
		pcm[i * 2 + 1] = (short)r;
	}

	EM_ASM({
		var ka = Module.kaiosAudio2;
		if (!ka) {
			return;
		}

		var ptr = $0;
		var frames = $1;
		var rate = $2;

		try {
			var buf = ka.ctx.createBuffer(2, frames, rate);
			var left = buf.getChannelData(0);
			var right = buf.getChannelData(1);
			var base = ptr >> 1;

			for (var i = 0; i < frames; i++) {
				left[i] = HEAP16[base + i * 2] / 32768.0;
				right[i] = HEAP16[base + i * 2 + 1] / 32768.0;
			}

			var src = ka.ctx.createBufferSource();
			src.buffer = buf;
			src.connect(ka.master);

			var startAt = Math.max(ka.ctx.currentTime, ka.nextTime);
			src.start(startAt);
			ka.nextTime = startAt + buf.duration;
		} catch (e) {
			console.log('[kaios] webaudio2: queue chunk threw: ' + e);
		}
	}, pcm, WA2_CHUNK_SAMPLES, sound.speed);
}

void
WA2_Update(void)
{
	double now, scheduled;
	int chunksQueued;

	/* keep the AudioContext awake -- same reasoning as WA_Update's own
	 * per-call resume() retry in webaudio.c. */
	EM_ASM({
		var ka = Module.kaiosAudio2;
		if (ka && ka.ctx.state !== 'running' && typeof ka.ctx.resume === 'function') {
			try {
				ka.ctx.resume().catch(function(e) {
					console.log('[kaios] webaudio2: resume() rejected: ' + e);
				});
			} catch (e) {
				console.log('[kaios] webaudio2: resume() threw: ' + e);
			}
		}
	});

	if (s_volume->modified)
	{
		s_volume->modified = false;

		EM_ASM({
			var ka = Module.kaiosAudio2;
			if (ka) {
				ka.master.gain.value = $0;
			}
		}, s_volume->value);
	}

	if (s_show->value)
	{
		int i;
		channel_t *ch = channels;
		int total = 0;

		for (i = 0; i < s_numchannels; i++, ch++)
		{
			if (ch->sfx && (ch->leftvol || ch->rightvol))
			{
				Com_Printf("%3i %3i %s\n", ch->leftvol, ch->rightvol, ch->sfx->name);
				total++;
			}
		}

		Com_Printf("----(%i)---- painted: %i\n", total, paintedtime);
	}

	now = EM_ASM_DOUBLE({
		var ka = Module.kaiosAudio2;
		return ka ? ka.ctx.currentTime : 0;
	});

	scheduled = EM_ASM_DOUBLE({
		var ka = Module.kaiosAudio2;
		return ka ? ka.nextTime : 0;
	});

	if (scheduled < now)
	{
		scheduled = now;
	}

	chunksQueued = 0;

	while (((scheduled - now) * 1000.0 < WA2_MIX_AHEAD_MS) &&
		(chunksQueued < WA2_MAX_CHUNKS_PER_UPDATE))
	{
		WA2_IssueDuePlays();
		WA2_MixChunk();
		WA2_QueueChunk();

		scheduled += (double)WA2_CHUNK_SAMPLES / (double)sound.speed;
		chunksQueued++;
	}

	/* sdl.c/openal.c each call this themselves at the tail of their own
	 * per-frame update -- it was never wired in here, so on this port's
	 * actual default/active backend (Custom2, this file) OGG_Stream()
	 * was simply never called at all. For the chunked SDL/OpenAL music
	 * path that would mean no music (OGG_Read() never runs), but this
	 * backend's music goes through a real <audio> element instead
	 * (OGG_StartNative()/WA_PlayMusic(), webaudio_music.c) that loops
	 * and streams on its own once started, so the *symptom* was subtler
	 * than silence: OGG_Stream() is also where ogg_volume/mute changes,
	 * ogg_shuffle, ogg_pausewithgame, and the ogg_enabled on/off toggle
	 * all actually get pushed out or acted on every frame -- none of
	 * that was happening either. Dragging the in-game "OGG volume"
	 * slider (menu.c) while a track was already playing silently did
	 * nothing audible until the next track/map change, when
	 * OGG_StartNative() would pick up whatever ogg_volume had become by
	 * then. */
	OGG_Stream();
}

#endif /* __EMSCRIPTEN__ */
