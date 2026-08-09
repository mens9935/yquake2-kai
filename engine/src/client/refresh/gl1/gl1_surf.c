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
 * Surface generation and drawing
 *
 * =======================================================================
 */

#include <assert.h>

#include "header/local.h"

typedef struct
{
	int top, bottom, left, right;
} lmrect_t;

int c_visible_lightmaps;
int c_visible_textures;
static vec3_t modelorg; /* relative to viewpoint */
msurface_t *r_alpha_surfaces;

gllightmapstate_t gl_lms;
extern int cur_lm_copy;

void LM_InitBlock(void);
void LM_UploadBlock(qboolean dynamic);

#ifdef __EMSCRIPTEN__
/*
 * ==============================================================
 *
 * Static world-geometry vertex cache (r_gl1_static_vbo).
 *
 * R_UpdateGLBuffer/R_ApplyGLBuffer (gl1_buffer.c) batch multiple
 * surfaces into one draw call, but every flush still re-copies each
 * surface's position + diffuse UV + lightmap UV into gl_buf's CPU-side
 * arrays and re-uploads that to the GPU from scratch -- every frame,
 * even though the vast majority of world geometry never changes
 * position or texture coordinates from one frame to the next. Real
 * device testing (r_speeds/KAIOS_OCCL) kept showing drops even after
 * cutting both triangle count (occlusion/distance culling) and draw
 * call count (bigger lightmap atlas), pointing at this per-frame
 * vertex data upload itself as the remaining cost.
 *
 * Fix: upload every eligible world surface's vertex data to a real,
 * static (GL_STATIC_DRAW, uploaded once) GPU buffer up front, and at
 * draw time only ever build a small index list into it (buf_mtex_svbo
 * in gl1_buffer.c) -- the vertex/texcoord data itself never crosses
 * the JS/WASM boundary again after the initial build.
 *
 * "Eligible" excludes anything whose position or texcoords can change
 * after this is built: sky (drawn separately), translucent (TRANS33/
 * 66, drawn back-to-front from a separate chain), warp/water
 * (SURF_DRAWTURB, subdivided and animated by R_EmitWaterPolys), and
 * flowing/scrolling surfaces (SURF_FLOWING, whose diffuse U shifts
 * with r_newrefdef.time). Those all keep using the ordinary buf_mtex
 * path, unchanged. Animated *textures* (texinfo->next chains, e.g.
 * flickering screens) are NOT excluded -- only which GL texture gets
 * bound varies frame to frame for those, never this surface's own
 * position/texcoord data, so the cache stays correct; R_DrawTextureChains
 * below already re-resolves the current frame's texture per surface
 * exactly as before.
 *
 * Bounded to what fits in a 16-bit index (see gl1_buffer.c's
 * GLushort gl_buf.idx): if a map's eligible geometry needs more than
 * 65535 total vertices, the cache is left unbuilt and every surface
 * transparently falls back to the ordinary per-frame path instead --
 * this only ever costs performance, never correctness.
 *
 * ==============================================================
 */

GLuint r_svbo_pos_vbo, r_svbo_tex0_vbo, r_svbo_tex1_vbo;
int r_svbo_hits, r_svbo_misses; /* reset+reported alongside KAIOS_OCCL, gl1_main.c */

static int *r_svbo_base_vertex; /* [numsurfaces]; -1 = not cached */
static const model_t *r_svbo_built_for;

static void
R_SVBO_Clear(void)
{
	if (r_svbo_pos_vbo)
	{
		glDeleteBuffers(1, &r_svbo_pos_vbo);
		r_svbo_pos_vbo = 0;
	}

	if (r_svbo_tex0_vbo)
	{
		glDeleteBuffers(1, &r_svbo_tex0_vbo);
		r_svbo_tex0_vbo = 0;
	}

	if (r_svbo_tex1_vbo)
	{
		glDeleteBuffers(1, &r_svbo_tex1_vbo);
		r_svbo_tex1_vbo = 0;
	}

	if (r_svbo_base_vertex)
	{
		free(r_svbo_base_vertex);
		r_svbo_base_vertex = NULL;
	}
}

static qboolean
R_SVBO_SurfaceEligible(const msurface_t *surf)
{
	if (!surf->polys || surf->polys->numverts < 3)
	{
		return false;
	}

	if (surf->texinfo->flags & (SURF_SKY | SURF_TRANS33 | SURF_TRANS66 |
		SURF_WARP | SURF_FLOWING))
	{
		return false;
	}

	if (surf->flags & SURF_DRAWTURB)
	{
		return false;
	}

	return true;
}

/*
 * Rebuilds the static VBOs whenever the world model changes (new map)
 * -- cheap early-out otherwise. Called once per frame, before the
 * world is walked/drawn.
 */
void
R_SVBO_EnsureBuilt(void)
{
	int i, total_verts;
	GLfloat *pos, *tex0, *tex1;

	if (!r_gl1_static_vbo->value)
	{
		return;
	}

	if (r_svbo_built_for == r_worldmodel)
	{
		return;
	}

	R_SVBO_Clear();
	r_svbo_built_for = r_worldmodel;

	if (!r_worldmodel || r_worldmodel->numsurfaces <= 0)
	{
		return;
	}

	r_svbo_base_vertex = malloc(sizeof(int) * r_worldmodel->numsurfaces);

	if (!r_svbo_base_vertex)
	{
		return;
	}

	total_verts = 0;

	for (i = 0; i < r_worldmodel->numsurfaces; i++)
	{
		const msurface_t *surf = &r_worldmodel->surfaces[i];

		if (!R_SVBO_SurfaceEligible(surf))
		{
			r_svbo_base_vertex[i] = -1;
			continue;
		}

		/* Stay inside gl_buf.idx's GLushort range (gl1_buffer.c) --
		 * if this map has more eligible geometry than that, just
		 * leave the cache unbuilt entirely rather than risk an
		 * index overflowing 16 bits on a bigger map later. */
		if (total_verts + surf->polys->numverts > 65535)
		{
			Com_Printf("%s: world has more than 65535 eligible "
				"vertices, leaving the static vertex cache off "
				"for this map\n", __func__);
			free(r_svbo_base_vertex);
			r_svbo_base_vertex = NULL;
			return;
		}

		r_svbo_base_vertex[i] = total_verts;
		total_verts += surf->polys->numverts;
	}

	if (total_verts <= 0)
	{
		free(r_svbo_base_vertex);
		r_svbo_base_vertex = NULL;
		return;
	}

	pos = malloc(sizeof(GLfloat) * 3 * total_verts);
	tex0 = malloc(sizeof(GLfloat) * 2 * total_verts);
	tex1 = malloc(sizeof(GLfloat) * 2 * total_verts);

	if (!pos || !tex0 || !tex1)
	{
		free(pos);
		free(tex0);
		free(tex1);
		free(r_svbo_base_vertex);
		r_svbo_base_vertex = NULL;
		return;
	}

	for (i = 0; i < r_worldmodel->numsurfaces; i++)
	{
		const msurface_t *surf = &r_worldmodel->surfaces[i];
		int base = r_svbo_base_vertex[i];
		const float *v;
		int vi;

		if (base < 0)
		{
			continue;
		}

		v = surf->polys->verts[0];

		for (vi = 0; vi < surf->polys->numverts; vi++, v += VERTEXSIZE)
		{
			pos[(base+vi)*3+0] = v[0];
			pos[(base+vi)*3+1] = v[1];
			pos[(base+vi)*3+2] = v[2];
			tex0[(base+vi)*2+0] = v[3];
			tex0[(base+vi)*2+1] = v[4];
			tex1[(base+vi)*2+0] = v[5];
			tex1[(base+vi)*2+1] = v[6];
		}
	}

	glGenBuffers(1, &r_svbo_pos_vbo);
	glBindBuffer(GL_ARRAY_BUFFER, r_svbo_pos_vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(GLfloat) * 3 * total_verts, pos, GL_STATIC_DRAW);

	glGenBuffers(1, &r_svbo_tex0_vbo);
	glBindBuffer(GL_ARRAY_BUFFER, r_svbo_tex0_vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(GLfloat) * 2 * total_verts, tex0, GL_STATIC_DRAW);

	glGenBuffers(1, &r_svbo_tex1_vbo);
	glBindBuffer(GL_ARRAY_BUFFER, r_svbo_tex1_vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(GLfloat) * 2 * total_verts, tex1, GL_STATIC_DRAW);

	glBindBuffer(GL_ARRAY_BUFFER, 0);

	Com_Printf("%s: cached %d vertices from %s\n", __func__,
		total_verts, r_worldmodel->name);

	free(pos);
	free(tex0);
	free(tex1);
}

