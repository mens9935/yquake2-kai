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
 * This file implements the 2D stuff. For example the HUD and the
 * networkgraph.
 *
 * =======================================================================
 */

#include "header/client.h"

#ifdef __EMSCRIPTEN__
#include <malloc.h>
#include <emscripten/heap.h>
#endif

static float scr_con_current; /* aproaches scr_conlines at scr_conspeed */
static float scr_conlines; /* 0.0 to 1.0 lines of console to display */

static qboolean scr_initialized; /* ready to draw */

static int scr_draw_loading;

vrect_t scr_vrect; /* position of render window on screen */

cvar_t *scr_viewsize;
cvar_t *scr_conspeed;
cvar_t *scr_centertime;
cvar_t *scr_showturtle;
cvar_t *scr_showpause;

cvar_t *scr_netgraph;
cvar_t *scr_timegraph;
cvar_t *scr_debuggraph;
cvar_t *scr_graphheight;
cvar_t *scr_graphscale;
cvar_t *scr_graphshift;
cvar_t *scr_drawall;

cvar_t *r_hudscale; /* named for consistency with R1Q2 */
cvar_t *r_consolescale;
cvar_t *r_menuscale;

typedef struct
{
	int x1, y1, x2, y2;
} dirty_t;

static dirty_t scr_dirty, scr_old_dirty[2];

static char crosshair_pic[MAX_QPATH];
static int crosshair_width, crosshair_height;

extern cvar_t *cl_showfps;
extern cvar_t *crosshair_scale;
extern cvar_t *crosshair_color_r;
extern cvar_t *crosshair_color_g;
extern cvar_t *crosshair_color_b;
extern cvar_t *cl_showspeed;
extern void GetPlayerSpeed(float *, float *);

static void SCR_TimeRefresh_f(void);
static void SCR_Loading_f(void);

#define CHAR_SIZE 8

/*
 * A new packet was just parsed
 */
void
CL_AddNetgraph(void)
{
	int i;
	int in;
	int ping;

	/* if using the debuggraph for something
	   else, don't add the net lines */
	if (scr_debuggraph->value || scr_timegraph->value)
	{
		return;
	}

	for (i = 0; i < cls.netchan.dropped; i++)
	{
		SCR_DebugGraph(30, 0x40);
	}

	for (i = 0; i < cl.surpressCount; i++)
	{
		SCR_DebugGraph(30, 0xdf);
	}

	/* see what the latency was on this packet */
	in = cls.netchan.incoming_acknowledged & (CMD_BACKUP - 1);
	ping = cls.realtime - cl.cmd_time[in];
	ping /= 30;

	if (ping > 30)
	{
		ping = 30;
	}

	SCR_DebugGraph((float)ping, 0xd0);
}

typedef struct
{
	float value;
	int color;
} graphsamp_t;

static int current;
static graphsamp_t values[2024];

void
SCR_DebugGraph(float value, int color)
{
	values[current & 2023].value = value;
	values[current & 2023].color = color;
	current++;
}

static void
SCR_DrawDebugGraph(void)
{
	int a, x, y, w, i, h;
	float v;
	int color;

	/* draw the graph */
	w = scr_vrect.width;

	x = scr_vrect.x;
	y = scr_vrect.y + scr_vrect.height;
	Draw_Fill(x, y - scr_graphheight->value,
			w, scr_graphheight->value, 8);

	for (a = 0; a < w; a++)
	{
		i = (current - 1 - a + 1024) & 1023;
		v = values[i].value;
		color = values[i].color;
		v = v * scr_graphscale->value + scr_graphshift->value;

		if (v < 0)
		{
			v += scr_graphheight->value *
				 (1 + (int)(-v / scr_graphheight->value));
		}

		h = (int)v % (int)scr_graphheight->value;
		Draw_Fill(x + w - 1 - a, y - h, 1, h, color);
	}
}

static char scr_centerstring[1024];
static float scr_centertime_start; /* for slow victory printing */
static float scr_centertime_off;
static int scr_center_lines;
static int scr_erase_center;

/*
 * Called for important messages that should stay
 * in the center of the screen for a few moments
 */
void
SCR_CenterPrint(char *str)
{
	char *s;

	Q_strlcpy(scr_centerstring, str, sizeof(scr_centerstring));
	scr_centertime_off = scr_centertime->value;
	scr_centertime_start = cl.time;

	/* count the number of lines for centering */
	scr_center_lines = 1;
	s = str;

	while (*s)
	{
		if (*s == '\n')
		{
			scr_center_lines++;
		}

		s++;
	}

	/* echo it to the console */
	Com_Printf("\n\n\35\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\37\n\n");

	s = str;

	do
	{
		char line[64];
		int i, j, l;

		/* scan the width of the line */
		for (l = 0; l < 40; l++)
		{
			if ((s[l] == '\n') || !s[l])
			{
				break;
			}
		}

		for (i = 0; i < (40 - l) / 2; i++)
		{
			line[i] = ' ';
		}

		for (j = 0; j < l; j++)
		{
			line[i++] = s[j];
		}

		line[i] = '\n';
		line[i + 1] = 0;

		Com_Printf("%s", line);

		while (*s && *s != '\n')
		{
			s++;
		}

		if (!*s)
		{
			break;
		}

		s++; /* skip the \n */
	}
	while (1);

	Com_Printf("\n\n\35\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\36\37\n\n");
	Con_ClearNotify();
}

static void
SCR_DrawCenterString(void)
{
	char *start;
	int l;
	int j;
	int x, y;
	int remaining;
	float scale;
    const int char_unscaled_width  = 8;
    const int char_unscaled_height = 8;

	/* the finale prints the characters one at a time */
	remaining = 9999;

	scr_erase_center = 0;
	start = scr_centerstring;
	scale = SCR_GetConsoleScale();

	if (scr_center_lines <= 4)
	{
		y = (viddef.height * 0.35) / scale;
	}

	else
	{
		y = 48 / scale;
	}

	do
	{
		/* scan the width of the line */
		for (l = 0; l < 40; l++)
		{
			if ((start[l] == '\n') || !start[l])
			{
				break;
			}
		}

		x = ((viddef.width / scale) - (l * char_unscaled_width)) / 2;
		SCR_AddDirtyPoint(x, y);

		for (j = 0; j < l; j++, x += char_unscaled_width)
		{
			Draw_CharScaled(x * scale, y * scale, start[j], scale);

			if (!remaining--)
			{
				return;
			}
		}

		SCR_AddDirtyPoint(x, y + char_unscaled_height);

		y += char_unscaled_height;

		while (*start && *start != '\n')
		{
			start++;
		}

		if (!*start)
		{
			break;
		}

		start++; /* skip the \n */
	}
	while (1);
}

static void
SCR_CheckDrawCenterString(void)
{
	scr_centertime_off -= cls.rframetime;

	if (scr_centertime_off <= 0)
	{
		return;
	}

	SCR_DrawCenterString();
}

/*
 * Sets scr_vrect, the coordinates of the rendered window
 */
static void
SCR_CalcVrect(void)
{
	int size;

	/* bound viewsize */
	if (scr_viewsize->value < 40)
	{
		Cvar_Set("viewsize", "40");
	}

	if (scr_viewsize->value > 100)
	{
		Cvar_Set("viewsize", "100");
	}

	size = scr_viewsize->value;

	scr_vrect.width = viddef.width * size / 100;
	scr_vrect.height = viddef.height * size / 100;

	scr_vrect.x = (viddef.width - scr_vrect.width) / 2;
	scr_vrect.y = (viddef.height - scr_vrect.height) / 2;
}

