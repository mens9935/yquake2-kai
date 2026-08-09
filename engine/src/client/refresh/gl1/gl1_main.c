/*
 * Copyright (C) 1997-2001 Id Software, Inc.
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
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 * =======================================================================
 *
 * Refresher setup and main part of the frame generation
 *
 * =======================================================================
 */

#include "header/local.h"

#define NUM_BEAM_SEGS 6

model_t *r_worldmodel;

float gldepthmin, gldepthmax;

glconfig_t gl_config;
glstate_t gl_state;

image_t *r_notexture; /* use for bad textures */
image_t *r_particletexture; /* little dot for particles */

cplane_t frustum[4];

int r_visframecount; /* bumped when going to a new PVS */
int r_framecount; /* used for dlight push checking */

int c_brush_polys, c_alias_polys;

#ifdef __EMSCRIPTEN__
/* cl_screen.c's KAIOS_STATS diagnostic (SCR_DrawKaiosStats) references
 * r_polycount directly (extern int r_polycount;) -- that global only
 * really exists in the software renderer (sw_main.c), which is the one
 * KaiOS build this diagnostic was originally written for. Declared here
 * so a gl1 build (RENDERER=gl1, see build.sh) links at all, and kept
 * up to date every frame (RE_RenderFrame below, after R_Flash()) from
 * gl1's own poly counters, c_brush_polys/c_alias_polys above, so
 * KAIOS_STATS shows real numbers here too instead of always 0.
 *
 * RENDERER=unified links sw_main.c's own r_polycount into the same
 * binary as this file's -- extern to that single copy instead of
 * defining a second, competing one; still kept up to date for real
 * below regardless of which #branch defined the storage. Standalone
 * RENDERER=gl1 (soft not linked at all) still needs its own copy. */
#ifdef YQ2_KAIOS_UNIFIED_RENDERERS
extern int r_polycount;
#else
int r_polycount;
#endif

/* Spinning-cube-plus-frame-counter sanity check, drawn directly with
 * this exact GL1 context right after it's created and fully
 * initialized (RI_Init() below has already completed by the time
 * this ever runs -- see Qcommon_EmscriptenTick(), frame.c), before
 * any real game content (demomap/menu/console) is allowed to load.
 * The whole gl1/GLES1-on-WebGL1 investigation kept finding real
 * content silently failing to show up for reasons buried deep in game
 * code (glTexEnvi/glPointSize missing from Emscripten's own proc-
 * address allow-list, SCR_UpdateScreen early-returning, etc.) -- this
 * answers a much simpler, prior question first: can this context
 * actually present a moving frame to the screen at all, with nothing
 * else (menu, console, textures, sound) in the way. keydown[]/K_ENTER
 * reach across into client code the same way r_polycount does above
 * (single static binary, no dlopen boundary) rather than pulling in
 * client.h wholesale, which the renderer is deliberately decoupled
 * from everywhere else in this file.
 *
 * Back on: a synthetic random texture proved upload+sampling works in
 * general, but real game content (fonts, pics, world) still doesn't
 * show up and R_ApplyGLBuffer's own trace (gl1_buffer.c) never once
 * fired -- meaning the real 2D buffered draw path (glDrawElements,
 * used for every character/pic/HUD element) may never even be
 * reached with real content, or real (non-synthetic, mip-mapped)
 * texture data specifically may be the problem. Test both directly,
 * in this same proven-working context: bind the real already-loaded
 * conchars texture (draw_chars, gl1_draw.c -- guaranteed non-NULL by
 * now, Draw_InitLocal() would have fatally errored out otherwise, and
 * the engine clearly reached "Initialized") onto the cube instead of
 * synthetic noise, and directly call the real buffered 2D pipeline
 * (R_UpdateGLBuffer/R_Buffer2DQuad/R_ApplyGLBuffer, gl1_buffer.c) to
 * draw actual HUD-style content as a 2D overlay, the same functions
 * Draw_CharScaled/Draw_Pic use for real console/menu/HUD text. */
qboolean kaios_cube_test_active = false;
extern qboolean keydown[];
extern image_t *draw_chars;
extern void RI_KaiosResetBufTrace(void);

/* sky_images[6]/RI_SetSky (gl1_warp.c) are the exact same real-content
 * texture-loading path RI_SetSky() (part of the refexport_t interface,
 * called for every real map load) uses -- reusing it here means a
 * successful load proves the production sky codepath works, not a
 * synthetic stand-in for it. There's no map loaded yet at this point
 * (the cube test deliberately runs before kaios_startcmd), so there's
 * no worldspawn "sky" key to read -- try a short list of well-known
 * baseq2 skybox basenames until one actually resolves (sky_images[0]
 * staying r_notexture means GetSkyImage() couldn't find that name). */
extern image_t *sky_images[6];
extern void RI_SetSky(const char *name, float rotate, vec3_t axis);
extern unsigned d_8to24table[256];
#define KAIOS_K_ENTER 13
#define KAIOS_K_KP_ENTER 158