/*
 * Returns this surface's base vertex offset into the static VBOs, or
 * -1 if it isn't (or can't be) cached -- the caller must fall back to
 * the ordinary per-frame buf_mtex path in that case.
 */
int
R_SVBO_BaseVertexForSurface(const msurface_t *surf)
{
	ptrdiff_t idx;

	if (!r_gl1_static_vbo->value || !r_svbo_base_vertex || !r_worldmodel)
	{
		return -1;
	}

	idx = surf - r_worldmodel->surfaces;

	if (idx < 0 || idx >= r_worldmodel->numsurfaces)
	{
		return -1;
	}

	return r_svbo_base_vertex[idx];
}
#endif

/*
 * ==============================================================
 *
 * Coarse CPU-side screen-space occlusion cull (r_occlusion_cull).
 *
 * This renderer has no GPU occlusion query available and, unlike the
 * software renderer, no scanline occlusion built into how it draws --
 * see r_occlusion_cull's registration comment in gl1_main.c for the
 * real-device numbers that motivated this. The world BSP is walked
 * front-to-back (R_RecursiveWorldNode recurses the near child, then
 * this node's own surfaces, then the far child), so a small grid of
 * screen cells can be marked "covered" from opaque surfaces as they're
 * found visible, and later (farther) nodes whose whole projected
 * screen rect already falls inside covered cells can be skipped
 * outright -- same idea as the software renderer's front-to-back
 * scanline occlusion, just coarse and CPU-only instead of per-pixel.
 *
 * Deliberately conservative in both directions: a screen rect this
 * can't confidently compute (e.g. a box straddling the near plane)
 * is never used to cull and never used to mark coverage. Getting this
 * wrong can only cost performance (an occluded node drawn anyway),
 * never correctness (nothing gets skipped without being provably
 * behind already-drawn opaque geometry).
 *
 * ==============================================================
 */

#define R_OCCL_GRID_W 16
#define R_OCCL_GRID_H 12

static qboolean r_occl_grid[R_OCCL_GRID_W * R_OCCL_GRID_H];
static qboolean r_occl_grid_active;
static float r_occl_tanx, r_occl_tany;

/* Debug counters, reported via KAIOS_OCCL in gl1_main.c -- real-device
 * testing showed zero change in polycount with this on, which a
 * working occlusion cull should never do in a normal indoor scene.
 * These settle whether marking/culling are doing anything at all
 * instead of guessing at the math again. */
int r_occl_nodes_tested, r_occl_nodes_culled;
int r_occl_cells_marked, r_occl_surfs_projected, r_occl_surfs_skipped;

void
R_OcclusionGridClear(void)
{
	memset(r_occl_grid, 0, sizeof(r_occl_grid));

	r_occl_nodes_tested = 0;
	r_occl_nodes_culled = 0;
	r_occl_cells_marked = 0;
	r_occl_surfs_projected = 0;
	r_occl_surfs_skipped = 0;

	r_occl_grid_active = false;

	if (!r_occlusion_cull->value)
	{
		return;
	}

	if (r_newrefdef.fov_x <= 1.0f || r_newrefdef.fov_x >= 179.0f ||
		r_newrefdef.fov_y <= 1.0f || r_newrefdef.fov_y >= 179.0f ||
		viddef.width <= 0 || viddef.height <= 0)
	{
		return; /* degenerate fov/viewport this frame, don't trust it */
	}

	r_occl_tanx = (float)tan((r_newrefdef.fov_x * M_PI / 180.0) * 0.5);
	r_occl_tany = (float)tan((r_newrefdef.fov_y * M_PI / 180.0) * 0.5);

	if (r_occl_tanx < 0.001f || r_occl_tany < 0.001f)
	{
		return;
	}

	r_occl_grid_active = true;
}

/*
 * Projects a world point to screen pixel coordinates. Returns false
 * (leaving the outputs untouched) if the point is behind or right at
 * the near plane -- the caller must treat that as "can't safely
 * project this", not as some off-to-one-side result.
 */
static qboolean
R_WorldPointToScreen(const vec3_t point, float *sx, float *sy)
{
	vec3_t delta;
	float fwd, right, up;

	VectorSubtract(point, r_origin, delta);
	fwd = DotProduct(delta, vpn);

	if (fwd < 1.0f)
	{
		return false;
	}

	right = DotProduct(delta, vright);
	up = DotProduct(delta, vup);

	*sx = (0.5f + 0.5f * (right / fwd) / r_occl_tanx) * viddef.width;
	*sy = (0.5f - 0.5f * (up / fwd) / r_occl_tany) * viddef.height;

	return true;
}

static void
R_OcclusionGridCellRange(float rx0, float ry0, float rx1, float ry1,
	int *cx0, int *cy0, int *cx1, int *cy1)
{
	*cx0 = (int)(rx0 / viddef.width * R_OCCL_GRID_W);
	*cy0 = (int)(ry0 / viddef.height * R_OCCL_GRID_H);
	*cx1 = (int)ceil(rx1 / viddef.width * R_OCCL_GRID_W);
	*cy1 = (int)ceil(ry1 / viddef.height * R_OCCL_GRID_H);

	if (*cx0 < 0) { *cx0 = 0; }
	if (*cy0 < 0) { *cy0 = 0; }
	if (*cx1 > R_OCCL_GRID_W) { *cx1 = R_OCCL_GRID_W; }
	if (*cy1 > R_OCCL_GRID_H) { *cy1 = R_OCCL_GRID_H; }
}

static void
R_OcclusionMarkCovered(float rx0, float ry0, float rx1, float ry1)
{
	int cx0, cy0, cx1, cy1, x, y;

	R_OcclusionGridCellRange(rx0, ry0, rx1, ry1, &cx0, &cy0, &cx1, &cy1);

	for (y = cy0; y < cy1; y++)
	{
		for (x = cx0; x < cx1; x++)
		{
			r_occl_grid[y * R_OCCL_GRID_W + x] = true;
		}
	}
}

/*
 * True only if every grid cell touched by this rect is already marked
 * covered. Any uncovered cell -- including a degenerate/empty cell
 * range -- means "not sure", which must count as NOT occluded.
 */
static qboolean
R_OcclusionRectCovered(float rx0, float ry0, float rx1, float ry1)
{
	int cx0, cy0, cx1, cy1, x, y;

	R_OcclusionGridCellRange(rx0, ry0, rx1, ry1, &cx0, &cy0, &cx1, &cy1);

	if (cx1 <= cx0 || cy1 <= cy0)
	{
		return false;
	}

	for (y = cy0; y < cy1; y++)
	{
		for (x = cx0; x < cx1; x++)
		{
			if (!r_occl_grid[y * R_OCCL_GRID_W + x])
			{
				return false;
			}
		}
	}

	return true;
}