/*
 * Keybinding command
 */
static void
SCR_SizeUp_f(void)
{
	Cvar_SetValue("viewsize", (float)scr_viewsize->value + 10);
}

/*
 * Keybinding command
 */
static void
SCR_SizeDown_f(void)
{
	Cvar_SetValue("viewsize", (float)scr_viewsize->value - 10);
}

/*
 * Set a specific sky and rotation speed
 */
static void
SCR_Sky_f(void)
{
	float rotate = 0;
	vec3_t axis;

	if (Cmd_Argc() < 2)
	{
		Com_Printf("Usage: sky <basename> <rotate> <axis x y z>\n");
		return;
	}

	if (Cmd_Argc() > 2)
	{
		rotate = (float)strtod(Cmd_Argv(2), (char **)NULL);
	}

	else
	{
		rotate = 0;
	}

	if (Cmd_Argc() == 6)
	{
		axis[0] = (float)strtod(Cmd_Argv(3), (char **)NULL);
		axis[1] = (float)strtod(Cmd_Argv(4), (char **)NULL);
		axis[2] = (float)strtod(Cmd_Argv(5), (char **)NULL);
	}
	else
	{
		axis[0] = 0;
		axis[1] = 0;
		axis[2] = 1;
	}

	R_SetSky(Cmd_Argv(1), rotate, axis);
}

void
SCR_Init(void)
{
	scr_viewsize = Cvar_Get("viewsize", "100", CVAR_ARCHIVE);
	scr_conspeed = Cvar_Get("scr_conspeed", "3", 0);
	scr_centertime = Cvar_Get("scr_centertime", "2.5", 0);
	scr_showturtle = Cvar_Get("scr_showturtle", "0", 0);
	scr_showpause = Cvar_Get("scr_showpause", "1", 0);
	scr_netgraph = Cvar_Get("netgraph", "0", 0);
	scr_timegraph = Cvar_Get("timegraph", "0", 0);
	scr_debuggraph = Cvar_Get("debuggraph", "0", 0);
	scr_graphheight = Cvar_Get("graphheight", "32", 0);
	scr_graphscale = Cvar_Get("graphscale", "1", 0);
	scr_graphshift = Cvar_Get("graphshift", "0", 0);
	scr_drawall = Cvar_Get("scr_drawall", "0", 0);
	r_hudscale = Cvar_Get("r_hudscale", "-1", CVAR_ARCHIVE);
	r_consolescale = Cvar_Get("r_consolescale", "-1", CVAR_ARCHIVE);
	r_menuscale = Cvar_Get("r_menuscale", "-1", CVAR_ARCHIVE);

	/* register our commands */
	Cmd_AddCommand("timerefresh", SCR_TimeRefresh_f);
	Cmd_AddCommand("loading", SCR_Loading_f);
	Cmd_AddCommand("sizeup", SCR_SizeUp_f);
	Cmd_AddCommand("sizedown", SCR_SizeDown_f);
	Cmd_AddCommand("sky", SCR_Sky_f);

	scr_initialized = true;
}

static void
SCR_DrawNet(void)
{
	float scale = SCR_GetMenuScale();

	if (cls.netchan.outgoing_sequence - cls.netchan.incoming_acknowledged < CMD_BACKUP - 1)
	{
		return;
	}

	Draw_PicScaled(scr_vrect.x + 64 * scale, scr_vrect.y, "net", scale);
}

static void
SCR_DrawPause(void)
{
	int w, h;
	float scale = SCR_GetMenuScale();

	if (!scr_showpause->value) /* turn off for screenshots */
	{
		return;
	}

	if (!cl_paused->value)
	{
		return;
	}

	Draw_GetPicSize(&w, &h, "pause");
	Draw_PicScaled((viddef.width - w * scale) / 2, viddef.height / 2 + 8 * scale, "pause", scale);
}

static void
SCR_DrawLoading(void)
{
	int w, h;
	float scale = SCR_GetMenuScale();

	if (!scr_draw_loading)
	{
		return;
	}

	Draw_GetPicSize(&w, &h, "loading");
	Draw_PicScaled((viddef.width - w * scale) / 2, (viddef.height - h * scale) / 2, "loading", scale);
}

/*
 * Scroll it up or down
 */
void
SCR_RunConsole(void)
{
	/* src_conspeed must be a positiv integer,
	   otherwise things go wrong. Clamp it. */
	if (scr_conspeed->value < 0.1f)
	{
		Cvar_Set("scr_conspeed", "0.1");
	}

	/* decide on the height of the console */
	if (cls.key_dest == key_console)
	{
		scr_conlines = 0.5; /* half screen */
	}
	else
	{
		scr_conlines = 0; /* none visible */
	}

	if (scr_conlines < scr_con_current)
	{
		scr_con_current -= scr_conspeed->value * cls.rframetime;

		if (scr_conlines > scr_con_current)
		{
			scr_con_current = scr_conlines;
		}
	}
	else if (scr_conlines > scr_con_current)
	{
		scr_con_current += scr_conspeed->value * cls.rframetime;

		if (scr_conlines < scr_con_current)
		{
			scr_con_current = scr_conlines;
		}
	}
}

static void
SCR_DrawConsole(void)
{
	Con_CheckResize();

	if ((cls.state == ca_disconnected) || (cls.state == ca_connecting))
	{
		/* forced full screen console */
		Con_DrawConsole(1.0);
		return;
	}

	if ((cls.state != ca_active) || !cl.refresh_prepped)
	{
		/* connected, but can't render */
		Con_DrawConsole(0.5);
		Draw_Fill(0, viddef.height / 2, viddef.width, viddef.height / 2, 0);
		return;
	}

	if (scr_con_current)
	{
		Con_DrawConsole(scr_con_current);
	}
	else
	{
		if ((cls.key_dest == key_game) || (cls.key_dest == key_message))
		{
			Con_DrawNotify(); /* only draw notify in game */
		}
	}
}

void
SCR_BeginLoadingPlaque(void)
{
	S_StopAllSounds();
	cl.sound_prepped = false; /* don't play ambients */

	OGG_Stop();

	if (cls.disable_screen)
	{
		return;
	}

	if (developer->value)
	{
		/* Hack: When we are returning here (not drawing
		   the loading plaque) we don't reset the palette
		   later on. We might end up with the cinematic
		   palette applied to the world. Enforce the world
		   palette. */
		if (cl.cinematictime > 0)
		{
			R_SetPalette(NULL);
		}

		return;
	}

	if (cls.state == ca_disconnected)
	{
		/* if at console, don't bring up the plaque */
		return;
	}

	if (cls.key_dest == key_console)
	{
		return;
	}

	if (cl.cinematictime > 0)
	{
		scr_draw_loading = 2; /* clear to balack first */
	}
	else
	{
		scr_draw_loading = 1;
	}

	SCR_UpdateScreen();

	scr_draw_loading = false;

	SCR_StopCinematic();
	cls.disable_screen = Sys_Milliseconds();
	cls.disable_servercount = cl.servercount;
}

void
SCR_EndLoadingPlaque(void)
{
	cls.disable_screen = 0;
	Con_ClearNotify();
}

static void
SCR_Loading_f(void)
{
	SCR_BeginLoadingPlaque();
}