void
RI_KaiosCubeTestFrame(void)
{
	static float angle = 0.0f;
	static int frame_counter = 0;
	int bar_count, i;
	GLdouble aspect;
	qboolean kaios_trace;
	qboolean kaios_cube_sky_loaded;

	frame_counter++;
	angle += 2.0f;

	if (angle > 360.0f)
	{
		angle -= 360.0f;
	}

	/* The real-device log stopped dead right at tick #1 entering this
	 * function -- not even the glGetError() prints after the vertex-
	 * array setup made it out, meaning whatever throws does so before
	 * that point. Same before/after bracket technique that found
	 * glTexEnvi/glPointSize, applied to literally every call in this
	 * function, gated to frame 1 only so it doesn't spam every tick
	 * once this is past the culprit. */
	kaios_trace = (frame_counter == 1) ? true : false;
#define KAIOS_CUBE_TRACE(x) \
	do { \
		if (kaios_trace) { Com_Printf("KAIOS_CUBE_TRACE: before " #x "\n"); } \
		x; \
		if (kaios_trace) { Com_Printf("KAIOS_CUBE_TRACE: after " #x "\n"); } \
	} while (0)

	KAIOS_CUBE_TRACE(glViewport(0, 0, vid.width, vid.height));
	KAIOS_CUBE_TRACE(glClearColor(0.1f, 0.1f, 0.15f, 1.0f));
	KAIOS_CUBE_TRACE(glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT));

	/* glFinish() between draws did NOT stop the magenta A/B quad's
	 * color from bleeding onto the already-drawn white bars -- rules
	 * out "draws are batched, need an explicit flush point". The other
	 * candidate: this function's own R_ApplyGLBuffer() call at the end
	 * of the PREVIOUS frame (the real HUD-text test below) leaves
	 * GL_COLOR_ARRAY enabled if it doesn't get disabled on every
	 * return path, and once GL_COLOR_ARRAY is enabled, glColor4f()
	 * (the single-value uniform) is ignored entirely in favor of
	 * whatever stale per-vertex color array pointer/data is still
	 * bound -- which would explain a fixed, wrong color showing up
	 * across unrelated draw calls that all correctly called glColor4f.
	 * Defensively disable it at the very start of every frame here,
	 * before anything else runs. */
	KAIOS_CUBE_TRACE(glDisableClientState(GL_COLOR_ARRAY));

	/* The manually-built checkerboard mip-chain test (below) rendered
	 * correctly, proving mipmapped texturing works fine on this device
	 * in general -- but every REAL asset (conchars, sky) still comes
	 * back solid black regardless of filter/mipmap config. conchars is
	 * decoded from 8-bit indices through d_8to24table (populated by
	 * GetPCXPalette() from pics/colormap.pcx's embedded palette, early
	 * in RI_Init(), before Draw_InitLocal() loads conchars) -- if that
	 * table is actually all-zero/garbage at runtime on this device (a
	 * genuinely different failure than anything GL-related), decoding
	 * would silently produce uniformly black RGBA data that uploads
	 * and samples PERFECTLY FINE, matching every single black-texture
	 * observation so far without needing any GL bug at all. Print a
	 * few real entries directly to check. */
	if ((frame_counter % 60) == 1)
	{
		Com_Printf("KAIOS_CUBE_TEST: d_8to24table[0]=0x%08x [15]=0x%08x [32]=0x%08x [96]=0x%08x [255]=0x%08x\n",
			d_8to24table[0], d_8to24table[15], d_8to24table[32],
			d_8to24table[96], d_8to24table[255]);
	}

	/* One-time attempt to load a real skybox (6 distinct face textures,
	 * one per cube face -- a much better test than conchars: skies are
	 * full solid photographic images, not a mostly-transparent glyph
	 * atlas, so if texturing genuinely works this must be unmissable,
	 * no texcoord zooming required). Falls back to the conchars zoomed
	 * on all 6 faces if none of the candidate names resolve (e.g. this
	 * pak doesn't have them) or if draw_chars is somehow NULL. */
	{
		static qboolean sky_tried = false;
		static qboolean sky_loaded = false;

		if (!sky_tried)
		{
			static const char *sky_candidates[] = {
				"unit1_", "unit2_", "unit3_", "stad1_", "space1_", "warehou1_",
			};
			vec3_t axis = {0, 0, 1};
			int ci;

			sky_tried = true;

			for (ci = 0; ci < (int)(sizeof(sky_candidates) / sizeof(sky_candidates[0])); ci++)
			{
				RI_SetSky(sky_candidates[ci], 0, axis);

				if (sky_images[0] != r_notexture)
				{
					sky_loaded = true;
					Com_Printf("KAIOS_CUBE_TEST: sky '%s' loaded, texnum=%d\n",
						sky_candidates[ci], sky_images[0]->texnum);
					break;
				}
			}

			if (!sky_loaded)
			{
				Com_Printf("KAIOS_CUBE_TEST: no sky candidate resolved, "
					"falling back to conchars on cube faces\n");
			}
		}

		kaios_cube_sky_loaded = sky_loaded;
	}

	if (kaios_trace)
	{
		Com_Printf("KAIOS_CUBE_TEST: sky_loaded=%d draw_chars=%p texnum=%d\n",
			kaios_cube_sky_loaded, (void *)draw_chars, draw_chars ? draw_chars->texnum : -1);
	}

	KAIOS_CUBE_TRACE(glEnable(GL_TEXTURE_2D));
	KAIOS_CUBE_TRACE(glDisable(GL_BLEND));
	KAIOS_CUBE_TRACE(glDisable(GL_ALPHA_TEST));
	KAIOS_CUBE_TRACE(glEnable(GL_DEPTH_TEST));
	KAIOS_CUBE_TRACE(glDepthMask(GL_TRUE));

	aspect = (GLdouble)vid.width / (GLdouble)vid.height;
	KAIOS_CUBE_TRACE(glMatrixMode(GL_PROJECTION));
	KAIOS_CUBE_TRACE(glLoadIdentity());
	KAIOS_CUBE_TRACE(glFrustum(-aspect * 0.5, aspect * 0.5, -0.5, 0.5, 1.0, 20.0));

	KAIOS_CUBE_TRACE(glMatrixMode(GL_MODELVIEW));
	KAIOS_CUBE_TRACE(glLoadIdentity());
	KAIOS_CUBE_TRACE(glTranslatef(0.0f, 0.0f, -4.0f));
	KAIOS_CUBE_TRACE(glRotatef(angle, 1.0f, 1.0f, 0.0f));

	/* GLES1 has no glBegin/glEnd/glVertex3f/GL_QUADS at all -- that's
	 * desktop GL1.x-only, not part of the GLES1 header this renderer
	 * actually builds against (confirmed by the compiler rejecting
	 * them outright, not just at runtime). Vertex arrays + glDrawArrays
	 * is what the rest of this renderer already uses (R_DrawParticles
	 * above, for one) and what real GLES1 supports. Each face is 2
	 * triangles (6 verts: 0-1-2, 0-2-3), not GL_TRIANGLE_FAN -- plain
	 * GL_TRIANGLES to rule out the fan primitive specifically not
	 * being handled by Emscripten's client-array-to-WebGL-buffer
	 * emulation, since this is literally the first real draw call this
	 * whole investigation has ever visually confirmed (or not) -- every
	 * earlier "success" only meant the call *returned*, never that it
	 * produced pixels. */
	{
		static const GLfloat faces[6][18] = {
			{-1,-1, 1,  1,-1, 1,  1, 1, 1,   -1,-1, 1,  1, 1, 1, -1, 1, 1}, /* +Z */
			{-1,-1,-1, -1, 1,-1,  1, 1,-1,   -1,-1,-1,  1, 1,-1,  1,-1,-1}, /* -Z */
			{ 1,-1,-1,  1, 1,-1,  1, 1, 1,    1,-1,-1,  1, 1, 1,  1,-1, 1}, /* +X */
			{-1,-1,-1, -1,-1, 1, -1, 1, 1,   -1,-1,-1, -1, 1, 1, -1, 1,-1}, /* -X */
			{-1, 1,-1, -1, 1, 1,  1, 1, 1,   -1, 1,-1,  1, 1, 1,  1, 1,-1}, /* +Y */
			{-1,-1,-1,  1,-1,-1,  1,-1, 1,   -1,-1,-1,  1,-1, 1, -1,-1, 1}, /* -Y */
		};
		static const GLfloat colors[6][3] = {
			{1,0,0}, {0,1,0}, {0,0,1}, {1,1,0}, {1,0,1}, {0,1,1},
		};
		/* Same 0-1-2,0-2-3 vertex order as each face above, one UV
		 * pair per vertex -- GL_REPLACE means colors[] above no
		 * longer affects anything once texturing is on, kept only so
		 * a texture-sampling failure that falls back to vertex color
		 * would still be visible as the old rainbow cube instead of
		 * going invisible entirely.
		 *
		 * Sky face images are full solid photos, so the whole
		 * [0,1]x[0,1] sheet is meaningful -- unlike conchars.pcx (a
		 * 256x256 16x16 grid of 8x8 glyph cells, mostly transparent
		 * background around each tiny glyph), where mapping the WHOLE
		 * sheet onto one face samples predominantly empty space and
		 * would look near-black even with texturing fully working. So
		 * the conchars fallback zooms into one specific known-opaque
		 * cell instead: digit '0' is ASCII 48, row = 48>>4 = 3,
		 * col = 48&15 = 0, cell covers u=[0,0.0625] v=[0.1875,0.25]. */
		static const GLfloat texcoords_full[12] = {
			0,0, 1,0, 1,1,  0,0, 1,1, 0,1,
		};
		static const GLfloat texcoords_zoomed[12] = {
			0.0f,    0.1875f,
			0.0625f, 0.1875f,
			0.0625f, 0.25f,
			0.0f,    0.1875f,
			0.0625f, 0.25f,
			0.0f,    0.25f,
		};

		KAIOS_CUBE_TRACE(glEnableClientState(GL_VERTEX_ARRAY));
		KAIOS_CUBE_TRACE(glEnableClientState(GL_TEXTURE_COORD_ARRAY));

		if (kaios_trace)
		{
			Com_Printf("KAIOS_CUBE_TRACE: glGetError after glEnableClientState: 0x%x\n", glGetError());
		}

		for (i = 0; i < 6; i++)
		{
			if (kaios_trace) { Com_Printf("KAIOS_CUBE_TRACE: before face %d glColor4f\n", i); }
			glColor4f(colors[i][0], colors[i][1], colors[i][2], 1.0f);

			/* One real sky face texture per cube face when a sky
			 * loaded (6 faces, 6 sky images -- a natural 1:1 match),
			 * otherwise conchars zoomed into the '0' glyph on every
			 * face as before. */
			if (kaios_cube_sky_loaded)
			{
				glBindTexture(GL_TEXTURE_2D, sky_images[i]->texnum);
			}
			else if (draw_chars)
			{
				glBindTexture(GL_TEXTURE_2D, draw_chars->texnum);
			}

			if (kaios_trace) { Com_Printf("KAIOS_CUBE_TRACE: before face %d glVertexPointer\n", i); }
			glVertexPointer(3, GL_FLOAT, 0, faces[i]);
			if (kaios_trace) { Com_Printf("KAIOS_CUBE_TRACE: before face %d glTexCoordPointer\n", i); }
			glTexCoordPointer(2, GL_FLOAT, 0, kaios_cube_sky_loaded ? texcoords_full : texcoords_zoomed);
			if (kaios_trace) { Com_Printf("KAIOS_CUBE_TRACE: before face %d glDrawArrays\n", i); }
			glDrawArrays(GL_TRIANGLES, 0, 6);

			/* A LATER glColor4f() call on real hardware retroactively
			 * changed the color of geometry drawn EARLIER in the same
			 * frame (white bars from the bar_count loop turned pink
			 * after the magenta A/B quad's glColor4f ran) -- strong
			 * evidence Emscripten's LEGACY_GL_EMULATION batches
			 * sequential state+draw calls and only applies the LAST
			 * uniform value to the whole batch, rather than each draw
			 * capturing the state active when IT ran. If that's also
			 * true of glBindTexture, it would explain the black cube
			 * faces independent of which texture/texcoords were tried:
			 * only the LAST bind (face 5's) would ever really apply,
			 * so faces 0-4 would sample whatever was bound before this
			 * loop even started (nothing -- hence black). glFinish()
			 * forces each face's state+draw to actually flush before
			 * the next face's bind can change it. */
			glFinish();

			/* Unconditional drain -- glGetError() empties a QUEUE, not
			 * just "how did the last call go". Gating the call itself
			 * behind kaios_trace (frame 1 only) meant frames 2+ never
			 * drained here at all, so any real error from this loop
			 * would sit unread until whatever unrelated code happened
			 * to check glGetError() next (e.g. R_ApplyGLBuffer's own
			 * per-frame trace) -- misattributing it entirely. */
			{
				GLenum err = glGetError();
				if (kaios_trace) { Com_Printf("KAIOS_CUBE_TRACE: after face %d glDrawArrays+glFinish, glGetError=0x%x\n", i, err); }
			}
		}

		{
			GLenum err = glGetError();
			if (kaios_trace)
			{
				Com_Printf("KAIOS_CUBE_TEST: glGetError after cube glDrawArrays: 0x%x\n", err);
			}
		}

		KAIOS_CUBE_TRACE(glDisableClientState(GL_TEXTURE_COORD_ARRAY));
		KAIOS_CUBE_TRACE(glDisableClientState(GL_VERTEX_ARRAY));
	}

	KAIOS_CUBE_TRACE(glDisable(GL_TEXTURE_2D));

	/* Wireframe outline pass: GLES1 has no glPolygonMode(GL_LINE) at all
	 * (local.h's glPolygonMode(...) macro is a no-op under YQ2_GL1_GLES),
	 * so draw each face's 4-corner outline explicitly as GL_LINE_LOOP,
	 * textureless, bright yellow, depth test off so it's guaranteed
	 * visible regardless of the solid pass -- this answers a strictly
	 * simpler question than "why is texturing black": are the cube's
	 * polygons being transformed/rasterized at all. If the wireframe
	 * shows a proper cube outline, geometry+matrices are fine and the
	 * bug is isolated to texture sampling; if even this is invisible,
	 * the bug is upstream (transform/clipping/rasterization). */
	{
		static const GLfloat wire_faces[6][12] = {
			{-1,-1, 1,  1,-1, 1,  1, 1, 1, -1, 1, 1}, /* +Z */
			{-1,-1,-1,  1,-1,-1,  1, 1,-1, -1, 1,-1}, /* -Z */
			{ 1,-1,-1,  1, 1,-1,  1, 1, 1,  1,-1, 1}, /* +X */
			{-1,-1,-1, -1,-1, 1, -1, 1, 1, -1, 1,-1}, /* -X */
			{-1, 1,-1, -1, 1, 1,  1, 1, 1,  1, 1,-1}, /* +Y */
			{-1,-1,-1,  1,-1,-1,  1,-1, 1, -1,-1, 1}, /* -Y */
		};

		KAIOS_CUBE_TRACE(glDisable(GL_DEPTH_TEST));
		KAIOS_CUBE_TRACE(glColor4f(1.0f, 1.0f, 0.0f, 1.0f));
		KAIOS_CUBE_TRACE(glEnableClientState(GL_VERTEX_ARRAY));

		for (i = 0; i < 6; i++)
		{
			glVertexPointer(3, GL_FLOAT, 0, wire_faces[i]);
			glDrawArrays(GL_LINE_LOOP, 0, 4);
		}

		{
			GLenum err = glGetError();
			if (kaios_trace)
			{
				Com_Printf("KAIOS_CUBE_TEST: glGetError after wireframe glDrawArrays: 0x%x\n", err);
			}
		}

		KAIOS_CUBE_TRACE(glDisableClientState(GL_VERTEX_ARRAY));
		KAIOS_CUBE_TRACE(glEnable(GL_DEPTH_TEST));
	}

	/* 2D overlay: a bar that fills up and resets every 20 frames, the
	 * simplest possible "is this actually updating" indicator without
	 * needing the game's own font/pic system (Draw_InitLocal has not
	 * run against any real content yet -- this deliberately runs
	 * before all of that). Same vertex-array approach as the cube
	 * above, GL_TRIANGLES (2 tris, 6 verts) per bar, not
	 * GL_TRIANGLE_FAN, for the same reason as the cube faces above. */
	KAIOS_CUBE_TRACE(glDisable(GL_DEPTH_TEST));
	KAIOS_CUBE_TRACE(glMatrixMode(GL_PROJECTION));
	KAIOS_CUBE_TRACE(glLoadIdentity());
	KAIOS_CUBE_TRACE(glOrtho(0, vid.width, vid.height, 0, -1, 1));
	KAIOS_CUBE_TRACE(glMatrixMode(GL_MODELVIEW));
	KAIOS_CUBE_TRACE(glLoadIdentity());

	bar_count = frame_counter % 20;
	KAIOS_CUBE_TRACE(glColor4f(1.0f, 1.0f, 1.0f, 1.0f));
	KAIOS_CUBE_TRACE(glEnableClientState(GL_VERTEX_ARRAY));

	for (i = 0; i < bar_count; i++)
	{
		float x = 4 + i * 10;
		GLfloat quad[12] = {
			x, 4,  x + 8, 4,  x + 8, 12,
			x, 4,  x + 8, 12,  x, 12,
		};

		glVertexPointer(2, GL_FLOAT, 0, quad);
		glDrawArrays(GL_TRIANGLES, 0, 6);
	}

	{
		GLenum err = glGetError();
		if (kaios_trace)
		{
			Com_Printf("KAIOS_CUBE_TEST: glGetError after bar glDrawArrays: 0x%x\n", err);
		}
	}

	/* Force the white bars above to actually flush before the magenta
	 * quad below changes glColor4f -- real-device testing showed the
	 * later magenta glColor4f() call retroactively turning these
	 * already-drawn white bars pink, meaning the color uniform wasn't
	 * captured per-draw. glFinish() is the direct test of that. */
	glFinish();

	{
		GLenum err = glGetError();
		if (kaios_trace)
		{
			Com_Printf("KAIOS_CUBE_TEST: glGetError after bar glFinish: 0x%x\n", err);
		}
	}

	/* glDrawArrays vs glDrawElements A/B test: an identical-shaped
	 * quad, but drawn via glDrawElements(GL_UNSIGNED_SHORT indices)
	 * instead of glDrawArrays -- the exact draw call R_ApplyGLBuffer
	 * (gl1_buffer.c) uses for literally every piece of real 2D
	 * content (fonts, pics, HUD) and world geometry, which has never
	 * once been visually confirmed to produce a pixel despite
	 * glGetError() reading 0 on every call. Fixed position (not tied
	 * to bar_count) and a color used nowhere else so it's unambiguous:
	 * if the glDrawArrays bars above are visible but this magenta
	 * square isn't, glDrawElements itself -- not texturing -- is the
	 * culprit. */
	{
		GLfloat idx_quad[8] = {
			200, 4,  216, 4,  216, 20,  200, 20,
		};
		static const GLushort idx_indices[6] = {0, 1, 2, 0, 2, 3};

		glColor4f(1.0f, 0.0f, 1.0f, 1.0f);
		glVertexPointer(2, GL_FLOAT, 0, idx_quad);
		glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, idx_indices);

		{
			GLenum err = glGetError();
			if (kaios_trace)
			{
				Com_Printf("KAIOS_CUBE_TEST: glGetError after glDrawElements A/B quad: 0x%x\n", err);
			}
		}
	}

	/* Minimal isolated single textured quad: one glBindTexture, one
	 * glDrawArrays, nothing else competing for texture-unit state
	 * nearby -- unlike the cube's per-face loop (6 rapid sequential
	 * binds) or the full RDraw_CharScaled/R_ApplyGLBuffer pipeline
	 * (many layers of indirection). If even this doesn't show the '0'
	 * glyph, the bug is in this device's glBindTexture/glTexCoordPointer
	 * handling itself, not in looping or buffering. */
	if (draw_chars)
	{
		GLfloat tex_quad[8] = {
			60, 4,  76, 4,  76, 20,  60, 20,
		};
		static const GLfloat tex_quad_uv[8] = {
			0.0f,    0.1875f,
			0.0625f, 0.1875f,
			0.0625f, 0.25f,
			0.0f,    0.25f,
		};

		glEnable(GL_TEXTURE_2D);
		glBindTexture(GL_TEXTURE_2D, draw_chars->texnum);

		/* Every texture test so far (whole-sheet conchars, zoomed
		 * conchars, sky candidates, and now this maximally-isolated
		 * single quad) has come back solid black -- force NEAREST
		 * filtering explicitly on THIS bind, overriding whatever
		 * min/mag filter state the texture object actually has, to
		 * rule out an incomplete-mipmap-chain sampling failure (which
		 * some GL implementations silently render as black rather
		 * than erroring) as yet another candidate, independent of
		 * whatever gl1_image.c's R_Upload32()/R_TextureMode() decided
		 * at load/boot time. */
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

		/* glGetError() drains a QUEUE, not just "how did the last call
		 * go" -- if this isolated quad's own calls generate an error
		 * here and nothing reads it until some totally unrelated LATER
		 * call checks glGetError() (e.g. R_ApplyGLBuffer's own trace),
		 * that later, innocent call gets blamed for an error it never
		 * caused. Real-device testing showed exactly this after this
		 * NEAREST change landed: R_ApplyGLBuffer's glDrawElements
		 * trace, previously always 0x0, started reading 0x502 (
		 * GL_INVALID_OPERATION) on every single call -- almost
		 * certainly this block's own unconsumed error leaking forward,
		 * not a new problem in R_ApplyGLBuffer. Drain unconditionally,
		 * every frame, so no error from here can contaminate later
		 * checks; only print every ~60 frames to avoid flooding. */
		{
			qboolean isolated_quad_trace = ((frame_counter % 60) == 1) ? true : false;
			GLenum err = glGetError();

			if (isolated_quad_trace)
			{
				Com_Printf("KAIOS_CUBE_TEST: glGetError after forced NEAREST glTexParameteri: 0x%x\n", err);
			}
		}

		glEnableClientState(GL_VERTEX_ARRAY);
		glEnableClientState(GL_TEXTURE_COORD_ARRAY);
		glVertexPointer(2, GL_FLOAT, 0, tex_quad);
		glTexCoordPointer(2, GL_FLOAT, 0, tex_quad_uv);
		glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

		/* Drain again after the draw, same reasoning. */
		{
			qboolean isolated_quad_trace = ((frame_counter % 60) == 1) ? true : false;
			GLenum err = glGetError();

			if (isolated_quad_trace)
			{
				Com_Printf("KAIOS_CUBE_TEST: glGetError after isolated textured quad glDrawArrays: 0x%x\n", err);
			}
		}

		glDisableClientState(GL_TEXTURE_COORD_ARRAY);
		glDisable(GL_TEXTURE_2D);
	}

	/* Earlier this session (before the glFrustum fix), a cube face
	 * textured with SYNTHETIC random noise -- freshly uploaded right
	 * here at runtime, single level, no mipmaps -- rendered visibly
	 * correctly. Every REAL asset (conchars, sky images) has rendered
	 * solid black ever since, in every configuration tried (whole
	 * sheet, zoomed, forced NEAREST). Build a small, deliberately
	 * COMPLETE mipmap chain (8x8 down to 1x1, a red/white checkerboard
	 * so it's visually unambiguous) entirely ourselves, with the
	 * DEFAULT (non-forced) GL_LINEAR_MIPMAP_NEAREST filter gl_filter_min
	 * normally uses -- unlike the isolated quad above, which forces
	 * NEAREST. If this renders the checkerboard fine, mipmapped
	 * texturing itself works on this device and the bug is specific to
	 * how gl1_image.c's R_Upload32Soft() builds real assets' mip chains
	 * (or their pixel data); if this ALSO comes back black, the bug is
	 * in this device's mipmap sampling generally, regardless of data
	 * source. */
	{
		static GLuint synth_tex = 0;
		static qboolean synth_tex_ready = false;

		if (!synth_tex_ready)
		{
			int lvl, size;

			synth_tex_ready = true;
			glGenTextures(1, &synth_tex);
			glBindTexture(GL_TEXTURE_2D, synth_tex);

			for (lvl = 0, size = 8; size >= 1; lvl++, size /= 2)
			{
				GLubyte pixels[8 * 8 * 4];
				int x, y;

				for (y = 0; y < size; y++)
				{
					for (x = 0; x < size; x++)
					{
						int idx = (y * size + x) * 4;
						qboolean white = ((x + y) & 1) ? true : false;

						pixels[idx + 0] = 255;
						pixels[idx + 1] = white ? 255 : 0;
						pixels[idx + 2] = white ? 255 : 0;
						pixels[idx + 3] = 255;
					}
				}

				glTexImage2D(GL_TEXTURE_2D, lvl, GL_RGBA, size, size, 0,
					GL_RGBA, GL_UNSIGNED_BYTE, pixels);
			}

			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

			if (kaios_trace)
			{
				Com_Printf("KAIOS_CUBE_TEST: synth mip chain built, texid=%u glGetError=0x%x\n",
					synth_tex, glGetError());
			}
		}

		{
			GLfloat synth_quad[8] = {
				120, 4,  136, 4,  136, 20,  120, 20,
			};
			static const GLfloat synth_uv[8] = {
				0,0, 1,0, 1,1, 0,1,
			};

			glEnable(GL_TEXTURE_2D);
			glBindTexture(GL_TEXTURE_2D, synth_tex);
			glEnableClientState(GL_VERTEX_ARRAY);
			glEnableClientState(GL_TEXTURE_COORD_ARRAY);
			glVertexPointer(2, GL_FLOAT, 0, synth_quad);
			glTexCoordPointer(2, GL_FLOAT, 0, synth_uv);
			glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

			{
				qboolean synth_trace = ((frame_counter % 60) == 1) ? true : false;
				GLenum err = glGetError();

				if (synth_trace)
				{
					Com_Printf("KAIOS_CUBE_TEST: glGetError after synth mip quad: 0x%x\n", err);
				}
			}

			glDisableClientState(GL_TEXTURE_COORD_ARRAY);
			glDisable(GL_TEXTURE_2D);
		}
	}

	/* d_8to24table is confirmed correctly populated with real, varied
	 * color data ([15]=0xffebebeb is white) -- not the unpopulated-
	 * palette theory. The small (8x8) synthetic mip chain above proved
	 * mipmapped texturing works, but conchars is NOT mipmapped at all
	 * (it_pic type, mipmap=false in R_Upload32Soft() -- a single
	 * glTexImage2D call, no mip levels, gl_filter_max/GL_LINEAR filter).
	 * The one thing never yet isolated: SIZE. conchars is 256x256;
	 * every synthetic texture tried so far has been 8x8. Build a
	 * single-level (no mipmaps -- matching conchars' real config
	 * exactly), 256x256 synthetic texture with a coarse, unambiguous
	 * checkerboard, GL_LINEAR filter (matching gl_filter_max), to
	 * isolate whether large single-level non-mipmapped textures
	 * specifically are what's broken on this device. */
	{
		static GLuint synth_tex_big = 0;
		static qboolean synth_tex_big_ready = false;

		if (!synth_tex_big_ready)
		{
			static GLubyte pixels[256 * 256 * 4];
			int x, y;

			synth_tex_big_ready = true;

			for (y = 0; y < 256; y++)
			{
				for (x = 0; x < 256; x++)
				{
					int idx = (y * 256 + x) * 4;
					qboolean white = (((x >> 4) + (y >> 4)) & 1) ? true : false;

					pixels[idx + 0] = 255;
					pixels[idx + 1] = white ? 255 : 0;
					pixels[idx + 2] = white ? 255 : 0;
					pixels[idx + 3] = 255;
				}
			}

			glGenTextures(1, &synth_tex_big);
			glBindTexture(GL_TEXTURE_2D, synth_tex_big);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 256, 0,
				GL_RGBA, GL_UNSIGNED_BYTE, pixels);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

			if (kaios_trace)
			{
				Com_Printf("KAIOS_CUBE_TEST: 256x256 synth texture built, texid=%u glGetError=0x%x\n",
					synth_tex_big, glGetError());
			}
		}

		{
			GLfloat quad[8] = {
				160, 4,  176, 4,  176, 20,  160, 20,
			};
			static const GLfloat uv[8] = {
				0,0, 1,0, 1,1, 0,1,
			};

			glEnable(GL_TEXTURE_2D);
			glBindTexture(GL_TEXTURE_2D, synth_tex_big);
			glEnableClientState(GL_VERTEX_ARRAY);
			glEnableClientState(GL_TEXTURE_COORD_ARRAY);
			glVertexPointer(2, GL_FLOAT, 0, quad);
			glTexCoordPointer(2, GL_FLOAT, 0, uv);
			glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

			{
				qboolean t = ((frame_counter % 60) == 1) ? true : false;
				GLenum err = glGetError();

				if (t)
				{
					Com_Printf("KAIOS_CUBE_TEST: glGetError after 256x256 synth quad: 0x%x\n", err);
				}
			}

			glDisableClientState(GL_TEXTURE_COORD_ARRAY);
			glDisable(GL_TEXTURE_2D);
		}
	}

	KAIOS_CUBE_TRACE(glDisableClientState(GL_VERTEX_ARRAY));

	/* Draw actual HUD-style text through the REAL buffered 2D pipeline
	 * -- RDraw_CharScaled (gl1_draw.c) is the exact function
	 * SCR_DrawConsole()/M_Draw() call per on-screen character, which
	 * internally does R_UpdateGLBuffer()+R_Buffer2DQuad() to queue
	 * into gl_buf, then R_ApplyGLBuffer() (called here explicitly,
	 * since this test loop bypasses RI_EndFrame()'s own call to it)
	 * actually flushes and draws it via glDrawElements(). Real device
	 * testing showed R_ApplyGLBuffer's own trace never firing at all
	 * during real gameplay -- this calls the exact same functions
	 * directly, in this already-proven-working context, to see
	 * whether the buffered pipeline itself works when it does get
	 * reached. */
	if (draw_chars)
	{
		int ch;

		if (kaios_trace)
		{
			Com_Printf("KAIOS_CUBE_TEST: before RDraw_CharScaled loop\n");
		}

		/* Cycle through a sliding window of the real printable ASCII
		 * range (33..126, the full glyph set conchars.pcx actually
		 * has drawable content for, skipping 32=space which
		 * RDraw_CharScaled itself no-ops on) instead of just
		 * repeating '0'-'9' -- shows far more of the real font atlas,
		 * shifting to a new window every ~30 frames (~1s). */
		for (ch = 0; ch < 16; ch++)
		{
			int range = 126 - 33;
			int base = 33 + ((frame_counter / 30) * 16) % range;
			int c = 33 + ((base - 33 + ch) % range);

			RDraw_CharScaled(4 + ch * 10, vid.height - 20, c, 1.0f);
		}

		if (kaios_trace)
		{
			Com_Printf("KAIOS_CUBE_TEST: before R_ApplyGLBuffer (HUD text)\n");
		}

		R_ApplyGLBuffer();

		if (kaios_trace)
		{
			Com_Printf("KAIOS_CUBE_TEST: after R_ApplyGLBuffer (HUD text), glGetError=0x%x\n",
				glGetError());
		}

		/* The manual call above just consumed R_ApplyGLBuffer's own
		 * one-shot trace budget -- reset it so the real game's first
		 * buffered 2D draw (once the demo actually loads after OK) is
		 * still what gets traced, not this test call. */
		RI_KaiosResetBufTrace();
	}
#undef KAIOS_CUBE_TRACE

	if (frame_counter == 1 || (frame_counter % 60) == 0)
	{
		Com_Printf("KAIOS_CUBE_TEST: frame %d, press OK to continue loading\n", frame_counter);
	}

	/* Edge-triggered (was up last frame, is down now), not just "is
	 * currently down" -- the physical OK press used to launch this app
	 * from the KaiOS home screen could plausibly still read as held by
	 * the time this first frame runs, which would otherwise exit the
	 * test on frame 1 before anyone ever saw the cube. */
	{
		static qboolean was_down = false;
		qboolean is_down = (keydown[KAIOS_K_ENTER] || keydown[KAIOS_K_KP_ENTER]) ? true : false;

		if (is_down && !was_down)
		{
			kaios_cube_test_active = false;
			Com_Printf("KAIOS_CUBE_TEST: OK pressed after %d frames, continuing to load\n", frame_counter);
		}

		was_down = is_down;
	}
}
#endif

float v_blend[4]; /* final blending color */

void R_Strings(void);

/* view origin */
vec3_t vup;
vec3_t vpn;
vec3_t vright;
vec3_t r_origin;

float r_world_matrix[16];
float r_base_world_matrix[16];

/* screen size info */
int r_viewcluster, r_viewcluster2, r_oldviewcluster, r_oldviewcluster2;
unsigned r_rawpalette[256];

cvar_t *r_norefresh;
cvar_t *r_drawentities;
cvar_t *r_drawworld;
cvar_t *r_speeds;
cvar_t *r_fullbright;
cvar_t *r_novis;
cvar_t *r_lerpmodels;
cvar_t *gl_lefthand;
cvar_t *r_gunfov;
cvar_t *r_farsee;
cvar_t *r_validation;

cvar_t *r_lightlevel;
cvar_t *gl1_overbrightbits;

cvar_t *gl1_particle_min_size;
cvar_t *gl1_particle_max_size;
cvar_t *gl1_particle_size;
cvar_t *gl1_particle_att_a;
cvar_t *gl1_particle_att_b;
cvar_t *gl1_particle_att_c;
cvar_t *gl1_particle_square;

cvar_t *gl1_palettedtexture;
cvar_t *gl1_pointparameters;
cvar_t *gl1_multitexture;
cvar_t *gl1_lightmapcopies;
cvar_t *gl1_discardfb;

cvar_t *gl_drawbuffer;
cvar_t *gl_lightmap;
cvar_t *gl_shadows;
cvar_t *gl1_stencilshadow;
cvar_t *r_mode;
cvar_t *r_fixsurfsky;
cvar_t *gl1_minlight;

cvar_t *r_customwidth;
cvar_t *r_customheight;

cvar_t *r_retexturing;
cvar_t *r_scale8bittextures;

cvar_t *gl_nolerp_list;
cvar_t *r_lerp_list;
cvar_t *r_2D_unfiltered;
cvar_t *r_videos_unfiltered;

cvar_t *gl1_dynamic;
cvar_t *r_modulate;
cvar_t *gl_nobind;
cvar_t *gl1_round_down;
cvar_t *gl1_picmip;
cvar_t *gl_showtris;
cvar_t *gl_showbbox;
cvar_t *gl1_ztrick;
cvar_t *gl_zfix;
cvar_t *gl_finish;
cvar_t *r_clear;
cvar_t *r_cull;
cvar_t *r_distcull_dist;
cvar_t *r_occlusion_cull;
#ifdef __EMSCRIPTEN__
cvar_t *r_gl1_static_vbo;
#endif
cvar_t *gl_polyblend;
cvar_t *gl1_flashblend;
cvar_t *gl1_saturatelighting;
cvar_t *r_vsync;
cvar_t *gl_texturemode;
cvar_t *gl1_texturealphamode;
cvar_t *gl1_texturesolidmode;
cvar_t *gl_anisotropic;
cvar_t *r_lockpvs;
cvar_t *gl_msaa_samples;

cvar_t *vid_fullscreen;
cvar_t *vid_gamma;

cvar_t *gl1_stereo;
cvar_t *gl1_stereo_separation;
cvar_t *gl1_stereo_anaglyph_colors;
cvar_t *gl1_stereo_convergence;

static cvar_t *gl_znear;
static cvar_t *gl1_waterwarp;

/* RENDERER=unified links sw_main.c's own `refimport_t ri;` into the
 * same binary as this file's -- extern to that single copy instead of
 * defining a second, competing one (ref.h's `extern refimport_t ri;`
 * is what every renderer's own code, and client/refresh/files/, reach
 * it through either way). GetRefAPI() below still assigns `ri = imp;`
 * on load same as always. Standalone RENDERER=gl1 (soft not linked at
 * all) still needs its own copy. */
#if defined(__EMSCRIPTEN__) && defined(YQ2_KAIOS_UNIFIED_RENDERERS)
extern refimport_t ri;
#else
refimport_t ri;
#endif

void LM_FreeLightmapBuffers(void);
void Scrap_Init(void);

extern void R_SetDefaultState(void);
extern void R_ResetGLBuffer(void);

void
R_RotateForEntity(entity_t *e)
{
	glTranslatef(e->origin[0], e->origin[1], e->origin[2]);

	glRotatef(e->angles[1], 0, 0, 1);
	glRotatef(-e->angles[0], 0, 1, 0);
	glRotatef(-e->angles[2], 1, 0, 0);
}

void
R_DrawSpriteModel(entity_t *currententity, const model_t *currentmodel)
{
	float alpha = 1.0F;
	vec3_t point[4];
	dsprframe_t *frame;
	float *up, *right;
	dsprite_t *psprite;
	image_t *skin;

	R_EnableMultitexture(false);
	/* don't even bother culling, because it's just
	   a single polygon without a surface cache */
	psprite = (dsprite_t *)currentmodel->extradata;

	currententity->frame %= psprite->numframes;
	frame = &psprite->frames[currententity->frame];

	/* normal sprite */
	up = vup;
	right = vright;

	if (currententity->flags & RF_TRANSLUCENT)
	{
		alpha = currententity->alpha;
	}

	if (alpha != 1.0F)
	{
		glEnable(GL_BLEND);
	}

	glColor4f(1, 1, 1, alpha);

	skin = currentmodel->skins[currententity->frame];
	if (!skin)
	{
		skin = r_notexture; /* fallback... */
	}

	R_Bind(skin->texnum);

	R_TexEnv(GL_MODULATE);

	if (alpha == 1.0)
	{
		glEnable(GL_ALPHA_TEST);
	}
	else
	{
		glDisable(GL_ALPHA_TEST);
	}

	GLfloat tex[] = {
		0, 1,
		0, 0,
		1, 0,
		1, 1
	};

	VectorMA( currententity->origin, -frame->origin_y, up, point[0] );
	VectorMA( point[0], -frame->origin_x, right, point[0] );

	VectorMA( currententity->origin, frame->height - frame->origin_y, up, point[1] );
	VectorMA( point[1], -frame->origin_x, right, point[1] );

	VectorMA( currententity->origin, frame->height - frame->origin_y, up, point[2] );
	VectorMA( point[2], frame->width - frame->origin_x, right, point[2] );

	VectorMA( currententity->origin, -frame->origin_y, up, point[3] );
	VectorMA( point[3], frame->width - frame->origin_x, right, point[3] );

	glEnableClientState( GL_VERTEX_ARRAY );
	glEnableClientState( GL_TEXTURE_COORD_ARRAY );

	glVertexPointer( 3, GL_FLOAT, 0, point );
	glTexCoordPointer( 2, GL_FLOAT, 0, tex );
	glDrawArrays( GL_TRIANGLE_FAN, 0, 4 );

	glDisableClientState( GL_VERTEX_ARRAY );
	glDisableClientState( GL_TEXTURE_COORD_ARRAY );

	glDisable(GL_ALPHA_TEST);
	R_TexEnv(GL_REPLACE);

	if (alpha != 1.0F)
	{
		glDisable(GL_BLEND);
	}

	glColor4f(1, 1, 1, 1);
}

void
R_DrawNullModel(entity_t *currententity)
{
	vec3_t shadelight;

	if (currententity->flags & RF_FULLBRIGHT)
	{
		shadelight[0] = shadelight[1] = shadelight[2] = 1.0F;
	}
	else
	{
		R_LightPoint(currententity, currententity->origin, shadelight);
	}

	R_EnableMultitexture(false);
	glPushMatrix();
	R_RotateForEntity(currententity);

	glDisable(GL_TEXTURE_2D);
	glColor4f( shadelight[0], shadelight[1], shadelight[2], 1 );

    GLfloat vtxA[] = {
        0, 0, -16,
        16 * cos( 0 * M_PI / 2 ), 16 * sin( 0 * M_PI / 2 ), 0,
        16 * cos( 1 * M_PI / 2 ), 16 * sin( 1 * M_PI / 2 ), 0,
        16 * cos( 2 * M_PI / 2 ), 16 * sin( 2 * M_PI / 2 ), 0,
        16 * cos( 3 * M_PI / 2 ), 16 * sin( 3 * M_PI / 2 ), 0,
        16 * cos( 4 * M_PI / 2 ), 16 * sin( 4 * M_PI / 2 ), 0
    };

    glEnableClientState( GL_VERTEX_ARRAY );

    glVertexPointer( 3, GL_FLOAT, 0, vtxA );
    glDrawArrays( GL_TRIANGLE_FAN, 0, 6 );

    glDisableClientState( GL_VERTEX_ARRAY );

	GLfloat vtxB[] = {
		0, 0, 16,
		16 * cos( 4 * M_PI / 2 ), 16 * sin( 4 * M_PI / 2 ), 0,
		16 * cos( 3 * M_PI / 2 ), 16 * sin( 3 * M_PI / 2 ), 0,
		16 * cos( 2 * M_PI / 2 ), 16 * sin( 2 * M_PI / 2 ), 0,
		16 * cos( 1 * M_PI / 2 ), 16 * sin( 1 * M_PI / 2 ), 0,
		16 * cos( 0 * M_PI / 2 ), 16 * sin( 0 * M_PI / 2 ), 0
	};

	glEnableClientState( GL_VERTEX_ARRAY );

	glVertexPointer( 3, GL_FLOAT, 0, vtxB );
	glDrawArrays( GL_TRIANGLE_FAN, 0, 6 );

	glDisableClientState( GL_VERTEX_ARRAY );

	glColor4f(1, 1, 1, 1);
	glPopMatrix();
	glEnable(GL_TEXTURE_2D);
}

void
R_DrawEntitiesOnList(void)
{
	int i;

	if (!r_drawentities->value)
	{
		return;
	}

	/* draw non-transparent first */
	for (i = 0; i < r_newrefdef.num_entities; i++)
	{
		entity_t *currententity = &r_newrefdef.entities[i];

		if (currententity->flags & RF_TRANSLUCENT)
		{
			continue; /* solid */
		}

		if (currententity->flags & RF_BEAM)
		{
			R_DrawBeam(currententity);
		}
		else
		{
			const model_t *currentmodel = currententity->model;

			if (!currentmodel)
			{
				R_DrawNullModel(currententity);
				continue;
			}

			switch (currentmodel->type)
			{
				case mod_alias:
					R_DrawAliasModel(currententity, currentmodel);
					break;
				case mod_brush:
					R_DrawBrushModel(currententity, currentmodel);
					break;
				case mod_sprite:
					R_DrawSpriteModel(currententity, currentmodel);
					break;
				default:
					Com_Error(ERR_DROP, "Bad modeltype");
					break;
			}
		}
	}

	/* draw transparent entities
	   we could sort these if it ever
	   becomes a problem... */
	glDepthMask(GL_FALSE);

	for (i = 0; i < r_newrefdef.num_entities; i++)
	{
		entity_t *currententity = &r_newrefdef.entities[i];

		if (!(currententity->flags & RF_TRANSLUCENT))
		{
			continue; /* solid */
		}

		if (currententity->flags & RF_BEAM)
		{
			R_DrawBeam(currententity);
		}
		else
		{
			const model_t *currentmodel = currententity->model;

			if (!currentmodel)
			{
				R_DrawNullModel(currententity);
				continue;
			}

			switch (currentmodel->type)
			{
				case mod_alias:
					R_DrawAliasModel(currententity, currentmodel);
					break;
				case mod_brush:
					R_DrawBrushModel(currententity, currentmodel);
					break;
				case mod_sprite:
					R_DrawSpriteModel(currententity, currentmodel);
					break;
				default:
					Com_Error(ERR_DROP, "Bad modeltype");
					break;
			}
		}
	}

	glDepthMask(GL_TRUE); /* back to writing */
	R_EnableMultitexture(false);
}

void
R_DrawParticles2(int num_particles, const particle_t particles[],
		const unsigned *colortable)
{
	const particle_t *p;
	int i;
	vec3_t up, right;
	float scale;
	YQ2_ALIGNAS_TYPE(unsigned) byte color[4];

	YQ2_VLA(GLfloat, vtx, 3 * num_particles * 3);
	YQ2_VLA(GLfloat, tex, 2 * num_particles * 3);
	YQ2_VLA(GLubyte, clr, 4 * num_particles * 3);

	unsigned int index_vtx = 0;
	unsigned int index_tex = 0;
	unsigned int index_clr = 0;
	unsigned int j;

	R_Bind(r_particletexture->texnum);
	glDepthMask(GL_FALSE); /* no z buffering */
	glEnable(GL_BLEND);
	R_TexEnv(GL_MODULATE);

	VectorScale( vup, 1.5, up );
	VectorScale( vright, 1.5, right );

	for ( p = particles, i = 0; i < num_particles; i++, p++ )
	{
		/* hack a scale up to keep particles from disapearing */
		scale = ( p->origin [ 0 ] - r_origin [ 0 ] ) * vpn [ 0 ] +
			( p->origin [ 1 ] - r_origin [ 1 ] ) * vpn [ 1 ] +
			( p->origin [ 2 ] - r_origin [ 2 ] ) * vpn [ 2 ];

		if ( scale < 20 )
		{
			scale = 1;
		}
		else
		{
			scale = 1 + scale * 0.004;
		}

		*(unsigned *) color = colortable [ p->color ];

		for (j=0; j<3; j++) // Copy the color for each point
		{
			clr[index_clr++] = gammatable[color[0]];
			clr[index_clr++] = gammatable[color[1]];
			clr[index_clr++] = gammatable[color[2]];
			clr[index_clr++] = p->alpha * 255;
		}

		// point 0
		tex[index_tex++] = 0.0625f;
		tex[index_tex++] = 0.0625f;

		vtx[index_vtx++] = p->origin[0];
		vtx[index_vtx++] = p->origin[1];
		vtx[index_vtx++] = p->origin[2];

		// point 1
		tex[index_tex++] = 1.0625f;
		tex[index_tex++] = 0.0625f;

		vtx[index_vtx++] = p->origin [ 0 ] + up [ 0 ] * scale;
		vtx[index_vtx++] = p->origin [ 1 ] + up [ 1 ] * scale;
		vtx[index_vtx++] = p->origin [ 2 ] + up [ 2 ] * scale;

		// point 2
		tex[index_tex++] = 0.0625f;
		tex[index_tex++] = 1.0625f;

		vtx[index_vtx++] = p->origin [ 0 ] + right [ 0 ] * scale;
		vtx[index_vtx++] = p->origin [ 1 ] + right [ 1 ] * scale;
		vtx[index_vtx++] = p->origin [ 2 ] + right [ 2 ] * scale;
	}

	glEnableClientState( GL_VERTEX_ARRAY );
	glEnableClientState( GL_TEXTURE_COORD_ARRAY );
	glEnableClientState( GL_COLOR_ARRAY );

	glVertexPointer( 3, GL_FLOAT, 0, vtx );
	glTexCoordPointer( 2, GL_FLOAT, 0, tex );
	glColorPointer( 4, GL_UNSIGNED_BYTE, 0, clr );
	glDrawArrays( GL_TRIANGLES, 0, num_particles*3 );

	glDisableClientState( GL_VERTEX_ARRAY );
	glDisableClientState( GL_TEXTURE_COORD_ARRAY );
	glDisableClientState( GL_COLOR_ARRAY );

	glDisable(GL_BLEND);
	glColor4f(1, 1, 1, 1);
	glDepthMask(GL_TRUE); /* back to normal Z buffering */
	R_TexEnv(GL_REPLACE);

	YQ2_VLAFREE(vtx);
	YQ2_VLAFREE(tex);
	YQ2_VLAFREE(clr);
}

void
R_DrawParticles(void)
{
	qboolean stereo_split_tb = ((gl_state.stereo_mode == STEREO_SPLIT_VERTICAL) && gl_state.camera_separation);
	qboolean stereo_split_lr = ((gl_state.stereo_mode == STEREO_SPLIT_HORIZONTAL) && gl_state.camera_separation);

	if (r_newrefdef.num_particles <= 0) /* avoiding VLA with no size and vertexes built on it */
	{
		return;
	}

	if (gl_config.pointparameters && !(stereo_split_tb || stereo_split_lr))
	{
		int i;
		YQ2_ALIGNAS_TYPE(unsigned) byte color[4];
		const particle_t *p;

		YQ2_VLA(GLfloat, vtx, 3 * r_newrefdef.num_particles);
		YQ2_VLA(GLubyte, clr, 4 * r_newrefdef.num_particles);

		unsigned int index_vtx = 0;
		unsigned int index_clr = 0;

		glDepthMask(GL_FALSE);
		glEnable(GL_BLEND);
		glDisable(GL_TEXTURE_2D);

		// assume the particle size looks good with window height 480px and scale according to real resolution
		glPointSize(gl1_particle_size->value * (float)r_newrefdef.height/480.0f);

		for ( i = 0, p = r_newrefdef.particles; i < r_newrefdef.num_particles; i++, p++ )
		{
			*(int *) color = d_8to24table [ p->color & 0xFF ];
			clr[index_clr++] = gammatable[color[0]];
			clr[index_clr++] = gammatable[color[1]];
			clr[index_clr++] = gammatable[color[2]];
			clr[index_clr++] = p->alpha * 255;

			vtx[index_vtx++] = p->origin[0];
			vtx[index_vtx++] = p->origin[1];
			vtx[index_vtx++] = p->origin[2];
		}

		glEnableClientState( GL_VERTEX_ARRAY );
		glEnableClientState( GL_COLOR_ARRAY );

		glVertexPointer( 3, GL_FLOAT, 0, vtx );
		glColorPointer( 4, GL_UNSIGNED_BYTE, 0, clr );
		glDrawArrays( GL_POINTS, 0, r_newrefdef.num_particles );

		glDisableClientState( GL_VERTEX_ARRAY );
		glDisableClientState( GL_COLOR_ARRAY );

		glDisable(GL_BLEND);
		glColor4f( 1, 1, 1, 1 );
		glDepthMask(GL_TRUE);
		glEnable(GL_TEXTURE_2D);

		YQ2_VLAFREE(vtx);
		YQ2_VLAFREE(clr);
	}
	else
	{
		R_DrawParticles2(r_newrefdef.num_particles,
				r_newrefdef.particles, d_8to24table);
	}
}

void
R_PolyBlend(void)
{
	if (!gl_polyblend->value)
	{
		return;
	}

	if (!v_blend[3])
	{
		return;
	}

	glDisable(GL_ALPHA_TEST);
	glEnable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_TEXTURE_2D);

	glLoadIdentity();

	glRotatef(-90, 1, 0, 0); /* put Z going up */
	glRotatef(90, 0, 0, 1); /* put Z going up */

	glColor4f( v_blend[0], v_blend[1], v_blend[2], v_blend[3] );

	GLfloat vtx[] = {
		10, 100, 100,
		10, -100, 100,
		10, -100, -100,
		10, 100, -100
	};

	glEnableClientState( GL_VERTEX_ARRAY );

	glVertexPointer( 3, GL_FLOAT, 0, vtx );
	glDrawArrays( GL_TRIANGLE_FAN, 0, 4 );

	glDisableClientState( GL_VERTEX_ARRAY );

	glDisable(GL_BLEND);
	glEnable(GL_TEXTURE_2D);
	glEnable(GL_ALPHA_TEST);

	glColor4f(1, 1, 1, 1);
}