/*
 * Projects an axis-aligned world bounding box to a screen-space pixel
 * rect. Returns false if any of the 8 corners can't be projected --
 * a box straddling the near plane (e.g. a BSP node containing the
 * viewer) is exactly the case where a naive screen rect would be
 * wrong, so it's safer to just not occlusion-test it at all.
 */
static qboolean
R_BoxToScreenRect(const vec3_t mins, const vec3_t maxs,
	float *rx0, float *ry0, float *rx1, float *ry1)
{
	int i;
	float x0 = 0, y0 = 0, x1 = 0, y1 = 0;

	for (i = 0; i < 8; i++)
	{
		vec3_t corner;
		float sx, sy;

		corner[0] = (i & 1) ? maxs[0] : mins[0];
		corner[1] = (i & 2) ? maxs[1] : mins[1];
		corner[2] = (i & 4) ? maxs[2] : mins[2];

		if (!R_WorldPointToScreen(corner, &sx, &sy))
		{
			return false;
		}

		if (i == 0)
		{
			x0 = x1 = sx;
			y0 = y1 = sy;
		}
		else
		{
			if (sx < x0) { x0 = sx; }
			if (sx > x1) { x1 = sx; }
			if (sy < y0) { y0 = sy; }
			if (sy > y1) { y1 = sy; }
		}
	}

	if (x0 < 0) { x0 = 0; }
	if (y0 < 0) { y0 = 0; }
	if (x1 > viddef.width) { x1 = viddef.width; }
	if (y1 > viddef.height) { y1 = viddef.height; }

	*rx0 = x0; *ry0 = y0; *rx1 = x1; *ry1 = y1;

	return (x1 > x0 && y1 > y0);
}

/*
 * True if (px, py) is inside (or on the edge of) the convex polygon
 * given by (sx[i], sy[i]), i in [0, n). Works for either winding
 * order: the sign of the first nonzero edge cross product sets the
 * expected sign, and any edge that disagrees means the point is
 * outside. BSP faces are always convex, so this is exact for them
 * (unlike a bounding-box test, which isn't).
 */
static qboolean
R_PointInConvexPoly2D(float px, float py, const float *sx, const float *sy, int n)
{
	int i;
	int sign = 0;

	for (i = 0; i < n; i++)
	{
		int j = (i + 1) % n;
		float ex = sx[j] - sx[i];
		float ey = sy[j] - sy[i];
		float cross = ex * (py - sy[i]) - ey * (px - sx[i]);

		if (cross != 0.0f)
		{
			int s = (cross > 0.0f) ? 1 : -1;

			if (sign == 0)
			{
				sign = s;
			}
			else if (s != sign)
			{
				return false;
			}
		}
	}

	return true;
}

#define R_OCCL_MAX_POLY_VERTS 64

/*
 * Marks the screen-space footprint of an opaque surface's own polygon
 * (not its parent leaf/node's much looser bounding box) as covered --
 * exactly, not by bounding box. A diagonal/angled wall's axis-aligned
 * screen bbox can cover a lot of area outside the wall itself; marking
 * that whole bbox as covered would wrongly hide anything actually
 * visible in the leftover corners. Instead, only a grid cell whose
 * all 4 corners land inside the (convex) polygon gets marked -- since
 * the polygon is convex, that guarantees the cell's whole rectangle is
 * inside it too, so this stays exact/safe rather than approximate.
 *
 * If any vertex fails to project (behind the near plane) or there are
 * more than R_OCCL_MAX_POLY_VERTS verts, this poly is skipped for
 * marking entirely rather than guessed at -- under-marking only costs
 * performance, never correctness.
 */
static void
R_OcclusionMarkSurface(const msurface_t *surf)
{
	const glpoly_t *p;

	for (p = surf->polys; p; p = p->chain)
	{
		float sx[R_OCCL_MAX_POLY_VERTS], sy[R_OCCL_MAX_POLY_VERTS];
		float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
		int vi, cx, cy, cx0, cy0, cx1, cy1;

		if (p->numverts < 3 || p->numverts > R_OCCL_MAX_POLY_VERTS)
		{
			r_occl_surfs_skipped++;
			continue;
		}

		for (vi = 0; vi < p->numverts; vi++)
		{
			if (!R_WorldPointToScreen(p->verts[vi], &sx[vi], &sy[vi]))
			{
				break;
			}

			if (vi == 0)
			{
				x0 = x1 = sx[vi];
				y0 = y1 = sy[vi];
			}
			else
			{
				if (sx[vi] < x0) { x0 = sx[vi]; }
				if (sx[vi] > x1) { x1 = sx[vi]; }
				if (sy[vi] < y0) { y0 = sy[vi]; }
				if (sy[vi] > y1) { y1 = sy[vi]; }
			}
		}

		if (vi != p->numverts)
		{
			r_occl_surfs_skipped++;
			continue; /* a vertex failed to project -- skip this poly */
		}

		if (x0 < 0) { x0 = 0; }
		if (y0 < 0) { y0 = 0; }
		if (x1 > viddef.width) { x1 = viddef.width; }
		if (y1 > viddef.height) { y1 = viddef.height; }

		if (x1 <= x0 || y1 <= y0)
		{
			r_occl_surfs_skipped++;
			continue;
		}

		r_occl_surfs_projected++;

		R_OcclusionGridCellRange(x0, y0, x1, y1, &cx0, &cy0, &cx1, &cy1);

		for (cy = cy0; cy < cy1; cy++)
		{
			for (cx = cx0; cx < cx1; cx++)
			{
				float px0 = (float)cx / R_OCCL_GRID_W * viddef.width;
				float py0 = (float)cy / R_OCCL_GRID_H * viddef.height;
				float px1 = (float)(cx + 1) / R_OCCL_GRID_W * viddef.width;
				float py1 = (float)(cy + 1) / R_OCCL_GRID_H * viddef.height;

				if (R_PointInConvexPoly2D(px0, py0, sx, sy, p->numverts) &&
					R_PointInConvexPoly2D(px1, py0, sx, sy, p->numverts) &&
					R_PointInConvexPoly2D(px0, py1, sx, sy, p->numverts) &&
					R_PointInConvexPoly2D(px1, py1, sx, sy, p->numverts))
				{
					if (!r_occl_grid[cy * R_OCCL_GRID_W + cx])
					{
						r_occl_cells_marked++;
					}
					r_occl_grid[cy * R_OCCL_GRID_W + cx] = true;
				}
			}
		}
	}
}
qboolean LM_AllocBlock(int w, int h, int *x, int *y);

void R_SetCacheState(msurface_t *surf);
void R_BuildLightMap(msurface_t *surf, byte *dest, int stride);

static void
R_DrawGLPoly(msurface_t *fa)
{
	int i, nv;
	float *v, scroll;

	v = fa->polys->verts[0];
	nv = fa->polys->numverts;

	if (fa->texinfo->flags & SURF_FLOWING)
	{
		scroll = -64 * ((r_newrefdef.time / 40.0) - (int)(r_newrefdef.time / 40.0));

		if (scroll == 0.0)
		{
			scroll = -64.0;
		}
	}
	else
	{
		scroll = 0.0;
	}

	R_SetBufferIndices(GL_TRIANGLE_FAN, nv);

	for ( i = 0; i < nv; i++, v += VERTEXSIZE )
	{
		GLBUFFER_VERTEX( v[0], v[1], v[2] )
		GLBUFFER_SINGLETEX( v[3] + scroll, v[4] )
	}
}