static void
SCR_TimeRefresh_f(void)
{
	int i;
	int start, stop;
	float time;

	if (cls.state != ca_active)
	{
		return;
	}

	start = Sys_Milliseconds();

	if (Cmd_Argc() == 2)
	{
		/* run without page flipping */
		int j;

		for (j = 0; j < 1000; j++)
		{
			R_BeginFrame(0);

			for (i = 0; i < 128; i++)
			{
				cl.refdef.viewangles[1] = i / 128.0f * 360.0f;
				R_RenderFrame(&cl.refdef);
			}

			R_EndFrame();
		}
	}
	else
	{
		for (i = 0; i < 128; i++)
		{
			cl.refdef.viewangles[1] = i / 128.0f * 360.0f;

			R_BeginFrame(0);
			R_RenderFrame(&cl.refdef);
			R_EndFrame();
		}
	}

	stop = Sys_Milliseconds();
	time = (stop - start) / 1000.0f;
	Com_Printf("%f seconds (%f fps)\n", time, 128 / time);
}

void
SCR_AddDirtyPoint(int x, int y)
{
	if (x < scr_dirty.x1)
	{
		scr_dirty.x1 = x;
	}

	if (x > scr_dirty.x2)
	{
		scr_dirty.x2 = x;
	}

	if (y < scr_dirty.y1)
	{
		scr_dirty.y1 = y;
	}

	if (y > scr_dirty.y2)
	{
		scr_dirty.y2 = y;
	}
}

void
SCR_DirtyScreen(void)
{
	SCR_AddDirtyPoint(0, 0);
	SCR_AddDirtyPoint(viddef.width - 1, viddef.height - 1);
}

/*
 * Clear any parts of the tiled background that were drawn on last frame
 */
static void
SCR_TileClear(void)
{
	int i;
	int top, bottom, left, right;
	dirty_t clear;

	if (scr_con_current == 1.0)
	{
		return; /* full screen console */
	}

	if (scr_viewsize->value == 100)
	{
		return; /* full screen rendering */
	}

	if (cl.cinematictime > 0)
	{
		return; /* full screen cinematic */
	}


	/* This highly complicated pseudo damage tracking is very effective
	   with the soft render, it gives about 10% speedup. On the GL
	   renderers the speedup is negligible, but in introduce a bug:
	   OpenGL requires to redraw the borders after every glClear(),
	   which the damage tracking doesn't take into account. Fix this
	   by not using the damage tracking when running somethinge else
	   than the soft renderer. Just redraw the borders every frame. */
	if (strcmp(vid_renderer->string, "soft") != 0) {
		// Top
		Draw_TileClear(scr_vrect.x, 0, scr_vrect.width, scr_vrect.y, "backtile");

		// Bottom
		Draw_TileClear(scr_vrect.x, scr_vrect.y + scr_vrect.height, scr_vrect.width, viddef.height, "backtile");

		// Left
		Draw_TileClear(0, 0, scr_vrect.x, viddef.height, "backtile");

		// Right
		Draw_TileClear(scr_vrect.x + scr_vrect.width, 0, viddef.width, viddef.height, "backtile");
	} else {
		/* erase rect will be the union of the past three
		   frames so tripple buffering works properly */
		clear = scr_dirty;

		for (i = 0; i < 2; i++)
		{
			if (scr_old_dirty[i].x1 < clear.x1)
			{
				clear.x1 = scr_old_dirty[i].x1;
			}

			if (scr_old_dirty[i].x2 > clear.x2)
			{
				clear.x2 = scr_old_dirty[i].x2;
			}

			if (scr_old_dirty[i].y1 < clear.y1)
			{
				clear.y1 = scr_old_dirty[i].y1;
			}

			if (scr_old_dirty[i].y2 > clear.y2)
			{
				clear.y2 = scr_old_dirty[i].y2;
			}
		}

		scr_old_dirty[1] = scr_old_dirty[0];
		scr_old_dirty[0] = scr_dirty;

		scr_dirty.x1 = 9999;
		scr_dirty.x2 = -9999;
		scr_dirty.y1 = 9999;
		scr_dirty.y2 = -9999;

		/* don't bother with anything convered by the console */
		top = (int)(scr_con_current * viddef.height);

		if (top >= clear.y1)
		{
			clear.y1 = top;
		}

		if (clear.y2 <= clear.y1)
		{
			return; /* nothing disturbed */
		}

		top = scr_vrect.y;
		bottom = top + scr_vrect.height - 1;
		left = scr_vrect.x;
		right = left + scr_vrect.width - 1;

		if (clear.y1 < top)
		{
			/* clear above view screen */
			i = clear.y2 < top - 1 ? clear.y2 : top - 1;
			Draw_TileClear(clear.x1, clear.y1,
					clear.x2 - clear.x1 + 1, i - clear.y1 + 1, "backtile");
			clear.y1 = top;
		}

		if (clear.y2 > bottom)
		{
			/* clear below view screen */
			i = clear.y1 > bottom + 1 ? clear.y1 : bottom + 1;
			Draw_TileClear(clear.x1, i,
					clear.x2 - clear.x1 + 1, clear.y2 - i + 1, "backtile");
			clear.y2 = bottom;
		}

		if (clear.x1 < left)
		{
			/* clear left of view screen */
			i = clear.x2 < left - 1 ? clear.x2 : left - 1;
			Draw_TileClear(clear.x1, clear.y1,
					i - clear.x1 + 1, clear.y2 - clear.y1 + 1, "backtile");
			clear.x1 = left;
		}

		if (clear.x2 > right)
		{
			/* clear left of view screen */
			i = clear.x1 > right + 1 ? clear.x1 : right + 1;
			Draw_TileClear(i, clear.y1,
					clear.x2 - i + 1, clear.y2 - clear.y1 + 1, "backtile");
			clear.x2 = right;
		}
	}
}

#define STAT_MINUS 10
static char *sb_nums[2][11] = {
	{
		"num_0", "num_1", "num_2", "num_3", "num_4", "num_5",
		"num_6", "num_7", "num_8", "num_9", "num_minus"
	},
	{
		"anum_0", "anum_1", "anum_2", "anum_3", "anum_4", "anum_5",
		"anum_6", "anum_7", "anum_8", "anum_9", "anum_minus"
	}
};

#define ICON_WIDTH 24
#define ICON_HEIGHT 24
#define CHARACTER_WIDTH 16
#define ICON_SPACE 8

/*
 * Allow embedded \n in the string
 */
void
SizeHUDString(char *string, int *w, int *h)
{
	int lines, width, current;

	lines = 1;
	width = 0;

	current = 0;

	while (*string)
	{
		if (*string == '\n')
		{
			lines++;
			current = 0;
		}
		else
		{
			current++;

			if (current > width)
			{
				width = current;
			}
		}

		string++;
	}

	*w = width * 8;
	*h = lines * 8;
}

static void
DrawHUDStringScaled(const char *string, int x, int y, int centerwidth, int xor, float factor)
{
	int margin;
	int i;

	margin = x;

	while (*string)
	{
		/* scan out one line of text from the string */
		int width;
		char line[1024];

		width = 0;

		while (*string && *string != '\n')
		{
			line[width++] = *string++;
		}

		line[width] = 0;

		if (centerwidth)
		{
			x = margin + (centerwidth - width * 8)*factor / 2;
		}

		else
		{
			x = margin;
		}

		for (i = 0; i < width; i++)
		{
			Draw_CharScaled(x, y, line[i] ^ xor, factor);
			x += 8*factor;
		}

		if (*string)
		{
			string++; /* skip the \n */
			y += CHAR_SIZE * factor;
		}
	}
}

void
DrawHUDString(char *string, int x, int y, int centerwidth, int xor)
{
	DrawHUDStringScaled(string, x, y, centerwidth, xor, 1.0f);
}