static void
R_ResetClearColor(void)
{
	if (gl1_discardfb->value == 1 && !r_clear->value)
	{
		glClearColor(0, 0, 0, 0.5);
	}
	else
	{
		glClearColor(1, 0, 0.5, 0.5);
	}
}

void
R_SetupFrame(void)
{
	int i;
	mleaf_t *leaf;

	r_framecount++;

	/* build the transformation matrix for the given view angles */
	VectorCopy(r_newrefdef.vieworg, r_origin);

	AngleVectors(r_newrefdef.viewangles, vpn, vright, vup);

	/* current viewcluster */
	if (!(r_newrefdef.rdflags & RDF_NOWORLDMODEL))
	{
		if (!r_worldmodel)
		{
			Com_Error(ERR_DROP, "%s: bad world model", __func__);
			return;
		}

		r_oldviewcluster = r_viewcluster;
		r_oldviewcluster2 = r_viewcluster2;
		leaf = Mod_PointInLeaf(r_origin, r_worldmodel->nodes);
		r_viewcluster = r_viewcluster2 = leaf->cluster;

		/* check above and below so crossing solid water doesn't draw wrong */
		if (!leaf->contents)
		{
			/* look down a bit */
			vec3_t temp;

			VectorCopy(r_origin, temp);
			temp[2] -= 16;
			leaf = Mod_PointInLeaf(temp, r_worldmodel->nodes);

			if (!(leaf->contents & CONTENTS_SOLID) &&
				(leaf->cluster != r_viewcluster2))
			{
				r_viewcluster2 = leaf->cluster;
			}
		}
		else
		{
			/* look up a bit */
			vec3_t temp;

			VectorCopy(r_origin, temp);
			temp[2] += 16;
			leaf = Mod_PointInLeaf(temp, r_worldmodel->nodes);

			if (!(leaf->contents & CONTENTS_SOLID) &&
				(leaf->cluster != r_viewcluster2))
			{
				r_viewcluster2 = leaf->cluster;
			}
		}
	}

	for (i = 0; i < 3; i++)
	{
		v_blend[i] = r_newrefdef.blend[i] * gl_state.sw_gamma;
	}
	v_blend[3] = r_newrefdef.blend[3];

	c_brush_polys = 0;
	c_alias_polys = 0;

	/* clear out the portion of the screen that the NOWORLDMODEL defines */
	if (r_newrefdef.rdflags & RDF_NOWORLDMODEL)
	{
		glEnable(GL_SCISSOR_TEST);
		glClearColor(0.3, 0.3, 0.3, 1);
		glScissor(r_newrefdef.x,
				vid.height - r_newrefdef.height - r_newrefdef.y,
				r_newrefdef.width, r_newrefdef.height);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		R_ResetClearColor();
		glDisable(GL_SCISSOR_TEST);
	}
}