static void
R_DrawTriangleOutlines(void)
{
	int i, j;
	glpoly_t *p;

	if (!gl_showtris->value)
	{
		return;
	}

	glDisable(GL_TEXTURE_2D);
	glDisable(GL_DEPTH_TEST);
	glColor4f(1, 1, 1, 1);

	for (i = 0; i < MAX_LIGHTMAPS; i++)
	{
		msurface_t *surf;

		for (surf = gl_lms.lightmap_surfaces[i];
			 surf != 0;
			 surf = surf->lightmapchain)
		{
			p = surf->polys;

			for ( ; p; p = p->chain)
			{
				for (j = 2; j < p->numverts; j++)
				{
                    GLfloat vtx[12];
                    unsigned int k;

                    for (k=0; k<3; k++)
                    {
                        vtx[0+k] = p->verts [ 0 ][ k ];
                        vtx[3+k] = p->verts [ j - 1 ][ k ];
                        vtx[6+k] = p->verts [ j ][ k ];
                        vtx[9+k] = p->verts [ 0 ][ k ];
                    }

                    glEnableClientState( GL_VERTEX_ARRAY );

                    glVertexPointer( 3, GL_FLOAT, 0, vtx );
                    glDrawArrays( GL_LINE_STRIP, 0, 4 );

                    glDisableClientState( GL_VERTEX_ARRAY );
				}
			}
		}
	}

	glEnable(GL_DEPTH_TEST);
	glEnable(GL_TEXTURE_2D);
}

static void
R_DrawGLPolyChain(glpoly_t *p, float soffset, float toffset)
{
	if ((soffset == 0) && (toffset == 0))
	{
		for ( ; p != 0; p = p->chain)
		{
			float *v;

			v = p->verts[0];

            glEnableClientState( GL_VERTEX_ARRAY );
            glEnableClientState( GL_TEXTURE_COORD_ARRAY );

            glVertexPointer( 3, GL_FLOAT, VERTEXSIZE*sizeof(GLfloat), v );
            glTexCoordPointer( 2, GL_FLOAT, VERTEXSIZE*sizeof(GLfloat), v+5 );
            glDrawArrays( GL_TRIANGLE_FAN, 0, p->numverts );

            glDisableClientState( GL_VERTEX_ARRAY );
            glDisableClientState( GL_TEXTURE_COORD_ARRAY );
		}
	}
	else
	{
		// workaround for lack of VLAs (=> our workaround uses alloca() which is bad in loops)
#ifdef _MSC_VER
		int maxNumVerts = 0;
		for (glpoly_t* tmp = p; tmp; tmp = tmp->chain)
		{
			if ( tmp->numverts > maxNumVerts )
				maxNumVerts = tmp->numverts;
		}

		YQ2_VLA( GLfloat, tex, 2 * maxNumVerts );
#endif

		for ( ; p != 0; p = p->chain)
		{
			float *v;
			int j;

			v = p->verts[0];
#ifndef _MSC_VER // we have real VLAs, so it's safe to use one in this loop
            YQ2_VLA(GLfloat, tex, 2*p->numverts);
#endif

            unsigned int index_tex = 0;

			for ( j = 0; j < p->numverts; j++, v += VERTEXSIZE )
			{
			    tex[index_tex++] = v [ 5 ] - soffset;
			    tex[index_tex++] = v [ 6 ] - toffset;
			}

			v = p->verts [ 0 ];

            glEnableClientState( GL_VERTEX_ARRAY );
            glEnableClientState( GL_TEXTURE_COORD_ARRAY );

            glVertexPointer( 3, GL_FLOAT, VERTEXSIZE*sizeof(GLfloat), v );
            glTexCoordPointer( 2, GL_FLOAT, 0, tex );
            glDrawArrays( GL_TRIANGLE_FAN, 0, p->numverts );

            glDisableClientState( GL_VERTEX_ARRAY );
            glDisableClientState( GL_TEXTURE_COORD_ARRAY );
		}

		YQ2_VLAFREE( tex );
	}
}

/*
 * This routine takes all the given light mapped surfaces
 * in the world and blends them into the framebuffer.
 */
static void
R_BlendLightmaps(const model_t *currentmodel)
{
	int i;
	msurface_t *surf, *newdrawsurf = 0;

	/* don't bother if we're set to fullbright or multitexture is enabled */
	if (gl_config.multitexture || r_fullbright->value || !r_worldmodel->lightdata)
	{
		return;
	}

	/* don't bother writing Z */
	glDepthMask(GL_FALSE);

	/* set the appropriate blending mode unless
	   we're only looking at the lightmaps. */
	if (!gl_lightmap->value)
	{
		glEnable(GL_BLEND);

		if (gl1_saturatelighting->value)
		{
			glBlendFunc(GL_ONE, GL_ONE);
		}
		else
		{
			glBlendFunc(GL_ZERO, GL_SRC_COLOR);
		}
	}

	if (currentmodel == r_worldmodel)
	{
		c_visible_lightmaps = 0;
	}

	/* render static lightmaps first */
	for (i = 1; i < MAX_LIGHTMAPS; i++)
	{
		if (gl_lms.lightmap_surfaces[i])
		{
			if (currentmodel == r_worldmodel)
			{
				c_visible_lightmaps++;
			}

			R_Bind(gl_state.lightmap_textures + i);

			for (surf = gl_lms.lightmap_surfaces[i];
				 surf != 0;
				 surf = surf->lightmapchain)
			{
				if (surf->polys)
				{
					// Apply overbright bits to the static lightmaps
					if (gl1_overbrightbits->value)
					{
						R_TexEnv(GL_COMBINE);
						glTexEnvi(GL_TEXTURE_ENV, GL_RGB_SCALE, gl1_overbrightbits->value);
					}

					R_DrawGLPolyChain(surf->polys, 0, 0);
				}
			}
		}
	}

	/* render dynamic lightmaps */
	if (gl1_dynamic->value)
	{
		LM_InitBlock();

		R_Bind(gl_state.lightmap_textures + 0);

		if (currentmodel == r_worldmodel)
		{
			c_visible_lightmaps++;
		}

		newdrawsurf = gl_lms.lightmap_surfaces[0];

		for (surf = gl_lms.lightmap_surfaces[0];
			 surf != 0;
			 surf = surf->lightmapchain)
		{
			int smax, tmax;
			byte *base;

			smax = (surf->extents[0] >> 4) + 1;
			tmax = (surf->extents[1] >> 4) + 1;

			if (LM_AllocBlock(smax, tmax, &surf->dlight_s, &surf->dlight_t))
			{
				base = gl_lms.lightmap_buffer[0];
				base += (surf->dlight_t * BLOCK_WIDTH +
						surf->dlight_s) * LIGHTMAP_BYTES;

				R_BuildLightMap(surf, base, BLOCK_WIDTH * LIGHTMAP_BYTES);
			}
			else
			{
				msurface_t *drawsurf;

				/* upload what we have so far */
				LM_UploadBlock(true);

				/* draw all surfaces that use this lightmap */
				for (drawsurf = newdrawsurf;
					 drawsurf != surf;
					 drawsurf = drawsurf->lightmapchain)
				{
					if (drawsurf->polys)
					{
						// Apply overbright bits to the dynamic lightmaps
						if (gl1_overbrightbits->value)
						{
							R_TexEnv(GL_COMBINE);
							glTexEnvi(GL_TEXTURE_ENV, GL_RGB_SCALE, gl1_overbrightbits->value);
						}

						R_DrawGLPolyChain(drawsurf->polys,
								(drawsurf->light_s - drawsurf->dlight_s) * (float)(1.0 / BLOCK_WIDTH),
								(drawsurf->light_t - drawsurf->dlight_t) * (float)(1.0 / BLOCK_HEIGHT));
					}
				}

				newdrawsurf = drawsurf;

				/* clear the block */
				LM_InitBlock();

				/* try uploading the block now */
				if (!LM_AllocBlock(smax, tmax, &surf->dlight_s, &surf->dlight_t))
				{
					Com_Error(ERR_FATAL,
							"%s: Consecutive calls to LM_AllocBlock(%d,%d) failed (dynamic)\n",
							__func__, smax, tmax);
				}

				base = gl_lms.lightmap_buffer[0];
				base += (surf->dlight_t * BLOCK_WIDTH +
						surf->dlight_s) * LIGHTMAP_BYTES;

				R_BuildLightMap(surf, base, BLOCK_WIDTH * LIGHTMAP_BYTES);
			}
		}

		/* draw remainder of dynamic lightmaps that haven't been uploaded yet */
		if (newdrawsurf)
		{
			LM_UploadBlock(true);
		}

		for (surf = newdrawsurf; surf != 0; surf = surf->lightmapchain)
		{
			if (surf->polys)
			{
				// Apply overbright bits to the remainder lightmaps
				if (gl1_overbrightbits->value)
				{
					R_TexEnv(GL_COMBINE);
					glTexEnvi(GL_TEXTURE_ENV, GL_RGB_SCALE, gl1_overbrightbits->value);
				}

				R_DrawGLPolyChain(surf->polys,
						(surf->light_s - surf->dlight_s) * (float)(1.0 / BLOCK_WIDTH),
						(surf->light_t - surf->dlight_t) * (float)(1.0 / BLOCK_HEIGHT));
			}
		}
	}

	/* restore state */
	glDisable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDepthMask(GL_TRUE);
}