void
SCR_DrawFieldScaled(int x, int y, int color, int width, int value, float factor)
{
	char num[16], *ptr;
	int l;
	int frame;

	if (width < 1)
	{
		return;
	}

	/* draw number string */
	if (width > 5)
	{
		width = 5;
	}

	SCR_AddDirtyPoint(x, y);
	SCR_AddDirtyPoint(x + (width * CHARACTER_WIDTH + 2) * factor, y + factor * 24);

	Com_sprintf(num, sizeof(num), "%i", value);
	l = (int)strlen(num);

	if (l > width)
	{
		l = width;
	}

	x += (2 + CHARACTER_WIDTH * (width - l)) * factor;

	ptr = num;

	while (*ptr && l)
	{
		if (*ptr == '-')
		{
			frame = STAT_MINUS;
		}

		else
		{
			frame = *ptr - '0';
		}

		Draw_PicScaled(x, y, sb_nums[color][frame], factor);
		x += CHARACTER_WIDTH * factor;
		ptr++;
		l--;
	}
}

void
SCR_DrawField(int x, int y, int color, int width, int value)
{
	SCR_DrawFieldScaled(x, y, color, width, value, 1.0f);
}

/*
 * Allows rendering code to cache all needed sbar graphics
 */
void
SCR_TouchPics(void)
{
	int i, j;

	for (i = 0; i < 2; i++)
	{
		for (j = 0; j < 11; j++)
		{
			Draw_FindPic(sb_nums[i][j]);
		}
	}

	if (crosshair->value)
	{
		if ((crosshair->value > 3) || (crosshair->value < 0))
		{
			crosshair->value = 3;
		}

		Com_sprintf(crosshair_pic, sizeof(crosshair_pic), "ch%i",
				(int)(crosshair->value));
		Draw_GetPicSize(&crosshair_width, &crosshair_height, crosshair_pic);

		if (!crosshair_width)
		{
			crosshair_pic[0] = 0;
		}
	}
}

#ifdef __EMSCRIPTEN__
/* The layout-string "xv"/"yv" commands (and the equivalent inline math
 * in the "client"/"ctf" blocks below) place elements in a virtual
 * 320x240 coordinate frame that's meant to sit CENTERED on the real
 * screen -- value 0 is that frame's left/top edge, 319/239 its right/
 * bottom edge. That's fine (and long-standing, correct behavior) on
 * any screen at least 320x240, where the frame just sits in the middle
 * with margin to spare. This KaiOS port's 240x320 screen is narrower
 * than that reference frame, so centering it means the frame's own
 * left and right edges -- xv 0 (health number in the stock statusbar)
 * and xv 296-319 (selected-item icon, timer) among them -- fall
 * outside the real, narrower screen entirely and never get drawn at
 * all: exactly the "missing HUD content" a real device showed.
 *
 * Compress the virtual frame to fit instead of centering-and-clipping
 * it, but ONLY on a screen actually narrower/shorter than the
 * reference -- anything at or above 320x240 keeps the original,
 * correct-for-decades centering behavior unchanged. */
static int
SCR_LayoutXV(int value, float scale)
{
	if (viddef.width < 320)
	{
		return (int)((value * (float)viddef.width) / 320.0f);
	}

	return (int)(viddef.width / 2 - scale * 160 + scale * value);
}

static int
SCR_LayoutYV(int value, float scale)
{
	if (viddef.height < 240)
	{
		return (int)((value * (float)viddef.height) / 240.0f);
	}

	return (int)(viddef.height / 2 - scale * 120 + scale * value);
}
#endif

