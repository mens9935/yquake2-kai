/*
 * Copyright (C) 1997-2001 Id Software, Inc.
 * Copyright (C) 2016 Daniel Gibson
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
 * SDL backend for the GL1 renderer.
 *
 * =======================================================================
 */

#include "header/local.h"

#include <stdint.h>

#ifdef USE_SDL3
#include <SDL3/SDL.h>
#else
#include <SDL2/SDL.h>
#endif

#if defined(YQ2_GL1_GLES) && defined(__EMSCRIPTEN__)
/* SDL_GL_CreateContext() under Emscripten's SDL2 port goes through a real
 * EGL emulation layer (src/video/emscripten/SDL_emscriptenopengles.c ->
 * eglCreateContext, src/video/SDL_egl.c) -- confirmed by reading that
 * source directly. That's exactly where two real-device attempts both
 * failed (EGL_BAD_CONFIG, then EGL_BAD_MATCH after fixing the first
 * cause), regardless of which context-version attributes were requested.
 *
 * ClassiCube's own KaiOS build -- confirmed working on this exact device
 * -- never goes through that layer at all: its web backend
 * (src/webclient/Window_Web.c) has no SDL dependency whatsoever and calls
 * Emscripten's native emscripten_webgl_create_context() (html5.h)
 * directly against the canvas, which talks straight to the browser's
 * canvas.getContext('webgl', ...) with no EGL emulation in between.
 *
 * Mirror that here: bypass SDL_GL_CreateContext/SDL_GL_SwapWindow/
 * SDL_GL_DeleteContext specifically for this one path, while still using
 * SDL for window creation, input, and everything else. */
#include <emscripten/html5.h>
#include <emscripten/em_asm.h>
static EMSCRIPTEN_WEBGL_CONTEXT_HANDLE em_ctx_handle = 0;
#endif

static SDL_Window* window = NULL;
static SDL_GLContext context = NULL;
qboolean IsHighDPIaware = false;
static qboolean vsyncActive = false;

extern cvar_t *gl1_discardfb;

// ----

/*
 * Swaps the buffers and shows the next frame.
 */
void
RI_EndFrame(void)
{
	R_ApplyGLBuffer();	// to draw buffered 2D text

#ifdef YQ2_GL1_GLES
	static const GLenum attachments[3] = {GL_COLOR_EXT, GL_DEPTH_EXT, GL_STENCIL_EXT};

	if (qglDiscardFramebufferEXT)
	{
		switch ((int)gl1_discardfb->value)
		{
			case 1:
				qglDiscardFramebufferEXT(GL_FRAMEBUFFER_OES, 3, &attachments[0]);
				break;
			case 2:
				qglDiscardFramebufferEXT(GL_FRAMEBUFFER_OES, 2, &attachments[1]);
				break;
			default:
				break;
		}
	}
#endif

#if defined(YQ2_GL1_GLES) && defined(__EMSCRIPTEN__)
	/* Nothing to do -- em_ctx_handle wasn't created through SDL, so
	 * window->driverdata has no egl_surface for SDL_GL_SwapWindow to
	 * swap (see SDL_egl.c). WebGL has no manual swap step anyway: the
	 * browser presents the canvas automatically once this frame's JS
	 * finishes, same as ClassiCube's GLContext_SwapBuffers(). */
#else
	SDL_GL_SwapWindow(window);
#endif
}

/*
 * Returns the adress of a GL function
 */
void *
RI_GetProcAddress(const char* proc)
{
	return SDL_GL_GetProcAddress(proc);
}

/*
 * Returns whether the vsync is enabled.
 */
qboolean RI_IsVSyncActive(void)
{
	return vsyncActive;
}

/*
 * This function returns the flags used at the SDL window
 * creation by GLimp_InitGraphics(). In case of error -1
 * is returned.
 */
int RI_PrepareForWindow(void)
{
	// Set GL context attributs bound to the window.
	SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	SDL_GL_SetAttribute(SDL_GL_ACCELERATED_VISUAL, 1);

#ifdef USE_SDL3
	if (SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8))
#else
	if (SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8) == 0)
#endif
	{
		gl_state.stencil = true;
	}
	else
	{
		gl_state.stencil = false;
	}