static void
R_RenderBrushPoly(msurface_t *fa)
{
	int maps;
	qboolean is_dynamic = false;

	c_brush_polys++;

	if (fa->flags & SURF_DRAWTURB)
	{
		R_EmitWaterPolys(fa);
		return;
	}

	R_DrawGLPoly(fa);

	if (gl_config.multitexture)
	{
		return;	// lighting already done at this point for mtex
	}

	/* check for lightmap modification */
	for (maps = 0; maps < MAXLIGHTMAPS && fa->styles[maps] != 255; maps++)
	{
		if (r_newrefdef.lightstyles[fa->styles[maps]].white !=
			fa->cached_light[maps])
		{
			goto dynamic;
		}
	}

	/* dynamic this frame or dynamic previously */
	if (fa->dlightframe == r_framecount)
	{
	dynamic:

		if (gl1_dynamic->value)
		{
			if (!(fa->texinfo->flags &
				  (SURF_SKY | SURF_TRANS33 |
				   SURF_TRANS66 | SURF_WARP)))
			{
				is_dynamic = true;
			}
		}
	}

	if (is_dynamic)
	{
		if (maps < MAXLIGHTMAPS &&
			((fa->styles[maps] >= 32) ||
			 (fa->styles[maps] == 0)) &&
			  (fa->dlightframe != r_framecount))
		{
			unsigned temp[34 * 34];
			int smax, tmax;

			smax = (fa->extents[0] >> 4) + 1;
			tmax = (fa->extents[1] >> 4) + 1;

			R_BuildLightMap(fa, (void *)temp, smax * 4);
			R_SetCacheState(fa);

			R_Bind(gl_state.lightmap_textures + fa->lightmaptexturenum);

			glTexSubImage2D(GL_TEXTURE_2D, 0, fa->light_s, fa->light_t,
					smax, tmax, GL_LIGHTMAP_FORMAT, GL_UNSIGNED_BYTE, temp);

			fa->lightmapchain = gl_lms.lightmap_surfaces[fa->lightmaptexturenum];
			gl_lms.lightmap_surfaces[fa->lightmaptexturenum] = fa;
		}
		else
		{
			fa->lightmapchain = gl_lms.lightmap_surfaces[0];
			gl_lms.lightmap_surfaces[0] = fa;
		}
	}
	else
	{
		fa->lightmapchain = gl_lms.lightmap_surfaces[fa->lightmaptexturenum];
		gl_lms.lightmap_surfaces[fa->lightmaptexturenum] = fa;
	}
}

/*
 * Draw water surfaces and windows.
 * The BSP tree is waled front to back, so unwinding the chain
 * of alpha_surfaces will draw back to front, giving proper ordering.
 */
void
R_DrawAlphaSurfaces(void)
{
	msurface_t *s;
	float alpha;

	/* go back to the world matrix */
	glLoadMatrixf(r_world_matrix);

	glEnable(GL_BLEND);
	R_TexEnv(GL_MODULATE);

	for (s = r_alpha_surfaces; s; s = s->texturechain)
	{
		c_brush_polys++;

		if (s->texinfo->flags & SURF_TRANS33)
		{
			alpha = 0.33f;
		}
		else if (s->texinfo->flags & SURF_TRANS66)
		{
			alpha = 0.66f;
		}
		else
		{
			alpha = 1.0f;
		}

		R_UpdateGLBuffer(buf_alpha, s->texinfo->image->texnum, 0, 0, alpha);

		if (s->flags & SURF_DRAWTURB)
		{
			R_EmitWaterPolys(s);
		}
		else
		{
			R_DrawGLPoly(s);
		}
	}
	R_ApplyGLBuffer();	// Flush the last batched array

	R_TexEnv(GL_REPLACE);
	glColor4f(1, 1, 1, 1);
	glDisable(GL_BLEND);

	r_alpha_surfaces = NULL;
}

static void
R_RenderLightmappedPoly(msurface_t *surf)
{
	const int nv = surf->polys->numverts;
	int i;
	float scroll;
	float *v;

	c_brush_polys++;
	v = surf->polys->verts[0];

	if (surf->texinfo->flags & SURF_FLOWING)
	{
		scroll = -64 * ((r_newrefdef.time / 40.0) - (int) (r_newrefdef.time / 40.0));

		if (scroll == 0.0)
		{
			scroll = -64.0;
		}
	}
	else
	{
		scroll = 0.0;
	}

	R_SetBufferIndices(GL_TRIANGLE_FAN, nv);

	for (i = 0; i < nv; i++, v += VERTEXSIZE)
	{
		GLBUFFER_VERTEX( v[0], v[1], v[2] )
		GLBUFFER_MULTITEX( v[3] + scroll, v[4], v[5], v[6] )
	}
}

/* Add "adding" area to "obj" */
static void
R_JoinAreas(lmrect_t *adding, lmrect_t *obj)
{
	if (adding->top < obj->top)
	{
		obj->top = adding->top;
	}
	if (adding->bottom > obj->bottom)
	{
		obj->bottom = adding->bottom;
	}
	if (adding->left < obj->left)
	{
		obj->left = adding->left;
	}
	if (adding->right > obj->right)
	{
		obj->right = adding->right;
	}
}