static void
SCR_ExecuteLayoutString(char *s)
{
	int x, y;
	float scale;

	scale = SCR_GetHUDScale();

	if ((cls.state != ca_active) || !cl.refresh_prepped)
	{
		return;
	}

	if (!s[0])
	{
		return;
	}

	x = 0;
	y = 0;

	while (s)
	{
		const char *token;

		token = COM_Parse(&s);

		if (!strcmp(token, "xl"))
		{
			token = COM_Parse(&s);
			x = scale * (int)strtol(token, (char **)NULL, 10);
			continue;
		}

		if (!strcmp(token, "xr"))
		{
			token = COM_Parse(&s);
			x = viddef.width + scale * (int)strtol(token, (char **)NULL, 10);
			continue;
		}

		if (!strcmp(token, "xv"))
		{
			token = COM_Parse(&s);
#ifdef __EMSCRIPTEN__
			x = SCR_LayoutXV((int)strtol(token, (char **)NULL, 10), scale);
#else
			x = viddef.width / 2 - scale * 160 + scale * (int)strtol(token, (char **)NULL, 10);
#endif
			continue;
		}

		if (!strcmp(token, "yt"))
		{
			token = COM_Parse(&s);
			y = scale * (int)strtol(token, (char **)NULL, 10);
			continue;
		}

		if (!strcmp(token, "yb"))
		{
			token = COM_Parse(&s);
			y = viddef.height + scale * (int)strtol(token, (char **)NULL, 10);
			continue;
		}

		if (!strcmp(token, "yv"))
		{
			token = COM_Parse(&s);
#ifdef __EMSCRIPTEN__
			y = SCR_LayoutYV((int)strtol(token, (char **)NULL, 10), scale);
#else
			y = viddef.height / 2 - scale * 120 + scale * (int)strtol(token, (char **)NULL, 10);
#endif
			continue;
		}

		if (!strcmp(token, "pic"))
		{
			int index, value;

			/* draw a pic from a stat number */
			token = COM_Parse(&s);
			index = (int)strtol(token, (char **)NULL, 10);

			if ((index < 0) || (index >= MAX_STATS))
			{
				Com_DPrintf("%s: bad stats index %d (0x%x) in pic\n",
					__func__, index, index);
				continue;
			}

			value = cl.frame.playerstate.stats[index];

			if (value >= MAX_IMAGES)
			{
				Com_DPrintf("%s: Pic %d >= MAX_IMAGES in pic\n",
					__func__, value);
				continue;
			}

			if (cl.configstrings[CS_IMAGES + value][0] != '\0')
			{
				const char *text;
				int w, h;

				text = cl.configstrings[CS_IMAGES + value];
				Draw_GetPicSize(&w, &h, text);
				SCR_AddDirtyPoint(x, y);
				SCR_AddDirtyPoint(x + (w - 1) * scale, y + (h - 1) * scale);
				Draw_PicScaled(x, y, text, scale);
			}

			continue;
		}

		if (!strcmp(token, "client"))
		{
			/* draw a deathmatch client block */
			int score, ping, time, value;
			clientinfo_t *ci;

			token = COM_Parse(&s);
#ifdef __EMSCRIPTEN__
			x = SCR_LayoutXV((int)strtol(token, (char **)NULL, 10), scale);
#else
			x = viddef.width / 2 - scale * 160 + scale * (int)strtol(token, (char **)NULL, 10);
#endif
			token = COM_Parse(&s);
#ifdef __EMSCRIPTEN__
			y = SCR_LayoutYV((int)strtol(token, (char **)NULL, 10), scale);
#else
			y = viddef.height / 2 - scale * 120 + scale * (int)strtol(token, (char **)NULL, 10);
#endif
			SCR_AddDirtyPoint(x, y);
			SCR_AddDirtyPoint(x + scale * 159, y + scale * 31);

			token = COM_Parse(&s);
			value = (int)strtol(token, (char **)NULL, 10);

			if ((value >= MAX_CLIENTS) || (value < 0))
			{
				Com_DPrintf("%s: client >= MAX_CLIENTS in client\n", __func__);
				continue;
			}

			ci = &cl.clientinfo[value];

			token = COM_Parse(&s);
			score = (int)strtol(token, (char **)NULL, 10);

			token = COM_Parse(&s);
			ping = (int)strtol(token, (char **)NULL, 10);

			token = COM_Parse(&s);
			time = (int)strtol(token, (char **)NULL, 10);

			DrawAltStringScaled(x + scale * 32, y, ci->name, scale);
			DrawAltStringScaled(x + scale * 32, y + scale * CHAR_SIZE, "Score: ", scale);
			DrawAltStringScaled(x + scale * (32 + 7 * CHAR_SIZE), y + scale * CHAR_SIZE, va("%i", score), scale);
			DrawStringScaled(x + scale * 32, y + scale * 16, va("Ping:  %i", ping), scale);
			DrawStringScaled(x + scale * 32, y + scale * 24, va("Time:  %i", time), scale);

			if (!ci->icon)
			{
				ci = &cl.baseclientinfo;
			}

			Draw_PicScaled(x, y, ci->iconname, scale);
			continue;
		}

		if (!strcmp(token, "ctf"))
		{
			/* draw a ctf client block */
			int score, ping, value;
			clientinfo_t *ci;
			char block[80];

			token = COM_Parse(&s);
#ifdef __EMSCRIPTEN__
			x = SCR_LayoutXV((int)strtol(token, (char **)NULL, 10), scale);
#else
			x = viddef.width / 2 - scale * 160 + scale*(int)strtol(token, (char **)NULL, 10);
#endif
			token = COM_Parse(&s);
#ifdef __EMSCRIPTEN__
			y = SCR_LayoutYV((int)strtol(token, (char **)NULL, 10), scale);
#else
			y = viddef.height / 2 - scale*120 + scale*(int)strtol(token, (char **)NULL, 10);
#endif
			SCR_AddDirtyPoint(x, y);
			SCR_AddDirtyPoint(x + scale * 159, y + scale * 31);

			token = COM_Parse(&s);
			value = (int)strtol(token, (char **)NULL, 10);

			if ((value >= MAX_CLIENTS) || (value < 0))
			{
				Com_DPrintf("%s: client >= MAX_CLIENTS in client\n", __func__);
				continue;
			}

			ci = &cl.clientinfo[value];

			token = COM_Parse(&s);
			score = (int)strtol(token, (char **)NULL, 10);

			token = COM_Parse(&s);
			ping = (int)strtol(token, (char **)NULL, 10);

			if (ping > 999)
			{
				ping = 999;
			}

			sprintf(block, "%3d %3d %-12.12s", score, ping, ci->name);

			if (value == cl.playernum)
			{
				DrawAltStringScaled(x, y, block, scale);
			}

			else
			{
				DrawStringScaled(x, y, block, scale);
			}

			continue;
		}

		if (!strcmp(token, "picn"))
		{
			int w, h;

			/* draw a pic from a name */
			token = COM_Parse(&s);
			Draw_GetPicSize(&w, &h, token);
			SCR_AddDirtyPoint(x, y);
			SCR_AddDirtyPoint(x + scale * (w - 1), y + scale * (h - 1));
			Draw_PicScaled(x, y, token, scale);
			continue;
		}

		if (!strcmp(token, "num"))
		{
			int value, width;

			/* draw a number */
			token = COM_Parse(&s);
			width = (int)strtol(token, (char **)NULL, 10);
			token = COM_Parse(&s);
			value = cl.frame.playerstate.stats[(int)strtol(token, (char **)NULL, 10)];
			SCR_DrawFieldScaled(x, y, 0, width, value, scale);
			continue;
		}

		if (!strcmp(token, "hnum"))
		{
			/* health number */
			int color, value, width;

			width = 3;
			value = cl.frame.playerstate.stats[STAT_HEALTH];

			if (value > 25)
			{
				color = 0;  /* green */
			}
			else if (value > 0)
			{
				color = (cl.frame.serverframe >> 2) & 1; /* flash */
			}
			else
			{
				color = 1;
			}

			if (cl.frame.playerstate.stats[STAT_FLASHES] & 1)
			{
				Draw_PicScaled(x, y, "field_3", scale);
			}

			SCR_DrawFieldScaled(x, y, color, width, value, scale);
			continue;
		}

		if (!strcmp(token, "anum"))
		{
			/* ammo number */
			int color, value, width;

			width = 3;
			value = cl.frame.playerstate.stats[STAT_AMMO];

			if (value > 5)
			{
				color = 0; /* green */
			}
			else if (value >= 0)
			{
				color = (cl.frame.serverframe >> 2) & 1; /* flash */
			}
			else
			{
				continue; /* negative number = don't show */
			}

			if (cl.frame.playerstate.stats[STAT_FLASHES] & 4)
			{
				Draw_PicScaled(x, y, "field_3", scale);
			}

			SCR_DrawFieldScaled(x, y, color, width, value, scale);
			continue;
		}

		if (!strcmp(token, "rnum"))
		{
			/* armor number */
			int color, value, width;

			width = 3;
			value = cl.frame.playerstate.stats[STAT_ARMOR];

			if (value < 1)
			{
				continue;
			}

			color = 0; /* green */

			if (cl.frame.playerstate.stats[STAT_FLASHES] & 2)
			{
				Draw_PicScaled(x, y, "field_3", scale);
			}

			SCR_DrawFieldScaled(x, y, color, width, value, scale);
			continue;
		}

		if (!strcmp(token, "stat_string"))
		{
			int index;

			token = COM_Parse(&s);
			index = (int)strtol(token, (char **)NULL, 10);

			if ((index < 0) || (index >= MAX_STATS))
			{
				Com_DPrintf("%s: bad stats index %d (0x%x) in stat_string\n",
					__func__, index, index);
				continue;
			}

			index = cl.frame.playerstate.stats[index];

			if ((index < 0) || (index >= MAX_CONFIGSTRINGS))
			{
				Com_DPrintf("%s: bad stats index %d (0x%x) in stat_string\n",
					__func__, index, index);
				continue;
			}

			DrawStringScaled(x, y, cl.configstrings[index], scale);
			continue;
		}

		if (!strcmp(token, "cstring"))
		{
			token = COM_Parse(&s);
			DrawHUDStringScaled(token, x, y, 320, 0, scale); // FIXME: or scale 320 here?
			continue;
		}

		if (!strcmp(token, "string"))
		{
			token = COM_Parse(&s);
			DrawStringScaled(x, y, token, scale);
			continue;
		}

		if (!strcmp(token, "cstring2"))
		{
			token = COM_Parse(&s);
			DrawHUDStringScaled(token, x, y, 320, 0x80, scale); // FIXME: or scale 320 here?
			continue;
		}

		if (!strcmp(token, "string2"))
		{
			token = COM_Parse(&s);
			DrawAltStringScaled(x, y, token, scale);
			continue;
		}

		if (!strcmp(token, "if"))
		{
			int index, value;

			/* draw a number */
			token = COM_Parse(&s);
			index = (int)strtol(token, (char **)NULL, 10);

			if ((index < 0) || (index >= MAX_STATS))
			{
				Com_DPrintf("%s: bad stats index %d (0x%x) in if\n",
					__func__, index, index);
				value = 0;
			}
			else
			{
				value = cl.frame.playerstate.stats[index];
			}

			if (!value)
			{
				/* skip to endif */
				while (s && strcmp(token, "endif"))
				{
					token = COM_Parse(&s);
				}
			}

			continue;
		}

		if (!strcmp(token, "endif") || !token[0])
		{
			/* just skip endif and empty line */
			continue;
		}

		Com_DPrintf("%s: Unknown token: %s\n", __func__, token);
	}
}