void
R_SetPerspective(GLdouble fovy)
{
	// gluPerspective style parameters
	const GLdouble zNear = Q_max(gl_znear->value, 0.1f);
	const GLdouble zFar = (r_farsee->value) ? 8192.0f : 4096.0f;
	const GLdouble aspectratio = (GLdouble)r_newrefdef.width / r_newrefdef.height;

	GLdouble xmin, xmax, ymin, ymax;

	// traditional gluPerspective calculations - https://youtu.be/YqSNGcF5nvM?t=644
	ymax = zNear * tan(fovy * M_PI / 360.0);
	xmax = ymax * aspectratio;

	if ((r_newrefdef.rdflags & RDF_UNDERWATER) && gl1_waterwarp->value)
	{
		const GLdouble warp = sin(r_newrefdef.time * 1.5) * 0.03 * gl1_waterwarp->value;
		ymax *= 1.0 - warp;
		xmax *= 1.0 + warp;
	}

	ymin = -ymax;
	xmin = -xmax;

	if (gl_state.camera_separation)
	{
		const GLdouble separation = - gl1_stereo_convergence->value * (2 * gl_state.camera_separation) / zNear;
		xmin += separation;
		xmax += separation;
	}

	glFrustum(xmin, xmax, ymin, ymax, zNear, zFar);
}