#if defined(YQ2_GL1_GLES) && !defined(__EMSCRIPTEN__)
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 1);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
#endif
	/* Under Emscripten, RI_InitContext() no longer calls
	 * SDL_GL_CreateContext() at all for YQ2_GL1_GLES (see em_ctx_handle) --
	 * it builds its own EmscriptenWebGLContextAttributes and calls
	 * emscripten_webgl_create_context() directly, so none of these
	 * SDL_GL_SetAttribute() calls are consulted for that path. Left
	 * unset here on purpose; don't add EGL-era version-attribute calls
	 * back in this function, they'd be dead code. */

	// Let's see if the driver supports MSAA.
	int msaa_samples = 0;

	if (gl_msaa_samples->value)
	{
		msaa_samples = gl_msaa_samples->value;

#ifdef USE_SDL3
		if (!SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1))
#else
		if (SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1) < 0)
#endif
		{
			Com_Printf("MSAA is unsupported: %s\n", SDL_GetError());

			ri.Cvar_SetValue ("r_msaa_samples", 0);

			SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
			SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);
		}
#ifdef USE_SDL3
		else if (!SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, msaa_samples))
#else
		else if (SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, msaa_samples) < 0)
#endif
		{
			Com_Printf("MSAA %ix is unsupported: %s\n", msaa_samples, SDL_GetError());

			ri.Cvar_SetValue("r_msaa_samples", 0);

			SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
			SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);
		}
	}
	else
	{
		SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 0);
		SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 0);
	}

#if defined(YQ2_GL1_GLES) && defined(__EMSCRIPTEN__)
	/* NOT SDL_WINDOW_OPENGL here -- confirmed by reading
	 * SDL_emscriptenvideo.c's Emscripten_CreateWindow() directly: that
	 * flag makes SDL_CreateWindow() itself (called right after this
	 * function returns, well before RI_InitContext runs) call
	 * SDL_EGL_CreateSurface() -> SDL_EGL_ChooseConfig() as a side effect
	 * of window creation alone -- the exact EGL emulation layer/config-
	 * matching code that produced EGL_BAD_CONFIG and EGL_BAD_MATCH in
	 * the first two attempts, and it runs regardless of whether
	 * SDL_GL_CreateContext() itself ever gets called afterwards. That
	 * EGL surface creation was consuming the canvas's one WebGL context
	 * slot before RI_InitContext's own emscripten_webgl_create_context()
	 * call ever got a chance to claim it -- confirmed by a real-device
	 * GL_DEBUG build showing canvas.getContext('webgl', <attribs
	 * matching ClassiCube's exactly>) itself returning null, with no
	 * EGL-layer error at all this time.
	 *
	 * Omitting SDL_WINDOW_OPENGL avoids that whole branch, so the canvas
	 * stays untouched until RI_InitContext's own direct call claims it --
	 * but that branch is also the only thing that calls
	 * SDL_GL_LoadLibrary() to set up eglGetProcAddress, which
	 * SDL_GL_GetProcAddress() (GLAD's loader) needs. RI_InitContext()
	 * below calls SDL_GL_LoadLibrary() itself to cover that -- loading
	 * function pointers is a separate step from creating a surface/
	 * context and doesn't touch the canvas. */
	return 0;
#else
	return SDL_WINDOW_OPENGL;
#endif
}

/*
 * Enables or disables the vsync.
 */
void RI_SetVsync(void)
{
	// Make sure that the user given
	// value is SDL compatible...
	int vsync = 0;

	if (r_vsync->value == 1)
	{
		vsync = 1;
	}
	else if (r_vsync->value == 2)
	{
		vsync = -1;
	}

#ifdef USE_SDL3
	if (!SDL_GL_SetSwapInterval(vsync))
#else
	if (SDL_GL_SetSwapInterval(vsync) == -1)
#endif
	{
		if (vsync == -1)
		{
			// Not every system supports adaptive
			// vsync, fallback to normal vsync.
			Com_Printf("Failed to set adaptive vsync, reverting to normal vsync.\n");
			SDL_GL_SetSwapInterval(1);
		}
	}

#ifdef USE_SDL3
	int vsyncState;
	if (!SDL_GL_GetSwapInterval(&vsyncState))
	{
		Com_Printf("Failed to get vsync state, assuming vsync inactive.\n");
		vsyncActive = false;
	}
	else
	{
		vsyncActive = vsyncState ? true : false;
	}
#else
	vsyncActive = SDL_GL_GetSwapInterval() != 0;
#endif
}