/*
 * The status bar is a small layout program that
 * is based on the stats array
 */
static void
SCR_DrawStats(void)
{
	SCR_ExecuteLayoutString(cl.configstrings[CS_STATUSBAR]);
}

#define STAT_LAYOUTS 13

static void
SCR_DrawLayout(void)
{
	if (!cl.frame.playerstate.stats[STAT_LAYOUTS])
	{
		return;
	}

	SCR_ExecuteLayoutString(cl.layout);
}

// ----

static void
SCR_DrawSpeed(void)
{
	if (cl_showspeed->value < 1)  //Disabled, do nothing
		return;

	char spd_str[32];
	float speed, speedxy;
	float scale = SCR_GetConsoleScale();
	int str_len, xPos, yPos = 0;

	GetPlayerSpeed(&speed, &speedxy);
	snprintf(spd_str, sizeof(spd_str), "%6.2f (%6.2f) QU/s", speed, speedxy);
	str_len = scale * (strlen(spd_str) * CHAR_SIZE + 2);

	if (cl_showspeed->value == 1) //Draw speed and xy speed at top right
	{
		xPos = viddef.width - str_len;

		if (cl_showfps->value == 1 || cl_showfps->value == 2)  // If showfps is enabled, draw it underneath
		{
			yPos = scale * 10;
		}
		else if (cl_showfps->value > 2)
		{
			yPos = scale * 20;
		}

		DrawStringScaled(xPos, yPos, spd_str, scale);
		SCR_AddDirtyPoint(xPos, yPos);
		SCR_AddDirtyPoint(viddef.width, yPos);
	}

	else if (cl_showspeed->value > 1) //Draw only xy speed under the crosshair
	{
		if (scale != 1)  // Check if low resolution
		{
			scale -= 1;
		}

		snprintf(spd_str, sizeof(spd_str), "%6.2f", speedxy);
		str_len = scale * (strlen(spd_str) * CHAR_SIZE + 2);
		yPos = scr_vrect.y + (scr_vrect.height / 2) + (scale * 10);
		xPos = scr_vrect.x + (scr_vrect.width / 2) - (str_len / 2);
		
		DrawStringScaled(xPos, yPos, spd_str, scale);
		SCR_AddDirtyPoint(xPos, yPos);
		SCR_AddDirtyPoint(xPos + str_len, yPos);
	}
}

static void
SCR_Framecounter(void)
{
	long long newtime;
	static int frame;
	static int frametimes[60] = {0};
	static long long oldtime;

	/* skip statistics without show fps */
	if (cl_showfps->value < 1)
	{
		return;
	}

	newtime = Sys_Microseconds();
	frametimes[frame] = (int)(newtime - oldtime);

	oldtime = newtime;
	frame++;
	if (frame > 59)
	{
		frame = 0;
	}

	float scale = SCR_GetConsoleScale();

	if (cl_showfps->value == 1)
	{
		// Calculate average of frames.
		char str[10];
		int avg = 0;
		int num = 0;

		for (int i = 0; i < 60; i++) {
			if (frametimes[i] != 0) {
				avg += frametimes[i];
				num++;
			}
		}

		snprintf(str, sizeof(str), "%3.2ffps", (1000.0 * 1000.0) / (avg / num));
		DrawStringScaled(viddef.width - scale * (strlen(str) * CHAR_SIZE + 2), 0, str, scale);
		SCR_AddDirtyPoint(viddef.width - scale * (strlen(str) * CHAR_SIZE + 2), 0);
		SCR_AddDirtyPoint(viddef.width, 0);
	}
	else if (cl_showfps->value >= 2)
	{
		// Calculate average of frames.
		int avg = 0;
		int num = 0;
		char str[64];

		for (int i = 0; i < 60; i++) {
			if (frametimes[i] != 0) {
				avg += frametimes[i];
				num++;
			}
		}

		// Find lowest and highest
		int min = frametimes[0];
		int max = frametimes[1];

		for (int i = 1; i < 60; i++)
		{
			if ((frametimes[i] > 0) &&  (min < frametimes[i]))
			{
				min = frametimes[i];
			}

			if ((frametimes[i] > 0) && (max > frametimes[i]))
			{
				max = frametimes[i];
			}
		}

		snprintf(str, sizeof(str), "Min: %7.2ffps, Max: %7.2ffps, Avg: %7.2ffps",
		         (1000.0 * 1000.0) / min, (1000.0 * 1000.0) / max, (1000.0 * 1000.0) / (avg / num));
		DrawStringScaled(viddef.width - scale * (strlen(str) * CHAR_SIZE + 2), 0, str, scale);
		SCR_AddDirtyPoint(viddef.width - scale * (strlen(str) * CHAR_SIZE + 2), 0);
		SCR_AddDirtyPoint(viddef.width, 0);

		if (cl_showfps->value > 2)
		{
			snprintf(str, sizeof(str), "Max: %5.2fms, Min: %5.2fms, Avg: %5.2fms",
			         0.001f * min, 0.001f * max, 0.001f * ((float)avg / num));
			DrawStringScaled(viddef.width - scale * (strlen(str) * CHAR_SIZE + 2), scale * 10, str, scale);
			SCR_AddDirtyPoint(viddef.width - scale * (strlen(str) * CHAR_SIZE + 2), scale * 10);
			SCR_AddDirtyPoint(viddef.width, scale + 10);
		}
	}
}

#ifdef __EMSCRIPTEN__
/* Both defined (and incremented) elsewhere -- sound.c/cl_tempentities.c
 * -- see kaios_sounds_started_this_frame's comment there for why this
 * reaches across client-side files directly instead of going through a
 * cvar or export table: this whole feature is KaiOS-only instrumentation,
 * not part of any cross-platform renderer/sound abstraction. */
extern int kaios_sounds_started_this_frame;
extern int kaios_effects_started_this_frame;
/* r_polycount lives in the software renderer (sw_main.c/sw_rast.c), not
 * behind the refexport_t table -- reached directly the same way, and
 * only safe because this KaiOS build only ever links the soft renderer
 * (see build.sh) into this one binary, no dlopen boundary to cross. */
extern int r_polycount;

/*
 * CPU/RAM/FPS/polycount/sound+effect-activation line for chasing
 * performance issues on real KaiOS hardware, where there's no devtools
 * profiler to attach. Accumulates every frame but only flushes once a
 * ~1 real-time second window has elapsed, averaging CPU/RAM/polycount
 * over that window and totalling frame count (== FPS for the window)
 * and sound/effect activations -- printing every single frame was
 * tried first and flooded the log without being any easier to read.
 *
 * Goes straight to Sys_ConsoleOutput() (stdout, captured by the
 * browser's own console via Module.print in module-init.js) instead of
 * Com_Printf(), which would *also* feed this into the in-game console
 * buffer/UI (Con_Print()) -- this is a log for offline analysis, not
 * something to clutter the in-game console with.
 */