/* Upload dynamic lights to each lightmap texture (multitexture path only) */
static void
R_RegenAllLightmaps()
{
	static lmrect_t lmchange[MAX_LIGHTMAPS][MAX_LIGHTMAP_COPIES];
	static qboolean altered[MAX_LIGHTMAPS][MAX_LIGHTMAP_COPIES];

	int i, lmtex;
#ifndef YQ2_GL1_GLES
	qboolean pixelstore_set = false;
#endif

	if ( !gl_config.multitexture || r_fullbright->value || !gl1_dynamic->value )
	{
		return;
	}

	if (gl_config.lightmapcopies)
	{
		cur_lm_copy = (cur_lm_copy + 1) % MAX_LIGHTMAP_COPIES;	// select the next lightmap copy
		lmtex = MAX_LIGHTMAPS * cur_lm_copy;	// ...and its corresponding texture
	}
	else
	{
		lmtex = 0;
	}

	for (i = 1; i < MAX_LIGHTMAPS; i++)
	{
		lmrect_t current, best;
		msurface_t *surf;
		byte *base;
		qboolean affected_lightmap;

		if (!gl_lms.lightmap_surfaces[i] || !gl_lms.lightmap_buffer[i])
		{
			continue;
		}

		affected_lightmap = false;
		best.top = BLOCK_HEIGHT;
		best.left = BLOCK_WIDTH;
		best.bottom = best.right = 0;

		for (surf = gl_lms.lightmap_surfaces[i];
			 surf != 0;
			 surf = surf->lightmapchain)
		{
			int map;

			if (surf->texinfo->flags & (SURF_SKY | SURF_TRANS33 | SURF_TRANS66 | SURF_WARP))
			{
				continue;
			}

			// Any dynamic lights on this surface?
			for (map = 0; map < MAXLIGHTMAPS && surf->styles[map] != 255; map++)
			{
				if (r_newrefdef.lightstyles[surf->styles[map]].white != surf->cached_light[map])
				{
					goto dynamic_surf;
				}
			}

			// Surface is considered to have dynamic lights if it had them in the previous frame
			if ( surf->dlightframe != r_framecount && !surf->dirty_lightmap )
			{
				continue;	// no dynamic lights affect this surface in this frame
			}

dynamic_surf:
			affected_lightmap = true;

			current.left = surf->light_s;
			current.right = surf->light_s + (surf->extents[0] >> 4) + 1;	// + smax
			current.top = surf->light_t;
			current.bottom = surf->light_t + (surf->extents[1] >> 4) + 1;	// + tmax

			base = gl_lms.lightmap_buffer[i];
			base += (current.top * BLOCK_WIDTH + current.left) * LIGHTMAP_BYTES;

			R_BuildLightMap(surf, base, BLOCK_WIDTH * LIGHTMAP_BYTES);

			surf->dirty_lightmap = (surf->dlightframe == r_framecount);
			if (!surf->dirty_lightmap || gl_config.lightmapcopies)
			{
				for (map = 0; map < MAXLIGHTMAPS && surf->styles[map] != 255; map++)
				{
					if ( (surf->styles[map] >= 32) || (surf->styles[map] == 0) )
					{
						R_SetCacheState(surf);
						break;
					}
				}
			}
			R_JoinAreas(&current, &best);
		}

		if (!gl_config.lightmapcopies && !affected_lightmap)
		{
			continue;
		}

		if (gl_config.lightmapcopies)
		{
			// Add all the changes that have happened in the last few frames,
			// at least just for consistency between them.
			qboolean apply_changes = affected_lightmap;
			current = best;		// save state for next frames... +

			for (int k = 0; k < MAX_LIGHTMAP_COPIES; k++)
			{
				if (altered[i][k])
				{
					apply_changes = true;
					R_JoinAreas(&lmchange[i][k], &best);
				}
			}

			altered[i][cur_lm_copy] = affected_lightmap;
			if (affected_lightmap)
			{
				lmchange[i][cur_lm_copy] = current;	// + ...here
			}

			if (!apply_changes)
			{
				continue;
			}
		}

#ifndef YQ2_GL1_GLES
		if (!pixelstore_set)
		{
			glPixelStorei(GL_UNPACK_ROW_LENGTH, BLOCK_WIDTH);
			pixelstore_set = true;
		}
#endif

		// upload changes
		base = gl_lms.lightmap_buffer[i];

#ifdef YQ2_GL1_GLES
		base += (best.top * BLOCK_WIDTH) * LIGHTMAP_BYTES;

		R_Bind(gl_state.lightmap_textures + i + lmtex);

		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, best.top,
			BLOCK_WIDTH, best.bottom - best.top,
			GL_LIGHTMAP_FORMAT, GL_UNSIGNED_BYTE, base);
#else
		base += (best.top * BLOCK_WIDTH + best.left) * LIGHTMAP_BYTES;

		R_Bind(gl_state.lightmap_textures + i + lmtex);

		glTexSubImage2D(GL_TEXTURE_2D, 0, best.left, best.top,
			best.right - best.left, best.bottom - best.top,
			GL_LIGHTMAP_FORMAT, GL_UNSIGNED_BYTE, base);
#endif
	}

#ifndef YQ2_GL1_GLES
	if (pixelstore_set)
	{
		glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	}
#endif
}

static void
R_DrawTextureChains(void)
{
	int i;
	msurface_t *s;
	image_t *image;

	c_visible_textures = 0;

	if (!gl_config.multitexture)	// classic path
	{
		for (i = 0, image = gltextures; i < numgltextures; i++, image++)
		{
			if (!image->registration_sequence)
			{
				continue;
			}

			s = image->texturechain;

			if (!s)
			{
				continue;
			}

			c_visible_textures++;

			for ( ; s; s = s->texturechain)
			{
				R_UpdateGLBuffer(buf_singletex, image->texnum, 0, s->flags, 1);
				R_RenderBrushPoly(s);
			}

			image->texturechain = NULL;
		}
		R_ApplyGLBuffer();	// Flush the last batched array
	}
	else	// multitexture
	{
		for (i = 0, image = gltextures; i < numgltextures; i++, image++)
		{
			if (!image->registration_sequence || !image->texturechain)
			{
				continue;
			}

			c_visible_textures++;

			for (s = image->texturechain; s; s = s->texturechain)
			{
				if (!(s->flags & SURF_DRAWTURB))
				{
#ifdef __EMSCRIPTEN__
					int svbo_base = R_SVBO_BaseVertexForSurface(s);

					if (svbo_base >= 0)
					{
						r_svbo_hits++;
						R_UpdateGLBuffer(buf_mtex_svbo, image->texnum, s->lightmaptexturenum, 0, 1);
						c_brush_polys++;
						R_SVBO_AppendIndices(GL_TRIANGLE_FAN, svbo_base, s->polys->numverts);
					}
					else
#endif
					{
#ifdef __EMSCRIPTEN__
						r_svbo_misses++;
#endif
						R_UpdateGLBuffer(buf_mtex, image->texnum, s->lightmaptexturenum, 0, 1);
						R_RenderLightmappedPoly(s);
					}
				}
			}
		}
		R_ApplyGLBuffer();

		R_EnableMultitexture(false);	// force disabling, SURF_DRAWTURB surfaces may not exist

		for (i = 0, image = gltextures; i < numgltextures; i++, image++)
		{
			if (!image->registration_sequence || !image->texturechain)
			{
				continue;
			}

			for (s = image->texturechain; s; s = s->texturechain)
			{
				if (s->flags & SURF_DRAWTURB)
				{
					R_UpdateGLBuffer(buf_singletex, image->texnum, 0, s->flags, 1);
					R_RenderBrushPoly(s);
				}
			}

			image->texturechain = NULL;
		}
		R_ApplyGLBuffer();
	}
}