void
R_SetupGL(void)
{
	int x, x2, y2, y, w, h;

	/* set up viewport */
	x = floor(r_newrefdef.x * vid.width / (float)vid.width);
	x2 = ceil((r_newrefdef.x + r_newrefdef.width) * vid.width / (float)vid.width);
	y = floor(vid.height - r_newrefdef.y * vid.height / (float)vid.height);
	y2 = ceil(vid.height -
			  (r_newrefdef.y + r_newrefdef.height) * vid.height / (float)vid.height);

	w = x2 - x;
	h = y - y2;

	qboolean drawing_left_eye = gl_state.camera_separation < 0;
	qboolean stereo_split_tb = ((gl_state.stereo_mode == STEREO_SPLIT_VERTICAL) && gl_state.camera_separation);
	qboolean stereo_split_lr = ((gl_state.stereo_mode == STEREO_SPLIT_HORIZONTAL) && gl_state.camera_separation);

	if(stereo_split_lr) {
		w = w / 2;
		x = drawing_left_eye ? (x / 2) : (x + vid.width) / 2;
	}

	if(stereo_split_tb) {
		h = h / 2;
		y2 = drawing_left_eye ? (y2 + vid.height) / 2 : (y2 / 2);
	}

	glViewport(x, y2, w, h);

	/* set up projection matrix */
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();

	R_SetPerspective(r_newrefdef.fov_y);

	glCullFace(GL_FRONT);

	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();

	glRotatef(-90, 1, 0, 0); /* put Z going up */
	glRotatef(90, 0, 0, 1); /* put Z going up */
	glRotatef(-r_newrefdef.viewangles[2], 1, 0, 0);
	glRotatef(-r_newrefdef.viewangles[0], 0, 1, 0);
	glRotatef(-r_newrefdef.viewangles[1], 0, 0, 1);
	glTranslatef(-r_newrefdef.vieworg[0], -r_newrefdef.vieworg[1],
			-r_newrefdef.vieworg[2]);

	glGetFloatv(GL_MODELVIEW_MATRIX, r_world_matrix);

	/* set drawing parms */
	if (r_cull->value)
	{
		glEnable(GL_CULL_FACE);
	}
	else
	{
		glDisable(GL_CULL_FACE);
	}

	glDisable(GL_BLEND);
	glDisable(GL_ALPHA_TEST);
	glEnable(GL_DEPTH_TEST);
}

void
R_Clear(void)
{
	// Define which buffers need clearing
	GLbitfield clearFlags = 0;
	GLenum depthFunc = GL_LEQUAL;

	// This breaks stereo modes, but we'll leave that responsibility to the user
	if (r_clear->value)
	{
		clearFlags |= GL_COLOR_BUFFER_BIT;
	}

	// No stencil shadows allowed when using certain stereo modes, otherwise "wallhack" happens
	if (gl_state.stereo_mode >= STEREO_MODE_ROW_INTERLEAVED && gl_state.stereo_mode <= STEREO_MODE_PIXEL_INTERLEAVED)
	{
		glClearStencil(0);
		clearFlags |= GL_STENCIL_BUFFER_BIT;
	}
	else if (gl_shadows->value && gl_state.stencil && gl1_stencilshadow->value)
	{
		glClearStencil(1);
		clearFlags |= GL_STENCIL_BUFFER_BIT;
	}

	if (gl1_ztrick->value)
	{
		static int trickframe;

		trickframe++;

		if (trickframe & 1)
		{
			gldepthmin = 0;
			gldepthmax = 0.49999;
		}
		else
		{
			gldepthmin = 1;
			gldepthmax = 0.5;
			depthFunc = GL_GEQUAL;
		}
	}
	else
	{
		clearFlags |= GL_DEPTH_BUFFER_BIT;

		gldepthmin = 0;
		gldepthmax = 1;
	}

	switch ((int)gl1_discardfb->value)
	{
		case 1:
			if (gl_state.stereo_mode == STEREO_MODE_NONE)
			{
				clearFlags |= GL_COLOR_BUFFER_BIT;
			}
			/* fall through */
		case 2:
			clearFlags |= GL_STENCIL_BUFFER_BIT;
			/* fall through */
		default:
			break;
	}

	if (clearFlags)
	{
		glClear(clearFlags);
	}
	glDepthFunc(depthFunc);
	glDepthRange(gldepthmin, gldepthmax);

	if (gl_zfix->value)
	{
		if (gldepthmax > gldepthmin)
		{
			glPolygonOffset(0.05, 1);
		}
		else
		{
			glPolygonOffset(-0.05, -1);
		}
	}
}

void
R_Flash(void)
{
	R_PolyBlend();
}

void
R_SetGL2D(void)
{
	int x, w, y, h;
	/* set 2D virtual screen size */
	qboolean drawing_left_eye = gl_state.camera_separation < 0;
	qboolean stereo_split_tb = ((gl_state.stereo_mode == STEREO_SPLIT_VERTICAL) && gl_state.camera_separation);
	qboolean stereo_split_lr = ((gl_state.stereo_mode == STEREO_SPLIT_HORIZONTAL) && gl_state.camera_separation);

	x = 0;
	w = vid.width;
	y = 0;
	h = vid.height;

	if(stereo_split_lr) {
		w =  w / 2;
		x = drawing_left_eye ? 0 : w;
	}

	if(stereo_split_tb) {
		h =  h / 2;
		y = drawing_left_eye ? h : 0;
	}

	glViewport(x, y, w, h);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0, vid.width, vid.height, 0, -99999, 99999);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_BLEND);
	glEnable(GL_ALPHA_TEST);
	glColor4f(1, 1, 1, 1);
}

/*
 * r_newrefdef must be set before the first call
 */
static void
R_RenderView(refdef_t *fd)
{
	if ((gl_state.stereo_mode != STEREO_MODE_NONE) && gl_state.camera_separation) {

		qboolean drawing_left_eye = gl_state.camera_separation < 0;
		switch (gl_state.stereo_mode) {
			case STEREO_MODE_ANAGLYPH:
				{

					// Work out the colour for each eye.
					int anaglyph_colours[] = { 0x4, 0x3 }; // Left = red, right = cyan.

					if (strlen(gl1_stereo_anaglyph_colors->string) == 2) {
						int eye, colour, missing_bits;
						// Decode the colour name from its character.
						for (eye = 0; eye < 2; ++eye) {
							colour = 0;
							switch (toupper((unsigned char)gl1_stereo_anaglyph_colors->string[eye])) {
								case 'B': ++colour; // 001 Blue
								case 'G': ++colour; // 010 Green
								case 'C': ++colour; // 011 Cyan
								case 'R': ++colour; // 100 Red
								case 'M': ++colour; // 101 Magenta
								case 'Y': ++colour; // 110 Yellow
									anaglyph_colours[eye] = colour;
									break;
							}
						}
						// Fill in any missing bits.
						missing_bits = ~(anaglyph_colours[0] | anaglyph_colours[1]) & 0x3;
						for (eye = 0; eye < 2; ++eye) {
							anaglyph_colours[eye] |= missing_bits;
						}
					}

					// Set the current colour.
					glColorMask(
						!!(anaglyph_colours[drawing_left_eye] & 0x4),
						!!(anaglyph_colours[drawing_left_eye] & 0x2),
						!!(anaglyph_colours[drawing_left_eye] & 0x1),
						GL_TRUE
					);
				}
				break;
			case STEREO_MODE_ROW_INTERLEAVED:
			case STEREO_MODE_COLUMN_INTERLEAVED:
			case STEREO_MODE_PIXEL_INTERLEAVED:
				{
					qboolean flip_eyes = true;
					int client_x, client_y;

					GLshort screen[] = {
						0, 0,
						(GLshort)vid.width, 0,
						(GLshort)vid.width, (GLshort)vid.height,
						0, (GLshort)vid.height
					};

					//GLimp_GetClientAreaOffset(&client_x, &client_y);
					client_x = 0;
					client_y = 0;

					R_SetGL2D();

					glEnable(GL_STENCIL_TEST);
					glStencilMask(1);
					glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

					glStencilOp(GL_REPLACE, GL_KEEP, GL_KEEP);
					glStencilFunc(GL_NEVER, 0, 1);

					glEnableClientState(GL_VERTEX_ARRAY);
					glVertexPointer(2, GL_SHORT, 0, screen);
					glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
					glDisableClientState(GL_VERTEX_ARRAY);

					glStencilOp(GL_INVERT, GL_KEEP, GL_KEEP);
					glStencilFunc(GL_NEVER, 1, 1);

					if (gl_state.stereo_mode == STEREO_MODE_ROW_INTERLEAVED || gl_state.stereo_mode == STEREO_MODE_PIXEL_INTERLEAVED)
					{
						for (int y = 0; y <= vid.height; y += 2)
						{
							gl_buf.vtx[gl_buf.vt    ] = 0;
							gl_buf.vtx[gl_buf.vt + 1] = y - 0.5f;
							gl_buf.vtx[gl_buf.vt + 2] = vid.width;
							gl_buf.vtx[gl_buf.vt + 3] = y - 0.5f;
							gl_buf.vt += 4;
						}
						flip_eyes ^= (client_y & 1);
					}

					if (gl_state.stereo_mode == STEREO_MODE_COLUMN_INTERLEAVED || gl_state.stereo_mode == STEREO_MODE_PIXEL_INTERLEAVED)
					{
						for (int x = 0; x <= vid.width; x += 2)
						{
							gl_buf.vtx[gl_buf.vt    ] = x - 0.5f;
							gl_buf.vtx[gl_buf.vt + 1] = 0;
							gl_buf.vtx[gl_buf.vt + 2] = x - 0.5f;
							gl_buf.vtx[gl_buf.vt + 3] = vid.height;
							gl_buf.vt += 4;
						}
						flip_eyes ^= (client_x & 1);
					}

					glEnableClientState(GL_VERTEX_ARRAY);
					glVertexPointer(2, GL_FLOAT, 0, gl_buf.vtx);
					glDrawArrays(GL_LINES, 0, gl_buf.vt / 2);
					glDisableClientState(GL_VERTEX_ARRAY);
					gl_buf.vt = 0;

					glStencilMask(0);
					glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

					glStencilFunc(GL_EQUAL, drawing_left_eye ^ flip_eyes, 1);
					glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
				}
				break;
			default:
				break;
		}
	}

	if (r_norefresh->value)
	{
		return;
	}

	r_newrefdef = *fd;

	if (!r_worldmodel && !(r_newrefdef.rdflags & RDF_NOWORLDMODEL))
	{
		Com_Error(ERR_DROP, "%s: NULL worldmodel", __func__);
	}

	if (r_speeds->value)
	{
		c_brush_polys = 0;
		c_alias_polys = 0;
#ifdef __EMSCRIPTEN__
		{
			extern int r_buf_flushes;
			r_buf_flushes = 0;
		}
#endif
	}

	R_PushDlights();

	if (gl_finish->value)
	{
		glFinish();
	}

	R_SetupFrame();

	R_SetFrustum(vup, vpn, vright, r_origin, r_newrefdef.fov_x, r_newrefdef.fov_y,
		frustum);

	R_SetupGL();

	R_MarkLeaves(); /* done here so we know if we're in water */

	R_OcclusionGridClear();

#ifdef __EMSCRIPTEN__
	R_SVBO_EnsureBuilt();
#endif

	R_DrawWorld();

	R_DrawEntitiesOnList();

	R_RenderDlights();

	R_DrawParticles();

	R_DrawAlphaSurfaces();

	R_Flash();

#ifdef __EMSCRIPTEN__
	/* r_polycount (declared above, stubbed to 0 purely so cl_screen.c's
	 * KAIOS_STATS diagnostic links against a gl1 build too) reflects
	 * this frame's real total here, unconditionally -- not gated on
	 * r_speeds like the console print below, so KAIOS_STATS shows a
	 * real number every second the same way it already does on the
	 * software renderer, whether or not r_speeds is ever turned on. */
	r_polycount = c_brush_polys + c_alias_polys;

	{
		extern int r_occl_nodes_tested, r_occl_nodes_culled;
		extern int r_occl_cells_marked, r_occl_surfs_projected, r_occl_surfs_skipped;
		extern int r_buf_flushes;
		extern int r_svbo_hits, r_svbo_misses;
		static int kaios_occl_frame;

		if ((kaios_occl_frame++ % 60) == 0)
		{
			Com_Printf("KAIOS_OCCL: active=%d nodes_tested=%d nodes_culled=%d "
				"cells_marked=%d surfs_projected=%d surfs_skipped=%d wpoly=%d "
				"buf_flushes=%d svbo=%d/%d(active=%d)\n",
				(int)r_occlusion_cull->value, r_occl_nodes_tested, r_occl_nodes_culled,
				r_occl_cells_marked, r_occl_surfs_projected, r_occl_surfs_skipped,
				c_brush_polys, r_buf_flushes, r_svbo_hits, r_svbo_hits + r_svbo_misses,
				(int)r_gl1_static_vbo->value);
		}

		r_svbo_hits = 0;
		r_svbo_misses = 0;
	}
#endif

	if (r_speeds->value)
	{
		Com_Printf("%4i wpoly %4i epoly %i tex %i lmaps\n",
				c_brush_polys, c_alias_polys, c_visible_textures,
				c_visible_lightmaps);
	}

	switch (gl_state.stereo_mode) {
		case STEREO_MODE_NONE:
			break;
		case STEREO_MODE_ANAGLYPH:
			glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
			break;
		case STEREO_MODE_ROW_INTERLEAVED:
		case STEREO_MODE_COLUMN_INTERLEAVED:
		case STEREO_MODE_PIXEL_INTERLEAVED:
			glDisable(GL_STENCIL_TEST);
			break;
		default:
			break;
	}
}