static void
SCR_DrawKaiosStats(void)
{
	static int window_start_ms = 0;
	static int frames_in_window = 0;
	static float cpu_pct_sum = 0.0f;
	static float ram_pct_sum = 0.0f;
	static int polys_sum = 0;
	static int sounds_sum = 0;
	static int effects_sum = 0;

	struct mallinfo mi = mallinfo();
	size_t heap_size = emscripten_get_heap_size();
	float ram_pct = heap_size ? (100.0f * (float)mi.uordblks / (float)heap_size) : 0.0f;

	/* There's no OS-level CPU utilization figure available to a
	 * sandboxed KaiOS webapp, so CPU% is a proxy: last frame's wall
	 * time against a budget. A real device showed this reading up to
	 * ~1000% against the original 50ms (20fps) budget -- 10x over,
	 * consistently -- so scale against a 500ms budget instead, putting
	 * that device's actual sustained frame cost closer to 100%. */
	float cpu_pct = (cls.rframetime / 0.5f) * 100.0f;

	frames_in_window++;
	cpu_pct_sum += cpu_pct;
	ram_pct_sum += ram_pct;
	polys_sum += r_polycount;
	sounds_sum += kaios_sounds_started_this_frame;
	effects_sum += kaios_effects_started_this_frame;

	kaios_sounds_started_this_frame = 0;
	kaios_effects_started_this_frame = 0;

	if (window_start_ms == 0)
	{
		window_start_ms = cls.realtime;
	}

	if ((cls.realtime - window_start_ms >= 1000) && (frames_in_window > 0))
	{
		char line[128];

		Com_sprintf(line, sizeof(line),
			"KAIOS_STATS: CPU:%.0f%% RAM:%.0f%% FPS:%d polys:%d sounds:%d effects:%d\n",
			cpu_pct_sum / frames_in_window, ram_pct_sum / frames_in_window,
			frames_in_window, polys_sum / frames_in_window, sounds_sum, effects_sum);
		Sys_ConsoleOutput(line);

		window_start_ms = cls.realtime;
		frames_in_window = 0;
		cpu_pct_sum = 0.0f;
		ram_pct_sum = 0.0f;
		polys_sum = 0;
		sounds_sum = 0;
		effects_sum = 0;
	}
}
#endif

// ----
/*
 * This is called every frame, and can also be called
 * explicitly to flush text to the screen.
 */