static void
R_DrawInlineBModel(entity_t *currententity, const model_t *currentmodel)
{
	int i, k;
	cplane_t *pplane;
	float dot;
	msurface_t *psurf;
	dlight_t *lt;
	image_t *image;

	/* calculate dynamic lighting for bmodel */
	if (!gl_config.multitexture && !gl1_flashblend->value)
	{
		lt = r_newrefdef.dlights;

		for (k = 0; k < r_newrefdef.num_dlights; k++, lt++)
		{
			R_MarkLights(lt, 1 << k,
				currentmodel->nodes + currentmodel->firstnode,
				r_dlightframecount, R_MarkSurfaceLights);
		}
	}

	psurf = &currentmodel->surfaces[currentmodel->firstmodelsurface];

	if (currententity->flags & RF_TRANSLUCENT)
	{
		glEnable(GL_BLEND);
		glColor4f(1, 1, 1, 0.25);
		R_TexEnv(GL_MODULATE);
	}

	/* draw texture */
	for (i = 0; i < currentmodel->nummodelsurfaces; i++, psurf++)
	{
		/* find which side of the node we are on */
		pplane = psurf->plane;

		dot = DotProduct(modelorg, pplane->normal) - pplane->dist;

		/* draw the polygon */
		if (((psurf->flags & SURF_PLANEBACK) && (dot < -BACKFACE_EPSILON)) ||
			(!(psurf->flags & SURF_PLANEBACK) && (dot > BACKFACE_EPSILON)))
		{
			if (psurf->texinfo->flags & (SURF_TRANS33 | SURF_TRANS66))
			{
				/* add to the translucent chain */
				psurf->texturechain = r_alpha_surfaces;
				r_alpha_surfaces = psurf;
			}
			else
			{
				image = R_TextureAnimation(currententity, psurf->texinfo);

				if (gl_config.multitexture && !(psurf->flags & SURF_DRAWTURB))
				{
					// Dynamic lighting already generated in R_GetBrushesLighting()
					R_UpdateGLBuffer(buf_mtex, image->texnum, psurf->lightmaptexturenum, 0, 1);
					R_RenderLightmappedPoly(psurf);
				}
				else
				{
					R_UpdateGLBuffer(buf_singletex, image->texnum, 0, psurf->flags, 1);
					R_RenderBrushPoly(psurf);
				}
			}
		}
	}
	R_ApplyGLBuffer();

	if (!(currententity->flags & RF_TRANSLUCENT))
	{
		R_BlendLightmaps(currentmodel);
	}
	else
	{
		glDisable(GL_BLEND);
		glColor4f(1, 1, 1, 1);
		R_TexEnv(GL_REPLACE);
	}
}

void
R_DrawBrushModel(entity_t *currententity, const model_t *currentmodel)
{
	vec3_t mins, maxs;
	int i;
	qboolean rotated;

	if (currentmodel->nummodelsurfaces == 0)
	{
		return;
	}

	gl_state.currenttextures[0] = gl_state.currenttextures[1] = -1;

	if (currententity->angles[0] || currententity->angles[1] || currententity->angles[2])
	{
		rotated = true;

		for (i = 0; i < 3; i++)
		{
			mins[i] = currententity->origin[i] - currentmodel->radius;
			maxs[i] = currententity->origin[i] + currentmodel->radius;
		}
	}
	else
	{
		rotated = false;
		VectorAdd(currententity->origin, currentmodel->mins, mins);
		VectorAdd(currententity->origin, currentmodel->maxs, maxs);
	}

	if (r_cull->value && R_CullBox(mins, maxs, frustum))
	{
		return;
	}

	if (gl_zfix->value)
	{
		glEnable(GL_POLYGON_OFFSET_FILL);
	}

	glColor4f(1, 1, 1, 1);
	memset(gl_lms.lightmap_surfaces, 0, sizeof(gl_lms.lightmap_surfaces));

	VectorSubtract(r_newrefdef.vieworg, currententity->origin, modelorg);

	if (rotated)
	{
		vec3_t temp;
		vec3_t forward, right, up;

		VectorCopy(modelorg, temp);
		AngleVectors(currententity->angles, forward, right, up);
		modelorg[0] = DotProduct(temp, forward);
		modelorg[1] = -DotProduct(temp, right);
		modelorg[2] = DotProduct(temp, up);
	}

	glPushMatrix();
	currententity->angles[0] = -currententity->angles[0];
	currententity->angles[2] = -currententity->angles[2];
	R_RotateForEntity(currententity);
	currententity->angles[0] = -currententity->angles[0];
	currententity->angles[2] = -currententity->angles[2];

	if (gl_lightmap->value)
	{
		R_TexEnv(GL_REPLACE);
	}
	else
	{
		R_TexEnv(GL_MODULATE);
	}

	R_DrawInlineBModel(currententity, currentmodel);

	glPopMatrix();

	if (gl_zfix->value)
	{
		glDisable(GL_POLYGON_OFFSET_FILL);
	}
}

/*
 * Squared distance from r_origin (the viewer) to the closest point on an
 * axis-aligned box -- 0 if the viewer is inside or touching the box.
 * Used by r_distcull_dist below: comparing squared distances avoids a
 * sqrt() on a call that can run thousands of times per frame walking a
 * big outdoor map's BSP tree. Same helper as the software renderer's
 * R_BoxDistSqFromOrigin (sw_bsp.c).
 */
static float
R_BoxDistSqFromOrigin(const vec3_t mins, const vec3_t maxs)
{
	float distsq = 0;
	int i;

	for (i = 0; i < 3; i++)
	{
		float v = r_origin[i];

		if (v < mins[i])
		{
			float d = mins[i] - v;
			distsq += d * d;
		}
		else if (v > maxs[i])
		{
			float d = v - maxs[i];
			distsq += d * d;
		}
	}

	return distsq;
}

static void
R_RecursiveWorldNode(entity_t *currententity, mnode_t *node)
{
	int c, side, sidebit;
	cplane_t *plane;
	msurface_t *surf, **mark;
	mleaf_t *pleaf;
	float dot;
	image_t *image;

	if (node->contents == CONTENTS_SOLID)
	{
		return; /* solid */
	}

	if (node->visframe != r_visframecount)
	{
		return;
	}

	if (r_cull->value && R_CullBox(node->minmaxs, node->minmaxs + 3, frustum))
	{
		return;
	}

	/* Cheap draw-distance cutoff, same trick and same cvar as the
	 * software renderer (see gl1_main.c's r_distcull_dist registration
	 * comment) -- skip whole BSP nodes farther than this from the
	 * viewer before any of their surfaces reach the draw calls below. */
	if (r_distcull_dist->value > 0)
	{
		float cutoff = r_distcull_dist->value;

		if (R_BoxDistSqFromOrigin(node->minmaxs, node->minmaxs + 3) > cutoff * cutoff)
		{
			return;
		}
	}

	if (r_occl_grid_active)
	{
		float rx0, ry0, rx1, ry1;

		r_occl_nodes_tested++;

		if (R_BoxToScreenRect(node->minmaxs, node->minmaxs + 3, &rx0, &ry0, &rx1, &ry1) &&
			R_OcclusionRectCovered(rx0, ry0, rx1, ry1))
		{
			r_occl_nodes_culled++;
			return;
		}
	}

	/* if a leaf node, draw stuff */
	if (node->contents != CONTENTS_NODE)
	{
		pleaf = (mleaf_t *)node;

		/* check for door connected areas */
		if (!R_AreaVisible(r_newrefdef.areabits, pleaf))
			return;	// not visible

		mark = pleaf->firstmarksurface;
		c = pleaf->nummarksurfaces;

		if (c)
		{
			do
			{
				(*mark)->visframe = r_framecount;
				mark++;
			}
			while (--c);
		}

		return;
	}

	/* node is just a decision point, so go down the apropriate
	   sides find which side of the node we are on */
	plane = node->plane;

	switch (plane->type)
	{
		case PLANE_X:
			dot = modelorg[0] - plane->dist;
			break;
		case PLANE_Y:
			dot = modelorg[1] - plane->dist;
			break;
		case PLANE_Z:
			dot = modelorg[2] - plane->dist;
			break;
		default:
			dot = DotProduct(modelorg, plane->normal) - plane->dist;
			break;
	}

	if (dot >= 0)
	{
		side = 0;
		sidebit = 0;
	}
	else
	{
		side = 1;
		sidebit = SURF_PLANEBACK;
	}

	/* recurse down the children, front side first */
	R_RecursiveWorldNode(currententity, node->children[side]);

	/* draw stuff */
	for (c = node->numsurfaces,
		 surf = r_worldmodel->surfaces + node->firstsurface;
		 c; c--, surf++)
	{
		if (surf->visframe != r_framecount)
		{
			continue;
		}

		if ((surf->flags & SURF_PLANEBACK) != sidebit)
		{
			continue; /* wrong side */
		}

		if (surf->texinfo->flags & SURF_SKY)
		{
			/* just adds to visible sky bounds */
			R_AddSkySurface(surf);
		}
		else if (surf->texinfo->flags & (SURF_TRANS33 | SURF_TRANS66))
		{
			/* add to the translucent chain */
			surf->texturechain = r_alpha_surfaces;
			r_alpha_surfaces = surf;
			r_alpha_surfaces->texinfo->image = R_TextureAnimation(currententity, surf->texinfo);
		}
		else
		{
			/* the polygon is visible, so add it to the texture sorted chain */
			image = R_TextureAnimation(currententity, surf->texinfo);
			surf->texturechain = image->texturechain;
			image->texturechain = surf;

			if (gl_config.multitexture && !(surf->texinfo->flags & SURF_WARP))	// needed for R_RegenAllLightmaps()
			{
				surf->lightmapchain = gl_lms.lightmap_surfaces[surf->lightmaptexturenum];
				gl_lms.lightmap_surfaces[surf->lightmaptexturenum] = surf;
			}

			/* This surface is opaque and confirmed visible -- mark its
			 * own screen footprint covered so farther nodes later in
			 * this same front-to-back walk can be occlusion-culled
			 * against it (see R_OcclusionGridClear's comment). */
			if (r_occl_grid_active)
			{
				R_OcclusionMarkSurface(surf);
			}
		}
	}

	/* recurse down the back side */
	R_RecursiveWorldNode(currententity, node->children[!side]);
}