enum opengl_special_buffer_modes
GL_GetSpecialBufferModeForStereoMode(enum stereo_modes stereo_mode) {
	switch (stereo_mode) {
		case STEREO_MODE_NONE:
		case STEREO_SPLIT_HORIZONTAL:
		case STEREO_SPLIT_VERTICAL:
		case STEREO_MODE_ANAGLYPH:
			return OPENGL_SPECIAL_BUFFER_MODE_NONE;
		case STEREO_MODE_OPENGL:
			return OPENGL_SPECIAL_BUFFER_MODE_STEREO;
		case STEREO_MODE_ROW_INTERLEAVED:
		case STEREO_MODE_COLUMN_INTERLEAVED:
		case STEREO_MODE_PIXEL_INTERLEAVED:
			return OPENGL_SPECIAL_BUFFER_MODE_STENCIL;
	}
	return OPENGL_SPECIAL_BUFFER_MODE_NONE;
}

static void
R_SetLightLevel(entity_t *currententity)
{
	vec3_t shadelight;

	if (r_newrefdef.rdflags & RDF_NOWORLDMODEL)
	{
		return;
	}

	/* save off light value for server to look at */
	R_LightPoint(currententity, r_newrefdef.vieworg, shadelight);

	/* pick the greatest component, which should be the
	 * same as the mono value returned by software */
	if (shadelight[0] > shadelight[1])
	{
		if (shadelight[0] > shadelight[2])
		{
			r_lightlevel->value = 150 * shadelight[0];
		}
		else
		{
			r_lightlevel->value = 150 * shadelight[2];
		}
	}
	else
	{
		if (shadelight[1] > shadelight[2])
		{
			r_lightlevel->value = 150 * shadelight[1];
		}
		else
		{
			r_lightlevel->value = 150 * shadelight[2];
		}
	}
}

static void
RI_RenderFrame(refdef_t *fd)
{
	R_ApplyGLBuffer();	// menu rendering when needed
	R_RenderView(fd);
	R_SetLightLevel (NULL);
	R_SetGL2D();
}

#ifdef YQ2_GL1_GLES
#define GLES1_ENABLED_ONLY	"1"
#else
#define GLES1_ENABLED_ONLY	"0"
#endif

void
R_Register(void)
{
	gl_lefthand = ri.Cvar_Get("hand", "0", CVAR_USERINFO | CVAR_ARCHIVE);
	r_gunfov = ri.Cvar_Get("r_gunfov", "80", CVAR_ARCHIVE);
	r_farsee = ri.Cvar_Get("r_farsee", "0", CVAR_LATCH | CVAR_ARCHIVE);
	r_norefresh = ri.Cvar_Get("r_norefresh", "0", 0);
	/* Both CVAR_ARCHIVE and defaulted differently on this platform so the
	 * new KaiOS Tuning toggles below persist across launches like every
	 * other tuning option there, instead of resetting to upstream's
	 * defaults every boot. r_fullbright defaults ON here specifically:
	 * disabling the multitexture lightmap pass is expected to be a real
	 * win on this device's slow WebGL emulation layer, at the cost of
	 * flat (unlit) world geometry -- toggle it back off from the menu
	 * to compare. r_speeds defaults ON too, purely so its per-frame
	 * wpoly/epoly/tex/lmaps console print is there without having to
	 * type the command in by hand on this platform's keypad. */
#ifdef __EMSCRIPTEN__
	r_fullbright = ri.Cvar_Get("r_fullbright", "0", CVAR_ARCHIVE);
#else
	r_fullbright = ri.Cvar_Get("r_fullbright", "0", 0);
#endif
	r_drawentities = ri.Cvar_Get("r_drawentities", "1", 0);
	r_drawworld = ri.Cvar_Get("r_drawworld", "1", 0);
	r_novis = ri.Cvar_Get("r_novis", "0", 0);
	r_lerpmodels = ri.Cvar_Get("r_lerpmodels", "1", 0);
#ifdef __EMSCRIPTEN__
	r_speeds = ri.Cvar_Get("r_speeds", "1", CVAR_ARCHIVE);
#else
	r_speeds = ri.Cvar_Get("r_speeds", "0", 0);
#endif

	r_lightlevel = ri.Cvar_Get("r_lightlevel", "0", 0);
	gl1_overbrightbits = ri.Cvar_Get("gl1_overbrightbits", "0", CVAR_ARCHIVE);

	gl1_particle_min_size = ri.Cvar_Get("gl1_particle_min_size", "2", CVAR_ARCHIVE);
	gl1_particle_max_size = ri.Cvar_Get("gl1_particle_max_size", "40", CVAR_ARCHIVE);
	gl1_particle_size = ri.Cvar_Get("gl1_particle_size", "40", CVAR_ARCHIVE);
	gl1_particle_att_a = ri.Cvar_Get("gl1_particle_att_a", "0.01", CVAR_ARCHIVE);
	gl1_particle_att_b = ri.Cvar_Get("gl1_particle_att_b", "0.0", CVAR_ARCHIVE);
	gl1_particle_att_c = ri.Cvar_Get("gl1_particle_att_c", "0.01", CVAR_ARCHIVE);
	gl1_particle_square = ri.Cvar_Get("gl1_particle_square", "0", CVAR_ARCHIVE);

	r_modulate = ri.Cvar_Get("r_modulate", "1", CVAR_ARCHIVE);
	r_mode = ri.Cvar_Get("r_mode", "4", CVAR_ARCHIVE);
	gl_lightmap = ri.Cvar_Get("r_lightmap", "0", 0);
	gl_shadows = ri.Cvar_Get("r_shadows", "0", CVAR_ARCHIVE);
	gl1_stencilshadow = ri.Cvar_Get("gl1_stencilshadow", "0", CVAR_ARCHIVE);
	/* Dynamic lights (explosions, muzzle flashes, weapon glow) make
	 * gl1_dynamic recompute AND re-upload (glTexSubImage2D) the
	 * lightmap texture data for every affected surface, every frame --
	 * a real per-surface CPU recompute plus GL upload that's entirely
	 * separate from triangle count or draw-call batching (occlusion
	 * culling, distcull, and the bigger lightmap atlas above all leave
	 * this untouched). Off by default on this platform: real-device
	 * testing after those other fixes still showed drops correlating
	 * with the effects count in combat. CVAR_ARCHIVE and exposed as a
	 * KaiOS Tuning toggle (menu.c) since it's a genuine visual
	 * trade-off (nearby walls stop flaring from explosions/muzzle
	 * flashes), not a free win.
	 */
#ifdef __EMSCRIPTEN__
	gl1_dynamic = ri.Cvar_Get("gl1_dynamic", "0", CVAR_ARCHIVE);
#else
	gl1_dynamic = ri.Cvar_Get("gl1_dynamic", "1", 0);
#endif
	gl_nobind = ri.Cvar_Get("gl_nobind", "0", 0);
	gl1_round_down = ri.Cvar_Get("gl1_round_down", "1", 0);
	gl1_picmip = ri.Cvar_Get("gl1_picmip", "0", 0);
	gl_showtris = ri.Cvar_Get("gl_showtris", "0", 0);
	gl_showbbox = ri.Cvar_Get("gl_showbbox", "0", 0);
	gl1_ztrick = ri.Cvar_Get("gl1_ztrick", "0", 0);
	gl_zfix = ri.Cvar_Get("gl_zfix", "0", 0);
	gl_finish = ri.Cvar_Get("gl_finish", "0", CVAR_ARCHIVE);
	r_clear = ri.Cvar_Get("r_clear", "0", 0);
	r_cull = ri.Cvar_Get("r_cull", "1", 0);

	/* Same cheap draw-distance cutoff as the software renderer
	 * (r_distcull_dist, see sw_main.c's registration comment for the
	 * fuller rationale and on-device tuning history) -- this renderer
	 * has no equivalent at all by default, so on the same map it walks
	 * and draws every PVS-visible surface regardless of distance,
	 * which is the real-device-confirmed reason gl1's polycount reads
	 * much higher than soft's on identical maps. Sharing the exact
	 * same cvar name means the existing KaiOS Tuning menu slider
	 * (menu.c, s_kaios_distcull_slider) works for this renderer too
	 * with no menu changes needed. */
#ifdef __EMSCRIPTEN__
	r_distcull_dist = ri.Cvar_Get("r_distcull_dist", "1200", CVAR_ARCHIVE);
#else
	r_distcull_dist = ri.Cvar_Get("r_distcull_dist", "0", CVAR_ARCHIVE);
#endif

	/* Coarse CPU-side screen-space occlusion cull (gl1_surf.c): this
	 * renderer has no GPU occlusion query extension available on this
	 * device's WebGL1 context and, unlike the software renderer, no
	 * built-in scanline occlusion either, so on the same map it submits
	 * far more geometry per frame than soft does -- confirmed on a real
	 * device (6x+ the poly count at the same viewpoint, with FPS
	 * sometimes worse than soft as a direct result on this slow WebGL
	 * emulation layer, where every extra draw carries real overhead).
	 * Marks a small grid of already-covered screen cells from opaque
	 * surfaces as the world BSP is walked front-to-back, and skips
	 * whole nodes once their projected screen rect is provably behind
	 * already-drawn geometry.
	 *
	 * A first version marked coverage by each surface's axis-aligned
	 * screen bounding box, which is unsafe for diagonal/angled walls
	 * (their bbox covers real screen area outside the wall itself) and
	 * caused confirmed holes on a real device. Fixed in gl1_surf.c by
	 * rasterizing each surface's actual convex polygon into the grid
	 * instead -- a cell is only marked covered if all 4 of its corners
	 * land inside the polygon, which is exact (not approximate) since
	 * BSP faces are always convex. Still conservative on the projection
	 * side: anything that can't be safely projected (behind the near
	 * plane) is skipped rather than guessed at, so a wrong result can
	 * only cost performance, never drop visible geometry.
	 *
	 * That fix flipped the default back to "1" at the time, but its own
	 * commit message says so itself: "still not verified on real
	 * hardware yet" -- and gl1 as a whole went unbuilt/unshipped after
	 * that (see build.sh's "gl1 shelved for now"), so this never
	 * actually got the real-device confirmation it was waiting on.
	 * Defaulting back to "0" now that gl1 is being wired back in
	 * (RENDERER=unified, build.sh) until it gets that confirmation for
	 * real -- CVAR_ARCHIVE and console/menu toggleable either way. */
	r_occlusion_cull = ri.Cvar_Get("r_occlusion_cull", "0", CVAR_ARCHIVE);

#ifdef __EMSCRIPTEN__
	/* Static world-geometry vertex cache -- see the block comment above
	 * R_SVBO_EnsureBuilt (gl1_surf.c) for the full rationale. Uploads
	 * eligible world surfaces' vertex/texcoord data to a real GPU
	 * buffer once instead of every frame. Real-device testing found
	 * two rounds of regressions in this path (wrong lightmaps, then a
	 * build where world geometry stopped rendering entirely once the
	 * lightmap-flush fix made it flush far more often) -- defaulting
	 * to off until root-caused. CVAR_ARCHIVE so it can still be
	 * opted into from the console for further debugging. */
	r_gl1_static_vbo = ri.Cvar_Get("r_gl1_static_vbo", "0", CVAR_ARCHIVE);
#endif

	gl_polyblend = ri.Cvar_Get("gl_polyblend", "1", 0);
	gl1_flashblend = ri.Cvar_Get("gl1_flashblend", "0", 0);
	r_fixsurfsky = ri.Cvar_Get("r_fixsurfsky", "0", CVAR_ARCHIVE);
	gl_znear = ri.Cvar_Get("gl_znear", "4", CVAR_ARCHIVE);
	gl1_minlight = ri.Cvar_Get("gl1_minlight", "0", CVAR_ARCHIVE);

	gl_texturemode = ri.Cvar_Get("gl_texturemode", "GL_LINEAR_MIPMAP_NEAREST", CVAR_ARCHIVE);
	gl1_texturealphamode = ri.Cvar_Get("gl1_texturealphamode", "default", CVAR_ARCHIVE);
	gl1_texturesolidmode = ri.Cvar_Get("gl1_texturesolidmode", "default", CVAR_ARCHIVE);
	gl_anisotropic = ri.Cvar_Get("r_anisotropic", "0", CVAR_ARCHIVE);
	r_lockpvs = ri.Cvar_Get("r_lockpvs", "0", 0);

	gl1_palettedtexture = ri.Cvar_Get("r_palettedtextures", "0", CVAR_ARCHIVE);
	gl1_pointparameters = ri.Cvar_Get("gl1_pointparameters", "1", CVAR_ARCHIVE);
	gl1_multitexture = ri.Cvar_Get("gl1_multitexture", "1", CVAR_ARCHIVE);
	gl1_lightmapcopies = ri.Cvar_Get("gl1_lightmapcopies", GLES1_ENABLED_ONLY, CVAR_ARCHIVE);
	gl1_discardfb = ri.Cvar_Get("gl1_discardfb", GLES1_ENABLED_ONLY, CVAR_ARCHIVE);

	gl_drawbuffer = ri.Cvar_Get("gl_drawbuffer", "GL_BACK", 0);
	r_vsync = ri.Cvar_Get("r_vsync", "1", CVAR_ARCHIVE);

	gl1_saturatelighting = ri.Cvar_Get("gl1_saturatelighting", "0", 0);

	vid_fullscreen = ri.Cvar_Get("vid_fullscreen", "0", CVAR_ARCHIVE);
	vid_gamma = ri.Cvar_Get("vid_gamma", "1.2", CVAR_ARCHIVE);

	r_customwidth = ri.Cvar_Get("r_customwidth", "1024", CVAR_ARCHIVE);
	r_customheight = ri.Cvar_Get("r_customheight", "768", CVAR_ARCHIVE);
	gl_msaa_samples = ri.Cvar_Get ( "r_msaa_samples", "0", CVAR_ARCHIVE );

	r_retexturing = ri.Cvar_Get("r_retexturing", "1", CVAR_ARCHIVE);
	r_validation = ri.Cvar_Get("r_validation", "0", CVAR_ARCHIVE);
	r_scale8bittextures = ri.Cvar_Get("r_scale8bittextures", "0", CVAR_ARCHIVE);

	/* don't bilerp characters and crosshairs */
	gl_nolerp_list = ri.Cvar_Get("r_nolerp_list", "pics/conchars.pcx pics/ch1.pcx pics/ch2.pcx pics/ch3.pcx", CVAR_ARCHIVE);
	/* textures that should always be filtered, even if r_2D_unfiltered or an unfiltered gl mode is used */
	r_lerp_list = ri.Cvar_Get("r_lerp_list", "", CVAR_ARCHIVE);
	/* don't bilerp any 2D elements */
	r_2D_unfiltered = ri.Cvar_Get("r_2D_unfiltered", "0", CVAR_ARCHIVE);
	/* don't bilerp videos */
	r_videos_unfiltered = ri.Cvar_Get("r_videos_unfiltered", "0", CVAR_ARCHIVE);

	gl1_stereo = ri.Cvar_Get( "gl1_stereo", "0", CVAR_ARCHIVE );
	gl1_stereo_separation = ri.Cvar_Get( "gl1_stereo_separation", "-0.4", CVAR_ARCHIVE );
	gl1_stereo_anaglyph_colors = ri.Cvar_Get( "gl1_stereo_anaglyph_colors", "rc", CVAR_ARCHIVE );
	gl1_stereo_convergence = ri.Cvar_Get( "gl1_stereo_convergence", "1", CVAR_ARCHIVE );

	gl1_waterwarp = ri.Cvar_Get( "gl1_waterwarp", "1.0", CVAR_ARCHIVE );

	ri.Cmd_AddCommand("imagelist", R_ImageList_f);
	ri.Cmd_AddCommand("screenshot", R_ScreenShot);
	ri.Cmd_AddCommand("modellist", Mod_Modellist_f);
	ri.Cmd_AddCommand("gl_strings", R_Strings);
}