/*
 * Updates the gamma ramp. Only used with SDL2.
 */
void
RI_UpdateGamma(void)
{
// Hardware gamma / gamma ramps are no longer supported with SDL3.
// There's no replacement and sdl2-compat won't support it either.
// See https://github.com/libsdl-org/SDL/pull/6617 for the rational.
// Gamma works with a lookup table when using SDL3 (or GLES1).
#ifndef GL1_GAMMATABLE
	float gamma = (vid_gamma->value);

	Uint16 ramp[256];
	SDL_CalculateGammaRamp(gamma, ramp);

	if (SDL_SetWindowGammaRamp(window, ramp, ramp, ramp) != 0)
	{
		Com_Printf("Setting gamma failed: %s\n", SDL_GetError());
	}
#endif
}

/*
 * Initializes the OpenGL context. Returns true at
 * success and false at failure.
 */
int RI_InitContext(void* win)
{
	// Coders are stupid.
	if (win == NULL)
	{
		Com_Error(ERR_FATAL, "%s must not be called with NULL argument!", __func__);

		return false;
	}

	window = (SDL_Window*)win;

#if defined(YQ2_GL1_GLES) && defined(__EMSCRIPTEN__)
	// See the comment by em_ctx_handle's declaration above for why this
	// bypasses SDL_GL_CreateContext entirely on this platform.
	{
		EmscriptenWebGLContextAttributes attribs;

		// RI_PrepareForWindow() deliberately didn't request
		// SDL_WINDOW_OPENGL (see its comment), so SDL_CreateWindow()
		// never called SDL_GL_LoadLibrary() on our behalf -- do it here.
		// This only loads eglGetProcAddress and friends for
		// SDL_GL_GetProcAddress() below; it doesn't create a surface or
		// touch the canvas, unlike SDL_EGL_CreateSurface().
		if (SDL_GL_LoadLibrary(NULL) < 0)
		{
			Com_Printf("%s: SDL_GL_LoadLibrary() failed: %s\n", __func__, SDL_GetError());

			window = NULL;

			return false;
		}

		emscripten_webgl_init_context_attributes(&attribs);
		attribs.alpha = false;
		attribs.depth = true;
		/* Requesting stencil=true here previously (real-device test:
		 * emscripten_webgl_create_context() itself returned failure, no
		 * WebGL-level error visible since this isn't a GL_DEBUG build).
		 * gl_state.stencil is already treated as best-effort everywhere
		 * else in this renderer (see the SDL_GL_GetAttribute check in
		 * RI_InitContext below), and ClassiCube's own known-working
		 * config on this exact device requests stencil=false -- match it
		 * exactly rather than requesting a buffer this GPU/driver
		 * combination may not actually be able to provide, which can
		 * make canvas.getContext('webgl', ...) return null outright
		 * instead of silently downgrading. */
		attribs.stencil = false;
		attribs.antialias = false;
		/* emscripten_webgl_init_context_attributes() already defaults
		 * majorVersion/minorVersion to 1/0 (confirmed by reading
		 * library_html5_webgl.js directly) -- set explicitly anyway so
		 * this isn't relying on an unstated default staying that way
		 * across emsdk versions; this old Gecko-48-class engine has no
		 * WebGL2 support at all, so a majorVersion of 2 here would
		 * request "webgl2" and fail outright regardless of anything
		 * else in this function. */
		attribs.majorVersion = 1;
		attribs.minorVersion = 0;

		/* Everything above this point was ruled out on a real device,
		 * one variable at a time: the canvas-poisoning self-inflicted
		 * bug (fixed), Module.canvas resolving to something stale
		 * (it doesn't -- confirmed identical to a fresh DOM lookup),
		 * and the extra/newer context-attribute properties Emscripten's
		 * own call adds beyond this file's 6 (raw getContext() with
		 * that *exact* full property set also succeeds standalone).
		 * Every raw getContext() call tried here has succeeded, every
		 * emscripten_webgl_create_context() call has failed, with
		 * canvas, attributes, and resolution all confirmed equivalent
		 * between them -- narrowing this to something inside
		 * Emscripten's own createContext()/registerContext() JS
		 * (library_webgl.js), not anything this C code controls via
		 * the public API's inputs.
		 *
		 * ClassiCube's own compiled output (confirmed working on this
		 * exact device) has a dramatically simpler GL.createContext --
		 * no OffscreenCanvas/Safari/GL_PREINITIALIZED_CONTEXT/chrome-
		 * version branches at all, just getContext() then
		 * GL.registerContext() -- consistent with an older/leaner
		 * emsdk than this build's 2.0.34. Bypass emscripten_webgl_
		 * create_context() entirely and do those exact two steps
		 * ourselves via EM_ASM_INT, wrapped in try/catch this time
		 * (unlike the C API, which gives no way to see an exception
		 * thrown *inside* GL.registerContext() itself -- if that's
		 * where this actually fails, every probe so far would have
		 * been blind to it, since none of them called it). GL is a
		 * real global object in this runtime (library_webgl.js runs
		 * unconditionally whenever any GL code is linked in), reachable
		 * directly from EM_ASM/EM_ASM_INT same as any other JS here. */
		em_ctx_handle = (EMSCRIPTEN_WEBGL_CONTEXT_HANDLE)EM_ASM_INT({
			try {
				var c = document.getElementById('canvas');
				if (!c) {
					console.log('[kaios] manual GL context: no #canvas element found');
					return 0;
				}
				var attrs = {};
				attrs.alpha = false;
				attrs.depth = true;
				attrs.stencil = false;
				attrs.antialias = false;
				attrs.majorVersion = 1;
				attrs.minorVersion = 0;
				var ctx = c.getContext('webgl', attrs) || c.getContext('experimental-webgl', attrs);
				if (!ctx) {
					console.log('[kaios] manual GL context: getContext returned null');
					return 0;
				}
				console.log('[kaios] manual GL context: getContext OK, registering with GL...');
				var handle = GL.registerContext(ctx, attrs);
				console.log('[kaios] manual GL context: GL.registerContext handle=' + handle);
				return handle;
			} catch (e) {
				console.log('[kaios] manual GL context threw: ' + e);
				return 0;
			}
		});

		if (!em_ctx_handle)
		{
			Com_Printf("%s: manual GL.registerContext() path failed too "
				"(see console for the JS-level reason)\n", __func__);

			window = NULL;

			return false;
		}

		emscripten_webgl_make_context_current(em_ctx_handle);

		/* The manual GL.registerContext()/make_context_current() path
		 * above got a real, working WebGL context on a real device --
		 * confirmed by "Initialized OpenGL ES version 2.0 context" and
		 * a real GL_VENDOR/GL_RENDERER/GL_EXTENSIONS dump right
		 * afterward -- but then hit "TypeError: s_texUnits is null"
		 * inside GLImmediate (library_glemu.js), the LEGACY_GL_EMULATION
		 * fixed-pipeline layer this GLES1 renderer's glBegin/glVertex/
		 * glTexEnv-style calls depend on entirely. s_texUnits is set up
		 * by GLImmediate.init(), which normal SDL2/Emscripten context
		 * creation reaches through Browser.createContext() -> ... ->
		 * Browser.moduleContextCreatedCallbacks.forEach(cb => cb())
		 * (library_browser.js) -- a step our manual, minimal
		 * getContext()+registerContext() bypass above has no equivalent
		 * of, since it never goes through Browser.createContext() at
		 * all. Fire those callbacks ourselves now that a context is
		 * current, so GLImmediate.init() (and anything else normally
		 * wired to "a WebGL context just became active") actually runs. */
		EM_ASM({
			try {
				if (typeof Browser !== 'undefined' && Browser.moduleContextCreatedCallbacks) {
					Browser.moduleContextCreatedCallbacks.forEach(function (callback) {
						callback();
					});
					console.log('[kaios] fired ' + Browser.moduleContextCreatedCallbacks.length +
						' moduleContextCreatedCallbacks manually');
				} else {
					console.log('[kaios] Browser.moduleContextCreatedCallbacks not available to fire manually');
				}
			} catch (e) {
				console.log('[kaios] firing moduleContextCreatedCallbacks threw: ' + e);
			}
		});

		// Only used by RI_ShutdownContext()'s guard -- never dereferenced
		// as a real SDL_GLContext.
		context = (SDL_GLContext)(uintptr_t)em_ctx_handle;
	}
#else
	// Initialize GL context.
	context = SDL_GL_CreateContext(window);

	if (context == NULL)
	{
		Com_Printf("%s: Creating OpenGL Context failed: %s\n", __func__, SDL_GetError());

		window = NULL;

		return false;
	}
#endif

#ifdef YQ2_GL1_GLES

	// Load GL pointers through GLAD and check context.
	if( !gladLoadGLES1Loader( (void * (*)(const char *)) SDL_GL_GetProcAddress ) )
	{
		Com_Printf("%s ERROR: loading OpenGL ES function pointers failed!\n", __func__);
		return false;
	}

	gl_config.major_version = GLVersion.major;
	gl_config.minor_version = GLVersion.minor;
	Com_Printf("Initialized OpenGL ES version %d.%d context\n", gl_config.major_version, gl_config.minor_version);

#else

	// Check if it's really OpenGL 1.4.
	const char* glver = (char *)glGetString(GL_VERSION);
	sscanf(glver, "%d.%d", &gl_config.major_version, &gl_config.minor_version);

	if (gl_config.major_version < 1 || (gl_config.major_version == 1 && gl_config.minor_version < 4))
	{
		Com_Printf("%s: Got an OpenGL version %d.%d context - need (at least) 1.4!\n", __func__, gl_config.major_version, gl_config.minor_version);

		return false;
	}

#endif

	// Check if we've got the requested MSAA.
	int msaa_samples = 0;

	if (gl_msaa_samples->value)
	{
#ifdef USE_SDL3
		if (SDL_GL_GetAttribute(SDL_GL_MULTISAMPLESAMPLES, &msaa_samples))
#else
		if (SDL_GL_GetAttribute(SDL_GL_MULTISAMPLESAMPLES, &msaa_samples) == 0)
#endif
		{
			ri.Cvar_SetValue("r_msaa_samples", msaa_samples);
		}
	}

	// Enable vsync if requested.
	RI_SetVsync();

	// Check if we've got 8 stencil bits.
	int stencil_bits = 0;

	if (gl_state.stencil)
	{
#ifdef USE_SDL3
		if (!SDL_GL_GetAttribute(SDL_GL_STENCIL_SIZE, &stencil_bits) || stencil_bits < 8)
#else
		if (SDL_GL_GetAttribute(SDL_GL_STENCIL_SIZE, &stencil_bits) < 0 || stencil_bits < 8)
#endif
		{
			gl_state.stencil = false;
		}
	}

	// Initialize gamma.
	vid_gamma->modified = true;

	// Window title - set here so we can display renderer name in it.
	char title[40] = {0};

#ifdef YQ2_GL1_GLES
	snprintf(title, sizeof(title), "Yamagi Quake II %s - OpenGL ES 1.0", YQ2VERSION);
#else
	snprintf(title, sizeof(title), "Yamagi Quake II %s - OpenGL 1.4", YQ2VERSION);
#endif
	SDL_SetWindowTitle(window, title);

#if SDL_VERSION_ATLEAST(2, 26, 0)
	// Figure out if we are high dpi aware.
	int flags = SDL_GetWindowFlags(win);
#ifdef USE_SDL3
	IsHighDPIaware = (flags & SDL_WINDOW_HIGH_PIXEL_DENSITY) ? true : false;
#else
	IsHighDPIaware = (flags & SDL_WINDOW_ALLOW_HIGHDPI) ? true : false;
#endif
#endif

	return true;
}

/*
 * Fills the actual size of the drawable into width and height.
 */
void RI_GetDrawableSize(int* width, int* height)
{
#ifdef USE_SDL3
	SDL_GetWindowSizeInPixels(window, width, height);
#else
	SDL_GL_GetDrawableSize(window, width, height);
#endif
}

/*
 * Shuts the GL context down.
 */
void
RI_ShutdownContext(void)
{
	if (window)
	{
		if(context)
		{
#if defined(YQ2_GL1_GLES) && defined(__EMSCRIPTEN__)
			emscripten_webgl_destroy_context(em_ctx_handle);
			em_ctx_handle = 0;
#elif defined(USE_SDL3)
			SDL_GL_DestroyContext(context);
#else
			SDL_GL_DeleteContext(context);
#endif
			context = NULL;
		}
	}
}

/*
 * Returns the SDL major version. Implemented
 * here to not polute gl1_main.c with the SDL
 * headers.
 */
int RI_GetSDLVersion()
{
#ifdef USE_SDL3
	int version = SDL_GetVersion();
	return SDL_VERSIONNUM_MAJOR(version);
#else
	SDL_version ver;
	SDL_VERSION(&ver);
	return ver.major;
#endif
}