/*
 * This is for the RegenAllLightmaps() function to be able to regenerate
 * lighting not only for the world, but also for the brushes in the entity list.
 * Logic extracted from R_DrawBrushModel() & R_DrawInlineBModel().
 */
static void
R_GetBrushesLighting(void)
{
	int i, k;
	vec3_t mins, maxs;
	msurface_t *surf;
	cplane_t *pplane;
	dlight_t *lt;
	float dot;

	if (!gl_config.multitexture || !r_drawentities->value || gl1_flashblend->value)
	{
		return;
	}

	for (i = 0; i < r_newrefdef.num_entities; i++)
	{
		entity_t *currententity = &r_newrefdef.entities[i];

		if (currententity->flags & RF_BEAM)
		{
			continue;
		}

		const model_t *currentmodel = currententity->model;

		if (!currentmodel || currentmodel->type != mod_brush || currentmodel->nummodelsurfaces == 0)
		{
			continue;
		}

		// from R_DrawBrushModel()
		if (currententity->angles[0] || currententity->angles[1] || currententity->angles[2])
		{
			for (k = 0; k < 3; k++)
			{
				mins[k] = currententity->origin[k] - currentmodel->radius;
				maxs[k] = currententity->origin[k] + currentmodel->radius;
			}
		}
		else
		{
			VectorAdd(currententity->origin, currentmodel->mins, mins);
			VectorAdd(currententity->origin, currentmodel->maxs, maxs);
		}

		if (r_cull->value && R_CullBox(mins, maxs, frustum))
		{
			continue;
		}

		// from R_DrawInlineBModel()
		lt = r_newrefdef.dlights;

		for (k = 0; k < r_newrefdef.num_dlights; k++, lt++)
		{
			R_MarkLights(lt, 1 << k,
				currentmodel->nodes + currentmodel->firstnode,
				r_dlightframecount, R_MarkSurfaceLights);
		}

		surf = &currentmodel->surfaces[currentmodel->firstmodelsurface];

		for (k = 0; k < currentmodel->nummodelsurfaces; k++, surf++)
		{
			if (surf->texinfo->flags & (SURF_TRANS33 | SURF_TRANS66 | SURF_WARP)
				|| surf->flags & SURF_DRAWTURB || surf->lmchain_frame == r_framecount)
			{
				continue;	// either not affected by light, or already in the chain
			}

			// find which side of the node we are on
			pplane = surf->plane;
			dot = DotProduct(modelorg, pplane->normal) - pplane->dist;

			if (((surf->flags & SURF_PLANEBACK) && (dot < -BACKFACE_EPSILON)) ||
				(!(surf->flags & SURF_PLANEBACK) && (dot > BACKFACE_EPSILON)))
			{
				surf->lmchain_frame = r_framecount;	// don't add this twice to the chain
				surf->lightmapchain = gl_lms.lightmap_surfaces[surf->lightmaptexturenum];
				gl_lms.lightmap_surfaces[surf->lightmaptexturenum] = surf;
			}
		}
	}
}

void
R_DrawWorld(void)
{
	entity_t ent;

	if (!r_drawworld->value)
	{
		return;
	}

	if (r_newrefdef.rdflags & RDF_NOWORLDMODEL)
	{
		return;
	}

	VectorCopy(r_newrefdef.vieworg, modelorg);

	/* auto cycle the world frame for texture animation */
	memset(&ent, 0, sizeof(ent));
	ent.frame = (int)(r_newrefdef.time * 2);

	gl_state.currenttextures[0] = gl_state.currenttextures[1] = -1;

	glColor4f(1, 1, 1, 1);
	memset(gl_lms.lightmap_surfaces, 0, sizeof(gl_lms.lightmap_surfaces));

	R_ClearSkyBox();
	R_RecursiveWorldNode(&ent, r_worldmodel->nodes);
	R_GetBrushesLighting();
	R_RegenAllLightmaps();
	R_DrawTextureChains();
	R_BlendLightmaps(r_worldmodel);
	R_DrawSkyBox();
	R_DrawTriangleOutlines();
}

/*
 * Mark the leaves and nodes that are
 * in the PVS for the current cluster
 */
void
R_MarkLeaves(void)
{
	const byte *vis;
	YQ2_ALIGNAS_TYPE(int) byte fatvis[MAX_MAP_LEAFS / 8];
	mnode_t *node;
	int i, c;
	mleaf_t *leaf;
	int cluster;

	if ((r_oldviewcluster == r_viewcluster) &&
		(r_oldviewcluster2 == r_viewcluster2) &&
		!r_novis->value &&
		(r_viewcluster != -1))
	{
		return;
	}

	/* development aid to let you run around
	   and see exactly where the pvs ends */
	if (r_lockpvs->value)
	{
		return;
	}

	r_visframecount++;
	r_oldviewcluster = r_viewcluster;
	r_oldviewcluster2 = r_viewcluster2;

	if (r_novis->value || (r_viewcluster == -1) || !r_worldmodel->vis)
	{
		/* mark everything */
		for (i = 0; i < r_worldmodel->numleafs; i++)
		{
			r_worldmodel->leafs[i].visframe = r_visframecount;
		}

		for (i = 0; i < r_worldmodel->numnodes; i++)
		{
			r_worldmodel->nodes[i].visframe = r_visframecount;
		}

		return;
	}

	vis = Mod_ClusterPVS(r_viewcluster, r_worldmodel);

	/* may have to combine two clusters because of solid water boundaries */
	if (r_viewcluster2 != r_viewcluster)
	{
		memcpy(fatvis, vis, (r_worldmodel->numleafs + 7) / 8);
		vis = Mod_ClusterPVS(r_viewcluster2, r_worldmodel);
		c = (r_worldmodel->numleafs + 31) / 32;

		for (i = 0; i < c; i++)
		{
			((int *)fatvis)[i] |= ((int *)vis)[i];
		}

		vis = fatvis;
	}

	for (i = 0, leaf = r_worldmodel->leafs;
		 i < r_worldmodel->numleafs;
		 i++, leaf++)
	{
		cluster = leaf->cluster;

		if (cluster == -1)
		{
			continue;
		}

		if (vis[cluster >> 3] & (1 << (cluster & 7)))
		{
			node = (mnode_t *)leaf;

			do
			{
				if (node->visframe == r_visframecount)
				{
					break;
				}

				node->visframe = r_visframecount;
				node = node->parent;
			}
			while (node);
		}
	}
}