#undef GLES1_ENABLED_ONLY

/*
 * Changes the video mode
 */
static int
SetMode_impl(int *pwidth, int *pheight, int mode, int fullscreen)
{
	Com_Printf("Setting mode %d:", mode);

	/* mode -1 is not in the vid mode table - so we keep the values in pwidth
	   and pheight and don't even try to look up the mode info */
	if ((mode >= 0) && !ri.Vid_GetModeInfo(pwidth, pheight, mode))
	{
		Com_Printf(" invalid mode\n");
		return rserr_invalid_mode;
	}

	/* We trying to get resolution from desktop */
	if (mode == -2)
	{
		if(!ri.GLimp_GetDesktopMode(pwidth, pheight))
		{
			Com_Printf(" can't detect mode\n" );
			return rserr_invalid_mode;
		}
	}

	Com_Printf(" %dx%d (vid_fullscreen %i)\n", *pwidth, *pheight, fullscreen);

	if (!ri.GLimp_InitGraphics(fullscreen, pwidth, pheight))
	{
		return rserr_invalid_mode;
	}

	/* This is totaly obscure: For some strange reasons the renderer
	   maintains two(!) repesentations of the resolution. One comes
	   from the client and is saved in r_newrefdef. The other one
	   is determined here and saved in vid. Several calculations take
	   both representations into account.

	   The values will always be the same. The GLimp_InitGraphics()
	   call above communicates the requested resolution to the client
	   where it ends up in the vid subsystem and the vid system writes
	   it into r_newrefdef.

	   We can't avoid the client roundtrip, because we can get the
	   real size of the drawable (which can differ from the resolution
	   due to high dpi awareness) only after the render context was
	   created by GLimp_InitGraphics() and need to communicate it
	   somehow to the client. So we just overwrite the values saved
	   in vid with a call to RI_GetDrawableSize(), just like the
	   client does. This makes sure that both values are the same
	   and everything is okay.

	   We also need to take the special case fullscreen window into
	   account. With the fullscreen windows we cannot use the
	   drawable size, it would scale all cases to the size of the
	   window. Instead use the drawable size when the user wants
	   native resolution (the fullscreen window fills the screen)
	   and use the requested resolution in all other cases. */
	if (IsHighDPIaware)
	{
		if (vid_fullscreen->value != 2)
		{
			RI_GetDrawableSize(pwidth, pheight);
		}
		else
		{
			if (r_mode->value == -2)
			{
				/* User requested native resolution. */
				RI_GetDrawableSize(pwidth, pheight);
			}
		}
	}

	return rserr_ok;
}

qboolean
R_SetMode(void)
{
	rserr_t err;
	int fullscreen;

	fullscreen = (int)vid_fullscreen->value;

	/* a bit hackish approach to enable custom resolutions:
	   Glimp_SetMode needs these values set for mode -1 */
	vid.width = r_customwidth->value;
	vid.height = r_customheight->value;

	if ((err = SetMode_impl(&vid.width, &vid.height, r_mode->value,
					 fullscreen)) == rserr_ok)
	{
		if (r_mode->value == -1)
		{
			gl_state.prev_mode = 4; /* safe default for custom mode */
		}
		else
		{
			gl_state.prev_mode = r_mode->value;
		}
	}
	else
	{
		if (err == rserr_invalid_mode)
		{
			Com_Printf("ref_gl::R_SetMode() - invalid mode\n");
			if (gl_msaa_samples->value != 0.0f)
			{
				Com_Printf("gl_msaa_samples was %d - will try again with gl_msaa_samples = 0\n", (int)gl_msaa_samples->value);
				ri.Cvar_SetValue("r_msaa_samples", 0.0f);
				gl_msaa_samples->modified = false;

				if ((err = SetMode_impl(&vid.width, &vid.height, r_mode->value, 0)) == rserr_ok)
				{
					return true;
				}
			}
			if(r_mode->value == gl_state.prev_mode)
			{
				// trying again would result in a crash anyway, give up already
				// (this would happen if your initing fails at all and your resolution already was 640x480)
				return false;
			}
			ri.Cvar_SetValue("r_mode", gl_state.prev_mode);
			r_mode->modified = false;
		}

		/* try setting it back to something safe */
		if ((err = SetMode_impl(&vid.width, &vid.height, gl_state.prev_mode, 0)) != rserr_ok)
		{
			Com_Printf("ref_gl::R_SetMode() - could not revert to safe mode\n");
			return false;
		}
	}

	return true;
}

// just to avoid too many preprocessor directives in RI_Init()
typedef enum
{
	rf_opengl14,
	rf_opengles10
} refresher_t;

qboolean
RI_Init(void)
{
	int j;
	byte *colormap;
	extern float r_turbsin[256];

#ifdef YQ2_GL1_GLES
#define GLEXTENSION_NPOT	"GL_OES_texture_npot"
	static const refresher_t refresher = rf_opengles10;
#else
#define GLEXTENSION_NPOT	"GL_ARB_texture_non_power_of_two"
	static const refresher_t refresher = rf_opengl14;
#endif

	Swap_Init();

	for (j = 0; j < 256; j++)
	{
		r_turbsin[j] *= 0.5;
	}

	Com_Printf("Refresh: " REF_VERSION "\n");
	Com_Printf("Client: " YQ2VERSION "\n\n");

#ifdef DEBUG
	Com_Printf("ref_gl1::%s - DEBUG mode enabled\n", __func__);
#endif

	GetPCXPalette (&colormap, d_8to24table);
	free(colormap);

	R_Register();

	/* initialize our QGL dynamic bindings */
	QGL_Init();

	/* set our "safe" mode */
	gl_state.prev_mode = 4;
	gl_state.stereo_mode = gl1_stereo->value;

	/* create the window and set up the context */
	if (!R_SetMode())
	{
		QGL_Shutdown();
		Com_Printf("ref_gl1::%s - could not R_SetMode()\n", __func__);
		return false;
	}

	ri.Vid_MenuInit();

	// --------

	/* get our various GL strings */
	Com_Printf("\nOpenGL setting:\n");

	gl_config.vendor_string = (char *)glGetString(GL_VENDOR);
	Com_Printf("GL_VENDOR: %s\n", gl_config.vendor_string);

	gl_config.renderer_string = (char *)glGetString(GL_RENDERER);
	Com_Printf("GL_RENDERER: %s\n", gl_config.renderer_string);

	gl_config.version_string = (char *)glGetString(GL_VERSION);
	Com_Printf("GL_VERSION: %s\n", gl_config.version_string);

	gl_config.extensions_string = (char *)glGetString(GL_EXTENSIONS);
	Com_Printf("GL_EXTENSIONS: %s\n", gl_config.extensions_string);

	sscanf(gl_config.version_string, "%d.%d", &gl_config.major_version, &gl_config.minor_version);

	if (refresher == rf_opengl14 && gl_config.major_version == 1)
	{
		if (gl_config.minor_version < 4)
		{
			QGL_Shutdown();
			Com_Printf("Support for OpenGL 1.4 is not available\n");

			return false;
		}
	}

	Com_Printf("\n\nProbing for OpenGL extensions:\n");

	// ----

	/* Point parameters */
	Com_Printf(" - Point parameters: ");

	if ( refresher == rf_opengles10 ||
		strstr(gl_config.extensions_string, "GL_ARB_point_parameters") ||
		strstr(gl_config.extensions_string, "GL_EXT_point_parameters") )	// should exist for all OGL 1.4 hw...
	{
		qglPointParameterf = (void (APIENTRY *)(GLenum, GLfloat))RI_GetProcAddress ( "glPointParameterf" );
		qglPointParameterfv = (void (APIENTRY *)(GLenum, const GLfloat *))RI_GetProcAddress ( "glPointParameterfv" );

		if (!qglPointParameterf || !qglPointParameterfv)
		{
			qglPointParameterf = (void (APIENTRY *)(GLenum, GLfloat))RI_GetProcAddress ( "glPointParameterfARB" );
			qglPointParameterfv = (void (APIENTRY *)(GLenum, const GLfloat *))RI_GetProcAddress ( "glPointParameterfvARB" );
		}
		if (!qglPointParameterf || !qglPointParameterfv)
		{
			qglPointParameterf = (void (APIENTRY *)(GLenum, GLfloat))RI_GetProcAddress ( "glPointParameterfEXT" );
			qglPointParameterfv = (void (APIENTRY *)(GLenum, const GLfloat *))RI_GetProcAddress ( "glPointParameterfvEXT" );
		}
	}

	gl_config.pointparameters = false;

	if (gl1_pointparameters->value)
	{
		if (qglPointParameterf && qglPointParameterfv)
		{
			gl_config.pointparameters = true;
			Com_Printf("Okay\n");
		}
		else
		{
			Com_Printf("Failed\n");
		}
	}
	else
	{
		Com_Printf("Disabled\n");
	}

	// ----

	/* Paletted texture */
	Com_Printf(" - Paletted texture: ");

	if (strstr(gl_config.extensions_string, "GL_EXT_paletted_texture") &&
		strstr(gl_config.extensions_string, "GL_EXT_shared_texture_palette"))
	{
			qglColorTableEXT = (void (APIENTRY *)(GLenum, GLenum, GLsizei, GLenum, GLenum, const GLvoid * ))
					RI_GetProcAddress ("glColorTableEXT");
	}

	gl_config.palettedtexture = false;

	if (gl1_palettedtexture->value)
	{
		if (qglColorTableEXT)
		{
			gl_config.palettedtexture = true;
			Com_Printf("Okay\n");
		}
		else
		{
			Com_Printf("Failed\n");
		}
	}
	else
	{
		Com_Printf("Disabled\n");
	}

	// --------

	/* Anisotropic */
	Com_Printf(" - Anisotropic: ");

	if (strstr(gl_config.extensions_string, "GL_EXT_texture_filter_anisotropic"))
	{
		gl_config.anisotropic = true;
		glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &gl_config.max_anisotropy);

		Com_Printf("%ux\n", (int)gl_config.max_anisotropy);
	}
	else
	{
		gl_config.anisotropic = false;
		gl_config.max_anisotropy = 0.0;

		Com_Printf("Failed\n");
	}

	// ----

	/* Non power of two textures */
	Com_Printf(" - Non power of two textures: ");

	if (strstr(gl_config.extensions_string, GLEXTENSION_NPOT))
	{
		gl_config.npottextures = true;
		Com_Printf("Okay\n");
	}
	else
	{
		gl_config.npottextures = false;
		Com_Printf("Failed\n");
	}

#undef GLEXTENSION_NPOT

	// ----

	/* Multitexturing */
	gl_config.multitexture = false;

	Com_Printf(" - Multitexturing: ");

	if ( refresher == rf_opengles10 || strstr(gl_config.extensions_string, "GL_ARB_multitexture") )
	{
		qglActiveTexture = (void (APIENTRY *)(GLenum))RI_GetProcAddress ("glActiveTexture");
		qglClientActiveTexture = (void (APIENTRY *)(GLenum))RI_GetProcAddress ("glClientActiveTexture");

		if (!qglActiveTexture || !qglClientActiveTexture)
		{
			qglActiveTexture = (void (APIENTRY *)(GLenum))RI_GetProcAddress ("glActiveTextureARB");
			qglClientActiveTexture = (void (APIENTRY *)(GLenum))RI_GetProcAddress ("glClientActiveTextureARB");
		}
	}

	if (gl1_multitexture->value)
	{
		if (qglActiveTexture && qglClientActiveTexture)
		{
			gl_config.multitexture = true;
			Com_Printf("Okay\n");
		}
		else
		{
			Com_Printf("Failed\n");
		}
	}
	else
	{
		Com_Printf("Disabled\n");
	}

	// ----

	/* Lightmap copies: keep multiple copies of "the same" lightmap on video memory.
	 * All of them are actually different, because they are affected by different dynamic lighting,
	 * in different frames. This is not meant for Immediate-Mode Rendering systems (desktop),
	 * but for Tile-Based / Deferred Rendering ones (embedded / mobile), since active manipulation
	 * of textures already being used in the last few frames can cause slowdown on these systems.
	 * Needless to say, GPU memory usage is highly increased, so watch out in low memory situations.
	 */

	Com_Printf(" - Lightmap copies: ");
	gl_config.lightmapcopies = false;
	if (gl_config.multitexture && gl1_lightmapcopies->value)
	{
		gl_config.lightmapcopies = true;
		Com_Printf("Okay\n");
	}
	else
	{
		Com_Printf("Disabled\n");
	}

	// ----

	/* Discard framebuffer: Enables the use of a "performance hint" to the graphic
	 * driver in GLES1, to get rid of the contents of the different framebuffers.
	 * Useful for some GPUs that may attempt to keep them and/or write them back to
	 * external/uniform memory, actions that are useless for Quake 2 rendering path.
	 * https://registry.khronos.org/OpenGL/extensions/EXT/EXT_discard_framebuffer.txt
	 * This extension is used by 'gl1_discardfb', and regardless of its existence,
	 * that cvar will enable glClear at the start of each frame, helping mobile GPUs.
	 */

#ifdef YQ2_GL1_GLES
	Com_Printf(" - Discard framebuffer: ");

	if (strstr(gl_config.extensions_string, "GL_EXT_discard_framebuffer"))
	{
		qglDiscardFramebufferEXT = (void (APIENTRY *)(GLenum, GLsizei, const GLenum *))
				RI_GetProcAddress ("glDiscardFramebufferEXT");
	}

	if (gl1_discardfb->value)
	{
		if (qglDiscardFramebufferEXT)	// enough to verify availability
		{
			Com_Printf("Okay\n");
		}
		else
		{
			Com_Printf("Failed\n");
		}
	}
	else
	{
		Com_Printf("Disabled\n");
	}