void
SCR_UpdateScreen(void)
{
	int numframes;
	int i;
	float separation[2] = {0, 0};
	float scale = SCR_GetMenuScale();

#ifdef __EMSCRIPTEN__
	/* Unconditional (not time/frame gated like KAIOS_SCR_TRACE below)
	 * entry counter -- every prior trace attempt in this investigation
	 * showed real work happening (sound loads) with zero inner
	 * KAIOS_SCR_TRACE output, meaning either this function is being
	 * entered more than once per traced CL_Frame call (reentrancy --
	 * cl_keyboard.c and cl_view.c both have their own direct
	 * SCR_UpdateScreen() call sites, separate from CL_Frame's), or it
	 * is hitting the disable_screen/scr_initialized early-return every
	 * single time despite real work happening elsewhere. This print
	 * fires on literally every entry, gated only to the gl1 diagnostic
	 * build, so the next log settles which one it is. */
	if (strcmp(vid_renderer->string, "gl1") == 0)
	{
		static int kaios_scr_entry_count = 0;
		kaios_scr_entry_count++;
		Com_Printf("KAIOS_SCR_ENTRY: #%d disable_screen=%d scr_initialized=%d con.initialized=%d\n",
			kaios_scr_entry_count, cls.disable_screen, scr_initialized, con.initialized);
	}
#endif

	/* if the screen is disabled (loading plaque is
	   up, or vid mode changing) do nothing at all */
	if (cls.disable_screen)
	{
		if (Sys_Milliseconds() - cls.disable_screen > 120000)
		{
			cls.disable_screen = 0;
			Com_Printf("Loading plaque timed out.\n");
		}

		return;
	}

	if (!scr_initialized || !con.initialized)
	{
		return; /* not initialized yet */
	}

	if ( gl1_stereo->value )
	{
		numframes = 2;
		separation[0] = -gl1_stereo_separation->value / 2;
		separation[1] = +gl1_stereo_separation->value / 2;
	}
	else
	{
		separation[0] = 0;
		separation[1] = 0;
		numframes = 1;
	}

#ifdef __EMSCRIPTEN__
	/* Real-device gl1 testing shows the engine reaching "==== Yamagi
	 * Quake II Initialized ====" and rendering (at least) one frame
	 * cleanly (a KAIOS_FRAMESPIKE for the expected cold-cache first
	 * frame). A first attempt at this trace gated on a fixed 8-call
	 * counter produced zero output on a real device despite the crash
	 * still happening -- an earlier real-device run (no tracing yet)
	 * had shown the menu staying up and interactive for a real stretch
	 * of time (loading menu navigation sounds in response to actual
	 * key presses) before eventually getting kicked back to the
	 * console, so 8 calls was very likely spent entirely on
	 * uninteresting early frames, well before whatever the real
	 * culprit frame is. Gate on wall-clock time instead (cls.realtime)
	 * so the trace window covers a real stretch of actual play instead
	 * of an arbitrary small number of frames. No JS stack trace has
	 * survived being copied off the device to say which call is
	 * responsible. Same technique that found the glTexEnvi/
	 * glPointSize gaps: bracket every top-level call in this function
	 * with a before/after print, so whichever one never gets its
	 * "after" print is the one that silently threw. */
	static int kaios_scr_trace_until = 0;
	/* Only for the gl1 diagnostic build -- vid_renderer stays "soft"
	 * for the shipped app, so this trace never fires there at all. */
	qboolean kaios_scr_trace_enabled = (strcmp(vid_renderer->string, "gl1") == 0);
	if (kaios_scr_trace_enabled && kaios_scr_trace_until == 0)
	{
		/* First real (non-disable_screen-gated) frame -- start a
		 * 60 real-second window from here. */
		kaios_scr_trace_until = cls.realtime + 60000;
	}
	qboolean kaios_do_trace = kaios_scr_trace_enabled &&
		(cls.realtime < kaios_scr_trace_until);
#define KAIOS_SCR_TRACE(x) \
	do { \
		if (kaios_do_trace) { Com_Printf("KAIOS_SCR_TRACE: before " #x "\n"); } \
		x; \
		if (kaios_do_trace) { Com_Printf("KAIOS_SCR_TRACE: after " #x "\n"); } \
	} while (0)
#else
#define KAIOS_SCR_TRACE(x) x
#endif

	for (i = 0; i < numframes; i++)
	{
		R_BeginFrame(separation[i]);

		if (scr_draw_loading == 2)
		{
			/* loading plaque over black screen */
			int w, h;

			R_EndWorldRenderpass();
			if(i == 0){
				R_SetPalette(NULL);
			}

			if(i == numframes - 1){
				scr_draw_loading = false;
			}

			Draw_GetPicSize(&w, &h, "loading");
			Draw_PicScaled((viddef.width - w * scale) / 2, (viddef.height - h * scale) / 2, "loading", scale);
		}

		/* if a cinematic is supposed to be running,
		   handle menus and console specially */
		else if (cl.cinematictime > 0)
		{
			if (cls.key_dest == key_menu)
			{
				if (cl.cinematicpalette_active)
				{
					R_SetPalette(NULL);
					cl.cinematicpalette_active = false;
				}

				R_EndWorldRenderpass();
				M_Draw();
			}
			else if (cls.key_dest == key_console)
			{
				if (cl.cinematicpalette_active)
				{
					R_SetPalette(NULL);
					cl.cinematicpalette_active = false;
				}

				R_EndWorldRenderpass();
				SCR_DrawConsole();
			}
			else
			{
				R_EndWorldRenderpass();
				SCR_DrawCinematic();
			}
		}
		else
		{
			/* make sure the game palette is active */
			if (cl.cinematicpalette_active)
			{
				R_SetPalette(NULL);
				cl.cinematicpalette_active = false;
			}

			/* do 3D refresh drawing, and then update the screen */
			KAIOS_SCR_TRACE(SCR_CalcVrect());

			/* clear any dirty part of the background */
			KAIOS_SCR_TRACE(SCR_TileClear());

			KAIOS_SCR_TRACE(V_RenderView(separation[i]));

			KAIOS_SCR_TRACE(SCR_DrawStats());
			KAIOS_SCR_TRACE(SCR_DrawSpeed());
#ifdef __EMSCRIPTEN__
			KAIOS_SCR_TRACE(SCR_DrawKaiosStats());
#endif

			if (cl.frame.playerstate.stats[STAT_LAYOUTS] & 1)
			{
				KAIOS_SCR_TRACE(SCR_DrawLayout());
			}

			if (cl.frame.playerstate.stats[STAT_LAYOUTS] & 2)
			{
				KAIOS_SCR_TRACE(CL_DrawInventory());
			}

			KAIOS_SCR_TRACE(SCR_DrawNet());
			KAIOS_SCR_TRACE(SCR_CheckDrawCenterString());

			if (scr_timegraph->value)
			{
				SCR_DebugGraph(cls.rframetime * 300, 0);
			}

			if (scr_debuggraph->value || scr_timegraph->value ||
				scr_netgraph->value)
			{
				SCR_DrawDebugGraph();
			}

			KAIOS_SCR_TRACE(SCR_DrawPause());

			KAIOS_SCR_TRACE(SCR_DrawConsole());

			KAIOS_SCR_TRACE(M_Draw());

			KAIOS_SCR_TRACE(SCR_DrawLoading());
		}
	}

	SCR_Framecounter();
	KAIOS_SCR_TRACE(R_EndFrame());

#undef KAIOS_SCR_TRACE
}

static float
SCR_ClampScale(float scale, qboolean is_crosshair)
{
	float f;

#ifdef __EMSCRIPTEN__
	/* Every other yquake2 platform's screen is at least the 320x240
	 * this scale system references, where flooring at 1x below is
	 * correct and intentional (never render UI smaller than native
	 * pixels). This KaiOS port's render buffer (240x320, see
	 * autoexec.cfg) is narrower than that reference, so flooring at 1x
	 * the same way means menus/HUD/console text is stamped at full
	 * 320-reference size onto a physically narrower buffer and runs
	 * off the edges. Shrink below 1x here instead, same "compress to
	 * fit, only when narrower/shorter than reference" carve-out
	 * SCR_LayoutXV/YV already use for the HUD status bar's element
	 * positions -- this is the matching fix for element/text *size*.
	 * RE_Draw_CharScaled (sw_draw.c) is what actually has to draw at a
	 * fractional scale for this to do anything. */
	if (viddef.width < 320 || viddef.height < 240)
	{
		/* The crosshair is a single small icon centered on its own,
		 * with no neighboring status-bar elements it could collide
		 * with or run off the edge of -- unlike HUD/console/menu text,
		 * it doesn't need the shrink-to-fit ceiling below at all.
		 * Reported hard to see/aim with at the same ~0.75x everything
		 * else on this screen is capped to (a 16-24px source pic
		 * scaled down to 12-18px is genuinely blurry on a 240-wide
		 * panel) -- give it a much higher ceiling instead, independent
		 * of whatever r_hudscale/r_menuscale/r_consolescale are set
		 * to. */
		if (is_crosshair)
		{
			if (scale > 3.0f)
			{
				scale = 3.0f;
			}
			if (scale < 0.1f)
			{
				scale = 0.1f;
			}
			return scale;
		}

		f = viddef.width / 320.0f;
		if (scale > f)
		{
			scale = f;
		}

		f = viddef.height / 240.0f;
		if (scale > f)
		{
			scale = f;
		}

		/* Defensive floor, not expected to bite at any real KaiOS
		 * resolution -- just keeps a pathological manual r_hudscale/
		 * r_consolescale/r_menuscale value from producing a
		 * degenerate (near-zero-width) glyph downstream. */
		if (scale < 0.1f)
		{
			scale = 0.1f;
		}

		return scale;
	}
#endif

	f = viddef.width / 320.0f;
	if (scale > f)
	{
		scale = f;
	}

	f = viddef.height / 240.0f;
	if (scale > f)
	{
		scale = f;
	}

	if (scale < 1)
	{
		scale = 1;
	}

	return scale;
}

static float
SCR_GetDefaultScale(void)
{
#ifdef __EMSCRIPTEN__
	/* Same narrower/shorter-than-reference carve-out as SCR_ClampScale()
	 * above, using its 320x240 reference (not the 640-wide one just
	 * below, which is a *different*, high-DPI-oriented reference for
	 * auto-upscaling that has no bearing on this port). */
	if (viddef.width < 320 || viddef.height < 240)
	{
		float f = viddef.width / 320.0f;
		float g = viddef.height / 240.0f;
		float scale = (f < g) ? f : g;

		/* Requested bump: the auto-computed size (0.75x at this port's
		 * native 240x320) read as noticeably too small on a real
		 * device. 8% puts it within the requested 5-10% range without
		 * being large enough to start clipping the HUD/menu layout
		 * that SCR_LayoutXV/YV positions against this same reference. */
		scale *= 1.08f;

		if (scale < 0.1f)
		{
			scale = 0.1f;
		}

		return scale;
	}
#endif

	int i = viddef.width / 640;
	int j = viddef.height / 240;

	if (i > j)
	{
		i = j;
	}
	if (i < 1)
	{
		i = 1;
	}

	return i;
}

void
SCR_DrawCrosshair(void)
{
	float scale;

	if (!crosshair->value)
	{
		return;
	}

	if (crosshair->modified)
	{
		crosshair->modified = false;
		SCR_TouchPics();
	}

	if (!crosshair_pic[0])
	{
		return;
	}

	if (crosshair_scale->value < 0)
	{
		scale = SCR_GetDefaultScale();
	}
	else
	{
		scale = SCR_ClampScale(crosshair_scale->value, true);
	}

	float color[3];
	color[0] = crosshair_color_r->value;
	color[1] = crosshair_color_g->value;
	color[2] = crosshair_color_b->value;

	Draw_PicScaledCol(scr_vrect.x + (scr_vrect.width - crosshair_width * scale) / 2,
			scr_vrect.y + (scr_vrect.height - crosshair_height * scale) / 2,
			crosshair_pic, scale, color);
}

float
SCR_GetHUDScale(void)
{
	float scale;

	if (!scr_initialized)
	{
		scale = 1;
	}
	else if (r_hudscale->value < 0)
	{
		scale = SCR_GetDefaultScale();
	}
	else if (r_hudscale->value == 0) /* HACK: allow scale 0 to hide the HUD */
	{
		scale = 0;
	}
	else
	{
		scale = SCR_ClampScale(r_hudscale->value, false);
	}

	return scale;
}

float
SCR_GetConsoleScale(void)
{
	float scale;

	if (!scr_initialized)
	{
		scale = 1;
	}
	else if (r_consolescale->value < 0)
	{
		scale = SCR_GetDefaultScale();
	}
	else
	{
		scale = SCR_ClampScale(r_consolescale->value, false);
	}

	return scale;
}

float
SCR_GetMenuScale(void)
{
	float scale;

	if (!scr_initialized)
	{
		scale = 1;
	}
	else if (r_menuscale->value < 0)
	{
		scale = SCR_GetDefaultScale();
	}
	else
	{
		scale = SCR_ClampScale(r_menuscale->value, false);
	}

	return scale;
}
