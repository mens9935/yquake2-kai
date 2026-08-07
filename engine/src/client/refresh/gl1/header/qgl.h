/*
 * Copyright (C) 2013 Alejandro Ricoveri
 * Copyright (C) 1999-2005 Id Software, Inc.
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
 * Quake GL prototypes based on ioquake3 source code
 *
 * =======================================================================
 */

#ifndef REF_QGL_H
#define REF_QGL_H

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef YQ2_GL1_GLES
#include "../glad-gles1/include/glad/glad.h"
#else
#if defined(__APPLE__)
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif
#endif

#ifdef __EMSCRIPTEN__
/* glad's runtime proc-address lookup for glTexEnvi resolves to NULL on
 * this platform. emscripten_GetProcAddress() (gl.c, part of
 * Emscripten's own LEGACY_GL_EMULATION runtime) only exposes a
 * hand-picked allow-list of legacy GL1.x function names through the
 * generic SDL_GL_GetProcAddress()-style lookup GLAD uses
 * (RETURN_GL_EMU_FN(...) entries in gl.c) -- glShadeModel is on that
 * list (confirmed harmless on a real device: round-trips as a "TODO:
 * glShadeModel" stub and returns), but the whole glTexEnv family
 * (glTexEnvi/glTexEnvf/glTexEnviv/glTexEnvfv) is not, despite the
 * underlying emscripten_glTexEnvi() etc. genuinely existing and being
 * fully wired into GLImmediate's fixed-pipeline texture-environment
 * emulation (TexEnvJIT.hook_texEnvi, library_glemu.js) -- just not
 * discoverable through that particular lookup path. Confirmed on a
 * real device: glad_glTexEnvi silently loads as NULL, and calling
 * through it crashes ("TypeError: ... is not a function", an invalid
 * function-table call) the moment R_SetDefaultState() first calls
 * R_TexEnv(). Bypass GLAD's runtime lookup for just this one function
 * and call the real, always-linked-in emscripten_glTexEnvi() directly
 * -- this renderer never calls glTexEnvf/glTexEnviv/glTexEnvfv at all
 * (confirmed by grep), so only this one needs the same treatment. */
extern void emscripten_glTexEnvi(GLenum target, GLenum pname, GLint param);
#undef glTexEnvi
#define glTexEnvi emscripten_glTexEnvi

/* Same gap, same fix, for glPointSize: gl.c declares
 * emscripten_glPointSize() but never lists it in RETURN_GL_EMU_FN
 * either, so glad_glPointSize also silently loads as NULL. Currently
 * unreached on this device (R_DrawParticles's glPointSize call is
 * gated on gl_config.pointparameters, which this device's extension
 * probe reports as false -- "Point parameters: Failed" -- so the
 * gate never opens), but it's the identical allow-list gap and the
 * scan in gl1_sdl.c would otherwise keep flagging it every run. */
extern void emscripten_glPointSize(GLfloat size);
#undef glPointSize
#define glPointSize emscripten_glPointSize

/* Same gap, same fix, for glFrustumf/glOrthof: local.h's
 * glFrustum(...)/glOrtho(...) macros (under YQ2_GL1_GLES) redirect to
 * these two, since real GLES1 has no double-precision glFrustum/
 * glOrtho at all. But RETURN_GL_EMU_FN (gl.c) only lists the bare
 * glFrustum/glOrtho names, not the "f"-suffixed ones this code
 * actually calls, so glad_glFrustumf/glad_glOrthof also silently load
 * as NULL -- confirmed on a real device: the cube test's before/after
 * trace showed "before glFrustum(...)" print and then nothing at all,
 * the exact same silent invalid-function-table-call signature as
 * glTexEnvi/glPointSize. Unlike those two, there is no emscripten_
 * C-callable wrapper for these in gl.c at all (glFrustumf/glOrthof
 * are pure JS: library_glemu.js implements them directly as GLImmediate
 * matrix-stack operations, glFrustumf is simply a JS-level alias of
 * glFrustum) -- reach the real linked JS function directly via GCC's
 * asm-label declarator instead, which names the actual object/import
 * symbol rather than the C identifier glad.h has already redefined to
 * its own (NULL) function pointer. */
extern void kaios_real_glFrustum(GLdouble left, GLdouble right, GLdouble bottom,
	GLdouble top, GLdouble zNear, GLdouble zFar) __asm__("glFrustum");
extern void kaios_real_glOrtho(GLdouble left, GLdouble right, GLdouble bottom,
	GLdouble top, GLdouble zNear, GLdouble zFar) __asm__("glOrtho");
#undef glFrustumf
#define glFrustumf kaios_real_glFrustum
#undef glOrthof
#define glOrthof kaios_real_glOrtho

/* glDepthRangef has the same gap but, like glTexEnvi/glPointSize
 * (and unlike glFrustumf/glOrthof above), gl.c does declare a proper
 * C-callable emscripten_glDepthRangef() wrapper -- use that directly,
 * no asm-label trick needed. local.h's glDepthRange(...) macro
 * redirects here the same way it redirects glFrustum/glOrtho. */
extern void emscripten_glDepthRangef(GLclampf n, GLclampf f);
#undef glDepthRangef
#define glDepthRangef emscripten_glDepthRangef
#endif

#ifndef APIENTRY
#define APIENTRY
#endif

// Extracted from <glext.h>
#ifndef GL_VERSION_1_4
#define GL_POINT_SIZE_MIN                 0x8126
#define GL_POINT_SIZE_MAX                 0x8127
#define GL_POINT_DISTANCE_ATTENUATION     0x8129
#define GL_GENERATE_MIPMAP                0x8191
#endif

#ifndef GL_VERSION_1_3
#define GL_TEXTURE0                       0x84C0
#define GL_TEXTURE1                       0x84C1
#define GL_MULTISAMPLE                    0x809D
#define GL_COMBINE                        0x8570
#define GL_RGB_SCALE                      0x8573
#endif

#ifndef GL_EXT_shared_texture_palette
#define GL_SHARED_TEXTURE_PALETTE_EXT     0x81FB
#endif

#ifndef GL_EXT_texture_filter_anisotropic
#define GL_TEXTURE_MAX_ANISOTROPY_EXT     0x84FE
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#endif

#ifndef GL_NV_multisample_filter_hint
#define GL_MULTISAMPLE_FILTER_HINT_NV     0x8534
#endif

// =======================================================================

/*
 * This is responsible for setting up our QGL extension pointers
 */
void QGL_Init ( void );

/*
 * Unloads the specified DLL then nulls out all the proc pointers.
 */
void QGL_Shutdown ( void );

/* GL extensions */
extern void ( APIENTRY *qglPointParameterf ) ( GLenum param, GLfloat value );
extern void ( APIENTRY *qglPointParameterfv ) ( GLenum param,
		const GLfloat *value );
extern void ( APIENTRY *qglColorTableEXT ) ( GLenum, GLenum, GLsizei, GLenum,
		GLenum, const GLvoid * );
extern void ( APIENTRY *qglActiveTexture ) ( GLenum texture );
extern void ( APIENTRY *qglClientActiveTexture ) ( GLenum texture );
extern void ( APIENTRY *qglDiscardFramebufferEXT ) ( GLenum target,
		GLsizei numAttachments, const GLenum *attachments );

#endif