#endif

	// ----

#ifdef __EMSCRIPTEN__
	/* Chasing a real-device crash ("TypeError: ... is not a function",
	 * an invalid function-pointer-table call) somewhere in this
	 * sequence -- one of these is calling through a GLAD-loaded
	 * function pointer that Emscripten's LEGACY_GL_EMULATION never
	 * actually backed with a real implementation on this browser (it
	 * warns outright on startup that it's "a collection of limited
	 * workarounds, do not expect it to work"). Bracket each step so
	 * the next real-device log pinpoints exactly which one it is. */
#define KAIOS_GL1_STEP(x) do { Com_Printf("KAIOS_GL1_INIT: before " #x "\n"); x; Com_Printf("KAIOS_GL1_INIT: after " #x "\n"); } while (0)
#else
#define KAIOS_GL1_STEP(x) x
#endif

	KAIOS_GL1_STEP(R_ResetClearColor());
	KAIOS_GL1_STEP(R_SetDefaultState());

	KAIOS_GL1_STEP(Scrap_Init());
	KAIOS_GL1_STEP(R_InitImages());
	KAIOS_GL1_STEP(Mod_Init());
	KAIOS_GL1_STEP(R_InitParticleTexture());
	KAIOS_GL1_STEP(Draw_InitLocal());
	KAIOS_GL1_STEP(R_ResetGLBuffer());

#undef KAIOS_GL1_STEP

	return true;
}

void
RI_Shutdown(void)
{
	ri.Cmd_RemoveCommand("modellist");
	ri.Cmd_RemoveCommand("screenshot");
	ri.Cmd_RemoveCommand("imagelist");
	ri.Cmd_RemoveCommand("gl_strings");

	LM_FreeLightmapBuffers();
	Mod_FreeAll();

	R_ShutdownImages();

	/* shutdown OS specific OpenGL stuff like contexts, etc.  */
	RI_ShutdownContext();

	/* shutdown our QGL subsystem */
	QGL_Shutdown();
}

void
RI_BeginFrame(float camera_separation)
{
	gl_state.camera_separation = camera_separation;

	// force a vid_restart if gl1_stereo has been modified.
	if ( gl_state.stereo_mode != gl1_stereo->value )
	{
		// If we've gone from one mode to another with the same special buffer requirements there's no need to restart.
		if ( GL_GetSpecialBufferModeForStereoMode( gl_state.stereo_mode ) == GL_GetSpecialBufferModeForStereoMode( gl1_stereo->value ) )
		{
			gl_state.stereo_mode = gl1_stereo->value;
		}
		else
		{
			Com_Printf("stereo supermode changed, restarting video!\n");
			ri.Cmd_ExecuteText(EXEC_APPEND, "vid_restart\n");
		}
	}

	if (vid_gamma->modified)
	{
		vid_gamma->modified = false;
		RI_UpdateGamma();
	}

	// Clamp overbrightbits
	if (gl1_overbrightbits->modified)
	{
		int obb_val = (int)gl1_overbrightbits->value;

		obb_val = Q_clamp(obb_val, 0, 4);
		if (obb_val == 3)	// allowed values: 0,1,2,4
		{
			obb_val = 2;
		}

		ri.Cvar_SetValue("gl1_overbrightbits", obb_val);
		gl1_overbrightbits->modified = false;
	}

	/* go into 2D mode */

	// FIXME: just call R_SetGL2D();

	int x, w, y, h;
	qboolean drawing_left_eye = gl_state.camera_separation < 0;
	qboolean stereo_split_tb = ((gl_state.stereo_mode == STEREO_SPLIT_VERTICAL) && gl_state.camera_separation);
	qboolean stereo_split_lr = ((gl_state.stereo_mode == STEREO_SPLIT_HORIZONTAL) && gl_state.camera_separation);

	x = 0;
	w = vid.width;
	y = 0;
	h = vid.height;

	if(stereo_split_lr) {
		w =  w / 2;
		x = drawing_left_eye ? 0 : w;
	}

	if(stereo_split_tb) {
		h =  h / 2;
		y = drawing_left_eye ? h : 0;
	}

	glViewport(x, y, w, h);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0, vid.width, vid.height, 0, -99999, 99999);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_BLEND);
	glEnable(GL_ALPHA_TEST);
	glColor4f(1, 1, 1, 1);

	if (gl1_particle_square->modified)
	{
		if (gl_config.pointparameters)
		{
			/* GL_POINT_SMOOTH is not implemented by some OpenGL
			   drivers, especially the crappy Mesa3D backends like
			   i915.so. That the points are squares and not circles
			   is not a problem by Quake II! */
			if (gl1_particle_square->value)
			{
				glDisable(GL_POINT_SMOOTH);
			}
			else
			{
				glEnable(GL_POINT_SMOOTH);
			}
		}
		else
		{
			// particles aren't drawn as GL_POINTS, but as textured triangles
			// => update particle texture to look square - or circle-ish
			R_InitParticleTexture();
		}

		gl1_particle_square->modified = false;
	}

	/* draw buffer stuff */
	if (gl_drawbuffer->modified)
	{
		gl_drawbuffer->modified = false;

#ifndef YQ2_GL1_GLES
		if ((gl_state.camera_separation == 0) || gl_state.stereo_mode != STEREO_MODE_OPENGL)
		{
			if (Q_stricmp(gl_drawbuffer->string, "GL_FRONT") == 0)
			{
				glDrawBuffer(GL_FRONT);
			}
			else
			{
				glDrawBuffer(GL_BACK);
			}
		}
#endif
	}

	/* texturemode stuff */
	if (gl_texturemode->modified || (gl_config.anisotropic && gl_anisotropic->modified)
	    || gl_nolerp_list->modified || r_lerp_list->modified
		|| r_2D_unfiltered->modified || r_videos_unfiltered->modified)
	{
		R_TextureMode(gl_texturemode->string);
		gl_texturemode->modified = false;
		gl_anisotropic->modified = false;
		gl_nolerp_list->modified = false;
		r_lerp_list->modified = false;
		r_2D_unfiltered->modified = false;
		r_videos_unfiltered->modified = false;
	}

	if (gl1_texturealphamode->modified)
	{
		R_TextureAlphaMode(gl1_texturealphamode->string);
		gl1_texturealphamode->modified = false;
	}

	if (gl1_texturesolidmode->modified)
	{
		R_TextureSolidMode(gl1_texturesolidmode->string);
		gl1_texturesolidmode->modified = false;
	}

	if (r_vsync->modified)
	{
		r_vsync->modified = false;
		RI_SetVsync();
	}

	/* clear screen if desired */
	R_Clear();
}

void
RI_SetPalette(const unsigned char *palette)
{
	int i;

	byte *rp = (byte *)r_rawpalette;

	if (palette)
	{
		for (i = 0; i < 256; i++)
		{
			rp[i * 4 + 0] = gammatable[palette[i * 3 + 0]];
			rp[i * 4 + 1] = gammatable[palette[i * 3 + 1]];
			rp[i * 4 + 2] = gammatable[palette[i * 3 + 2]];
			rp[i * 4 + 3] = 0xff;
		}
	}
	else
	{
		for (i = 0; i < 256; i++)
		{
			rp[i * 4 + 0] = LittleLong(d_8to24table[i]) & 0xff;
			rp[i * 4 + 1] = (LittleLong(d_8to24table[i]) >> 8) & 0xff;
			rp[i * 4 + 2] = (LittleLong(d_8to24table[i]) >> 16) & 0xff;
			rp[i * 4 + 3] = 0xff;
		}
	}

	R_SetTexturePalette(r_rawpalette);

	glClearColor(0, 0, 0, 0);
	glClear(GL_COLOR_BUFFER_BIT);
	R_ResetClearColor();
}

/* R_DrawBeam */
void
R_DrawBeam(entity_t *e)
{
	int i, clr[4];

	vec3_t perpvec;
	vec3_t direction, normalized_direction;
	vec3_t start_points[NUM_BEAM_SEGS], end_points[NUM_BEAM_SEGS];
	vec3_t oldorigin, origin;

	GLfloat vtx[3*NUM_BEAM_SEGS*4];
	unsigned int index_vtx = 0;
	unsigned int pointb;

	oldorigin[0] = e->oldorigin[0];
	oldorigin[1] = e->oldorigin[1];
	oldorigin[2] = e->oldorigin[2];

	origin[0] = e->origin[0];
	origin[1] = e->origin[1];
	origin[2] = e->origin[2];

	normalized_direction[0] = direction[0] = oldorigin[0] - origin[0];
	normalized_direction[1] = direction[1] = oldorigin[1] - origin[1];
	normalized_direction[2] = direction[2] = oldorigin[2] - origin[2];

	if (VectorNormalize(normalized_direction) == 0)
	{
		return;
	}

	PerpendicularVector(perpvec, normalized_direction);
	VectorScale(perpvec, e->frame / 2, perpvec);

	for (i = 0; i < 6; i++)
	{
		RotatePointAroundVector(start_points[i], normalized_direction, perpvec,
				(360.0 / NUM_BEAM_SEGS) * i);
		VectorAdd(start_points[i], origin, start_points[i]);
		VectorAdd(start_points[i], direction, end_points[i]);
	}

	R_EnableMultitexture(false);
	glDisable(GL_TEXTURE_2D);
	glEnable(GL_BLEND);
	glDepthMask(GL_FALSE);

	clr[0] = (LittleLong(d_8to24table[e->skinnum & 0xFF])) & 0xFF;
	clr[1] = (LittleLong(d_8to24table[e->skinnum & 0xFF]) >> 8) & 0xFF;
	clr[2] = (LittleLong(d_8to24table[e->skinnum & 0xFF]) >> 16) & 0xFF;
	clr[3] = e->alpha * 255;

	glColor4ub(gammatable[clr[0]], gammatable[clr[1]],
		gammatable[clr[2]], clr[3]);

	for ( i = 0; i < NUM_BEAM_SEGS; i++ )
	{
		vtx[index_vtx++] = start_points [ i ][ 0 ];
		vtx[index_vtx++] = start_points [ i ][ 1 ];
		vtx[index_vtx++] = start_points [ i ][ 2 ];

		vtx[index_vtx++] = end_points [ i ][ 0 ];
		vtx[index_vtx++] = end_points [ i ][ 1 ];
		vtx[index_vtx++] = end_points [ i ][ 2 ];

		pointb = ( i + 1 ) % NUM_BEAM_SEGS;
		vtx[index_vtx++] = start_points [ pointb ][ 0 ];
		vtx[index_vtx++] = start_points [ pointb ][ 1 ];
		vtx[index_vtx++] = start_points [ pointb ][ 2 ];

		vtx[index_vtx++] = end_points [ pointb ][ 0 ];
		vtx[index_vtx++] = end_points [ pointb ][ 1 ];
		vtx[index_vtx++] = end_points [ pointb ][ 2 ];
	}

	glEnableClientState( GL_VERTEX_ARRAY );

	glVertexPointer( 3, GL_FLOAT, 0, vtx );
	glDrawArrays( GL_TRIANGLE_STRIP, 0, NUM_BEAM_SEGS*4 );

	glDisableClientState( GL_VERTEX_ARRAY );

	glEnable(GL_TEXTURE_2D);
	glDisable(GL_BLEND);
	glDepthMask(GL_TRUE);
}

extern int RI_PrepareForWindow(void);
extern int RI_InitContext(void* win);

extern void RI_BeginRegistration(const char *model);
extern struct model_s * RI_RegisterModel(const char *name);
extern struct image_s * RI_RegisterSkin(const char *name);

extern void RI_SetSky(const char *name, float rotate, vec3_t axis);
extern void RI_EndRegistration(void);

extern void RI_RenderFrame(refdef_t *fd);

extern void RI_SetPalette(const unsigned char *palette);
extern qboolean RI_IsVSyncActive(void);
extern void RI_EndFrame(void);

/*
=====================
RI_EndWorldRenderpass
=====================
*/
static qboolean
RI_EndWorldRenderpass( void )
{
	return true;
}

Q2_DLL_EXPORTED refexport_t
GetRefAPI(refimport_t imp)
{
	refexport_t re = {0};

	ri = imp;

	re.api_version = API_VERSION;
	re.framework_version = RI_GetSDLVersion();

	re.Init = RI_Init;
	re.Shutdown = RI_Shutdown;
	re.PrepareForWindow = RI_PrepareForWindow;
	re.InitContext = RI_InitContext;
	re.GetDrawableSize = RI_GetDrawableSize;
	re.ShutdownContext = RI_ShutdownContext;
	re.IsVSyncActive = RI_IsVSyncActive;
	re.BeginRegistration = RI_BeginRegistration;
	re.RegisterModel = RI_RegisterModel;
	re.RegisterSkin = RI_RegisterSkin;

	re.SetSky = RI_SetSky;
	re.EndRegistration = RI_EndRegistration;

	re.RenderFrame = RI_RenderFrame;

	re.DrawFindPic = RDraw_FindPic;

	re.DrawGetPicSize = RDraw_GetPicSize;
	//re.DrawPic = Draw_Pic;
	re.DrawPicScaled = RDraw_PicScaled;
	re.DrawPicScaledCol = RDraw_PicScaledCol;
	re.DrawStretchPic = RDraw_StretchPic;
	//re.DrawChar = Draw_Char;
	re.DrawCharScaled = RDraw_CharScaled;
	re.DrawTileClear = RDraw_TileClear;
	re.DrawFill = RDraw_Fill;
	re.DrawFadeScreen = RDraw_FadeScreen;

	re.DrawStretchRaw = RDraw_StretchRaw;

	re.SetPalette = RI_SetPalette;
	re.BeginFrame = RI_BeginFrame;
	re.EndWorldRenderpass = RI_EndWorldRenderpass;
	re.EndFrame = RI_EndFrame;

    // Tell the client that we're unsing the
	// new renderer restart API.
    ri.Vid_RequestRestart(RESTART_NO);

	return re;
}

#ifdef DEBUG
void
glCheckError_(const char *file, const char *function, int line)
{
	GLenum errorCode;
	const char * msg;

#define MY_ERROR_CASE(X) case X : msg = #X; break;

	while ((errorCode = glGetError()) != GL_NO_ERROR)
	{
		switch(errorCode)
		{
			MY_ERROR_CASE(GL_INVALID_ENUM);
			MY_ERROR_CASE(GL_INVALID_VALUE);
			MY_ERROR_CASE(GL_INVALID_OPERATION);
			MY_ERROR_CASE(GL_STACK_OVERFLOW);
			MY_ERROR_CASE(GL_STACK_UNDERFLOW);
			MY_ERROR_CASE(GL_OUT_OF_MEMORY);
			default: msg = "UNKNOWN";
		}
		Com_Printf("glError: %s in %s (%s, %d)\n", msg, function, file, line);
	}

#undef MY_ERROR_CASE

}
#endif
