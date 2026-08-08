/* timegrid — a horizontal, zoomable time table for the dwm desktop.
 *
 * The top row is "Date" and holds sequential time buckets. Below it sit
 * freeform rows from the watched plans file, and a "+" row that adds one.
 * Click a cell to write in it, alt-click to cycle its colour; tab and the
 * arrows move between cells while editing. The grid pans left/right forever.
 * Shift-drag selects a block of cells: delete clears their text, alt-click
 * recolours the lot. Drag a row name to reorder it, right-click one to hide it;
 * hidden rows come back through the list below the "+" row. The colours control
 * under that opens an HSV picker (picker.c) for every element of the palette.
 *
 * Zoom is continuous: the viewport is one number, pixels-per-second. The
 * ladder below is only a choice of *label resolution*, picked automatically
 * from the current zoom, so scrolling scales the grid smoothly and labels swap
 * in when their columns get wide enough to read:
 *
 *     5m  15m  1h  6h  day  week  month  year
 *
 * Wheel zooms about the cursor, button-1 drags to pan. Three sliders in the top
 * margin drive zoom, scrolling and text size — the size is written back into
 * the plans file header, so it survives a restart. The process sleeps in poll()
 * on the X
 * connection and an inotify fd until something happens — the only timer is the
 * 16ms tick while the scroll shuttle is held, so idle is zero wakeups.
 *
 * Recompile to configure — see the block below.
 */

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/XKBlib.h>
#include <X11/keysym.h>
#include <X11/Xft/Xft.h>

#include "picker.h"

#include <locale.h>
#include <math.h>
#include <poll.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- configuration ---------------------------------------------------- */

/* Text size is live — the third slider in the top margin resizes the whole
 * widget, and the chosen size is written back into the plans file header so it
 * survives a restart. This is the one piece of UI state the file carries. */
static const char *font_family = "monospace";
static const int font_size_default = 10;
static const int font_size_min = 7;
static const int font_size_max = 32;

/* Where the rows live, relative to $HOME. Overridable with argv[1]. The
 * directory is created if missing. */
static const char *plans_path = "assets/notebooks/notemaster/202607/plans.q";

/* The palette. These are only the defaults — every one is editable from the
 * colours panel and any that differ are written back to the plans file header.
 *
 * `key` is what the file calls it and must stay stable; `label` is what the
 * panel calls it and is free to change.
 *
 * COL_WRITTEN, COL_RED, COL_BLUE and COL_MIXED must stay adjacent and in that
 * order: cell colours are looked up as COL_WRITTEN + CELL_PLAIN/RED/BLUE/MIXED. */
enum {
	COL_BG, COL_FG, COL_DIM, COL_LINE, COL_GUTTER, COL_HEADER,
	COL_NOW_HEAD, COL_NOW_BODY, COL_NOW_EDGE,
	COL_TRACK, COL_KNOB,
	COL_WRITTEN, COL_RED, COL_BLUE, COL_MIXED,
	COL_SELECT, COL_EDIT, COL_COUNT
};

static const struct {
	const char *key;
	const char *label;
	const char *hex;
} palette[COL_COUNT] = {
	{ "bg",       "background",   "222222" },
	{ "fg",       "text",         "bbbbbb" },
	{ "dim",      "text dim",     "666666" },
	{ "line",     "grid line",    "333333" },
	{ "gutter",   "gutter",       "1a1a1a" },
	{ "header",   "header",       "2a2a2a" },  /* context strip + Date row */
	{ "now_head", "now head",     "005577" },  /* current bucket, Date row */
	{ "now_body", "now body",     "00323f" },  /* current bucket, rows below */
	{ "now_edge", "now edge",     "33bbee" },  /* the exact instant */
	{ "track",    "slider track", "3a3a3a" },
	{ "knob",     "slider knob",  "005577" },
	{ "written",  "written cell", "2f2f2f" },  /* has content, no colour */
	{ "red",      "red cell",     "5a2222" },
	{ "blue",     "blue cell",    "1e3a5f" },
	{ "mixed",    "mixed cell",   "452a55" },  /* red and blue in one cell */
	{ "select",   "selection",    "e0b040" },  /* shift-drag outline */
	{ "edit",     "cell edit",    "26343d" },  /* the box being typed into */
};

static const int win_y = 10;   /* gap between the screen edge and the widget */

/* The position knob is a shuttle, not a scrollbar: displacement from centre is
 * a scroll *rate*, and it springs back on release. An absolute scrollbar over
 * an endless timeline has to choose a span, and every choice is either too
 * twitchy up close or too short to reach anything. A rate has no span to pick.
 *
 * Rate is quadratic in displacement so the middle of the track gives fine
 * control and the ends give travel. */
static const double shuttle_screens_per_sec = 2.0;  /* at full deflection */
static const double shuttle_dead_zone = 0.05;       /* so it can rest at centre */
static const int shuttle_tick_ms = 16;

#define ZOOM_STEP 1.12   /* per wheel click — gentle enough to stay oriented */
#define PPS_MIN   1.9e-6 /* a year is about 60px wide */
#define PPS_MAX   1.0    /* a minute is 60px wide */

#define MAX_ROWS    64
#define MAX_LABEL   128
#define MAX_ENTRIES 1024
#define MAX_THEMES  32

/* Saved themes live outside the notebook, in $HOME. The plans path is dated
 * (and overridable), so a theme kept beside it would vanish at the turn of the
 * month — and a theme is not a property of one month's plans anyway. */
static const char *themes_path = ".timegrid_themes";

/* ---- label resolution ladder ------------------------------------------- */

enum { LVL_5M, LVL_15M, LVL_1H, LVL_6H, LVL_DAY, LVL_WEEK, LVL_MONTH, LVL_YEAR, LVL_COUNT };

/* Narrowest column that level's labels remain readable in, at the default font
 * size. layout_apply scales these with the live size — bigger text needs wider
 * columns before a tier becomes legible. */
static const int min_col_w_base[LVL_COUNT] = { 54, 54, 54, 60, 68, 74, 84, 60 };

/* Nominal bucket length, only ever used to pick a level from the zoom. */
static const double level_secs[LVL_COUNT] = {
	300, 900, 3600, 21600, 86400, 604800, 2629800, 31557600
};

/* The coarser level drawn in the context strip, or -1 for none. Deliberately
 * not just level+1: at 5m what you have lost track of is the day, not the
 * quarter hour. */
static const int context_of[LVL_COUNT] = {
	LVL_DAY, LVL_DAY, LVL_DAY, LVL_DAY, LVL_MONTH, LVL_MONTH, LVL_YEAR, -1
};

enum { DRAG_NONE, DRAG_GRID, DRAG_ZOOM, DRAG_POS, DRAG_SIZE, DRAG_SELECT,
       DRAG_ROW, DRAG_COLOR, DRAG_SHIFT };
enum { EDIT_NONE, EDIT_ROW, EDIT_CELL, EDIT_THEME };
/* The first three are the alt-click cycle and the only ones an entry can hold.
 * CELL_MIXED is display-only: cell_gather reports it when red and blue land in
 * the same cell, and it is never stored or written to the file — which is why
 * the cycle stays `% 3`. */
enum { CELL_PLAIN, CELL_RED, CELL_BLUE, CELL_MIXED };

static const char *level_names[LVL_COUNT] = {
	"5m", "15m", "1h", "6h", "day", "week", "month", "year"
};
static const char *color_names[] = { "-", "red", "blue" };

/* One thing written into one cell, at the resolution it was written at.
 *
 * The span [start, start+one bucket at `level`) is the whole model. Colour
 * applies to every display cell that span overlaps, and text lands in the
 * single display cell containing `start`. Zooming out puts many entries in one
 * cell (texts concatenate); zooming in spreads one entry over many cells (text
 * stays in the first). The two directions are the same rule, which is why they
 * round-trip. */
struct entry {
	int row;
	int level;
	time_t start;
	int color;
	char text[MAX_LABEL];
};

/* A named snapshot of the whole appearance. Exactly the fields the colours
 * panel edits — if something else becomes editable, it belongs here too. */
struct theme {
	char name[MAX_LABEL];
	unsigned char rgb[COL_COUNT][3];
	int bold[COL_COUNT];
};

struct grid {
	Display *dpy;
	int screen;
	Window win;
	Pixmap buf;
	GC gc;
	XftDraw *draw;
	XftFont *font;
	XftFont *font_bold;

	/* One allocation mechanism for the whole palette: XftColor carries a
	 * `.pixel` alongside its render colour, so the same entry serves both the
	 * rectangle calls and the text calls. */
	unsigned char rgb[COL_COUNT][3];
	int bold[COL_COUNT];
	XftColor col[COL_COUNT];
	int col_allocated;

	struct picker picker;
	int picker_open;
	int picker_element;          /* which palette entry the picker is editing */

	struct theme themes[MAX_THEMES];
	int theme_count;
	char themes_file[512];

	int w, h;
	int win_top;                 /* y on screen; window_fit slides it up to fit */
	int screen_h;                /* read once at setup, so the fitting maths is
	                              * a function of state and testable without X */

	/* Text size, and every geometry figure derived from it. font_size is the
	 * only one anybody sets; layout_apply recomputes the rest from the font
	 * it opens, so there is one source of truth rather than a scale factor
	 * sprinkled through the draw code. */
	int font_size;
	int row_h, context_h, gutter_w, pad_x, margin_top;
	int slider_zoom_y, slider_pos_y, slider_size_y, knob_r, track_end_pad;
	int min_col_w[LVL_COUNT];

	/* The entire viewport. view_left is the instant at the left edge of the
	 * grid area; pps is how many pixels one second occupies. */
	double view_left;
	double pps;

	int layout_size;             /* the size layout_apply last ran with */

	int drag;
	int drag_x;
	int drag_x0, drag_x1;        /* track bounds frozen at grab — see DRAG_SIZE */
	int drag_row;                /* row being dragged, while DRAG_ROW/DRAG_SHIFT */
	int shift_level;             /* level the ctrl-drag steps in */
	time_t shift_from;           /* first cell that moves — travels with the drag */

	double shuttle;              /* knob displacement, -1..1, 0 when released */
	struct timespec shuttle_tick;

	/* rows[] is the file's order, and row indices in `entries` point into it.
	 * Hiding does not remove a row, so the drawn order is a separate list:
	 * visible[] maps a screen position to a row index, and everything that
	 * turns a y coordinate into a row goes through it. */
	char rows[MAX_ROWS][MAX_LABEL];
	int row_hidden[MAX_ROWS];
	int row_count;
	int visible[MAX_ROWS];
	int visible_count;
	int hidden_count;
	int menu_open;               /* the "hidden rows" list is expanded */

	struct entry entries[MAX_ENTRIES];
	int entry_count;

	/* Shift-drag selection: a rectangle of rows crossed with a span of time.
	 * Anchored in real instants rather than columns, so it stays over the same
	 * cells when the view is zoomed or panned underneath it. */
	int sel_active;
	int sel_level;               /* the zoom level it was drawn at */
	int sel_anchor_row, sel_cursor_row;
	time_t sel_anchor_t, sel_cursor_t;

	int edit_mode;               /* EDIT_NONE / EDIT_ROW / EDIT_CELL */
	int edit_row;
	int edit_level;
	time_t edit_start;
	char input[MAX_LABEL];
	int input_len;
	XIM xim;
	XIC xic;

	char file_path[512];
	char file_name[512];
	char dir_path[512];
};

/* Parse "rrggbb", with or without a leading #. Returns 0 and touches nothing on
 * anything malformed, so a mistyped line in the file leaves the default rather
 * than turning some element black. */
static int
hex_to_rgb(const char *hex, unsigned char rgb[3])
{
	unsigned int r, g, b;

	if (*hex == '#') {
		hex++;
	}
	if (strlen(hex) < 6 || sscanf(hex, "%2x%2x%2x", &r, &g, &b) != 3) {
		return 0;
	}
	rgb[0] = (unsigned char)r;
	rgb[1] = (unsigned char)g;
	rgb[2] = (unsigned char)b;
	return 1;
}

/* ---- time bucketing ---------------------------------------------------- */

/* Everything walks struct tm through mktime rather than doing arithmetic on
 * time_t, so DST transitions and short/long months fall out for free. */

static time_t
bucket_start(time_t t, int level)
{
	struct tm tm;
	localtime_r(&t, &tm);
	tm.tm_isdst = -1;

	switch (level) {
	case LVL_YEAR:  tm.tm_mon = 0;                      /* fall through */
	case LVL_MONTH: tm.tm_mday = 1;                     /* fall through */
	case LVL_DAY:   tm.tm_hour = 0;                     /* fall through */
	case LVL_1H:    tm.tm_min = 0; tm.tm_sec = 0; break;
	case LVL_WEEK:
		tm.tm_mday -= (tm.tm_wday + 6) % 7;         /* week starts Monday */
		tm.tm_hour = 0; tm.tm_min = 0; tm.tm_sec = 0;
		break;
	case LVL_6H:
		tm.tm_hour -= tm.tm_hour % 6;
		tm.tm_min = 0; tm.tm_sec = 0;
		break;
	case LVL_15M:
		tm.tm_min -= tm.tm_min % 15;
		tm.tm_sec = 0;
		break;
	case LVL_5M:
		tm.tm_min -= tm.tm_min % 5;
		tm.tm_sec = 0;
		break;
	}
	return mktime(&tm);
}

static time_t
bucket_step(time_t t, int level, int dir)
{
	struct tm tm;
	localtime_r(&t, &tm);
	tm.tm_isdst = -1;

	switch (level) {
	case LVL_5M:    tm.tm_min  += 5 * dir;  break;
	case LVL_15M:   tm.tm_min  += 15 * dir; break;
	case LVL_1H:    tm.tm_hour += dir;      break;
	case LVL_6H:    tm.tm_hour += 6 * dir;  break;
	case LVL_DAY:   tm.tm_mday += dir;      break;
	case LVL_WEEK:  tm.tm_mday += 7 * dir;  break;
	case LVL_MONTH: tm.tm_mon  += dir;      break;
	case LVL_YEAR:  tm.tm_year += dir;      break;
	}
	return mktime(&tm);  /* mktime normalises the out-of-range field */
}

static void
bucket_label(time_t t, int level, char *out, size_t n)
{
	struct tm tm;
	localtime_r(&t, &tm);

	switch (level) {
	case LVL_5M:
	case LVL_15M:
	case LVL_1H:
	case LVL_6H:    strftime(out, n, "%H:%M", &tm); break;
	case LVL_DAY:   strftime(out, n, "%a %d", &tm); break;
	case LVL_WEEK:  strftime(out, n, "%d %b", &tm); break;
	case LVL_MONTH: strftime(out, n, "%b %Y", &tm); break;
	case LVL_YEAR:  strftime(out, n, "%Y", &tm);    break;
	}
}

static void
context_label(time_t t, int level, char *out, size_t n)
{
	struct tm tm;
	localtime_r(&t, &tm);

	switch (level) {
	case LVL_DAY:   strftime(out, n, "%a %d %B %Y", &tm); break;
	case LVL_MONTH: strftime(out, n, "%B %Y", &tm);       break;
	case LVL_YEAR:  strftime(out, n, "%Y", &tm);          break;
	default:        out[0] = '\0';                        break;
	}
}

/* ---- viewport ---------------------------------------------------------- */

/* The finest resolution whose columns are still wide enough to read. */
static int
zoom_level(struct grid *g)
{
	int i;

	for (i = 0; i < LVL_COUNT; i++) {
		if (level_secs[i] * g->pps >= g->min_col_w[i]) {
			return i;
		}
	}
	return LVL_YEAR;
}

static double
time_at_pixel(struct grid *g, int px)
{
	return g->view_left + (px - g->gutter_w) / g->pps;
}

static double
pixel_at_time(struct grid *g, double t)
{
	return g->gutter_w + (t - g->view_left) * g->pps;
}

static void
pan_by(struct grid *g, int dx)
{
	g->view_left -= dx / g->pps;
}

/* Change zoom while keeping the instant under the cursor put. */
static void
zoom_at(struct grid *g, double factor, int px)
{
	double anchor = time_at_pixel(g, px);

	g->pps *= factor;
	if (g->pps < PPS_MIN) {
		g->pps = PPS_MIN;
	}
	if (g->pps > PPS_MAX) {
		g->pps = PPS_MAX;
	}
	g->view_left = anchor - (px - g->gutter_w) / g->pps;
}

/* ---- sliders ----------------------------------------------------------- */

/* Both tracks share the same span, so the two knobs read as one control
 * surface rather than two unrelated widgets. */
static void
track_bounds(struct grid *g, int *x0, int *x1)
{
	*x0 = g->gutter_w + g->knob_r;
	*x1 = g->w - g->track_end_pad - g->knob_r;
}

static double
zoom_fraction(struct grid *g)
{
	return (log(g->pps) - log(PPS_MIN)) / (log(PPS_MAX) - log(PPS_MIN));
}

static void
zoom_set_fraction(struct grid *g, double frac)
{
	double anchor;
	int centre = (g->gutter_w + g->w) / 2;

	if (frac < 0.0) {
		frac = 0.0;
	}
	if (frac > 1.0) {
		frac = 1.0;
	}
	anchor = time_at_pixel(g, centre);
	g->pps = exp(log(PPS_MIN) + frac * (log(PPS_MAX) - log(PPS_MIN)));
	g->view_left = anchor - (centre - g->gutter_w) / g->pps;
}

static void
size_set_fraction(struct grid *g, double frac)
{
	if (frac < 0.0) {
		frac = 0.0;
	}
	if (frac > 1.0) {
		frac = 1.0;
	}
	g->font_size = font_size_min +
	               (int)(frac * (font_size_max - font_size_min) + 0.5);
}

static void
shuttle_grab(struct grid *g, int px)
{
	int x0, x1, centre;

	track_bounds(g, &x0, &x1);
	centre = (x0 + x1) / 2;
	g->shuttle = (double)(px - centre) / ((x1 - x0) / 2);
	if (g->shuttle < -1.0) {
		g->shuttle = -1.0;
	}
	if (g->shuttle > 1.0) {
		g->shuttle = 1.0;
	}
}

/* Scroll by however much wall-clock has passed since the last tick, rather
 * than a fixed step per tick, so the speed you see is the speed you asked for
 * even if redraws are late. */
static void
shuttle_advance(struct grid *g)
{
	struct timespec now;
	double dt, rate;

	clock_gettime(CLOCK_MONOTONIC, &now);
	dt = (now.tv_sec - g->shuttle_tick.tv_sec) +
	     (now.tv_nsec - g->shuttle_tick.tv_nsec) / 1e9;
	g->shuttle_tick = now;

	if (dt <= 0.0 || dt > 0.5) {   /* first tick, or we were descheduled */
		return;
	}
	if (fabs(g->shuttle) < shuttle_dead_zone) {
		return;
	}
	rate = g->shuttle * fabs(g->shuttle) * shuttle_screens_per_sec;
	g->view_left += rate * (g->w - g->gutter_w) * dt / g->pps;
}

/* ---- saved themes ------------------------------------------------------- */

/* Same shape as the plans file, for the same reason: unindented is a name,
 * indented is an attribute of the name above it, and a line that does not
 * parse is skipped rather than guessed at.
 *
 * A theme records only what it was told to record. Colours the saving widget
 * had at their defaults are written out explicitly all the same — otherwise
 * loading a theme would leave whatever the *current* palette had in those
 * slots, and a theme that is only a partial overwrite is not a theme. */
static void
themes_load(struct grid *g)
{
	FILE *f;
	char line[MAX_LABEL * 3];
	char *start, *end;
	int current = -1;

	g->theme_count = 0;
	if (!(f = fopen(g->themes_file, "r"))) {
		return;
	}
	while (fgets(line, sizeof line, f)) {
		int indented = (line[0] == ' ' || line[0] == '\t');
		char key[32], hex[16];
		int on, i;

		start = line;
		while (*start == ' ' || *start == '\t') {
			start++;
		}
		end = start + strlen(start);
		while (end > start && (end[-1] == '\n' || end[-1] == '\r' ||
		                       end[-1] == ' ' || end[-1] == '\t')) {
			end--;
		}
		*end = '\0';
		if (*start == '\0' || *start == '#') {
			continue;
		}
		if (!indented) {
			current = -1;
			if (g->theme_count < MAX_THEMES) {
				current = g->theme_count++;
				memset(&g->themes[current], 0, sizeof g->themes[current]);
				snprintf(g->themes[current].name, MAX_LABEL, "%s", start);
				for (i = 0; i < COL_COUNT; i++) {
					hex_to_rgb(palette[i].hex, g->themes[current].rgb[i]);
				}
			}
			continue;
		}
		if (current < 0) {
			continue;
		}
		if (sscanf(start, "color %31s %15s", key, hex) == 2) {
			for (i = 0; i < COL_COUNT; i++) {
				if (strcmp(key, palette[i].key) == 0) {
					hex_to_rgb(hex, g->themes[current].rgb[i]);
				}
			}
		} else if (sscanf(start, "bold %31s %d", key, &on) == 2) {
			for (i = 0; i < COL_COUNT; i++) {
				if (strcmp(key, palette[i].key) == 0) {
					g->themes[current].bold[i] = on != 0;
				}
			}
		}
	}
	fclose(f);
}

/* Temp file plus rename, like the plans file: losing a themes file matters
 * less, but there is no reason to write it worse. */
static int
themes_save(struct grid *g)
{
	char tmp_path[600];
	FILE *out;
	int t, i;

	snprintf(tmp_path, sizeof tmp_path, "%s.tmp", g->themes_file);
	if (!(out = fopen(tmp_path, "w"))) {
		return 0;
	}
	fputs("# timegrid themes — saved from the colours panel.\n"
	      "# Unindented lines name a theme; indented lines are its settings.\n\n",
	      out);
	for (t = 0; t < g->theme_count; t++) {
		fprintf(out, "%s\n", g->themes[t].name);
		for (i = 0; i < COL_COUNT; i++) {
			fprintf(out, "  color %-8s %02x%02x%02x\n", palette[i].key,
			        g->themes[t].rgb[i][0], g->themes[t].rgb[i][1],
			        g->themes[t].rgb[i][2]);
		}
		for (i = 0; i < COL_COUNT; i++) {
			if (g->themes[t].bold[i]) {
				fprintf(out, "  bold  %-8s 1\n", palette[i].key);
			}
		}
		fputc('\n', out);
	}
	if (fflush(out) != 0) {
		fclose(out);
		unlink(tmp_path);
		return 0;
	}
	fsync(fileno(out));
	fclose(out);
	if (rename(tmp_path, g->themes_file) != 0) {
		unlink(tmp_path);
		return 0;
	}
	return 1;
}

/* Save the current appearance under a name, replacing any theme already using
 * it — saving twice under one name should overwrite, not accumulate. */
static int
theme_store(struct grid *g, const char *name)
{
	int t, found = -1;

	while (*name == ' ' || *name == '\t' || *name == '#') {
		name++;   /* a name read back as a comment would lose the theme */
	}
	if (*name == '\0') {
		return 0;
	}
	for (t = 0; t < g->theme_count; t++) {
		if (strcmp(g->themes[t].name, name) == 0) {
			found = t;
		}
	}
	if (found < 0) {
		if (g->theme_count >= MAX_THEMES) {
			return 0;
		}
		found = g->theme_count++;
	}
	snprintf(g->themes[found].name, MAX_LABEL, "%s", name);
	memcpy(g->themes[found].rgb, g->rgb, sizeof g->rgb);
	memcpy(g->themes[found].bold, g->bold, sizeof g->bold);
	return themes_save(g);
}

static void
theme_recall(struct grid *g, int t)
{
	if (t < 0 || t >= g->theme_count) {
		return;
	}
	memcpy(g->rgb, g->themes[t].rgb, sizeof g->rgb);
	memcpy(g->bold, g->themes[t].bold, sizeof g->bold);
}

/* ---- rows -------------------------------------------------------------- */

/* Rebuild the screen-position -> row mapping. Call after anything that adds,
 * removes, reorders or hides a row; every y-to-row conversion reads it. */
static void
rows_index(struct grid *g)
{
	int i;

	g->visible_count = 0;
	g->hidden_count = 0;
	for (i = 0; i < g->row_count; i++) {
		if (g->row_hidden[i]) {
			g->hidden_count++;
		} else {
			g->visible[g->visible_count++] = i;
		}
	}
	if (g->hidden_count == 0) {
		g->menu_open = 0;   /* nothing to list, so no menu to leave open */
	}
}

/* Where a row sits on screen, or -1 if it is hidden. */
static int
row_visible_index(struct grid *g, int row)
{
	int i;

	for (i = 0; i < g->visible_count; i++) {
		if (g->visible[i] == row) {
			return i;
		}
	}
	return -1;
}

static void
plans_load(struct grid *g)
{
	FILE *f;
	char line[MAX_LABEL * 3];
	char *start, *end;
	int current_row = -1;

	g->row_count = 0;
	g->entry_count = 0;
	if (!(f = fopen(g->file_path, "r"))) {
		rows_index(g);   /* the file can go away under us */
		return;
	}
	while (fgets(line, sizeof line, f)) {
		/* Indentation is what distinguishes a cell from a row name, so it
		 * has to be read before the whitespace is trimmed off. */
		int indented = (line[0] == ' ' || line[0] == '\t');

		start = line;
		while (*start == ' ' || *start == '\t') {
			start++;
		}
		end = start + strlen(start);
		while (end > start && (end[-1] == '\n' || end[-1] == '\r' ||
		                       end[-1] == ' ' || end[-1] == '\t')) {
			end--;
		}
		*end = '\0';
		if (*start == '#') {
			/* The two settings the file carries. Reading them back here means
			 * a hand-edit takes effect on save, same as using the controls. */
			char key[32], hex[16];
			int size, on, i;

			if (sscanf(start, "# font_size %d", &size) == 1) {
				g->font_size = size;
			} else if (sscanf(start, "# color %31s %15s", key, hex) == 2) {
				for (i = 0; i < COL_COUNT; i++) {
					if (strcmp(key, palette[i].key) == 0) {
						hex_to_rgb(hex, g->rgb[i]);
					}
				}
			} else if (sscanf(start, "# bold %31s %d", key, &on) == 2) {
				for (i = 0; i < COL_COUNT; i++) {
					if (strcmp(key, palette[i].key) == 0) {
						g->bold[i] = on != 0;
					}
				}
			}
			continue;
		}
		if (*start == '\0') {
			continue;
		}

		if (!indented) {
			/* A leading ~ means hidden. Same shape as the # that means
			 * comment, and the same sharp edge: a row name cannot begin with
			 * one, so plans_add_row strips both. */
			int hidden = (*start == '~');

			if (hidden) {
				start++;
				while (*start == ' ' || *start == '\t') {
					start++;
				}
			}
			current_row = -1;
			if (g->row_count < MAX_ROWS) {
				snprintf(g->rows[g->row_count], MAX_LABEL, "%s", start);
				g->row_hidden[g->row_count] = hidden;
				current_row = g->row_count++;
			}
			continue;
		}

		if (current_row >= 0 && g->entry_count < MAX_ENTRIES) {
			struct entry *e = &g->entries[g->entry_count];
			struct tm tm = {0};
			char lvl[16], col[16];
			int year, mon, day, hour, min, rest = 0, i;

			if (sscanf(start, "%d-%d-%d %d:%d %15s %15s %n",
			           &year, &mon, &day, &hour, &min, lvl, col, &rest) < 7) {
				continue;   /* not a cell line; ignore rather than guess */
			}
			e->level = -1;
			for (i = 0; i < LVL_COUNT; i++) {
				if (strcmp(lvl, level_names[i]) == 0) {
					e->level = i;
				}
			}
			e->color = -1;
			for (i = 0; i < 3; i++) {
				if (strcmp(col, color_names[i]) == 0) {
					e->color = i;
				}
			}
			if (e->level < 0 || e->color < 0) {
				continue;
			}
			tm.tm_year = year - 1900;
			tm.tm_mon = mon - 1;
			tm.tm_mday = day;
			tm.tm_hour = hour;
			tm.tm_min = min;
			tm.tm_isdst = -1;
			e->row = current_row;
			e->start = bucket_start(mktime(&tm), e->level);
			snprintf(e->text, MAX_LABEL, "%s", rest > 0 ? start + rest : "");
			g->entry_count++;
		}
	}
	fclose(f);
	rows_index(g);
}

static int
entry_cmp(const void *a, const void *b)
{
	const struct entry *ea = a, *eb = b;

	if (ea->row != eb->row) {
		return ea->row - eb->row;
	}
	if (ea->start != eb->start) {
		return ea->start < eb->start ? -1 : 1;
	}
	return ea->level - eb->level;
}

static int
entry_find(struct grid *g, int row, int level, time_t start)
{
	int i;

	for (i = 0; i < g->entry_count; i++) {
		if (g->entries[i].row == row && g->entries[i].level == level &&
		    g->entries[i].start == start) {
			return i;
		}
	}
	return -1;
}

/* Empty text with no colour is not an entry, it is the absence of one — so
 * setting a cell back to blank deletes it rather than leaving a husk in the
 * file. */
static void
entry_set(struct grid *g, int row, int level, time_t start,
          const char *text, int color)
{
	int i = entry_find(g, row, level, start);

	if (text[0] == '\0' && color == CELL_PLAIN) {
		if (i >= 0) {
			memmove(&g->entries[i], &g->entries[i + 1],
			        (g->entry_count - i - 1) * sizeof *g->entries);
			g->entry_count--;
		}
		return;
	}
	if (i < 0) {
		if (g->entry_count >= MAX_ENTRIES) {
			return;
		}
		i = g->entry_count++;
		g->entries[i].row = row;
		g->entries[i].level = level;
		g->entries[i].start = start;
	}
	g->entries[i].color = color;
	snprintf(g->entries[i].text, MAX_LABEL, "%s", text);
	qsort(g->entries, g->entry_count, sizeof *g->entries, entry_cmp);
}

/* mkdir -p. The notebook directory is dated, so it routinely does not exist
 * yet the first time the widget runs in a new month. */
static void
mkdir_parents(const char *path)
{
	char partial[512];
	char *p;

	snprintf(partial, sizeof partial, "%s", path);
	for (p = partial + 1; *p; p++) {
		if (*p != '/') {
			continue;
		}
		*p = '\0';
		mkdir(partial, 0755);
		*p = '/';
	}
	mkdir(partial, 0755);
}

/* Written on first creation so the file explains itself to whoever opens it
 * in an editor, which is the point of keeping it plain text. */
static const char *plans_header =
	"# notemaster — plans\n"
	"#\n"
	"# One row per line. Each line becomes a row of the timegrid desktop\n"
	"# widget, in this order, top to bottom.\n"
	"#\n"
	"# Blank lines and lines starting with # are ignored, so a row name\n"
	"# cannot itself begin with # — leading hashes are dropped when a row is\n"
	"# added from the widget.\n"
	"#\n"
	"# Safe to edit by hand — the widget reloads by itself when you save.\n"
	"#\n"
	"# The text size below is driven by the third slider, and lives here so it\n"
	"# survives a restart. Editing the number by hand works too.\n";

/* Write the whole file out from memory.
 *
 * Cells are edited in place, so the append-only writer this replaced no longer
 * works and the body has to be regenerated. The leading comment block is
 * copied across verbatim so the header — and anything the user wrote at the
 * top — survives; comments further down, interleaved with rows, do not.
 *
 * Temp file plus rename, never a truncate in place: a crash midway would
 * otherwise cost the user the file. */
static int
plans_save(struct grid *g)
{
	char tmp_path[600];
	char line[MAX_LABEL * 3];
	FILE *in, *out;
	int row, i, wrote_header = 0;

	snprintf(tmp_path, sizeof tmp_path, "%s.tmp", g->file_path);
	if (!(out = fopen(tmp_path, "w"))) {
		return 0;
	}
	if ((in = fopen(g->file_path, "r"))) {
		while (fgets(line, sizeof line, in)) {
			const char *p = line;
			while (*p == ' ' || *p == '\t') {
				p++;
			}
			if (*p != '#' && *p != '\n' && *p != '\r' && *p != '\0') {
				break;   /* first real line: the header ends here */
			}
			if (*p == '#') {
				char key[32], hex[16];
				int size;

				/* Drop the settings lines; they are re-emitted below from the
				 * live values. Everything else in the header is copied. */
				if (sscanf(p, "# font_size %d", &size) == 1 ||
				    sscanf(p, "# color %31s %15s", key, hex) == 2 ||
				    sscanf(p, "# bold %31s %d", key, &size) == 2) {
					continue;
				}
				fputs(line, out);
				wrote_header = 1;
			}
		}
		fclose(in);
	}
	if (!wrote_header) {
		fputs(plans_header, out);
	}
	fprintf(out, "# font_size %d\n", g->font_size);
	/* Only colours that differ from the compiled-in defaults, so a file
	 * belonging to someone who never opened the panel stays clean. */
	for (i = 0; i < COL_COUNT; i++) {
		unsigned char def[3];

		if (hex_to_rgb(palette[i].hex, def) &&
		    def[0] == g->rgb[i][0] && def[1] == g->rgb[i][1] &&
		    def[2] == g->rgb[i][2]) {
			continue;
		}
		fprintf(out, "# color %-8s %02x%02x%02x\n", palette[i].key,
		        g->rgb[i][0], g->rgb[i][1], g->rgb[i][2]);
	}
	for (i = 0; i < COL_COUNT; i++) {
		if (g->bold[i]) {
			fprintf(out, "# bold  %-8s 1\n", palette[i].key);
		}
	}
	fputc('\n', out);

	qsort(g->entries, g->entry_count, sizeof *g->entries, entry_cmp);
	for (row = 0; row < g->row_count; row++) {
		fprintf(out, "%s%s\n", g->row_hidden[row] ? "~" : "", g->rows[row]);
		for (i = 0; i < g->entry_count; i++) {
			struct entry *e = &g->entries[i];
			struct tm tm;

			if (e->row != row) {
				continue;
			}
			localtime_r(&e->start, &tm);
			fprintf(out, "  %04d-%02d-%02d %02d:%02d  %-5s  %-4s  %s\n",
			        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
			        tm.tm_hour, tm.tm_min,
			        level_names[e->level], color_names[e->color], e->text);
		}
		fputc('\n', out);
	}

	if (fflush(out) != 0) {
		fclose(out);
		unlink(tmp_path);
		return 0;
	}
	fsync(fileno(out));
	fclose(out);

	if (rename(tmp_path, g->file_path) != 0) {
		unlink(tmp_path);
		return 0;
	}
	return 1;
}

static int
plans_add_row(struct grid *g, const char *name)
{
	/* A name starting with # would be read back as a comment, and one starting
	 * with ~ as a hidden row; either way the row silently vanishes or turns
	 * invisible. Drop the markers rather than lose the row. */
	while (*name == '#' || *name == '~' || *name == ' ' || *name == '\t') {
		name++;
	}
	if (*name == '\0' || g->row_count >= MAX_ROWS) {
		return 0;
	}
	snprintf(g->rows[g->row_count], MAX_LABEL, "%s", name);
	g->row_hidden[g->row_count] = 0;
	g->row_count++;
	rows_index(g);
	return plans_save(g);
}

/* Move a row to another index, sliding everything between it and the
 * destination along by one. Entry row indices point into rows[], so they have
 * to be remapped in the same breath or every cell lands under the wrong name.
 *
 * The file's line order *is* the row order, so saving afterwards is all the
 * persistence this needs. */
static void
rows_move(struct grid *g, int from, int to)
{
	char moved[MAX_LABEL];
	int moved_hidden, i, step;

	if (from == to || from < 0 || to < 0 ||
	    from >= g->row_count || to >= g->row_count) {
		return;
	}
	snprintf(moved, MAX_LABEL, "%s", g->rows[from]);
	moved_hidden = g->row_hidden[from];

	step = from < to ? +1 : -1;
	for (i = from; i != to; i += step) {
		snprintf(g->rows[i], MAX_LABEL, "%s", g->rows[i + step]);
		g->row_hidden[i] = g->row_hidden[i + step];
	}
	snprintf(g->rows[to], MAX_LABEL, "%s", moved);
	g->row_hidden[to] = moved_hidden;

	for (i = 0; i < g->entry_count; i++) {
		int r = g->entries[i].row;

		if (r == from) {
			g->entries[i].row = to;
		} else if (step > 0 && r > from && r <= to) {
			g->entries[i].row = r - 1;
		} else if (step < 0 && r >= to && r < from) {
			g->entries[i].row = r + 1;
		}
	}
	rows_index(g);
}

/* Ctrl-drag on a row slides everything from the grabbed cell rightwards along
 * the timeline — the gesture for "all of this is happening later than I
 * thought". Only that row moves, and only entries at or after `from`.
 *
 * Every entry keeps its own level; it is the *display* level being dragged at
 * that sets the step, so at day zoom a push is a day even for an entry written
 * at 5m. Stepping one bucket at a time through bucket_step rather than adding
 * seconds is what keeps a month push landing on the 1st and a day push landing
 * at midnight across a DST change.
 *
 * Two entries can be pushed onto the same instant — the group closing up on a
 * cell it did not reach past. That is left alone rather than merged: the display
 * already aggregates several entries in one cell, and dragging back apart
 * restores them.
 *
 * Saved per whole-cell step rather than on release, so the widget never holds
 * plans the file does not — which is the invariant the inotify echo trap depends
 * on. A step is coarse, so this is not a write per pixel. */
static void
row_shift_from(struct grid *g, int row, int level, time_t from, int steps)
{
	int i, n;

	if (steps == 0) {
		return;
	}
	plans_load(g);
	for (i = 0; i < g->entry_count; i++) {
		if (g->entries[i].row != row || g->entries[i].start < from) {
			continue;
		}
		for (n = 0; n < abs(steps); n++) {
			g->entries[i].start = bucket_step(g->entries[i].start, level,
			                                  steps > 0 ? +1 : -1);
		}
	}
	qsort(g->entries, g->entry_count, sizeof *g->entries, entry_cmp);
	plans_save(g);
	plans_load(g);
}

/* Hiding keeps the row and its cells; it only drops out of the drawn order and
 * gains a ~ in the file. */
static void
row_set_hidden(struct grid *g, int row, int hidden)
{
	if (row < 0 || row >= g->row_count) {
		return;
	}
	g->row_hidden[row] = hidden;
	rows_index(g);
	plans_save(g);
}

/* Delete the trailing word: any trailing spaces first, then the word itself.
 * Byte comparison is safe — no UTF-8 continuation byte can equal a space. */
static void
input_delete_word(struct grid *g)
{
	while (g->input_len > 0 && g->input[g->input_len - 1] == ' ') {
		g->input_len--;
	}
	while (g->input_len > 0 && g->input[g->input_len - 1] != ' ') {
		g->input_len--;
	}
	g->input[g->input_len] = '\0';
}

static void
cell_edit_begin(struct grid *g, int row, int level, time_t start)
{
	int idx = entry_find(g, row, level, start);

	g->edit_mode = EDIT_CELL;
	g->edit_row = row;
	g->edit_level = level;
	g->edit_start = start;
	snprintf(g->input, sizeof g->input, "%s",
	         idx >= 0 ? g->entries[idx].text : "");
	g->input_len = (int)strlen(g->input);
}

/* Re-read before applying: the file is the source of truth and may have been
 * edited since we loaded it, so the change goes onto that rather than onto a
 * stale snapshot.
 *
 * Returns without touching the disk when nothing changed, which is what keeps
 * arrow-key navigation from rewriting the file on every keystroke. */
static void
cell_commit(struct grid *g)
{
	int idx = entry_find(g, g->edit_row, g->edit_level, g->edit_start);
	const char *current = idx >= 0 ? g->entries[idx].text : "";

	if (strcmp(current, g->input) == 0) {
		return;
	}
	plans_load(g);
	idx = entry_find(g, g->edit_row, g->edit_level, g->edit_start);
	entry_set(g, g->edit_row, g->edit_level, g->edit_start, g->input,
	          idx >= 0 ? g->entries[idx].color : CELL_PLAIN);
	plans_save(g);
	plans_load(g);
}

/* Commit what is being typed, then move the edit one cell along and scroll it
 * into view if it fell off an edge. */
static void
cell_move(struct grid *g, int d_time, int d_row)
{
	time_t start = g->edit_start;
	double x, nx;
	int vis, row;

	if (d_time != 0) {
		start = bucket_step(start, g->edit_level, d_time);
	}
	cell_commit(g);

	if (g->visible_count == 0) {
		g->edit_mode = EDIT_NONE;
		return;
	}
	/* Step through drawn positions, not row indices, so the edit never lands
	 * on a hidden row. Clamps at the ends rather than wrapping. */
	vis = row_visible_index(g, g->edit_row);
	if (vis < 0) {
		vis = 0;
	}
	vis += d_row;
	if (vis < 0) {
		vis = 0;
	}
	if (vis >= g->visible_count) {
		vis = g->visible_count - 1;
	}
	row = g->visible[vis];
	cell_edit_begin(g, row, g->edit_level, start);

	x = pixel_at_time(g, (double)start);
	nx = pixel_at_time(g, (double)bucket_step(start, g->edit_level, +1));
	if (x < g->gutter_w) {
		pan_by(g, (int)(g->gutter_w - x));
	} else if (nx > g->w) {
		pan_by(g, -(int)(nx - g->w));
	}
}

/* ---- selection --------------------------------------------------------- */

/* The shift-drag block, normalised: drawn positions [v0, v1] inclusive, time
 * [t0, t1) half-open.
 *
 * Rows are held as *drawn positions*, not row indices. A block the user dragged
 * over is contiguous on screen but its row indices need not be — a hidden row
 * can sit between two of them — and treating it as an index range would quietly
 * sweep that hidden row into every bulk edit. Positions are clamped on the way
 * out because the file can be reloaded, and rows removed, while a block is up. */
static void
selection_bounds(struct grid *g, int *v0, int *v1, time_t *t0, time_t *t1)
{
	time_t lo = g->sel_anchor_t, hi = g->sel_cursor_t;

	*v0 = g->sel_anchor_row < g->sel_cursor_row ? g->sel_anchor_row : g->sel_cursor_row;
	*v1 = g->sel_anchor_row > g->sel_cursor_row ? g->sel_anchor_row : g->sel_cursor_row;
	if (*v0 < 0) {
		*v0 = 0;
	}
	if (*v1 >= g->visible_count) {
		*v1 = g->visible_count - 1;
	}
	if (lo > hi) {
		lo = g->sel_cursor_t;
		hi = g->sel_anchor_t;
	}
	*t0 = lo;
	*t1 = bucket_step(hi, g->sel_level, +1);
}

/* Delete over a selection clears the text of every entry that *starts* inside
 * it. Text is drawn in the cell holding the entry's start, so this removes
 * exactly the words you can see in the selected cells, and it can never reach
 * an entry you did not select: a month-long entry beginning before the block
 * keeps its text, which is drawn in a cell outside the block anyway.
 *
 * Clearing follows the single-cell rule — colour survives, and an entry left
 * with neither text nor colour stops being an entry. */
static void
selection_clear_text(struct grid *g)
{
	int i, kept = 0, v0, v1;
	time_t t0, t1;

	plans_load(g);
	selection_bounds(g, &v0, &v1, &t0, &t1);

	for (i = 0; i < g->entry_count; i++) {
		struct entry *e = &g->entries[i];
		int vis = row_visible_index(g, e->row);

		if (vis >= v0 && vis <= v1 && e->start >= t0 && e->start < t1) {
			e->text[0] = '\0';
		}
	}
	/* Compacting in place rather than calling entry_set per cell: entry_set
	 * re-sorts, which would invalidate the index we are walking. */
	for (i = 0; i < g->entry_count; i++) {
		if (g->entries[i].text[0] == '\0' && g->entries[i].color == CELL_PLAIN) {
			continue;
		}
		g->entries[kept++] = g->entries[i];
	}
	g->entry_count = kept;

	plans_save(g);
	plans_load(g);
}

/* Alt-click over a selection is the single-cell alt-click repeated across every
 * cell in it, at the level the selection was drawn at. The whole block takes
 * one step on from the top-left cell's colour, so a mixed block converges on
 * the first click and cycles together from then on. */
static void
selection_set_color(struct grid *g)
{
	int v0, v1, vis, row, idx, next;
	time_t t0, t1, t;

	plans_load(g);
	selection_bounds(g, &v0, &v1, &t0, &t1);
	if (v1 < v0) {
		return;
	}
	idx = entry_find(g, g->visible[v0], g->sel_level, t0);
	next = (idx >= 0 ? g->entries[idx].color + 1 : CELL_RED) % 3;

	for (vis = v0; vis <= v1; vis++) {
		row = g->visible[vis];
		for (t = t0; t < t1; t = bucket_step(t, g->sel_level, +1)) {
			char kept[MAX_LABEL];

			idx = entry_find(g, row, g->sel_level, t);
			snprintf(kept, sizeof kept, "%s",
			         idx >= 0 ? g->entries[idx].text : "");
			entry_set(g, row, g->sel_level, t, kept, next);
		}
	}
	plans_save(g);
	plans_load(g);
}

#define MAX_PIECES 32

/* What one display cell holds, given that entries may be coarser than the
 * current zoom (expand), finer (aggregate), or exactly on it. Returns the
 * number of text pieces, in time order, and reports the cell's colour and how
 * many contributing entries are not at the display level.
 *
 * Colour votes: a single chromatic colour wins; red and blue together are
 * CELL_MIXED, a colour of its own. They used to cancel to the plain "written"
 * shade, which lost the fact that anything chromatic was there at all.
 *
 * `extra` counts entries at any level but `level` that have **text** — the
 * writing you cannot fully see here, because it was aggregated into this cell
 * from finer ones or expanded into it from a coarser one. At most one entry can
 * sit at the display level (same level and same bucket means the same entry),
 * so this is also "all the writing beyond the cell's own".
 *
 * A colour-only entry is deliberately not counted. It has nothing to show, and
 * what it contributes — its colour — is already in the cell's own fill, so a
 * count pointing at it sends you looking for writing that was never there.
 * edit_stack_gather skips them for the same reason and must keep agreeing.
 *
 * Deliberately does no text measurement, so the whole aggregate/expand model
 * is testable without an X connection. Fitting is cell_text's job. */
static int
cell_gather(struct grid *g, int row, time_t t, time_t t_next, int level,
            int *color, int *filled, int *extra, const char *pieces[],
            int max_pieces)
{
	int i, count = 0, has_red = 0, has_blue = 0;

	*color = CELL_PLAIN;
	*filled = 0;
	*extra = 0;

	for (i = 0; i < g->entry_count; i++) {
		struct entry *e = &g->entries[i];
		time_t e_end;

		if (e->row != row) {
			continue;
		}
		e_end = bucket_step(e->start, e->level, +1);
		if (e_end <= t || e->start >= t_next) {
			continue;   /* span does not reach this cell */
		}
		*filled = 1;
		if (e->level != level && e->text[0] != '\0') {
			(*extra)++;
		}
		if (e->color == CELL_RED) {
			has_red = 1;
		}
		if (e->color == CELL_BLUE) {
			has_blue = 1;
		}

		/* Text belongs to the cell holding the entry's start — which is the
		 * first cell when zoomed in, and this cell when zoomed out. */
		if (e->start >= t && e->start < t_next && e->text[0] != '\0' &&
		    count < max_pieces) {
			pieces[count++] = e->text;
		}
	}
	if (has_red && has_blue) {
		*color = CELL_MIXED;
	} else if (has_red) {
		*color = CELL_RED;
	} else if (has_blue) {
		*color = CELL_BLUE;
	}
	return count;
}

/* Everything else inside one cell's span that was written at some other
 * resolution — what the edit box opens out around itself, and exactly what
 * cell_gather counted as `extra`.
 *
 * The same overlap test as cell_gather, and it must keep the same acceptance,
 * because the count drawn in the cell is a promise about what opens out of it.
 * Getting that wrong has cost two rounds already: requiring text here but not
 * there dropped a strip the count had promised, and taking colour-only entries
 * in both places put an empty band at the top of the stack — the earliest hour
 * of a shift-coloured block has no writing in it, so it sorts first and shows
 * nothing. Neither reads as a cell; both read as a bug.
 *
 * So: **entries with text, at another level.** A colour-only entry has nothing
 * to open out, and its colour is already in the cell's fill. Two tests hold the
 * two functions to the same predicate; if one gains a condition, so must the
 * other.
 *
 * No measurement and no geometry, so the choice of what to show is testable
 * without an X connection. Fitting the strips into the window is the draw
 * pass's job, the same split as cell_gather and cell_text. */
static int
edit_stack_gather(struct grid *g, int row, int level, time_t start,
                  const struct entry *out[], int max)
{
	time_t span_end = bucket_step(start, level, +1);
	int i, count = 0;

	for (i = 0; i < g->entry_count && count < max; i++) {
		const struct entry *e = &g->entries[i];

		if (e->row != row || e->level == level || e->text[0] == '\0') {
			continue;
		}
		if (bucket_step(e->start, e->level, +1) <= start ||
		    e->start >= span_end) {
			continue;
		}
		out[count++] = e;
	}
	return count;
}

/* Bold is a property of the palette entry, not of the call site: turning it on
 * for "text dim" makes everything drawn in that colour bold. Only fg and dim
 * currently reach any text, so the other toggles are inert until something
 * draws text in those colours — which is the point of doing it uniformly
 * rather than special-casing the two. */
static XftFont *
font_for(struct grid *g, int col)
{
	return g->bold[col] && g->font_bold ? g->font_bold : g->font;
}

/* Join as many pieces as fit the cell. Pieces that would overflow are dropped
 * rather than ellipsised — the user asked for text "only as long as it would
 * fit". */
static void
cell_text(struct grid *g, const char *pieces[], int count, int max_w,
          char *out, size_t out_n)
{
	XftFont *font = font_for(g, COL_FG);
	XGlyphInfo ext;
	char candidate[MAX_LABEL * 2];
	int i;

	out[0] = '\0';
	for (i = 0; i < count; i++) {
		snprintf(candidate, sizeof candidate, "%s%s%s",
		         out, out[0] ? " " : "", pieces[i]);
		/* The first piece always goes in, however long it is — draw_text
		 * clips it to the cell. Measuring it here and dropping it made a
		 * cell's text vanish outright the moment the column got narrower than
		 * the note, which is worse than showing the start of it. Later pieces
		 * are still only added while the whole lot fits, so aggregating
		 * several entries never spills past the cell. */
		if (i > 0) {
			XftTextExtentsUtf8(g->dpy, font, (FcChar8 *)candidate,
			                   (int)strlen(candidate), &ext);
			if (ext.xOff > max_w) {
				break;
			}
		}
		snprintf(out, out_n, "%s", candidate);
	}
}

/* ---- drawing ----------------------------------------------------------- */

static void
draw_text(struct grid *g, int x, int y, int max_w, int col, const char *text)
{
	XftFont *font = font_for(g, col);
	XGlyphInfo ext;
	char clipped[MAX_LABEL];
	int len;

	len = snprintf(clipped, sizeof clipped, "%s", text);
	if (len >= (int)sizeof clipped) {
		len = sizeof clipped - 1;
	}
	/* Clip to the width by dropping trailing characters — whole characters,
	 * stepping over UTF-8 continuation bytes, or a truncated multi-byte
	 * sequence would render as a broken glyph.
	 *
	 * One measurement per character dropped. Fine for labels and for the short
	 * notes cells usually hold; if a row of long notes ever makes redraws feel
	 * slow when zoomed out, this loop is where the time goes, and a
	 * proportional first guess would cut it to a couple of passes. */
	XftTextExtentsUtf8(g->dpy, font, (FcChar8 *)clipped, len, &ext);
	while (len > 0 && ext.xOff > max_w) {
		do {
			len--;
		} while (len > 0 && (clipped[len] & 0xC0) == 0x80);
		clipped[len] = '\0';
		XftTextExtentsUtf8(g->dpy, font, (FcChar8 *)clipped, len, &ext);
	}
	if (len > 0) {
		XftDrawStringUtf8(g->draw, &g->col[col], font, x, y + font->ascent,
		                  (FcChar8 *)clipped, len);
	}
}

/* Reopen the font at g->font_size and rebuild every measurement that depends on
 * it. Everything here is derived, so the only thing anyone ever assigns is
 * font_size — set it and mark the grid dirty, and the main loop calls this.
 *
 * A font that fails to open leaves the previous one in place rather than
 * dropping the widget to no text at all. */
static void
layout_apply(struct grid *g)
{
	char name[64];
	XftFont *font;
	int i, gap, line_h;

	if (g->font_size < font_size_min) {
		g->font_size = font_size_min;
	}
	if (g->font_size > font_size_max) {
		g->font_size = font_size_max;
	}
	g->layout_size = g->font_size;   /* set before opening, so a failure to
	                                  * open is not retried every redraw */

	snprintf(name, sizeof name, "%s:size=%d", font_family, g->font_size);
	if (!(font = XftFontOpenName(g->dpy, g->screen, name))) {
		return;
	}
	if (g->font) {
		XftFontClose(g->dpy, g->font);
	}
	g->font = font;

	/* The bold face is optional: if the family has none, font_for falls back
	 * to the regular one and the toggles simply do nothing. */
	snprintf(name, sizeof name, "%s:bold:size=%d", font_family, g->font_size);
	if (g->font_bold) {
		XftFontClose(g->dpy, g->font_bold);
		g->font_bold = NULL;
	}
	g->font_bold = XftFontOpenName(g->dpy, g->screen, name);

	/* Row height follows the taller face, or turning a toggle on would clip
	 * the text it was meant to emphasise. */
	line_h = g->font->height;
	if (g->font_bold && g->font_bold->height > line_h) {
		line_h = g->font_bold->height;
	}
	g->row_h = line_h + 8;
	g->context_h = line_h + 4;
	g->pad_x = g->font_size / 2;
	g->gutter_w = g->font_size * 11;
	g->knob_r = g->font_size * 3 / 5;
	if (g->knob_r < 4) {
		g->knob_r = 4;
	}
	g->track_end_pad = g->knob_r * 2 + 2;

	/* Three slider rows stacked in the top margin, with the same gap above the
	 * first as below the last. */
	gap = g->row_h - 2;
	g->slider_zoom_y = gap * 7 / 10;
	g->slider_pos_y  = g->slider_zoom_y + gap;
	g->slider_size_y = g->slider_pos_y + gap;
	g->margin_top    = g->slider_size_y + g->slider_zoom_y;

	/* The picker's *size* is geometry like everything else here, so it belongs
	 * in this one place. Its y is not: that follows the row count and is set by
	 * picker_layout. Keeping the two apart is what lets the panel's height be
	 * known before its position has been decided — which is what the clamp in
	 * colors_row_y needs. */
	g->picker.area_w = g->row_h * 10;
	g->picker.area_h = g->row_h * 5;
	g->picker.bar_w = g->row_h;
	g->picker.gap = g->pad_x * 2;
	g->picker.x = g->pad_x * 4;

	/* Bigger text needs wider columns before a label tier becomes legible, so
	 * the zoom ladder's thresholds move with the size too. */
	for (i = 0; i < LVL_COUNT; i++) {
		g->min_col_w[i] = min_col_w_base[i] * g->font_size / font_size_default;
	}
}

/* Everything the colours panel needs below its own header row: the swatches, the
 * name/bold row, then the picker with the themes list in a column beside it. */
static int
picker_panel_h(struct grid *g)
{
	return g->row_h * 2 + picker_height(&g->picker) + g->pad_x * 2;
}

/* y of the "hidden rows" control, which sits under "+ add row". The names run
 * below it while the list is open.
 *
 * It floats for the same reason the colours panel does, and by the same
 * arithmetic: once the rows fill the screen there is no upward room left for
 * window_fit to find, so the list stops being pushed down and is drawn over the
 * bottom rows instead. Room is kept below it for whatever still has to sit
 * there — the colours control, and its panel when that is open — so when both
 * float they stack against the bottom edge rather than over each other. */
static int
hidden_row_y(struct grid *g)
{
	/* Date row, the visible rows, then the "+" row. */
	int y = g->margin_top + g->context_h + g->row_h * (g->visible_count + 2);

	if (g->menu_open && g->hidden_count > 0) {
		int below = g->row_h * (2 + g->hidden_count);
		int max_y;

		if (g->picker_open) {
			below += picker_panel_h(g);
		}
		max_y = g->screen_h - below;
		if (y > max_y) {
			y = max_y > g->margin_top ? max_y : g->margin_top;
		}
	}
	return y;
}

/* y of the colours control, which sits under "+ add row" and under the hidden
 * rows list when that is open. The draw pass, the hit test and the window
 * height all derive from this one function so they cannot drift apart — which
 * is also why the clamp below only has to be written once. */
static int
colors_row_y(struct grid *g)
{
	int y = hidden_row_y(g);

	if (g->hidden_count > 0) {
		y += g->row_h;   /* the "hidden rows" control */
		if (g->menu_open) {
			y += g->row_h * g->hidden_count;
		}
	}
	/* With enough rows the panel is pushed off the bottom of the screen and
	 * becomes unreachable — you cannot click what is not there. Past that point
	 * it stops being pushed and floats over the rows instead, which is why it
	 * paints its own background.
	 *
	 * Measured against the whole screen height rather than the space below
	 * win_y, because window_fit slides the window up to make room and will go
	 * all the way to the top edge if that is what it takes. */
	if (g->picker_open) {
		int max_y = g->screen_h - g->row_h - picker_panel_h(g);

		if (y > max_y) {
			y = max_y > g->margin_top ? max_y : g->margin_top;
		}
	}
	return y;
}

/* x of the bold toggle, just past the swatch strip. Fixed rather than measured
 * from the label text beside it, so the draw pass and the hit test agree
 * without either having to measure a string. */
static int
bold_toggle_x(struct grid *g)
{
	return g->pad_x * 4 + COL_COUNT * (g->row_h + 2) + g->pad_x * 2;
}

/* Where one item of the themes list sits: item 0 is "save theme as...", 1 and up
 * are the saved names.
 *
 * The list used to run down the full width below the picker, which meant the
 * panel grew a row per theme and eventually ran off the bottom of the screen
 * while the whole area to the right of the swatches sat empty. It is a column
 * beside the picker instead, wrapping into a second column when it reaches the
 * picker's depth — so the panel's height no longer depends on how many themes
 * there are, and the space that was already there does the work.
 *
 * One function for the geometry, and the hit test scans it rather than inverting
 * it, so there is a single source of truth for where an item is. Reads picker.y,
 * so it is only correct after the draw pass has set it.
 *
 * With MAX_THEMES names this is seven columns; the window is screen-wide, so
 * that fits. If it ever has to fit a narrow screen, wrap on the window width
 * here and the hit test follows automatically. */
static void
themes_item_rect(struct grid *g, int item, int *x, int *y, int *w)
{
	int per_col = picker_height(&g->picker) / g->row_h;

	if (per_col < 1) {
		per_col = 1;
	}
	*w = g->row_h * 8;
	*x = g->picker.x + picker_width(&g->picker) + g->row_h + item / per_col * *w;
	*y = g->picker.y + item % per_col * g->row_h;
}

/* Which themes item the point is in, or -1. A scan over the rects rather than
 * the arithmetic inverted, so themes_item_rect stays the only place the layout
 * is expressed — at a couple of dozen names, anything cleverer is waste. */
static int
themes_item_at(struct grid *g, int px, int py)
{
	int item, x, y, w;

	for (item = 0; item <= g->theme_count; item++) {
		themes_item_rect(g, item, &x, &y, &w);
		if (px >= x && px < x + w && py >= y && py < y + g->row_h) {
			return item;
		}
	}
	return -1;
}

/* Rows come and go, so the window height follows the row count — and the top
 * edge follows the height, because a widget anchored at win_y that grows
 * downwards eventually grows off the bottom of the screen and takes whichever
 * control is last with it. Growing upwards instead keeps everything reachable
 * and costs one XMoveResizeWindow.
 *
 * The window position is a function of its height alone, so the early return on
 * an unchanged height covers both. */
static void
window_fit(struct grid *g)
{
	int h = colors_row_y(g) + g->row_h;

	if (g->picker_open) {
		h += picker_panel_h(g);
	}
	if (h == g->h) {
		return;
	}
	g->h = h;
	g->win_top = win_y;
	if (g->win_top + h > g->screen_h) {
		g->win_top = g->screen_h - h;
	}
	if (g->win_top < 0) {
		g->win_top = 0;   /* taller than the screen: the top edge is the best of it */
	}
	if (!g->win) {
		return;   /* called during setup, to size the window before creating it */
	}
	XMoveResizeWindow(g->dpy, g->win, 0, g->win_top, g->w, g->h);
	XFreePixmap(g->dpy, g->buf);
	g->buf = XCreatePixmap(g->dpy, g->win, g->w, g->h,
	                       DefaultDepth(g->dpy, g->screen));
	XftDrawChange(g->draw, g->buf);
}

static void
redraw(struct grid *g)
{
	XRectangle clip;
	time_t t, next, now, ctx_prev, ctx_now;
	char label[MAX_LABEL];
	double x, nx;
	int level, ctx_level, y, vis, row, bottom, x0, x1, cx;

	level = zoom_level(g);
	bottom = g->margin_top + g->context_h + g->row_h * (g->visible_count + 1);

	XSetForeground(g->dpy, g->gc, g->col[COL_BG].pixel);
	XFillRectangle(g->dpy, g->buf, g->gc, 0, 0, g->w, g->h);
	XSetForeground(g->dpy, g->gc, g->col[COL_GUTTER].pixel);
	XFillRectangle(g->dpy, g->buf, g->gc, 0, g->margin_top, g->gutter_w, bottom - g->margin_top);

	/* The two rows that hold no cells — the context strip and the Date row —
	 * get their own colour, across the gutter as well as the grid, so they
	 * read as a header rather than as rows you failed to click into. Drawn
	 * before the current-bucket fill, which deliberately covers this part of
	 * them. */
	XSetForeground(g->dpy, g->gc, g->col[COL_HEADER].pixel);
	XFillRectangle(g->dpy, g->buf, g->gc, 0, g->margin_top, g->w,
	               g->context_h + g->row_h);

	/* Columns and both header strips live inside the grid area; the gutter
	 * must not scroll under them. */
	clip.x = g->gutter_w;
	clip.y = g->margin_top;
	clip.width = g->w - g->gutter_w;
	clip.height = bottom - g->margin_top;
	XSetClipRectangles(g->dpy, g->gc, 0, 0, &clip, 1, Unsorted);
	XftDrawSetClipRectangles(g->draw, 0, 0, &clip, 1);

	/* The whole current bucket is filled, not just a hairline at now — at a
	 * glance you want "which day is it", not "which instant". Drawn first so
	 * the grid lines and labels land on top. */
	now = time(NULL);
	t = bucket_start(now, level);
	x = pixel_at_time(g, (double)t);
	nx = pixel_at_time(g, (double)bucket_step(t, level, +1));
	if (nx > g->gutter_w && x < g->w) {
		XSetForeground(g->dpy, g->gc, g->col[COL_NOW_BODY].pixel);
		XFillRectangle(g->dpy, g->buf, g->gc, (int)x, g->margin_top,
		               (int)(nx - x), bottom - g->margin_top);
		XSetForeground(g->dpy, g->gc, g->col[COL_NOW_HEAD].pixel);
		XFillRectangle(g->dpy, g->buf, g->gc, (int)x, g->margin_top,
		               (int)(nx - x), g->context_h + g->row_h);
	}

	ctx_level = context_of[level];
	ctx_prev = 0;

	for (t = bucket_start((time_t)g->view_left, level); ; t = next) {
		x = pixel_at_time(g, (double)t);
		if (x > g->w) {
			break;
		}
		next = bucket_step(t, level, +1);
		nx = pixel_at_time(g, (double)next);

		for (vis = 0; vis < g->visible_count; vis++) {
			const char *pieces[MAX_PIECES];
			char cell[MAX_LABEL], count[16];
			int cell_color, cell_filled, extra, piece_count;
			int cy = g->margin_top + g->context_h + g->row_h * (vis + 1);
			int tx = (int)x + g->pad_x, tw = (int)(nx - x) - g->pad_x * 2;
			int ty = cy + (g->row_h - g->font->height) / 2;

			row = g->visible[vis];
			piece_count = cell_gather(g, row, t, next, level, &cell_color,
			                          &cell_filled, &extra, pieces, MAX_PIECES);
			if (!cell_filled) {
				continue;
			}
			XSetForeground(g->dpy, g->gc, g->col[COL_WRITTEN + cell_color].pixel);
			XFillRectangle(g->dpy, g->buf, g->gc, (int)x + 1, cy + 1,
			               (int)(nx - x) - 1, g->row_h - 1);

			/* A count of what is in the cell at some other resolution, in the
			 * left margin. Without it an aggregated cell looks like a cell that
			 * simply has that text in it, and there is nothing to say the zoom
			 * is hiding anything. It takes its width off the text beside it
			 * rather than overlapping — a truncated note is recoverable by
			 * zooming, a hidden count is not. */
			if (extra > 0) {
				XGlyphInfo ext;

				snprintf(count, sizeof count, "%d", extra);
				XftTextExtentsUtf8(g->dpy, font_for(g, COL_DIM), (FcChar8 *)count,
				                   (int)strlen(count), &ext);
				draw_text(g, tx, ty, tw, COL_DIM, count);
				tx += ext.xOff + g->pad_x;
				tw -= ext.xOff + g->pad_x;
			}
			cell_text(g, pieces, piece_count, tw, cell, sizeof cell);
			if (cell[0] != '\0' && tw > 0) {
				draw_text(g, tx, ty, tw, COL_FG, cell);
			}
		}

		XSetForeground(g->dpy, g->gc, g->col[COL_LINE].pixel);
		XDrawLine(g->dpy, g->buf, g->gc, (int)x, g->margin_top, (int)x, bottom);

		bucket_label(t, level, label, sizeof label);
		draw_text(g, (int)x + g->pad_x,
		          g->margin_top + g->context_h + (g->row_h - g->font->height) / 2,
		          (int)(nx - x) - g->pad_x * 2,
		          COL_DIM, label);

		if (ctx_level < 0) {
			continue;
		}
		ctx_now = bucket_start(t, ctx_level);
		if (ctx_now != ctx_prev) {
			context_label(ctx_now, ctx_level, label, sizeof label);
			draw_text(g, (int)x + g->pad_x, g->margin_top + (g->context_h - g->font->height) / 2,
			          g->w - (int)x, COL_DIM, label);
			ctx_prev = ctx_now;
		}
	}

	/* The shift-drag block, as one rectangle. Positioned from instants rather
	 * than columns, so it stays over the cells it was drawn on when the view is
	 * panned or zoomed underneath it. */
	if (g->sel_active && g->visible_count > 0) {
		int v0, v1;
		time_t t0, t1;

		selection_bounds(g, &v0, &v1, &t0, &t1);
		x = pixel_at_time(g, (double)t0);
		nx = pixel_at_time(g, (double)t1);
		y = g->margin_top + g->context_h + g->row_h * (v0 + 1);
		XSetForeground(g->dpy, g->gc, g->col[COL_SELECT].pixel);
		XDrawRectangle(g->dpy, g->buf, g->gc, (int)x, y,
		               (int)(nx - x), g->row_h * (v1 - v0 + 1));
	}

	/* The exact instant, on top of its highlighted bucket. Read at draw time
	 * rather than tracked — there is no timer, so it is accurate as of the
	 * last interaction, which is enough. */
	x = pixel_at_time(g, (double)now);
	if (x >= g->gutter_w && x < g->w) {
		XSetForeground(g->dpy, g->gc, g->col[COL_NOW_EDGE].pixel);
		XFillRectangle(g->dpy, g->buf, g->gc, (int)x, g->margin_top, 1, bottom - g->margin_top);
	}

	XSetClipMask(g->dpy, g->gc, None);
	XftDrawSetClip(g->draw, None);

	XSetForeground(g->dpy, g->gc, g->col[COL_LINE].pixel);
	XDrawLine(g->dpy, g->buf, g->gc, g->gutter_w, g->margin_top, g->gutter_w, bottom);
	for (y = g->margin_top + g->context_h; y <= bottom; y += g->row_h) {
		XDrawLine(g->dpy, g->buf, g->gc, 0, y, g->w, y);
	}

	draw_text(g, g->pad_x, g->margin_top + g->context_h + (g->row_h - g->font->height) / 2,
	          g->gutter_w - g->pad_x * 2, COL_DIM, "Date");
	for (vis = 0; vis < g->visible_count; vis++) {
		row = g->visible[vis];
		y = g->margin_top + g->context_h + g->row_h * (vis + 1);
		/* The row being dragged is filled and drawn bright, so it stays
		 * findable as the rows slide around underneath it. A ctrl-drag names
		 * the same way: nothing else says which row is being pushed. */
		if ((g->drag == DRAG_ROW || g->drag == DRAG_SHIFT) &&
		    row == g->drag_row) {
			XSetForeground(g->dpy, g->gc, g->col[COL_TRACK].pixel);
			XFillRectangle(g->dpy, g->buf, g->gc, 0, y + 1,
			               g->gutter_w - 1, g->row_h - 1);
		}
		draw_text(g, g->pad_x, y + (g->row_h - g->font->height) / 2,
		          g->gutter_w - g->pad_x * 2,
		          (g->drag == DRAG_ROW || g->drag == DRAG_SHIFT) &&
		          row == g->drag_row ? COL_FG : COL_DIM,
		          g->rows[row]);
	}

	/* The cell being typed into, drawn over whatever it currently holds.
	 *
	 * The box is as wide as the cell or as wide as what is being typed,
	 * whichever is more, and it slides left off the right edge rather than
	 * clipping. Editing is the one moment the whole text has to be readable —
	 * a narrow column at a coarse zoom would otherwise hide the end of the
	 * word as you write it. It overlaps the cells to its right while open;
	 * they are redrawn on commit.
	 *
	 * Drawn outside the grid's clip rectangle, and after it, because the
	 * strips below open downwards and may reach past the last row. It is an
	 * overlay, like the colours panel — the same reasoning puts it last. */
	if (g->edit_mode == EDIT_CELL && row_visible_index(g, g->edit_row) >= 0) {
		XGlyphInfo ext;
		int cy = g->margin_top + g->context_h +
		         g->row_h * (row_visible_index(g, g->edit_row) + 1);
		const struct entry *stack[MAX_PIECES];
		int stack_lw[MAX_PIECES];
		int box_x, box_w, i, above = 0, below = 0, stack_n;
		int top_limit = g->margin_top + g->context_h + g->row_h;

		/* Everything else written inside this cell's span, at some other
		 * resolution, opened out around the box: coarser above, finer below.
		 * A cell aggregating finer writing used to show only its own text
		 * while being edited, so the rest of what was in there could only be
		 * found by abandoning the edit and zooming in to hunt for it.
		 *
		 * Each keeps the colour it has on its own timeframe rather than the
		 * edit colour, so the group reads as the cells it is made of and the
		 * one being typed into is the odd one out. Read-only all the same:
		 * this is a look at the neighbourhood, not a second edit. Ordered by
		 * entry_cmp, so each side reads in time order.
		 *
		 * A side that runs out of window simply stops. The count in the cell
		 * is the honest total, and it is still there underneath the box. */
		stack_n = edit_stack_gather(g, g->edit_row, g->edit_level,
		                            g->edit_start, stack, MAX_PIECES);

		x = pixel_at_time(g, (double)g->edit_start);
		nx = pixel_at_time(g, (double)bucket_step(g->edit_start, g->edit_level, +1));
		XftTextExtentsUtf8(g->dpy, font_for(g, COL_FG), (FcChar8 *)g->input,
		                   g->input_len, &ext);
		box_x = (int)x + 1;

		/* The widest of the cell, of what is being typed, and of every strip.
		 * The reason the box grows at all — this is the one moment the whole
		 * text has to be legible, whatever the zoom — applies just as much to
		 * the strips, which are the same words at another resolution.
		 *
		 * One width for the group rather than one per strip: they sit under a
		 * single selection border, and a ragged stack inside a bounding
		 * rectangle reads as a drawing bug rather than as a set of cells.
		 *
		 * The level-name widths are kept from this pass, so the draw loop below
		 * measures nothing. */
		box_w = (int)(nx - x) - 1;
		if (ext.xOff + g->pad_x * 2 + 2 > box_w) {
			box_w = ext.xOff + g->pad_x * 2 + 2;
		}
		for (i = 0; i < stack_n; i++) {
			const char *lvl = level_names[stack[i]->level];
			XGlyphInfo lext, text_ext;
			int need;

			XftTextExtentsUtf8(g->dpy, font_for(g, COL_DIM), (FcChar8 *)lvl,
			                   (int)strlen(lvl), &lext);
			XftTextExtentsUtf8(g->dpy, font_for(g, COL_FG),
			                   (FcChar8 *)stack[i]->text,
			                   (int)strlen(stack[i]->text), &text_ext);
			stack_lw[i] = lext.xOff + g->pad_x;
			need = g->pad_x + stack_lw[i] + text_ext.xOff + g->pad_x + 2;
			if (need > box_w) {
				box_w = need;
			}
		}
		if (box_x + box_w > g->w) {
			box_x = g->w - box_w;
		}
		if (box_x < g->gutter_w) {
			box_x = g->gutter_w;
		}

		for (i = 0; i < stack_n; i++) {
			const struct entry *e = stack[i];
			int sy, lw = stack_lw[i];

			if (e->level > g->edit_level) {
				sy = cy - g->row_h * (above + 1);
				if (sy < top_limit) {
					continue;
				}
				above++;
			} else {
				sy = cy + g->row_h * (below + 1);
				if (sy + g->row_h > g->h) {
					continue;
				}
				below++;
			}
			XSetForeground(g->dpy, g->gc, g->col[COL_WRITTEN + e->color].pixel);
			XFillRectangle(g->dpy, g->buf, g->gc, box_x, sy + 1, box_w, g->row_h - 1);
			/* A rule between the strips, so two of the same colour still read
			 * as two cells. */
			XSetForeground(g->dpy, g->gc, g->col[COL_LINE].pixel);
			XDrawLine(g->dpy, g->buf, g->gc, box_x, sy, box_x + box_w - 1, sy);
			/* The level is named because it is the whole point of the strip:
			 * "there is an hour's worth of writing inside this day". */
			draw_text(g, box_x + g->pad_x, sy + (g->row_h - g->font->height) / 2,
			          box_w - g->pad_x * 2, COL_DIM, level_names[e->level]);
			draw_text(g, box_x + g->pad_x + lw, sy + (g->row_h - g->font->height) / 2,
			          box_w - g->pad_x * 2 - lw, COL_FG, e->text);
		}

		XSetForeground(g->dpy, g->gc, g->col[COL_EDIT].pixel);
		XFillRectangle(g->dpy, g->buf, g->gc, box_x, cy + 1, box_w, g->row_h - 1);
		if (above > 0) {
			XSetForeground(g->dpy, g->gc, g->col[COL_LINE].pixel);
			XDrawLine(g->dpy, g->buf, g->gc, box_x, cy, box_x + box_w - 1, cy);
		}
		draw_text(g, box_x + g->pad_x, cy + (g->row_h - g->font->height) / 2,
		          box_w - g->pad_x * 2, COL_FG, g->input);
		XSetForeground(g->dpy, g->gc, g->col[COL_NOW_EDGE].pixel);
		XFillRectangle(g->dpy, g->buf, g->gc, box_x + g->pad_x + ext.xOff + 1,
		               cy + 4, 1, g->row_h - 8);

		/* One selection border around the whole group, not one per cell: what
		 * is selected is the span, and the strips are the same span at other
		 * resolutions. Drawn last, once the loop above has settled how far the
		 * group reaches, and it degenerates to exactly the old single-cell
		 * outline when there is nothing else in the span. */
		XSetForeground(g->dpy, g->gc, g->col[COL_SELECT].pixel);
		XDrawRectangle(g->dpy, g->buf, g->gc, box_x, cy - g->row_h * above + 1,
		               box_w - 1, g->row_h * (above + below + 1) - 2);
	}

	/* The "+" row sits below the grid proper — no columns run through it, so
	 * it reads as a control rather than as a row with no data. */
	y = bottom + (g->row_h - g->font->height) / 2;
	if (g->edit_mode == EDIT_ROW) {
		char typed[MAX_LABEL + 8];
		XGlyphInfo ext;

		snprintf(typed, sizeof typed, "%s", g->input);
		draw_text(g, g->pad_x, y, g->w - g->pad_x * 2, COL_FG, typed);
		XftTextExtentsUtf8(g->dpy, font_for(g, COL_FG), (FcChar8 *)typed,
		                   (int)strlen(typed), &ext);
		XSetForeground(g->dpy, g->gc, g->col[COL_NOW_EDGE].pixel);
		XFillRectangle(g->dpy, g->buf, g->gc, g->pad_x + ext.xOff + 1,
		               bottom + 4, 1, g->row_h - 8);
	} else {
		draw_text(g, g->pad_x, y, g->gutter_w - g->pad_x * 2, COL_DIM, "+  add row");
	}

	/* The hidden rows control, below "+", and only when there is something to
	 * list. Same visual language as "+  add row" so it reads as a control:
	 * "+" opens, "-" closes, and the names sit indented beneath.
	 *
	 * Like the colours panel it floats over the bottom rows when there is no
	 * room left below them, so an open list paints its own band and top edge —
	 * a no-op in the usual case, where there is nothing under it anyway. */
	if (g->hidden_count > 0) {
		char item[MAX_LABEL + 32];
		int i, listed = 0, top = hidden_row_y(g);

		if (g->menu_open) {
			XSetForeground(g->dpy, g->gc, g->col[COL_BG].pixel);
			XFillRectangle(g->dpy, g->buf, g->gc, 0, top, g->w,
			               g->row_h * (1 + g->hidden_count));
			XSetForeground(g->dpy, g->gc, g->col[COL_LINE].pixel);
			XDrawLine(g->dpy, g->buf, g->gc, 0, top, g->w, top);
		}
		snprintf(item, sizeof item, "%s  hidden rows (%d)",
		         g->menu_open ? "-" : "+", g->hidden_count);
		draw_text(g, g->pad_x, top + (g->row_h - g->font->height) / 2,
		          g->w - g->pad_x * 2, COL_DIM, item);

		for (i = 0; g->menu_open && i < g->row_count; i++) {
			if (!g->row_hidden[i]) {
				continue;
			}
			y = top + g->row_h * (1 + listed) + (g->row_h - g->font->height) / 2;
			draw_text(g, g->pad_x * 4, y, g->w - g->pad_x * 5, COL_FG, g->rows[i]);
			listed++;
		}
	}

	/* The colours control, and the panel it opens.
	 *
	 * Drawn last, and over an opaque band of its own: with enough rows above it
	 * colors_row_y stops being pushed down and the panel floats over the bottom
	 * of the grid instead of below it. The band is a no-op in the usual case,
	 * where there is nothing underneath it but background. */
	y = colors_row_y(g);
	if (g->picker_open) {
		XSetForeground(g->dpy, g->gc, g->col[COL_BG].pixel);
		XFillRectangle(g->dpy, g->buf, g->gc, 0, y, g->w, g->h - y);
		XSetForeground(g->dpy, g->gc, g->col[COL_LINE].pixel);
		XDrawLine(g->dpy, g->buf, g->gc, 0, y, g->w, y);
	}
	draw_text(g, g->pad_x, y + (g->row_h - g->font->height) / 2,
	          g->w - g->pad_x * 2, COL_DIM,
	          g->picker_open ? "-  colours" : "+  colours");

	if (g->picker_open) {
		char hex[32];
		int i, sw = g->row_h, stride = g->row_h + 2;
		int sx = g->pad_x * 4, sy = y + g->row_h + 2, ny, bx;

		/* One swatch per palette entry, the selected one ringed. Picking the
		 * element by its colour rather than by name means you point at the
		 * thing you want to change. */
		for (i = 0; i < COL_COUNT; i++) {
			XSetForeground(g->dpy, g->gc, g->col[i].pixel);
			XFillRectangle(g->dpy, g->buf, g->gc, sx + i * stride, sy,
			               sw, sw - 4);
			XSetForeground(g->dpy, g->gc,
			               i == g->picker_element ? g->col[COL_SELECT].pixel
			                                      : g->col[COL_LINE].pixel);
			XDrawRectangle(g->dpy, g->buf, g->gc, sx + i * stride, sy,
			               sw, sw - 4);
		}

		snprintf(hex, sizeof hex, "%s   #%02x%02x%02x",
		         palette[g->picker_element].label,
		         g->rgb[g->picker_element][0],
		         g->rgb[g->picker_element][1],
		         g->rgb[g->picker_element][2]);
		ny = y + g->row_h * 2;
		draw_text(g, sx, ny + (g->row_h - g->font->height) / 2,
		          bold_toggle_x(g) - sx - g->pad_x, COL_FG, hex);

		/* The bold toggle for the selected element: filled when on. */
		bx = bold_toggle_x(g);
		XSetForeground(g->dpy, g->gc,
		               g->bold[g->picker_element] ? g->col[COL_KNOB].pixel
		                                          : g->col[COL_TRACK].pixel);
		XFillRectangle(g->dpy, g->buf, g->gc, bx, ny + 3,
		               g->row_h * 3, g->row_h - 6);
		draw_text(g, bx + g->pad_x, ny + (g->row_h - g->font->height) / 2,
		          g->row_h * 3 - g->pad_x * 2,
		          g->bold[g->picker_element] ? COL_FG : COL_DIM, "bold");

		/* The picker's y is the one bit of layout that is stored rather than
		 * recomputed on demand, because picker_press needs it too. Set here,
		 * from the same y the panel is being drawn at, so a click always hits
		 * the panel the user is looking at. */
		g->picker.y = y + g->row_h * 3;
		picker_draw(&g->picker, g->buf, g->gc);

		/* Saved themes, in a column beside the picker: "save as" at the top
		 * starts text entry, each name below it loads that theme. */
		for (i = 0; i <= g->theme_count; i++) {
			int tx, ty, tw;

			themes_item_rect(g, i, &tx, &ty, &tw);
			ny = ty + (g->row_h - g->font->height) / 2;
			if (i > 0) {
				draw_text(g, tx + g->pad_x * 3, ny, tw - g->pad_x * 4, COL_FG,
				          g->themes[i - 1].name);
			} else if (g->edit_mode == EDIT_THEME) {
				char typed[MAX_LABEL + 16];
				XGlyphInfo ext;

				/* The typed name is allowed past the column width — the
				 * whole of what is being typed has to stay legible, the
				 * same reasoning as the cell edit box. */
				snprintf(typed, sizeof typed, "save as: %s", g->input);
				draw_text(g, tx, ny, g->w - tx - g->pad_x, COL_FG, typed);
				XftTextExtentsUtf8(g->dpy, font_for(g, COL_FG), (FcChar8 *)typed,
				                   (int)strlen(typed), &ext);
				XSetForeground(g->dpy, g->gc, g->col[COL_NOW_EDGE].pixel);
				XFillRectangle(g->dpy, g->buf, g->gc, tx + ext.xOff + 1,
				               ty + 4, 1, g->row_h - 8);
			} else {
				draw_text(g, tx, ny, tw - g->pad_x, COL_DIM,
				          "+  save theme as...");
			}
		}
	}

	/* Sliders. */
	track_bounds(g, &x0, &x1);

	draw_text(g, g->pad_x, g->slider_zoom_y - g->font->height / 2,
	          g->gutter_w - g->pad_x * 2, COL_DIM, "zoom");
	XSetForeground(g->dpy, g->gc, g->col[COL_TRACK].pixel);
	XDrawLine(g->dpy, g->buf, g->gc, x0, g->slider_zoom_y, x1, g->slider_zoom_y);
	cx = x0 + (int)(zoom_fraction(g) * (x1 - x0));
	XSetForeground(g->dpy, g->gc, g->col[COL_KNOB].pixel);
	XFillArc(g->dpy, g->buf, g->gc, cx - g->knob_r, g->slider_zoom_y - g->knob_r,
	         g->knob_r * 2, g->knob_r * 2, 0, 360 * 64);

	draw_text(g, g->pad_x, g->slider_pos_y - g->font->height / 2,
	          g->gutter_w - g->pad_x * 2, COL_DIM, "scroll");
	XSetForeground(g->dpy, g->gc, g->col[COL_TRACK].pixel);
	XDrawLine(g->dpy, g->buf, g->gc, x0, g->slider_pos_y, x1, g->slider_pos_y);
	/* Centre tick marks the rest position the knob springs back to. */
	XDrawLine(g->dpy, g->buf, g->gc, (x0 + x1) / 2, g->slider_pos_y - g->knob_r - 3,
	          (x0 + x1) / 2, g->slider_pos_y + g->knob_r + 3);
	cx = (x0 + x1) / 2 + (int)(g->shuttle * ((x1 - x0) / 2));
	XSetForeground(g->dpy, g->gc, g->col[COL_KNOB].pixel);
	XFillArc(g->dpy, g->buf, g->gc, cx - g->knob_r, g->slider_pos_y - g->knob_r,
	         g->knob_r * 2, g->knob_r * 2, 0, 360 * 64);

	/* Text size. Last of the three so it does not come between zoom and scroll,
	 * which read as one control surface. Linear, unlike zoom — the range is
	 * small enough that a log scale would only make it harder to land on a
	 * particular size. */
	draw_text(g, g->pad_x, g->slider_size_y - g->font->height / 2,
	          g->gutter_w - g->pad_x * 2, COL_DIM, "size");
	XSetForeground(g->dpy, g->gc, g->col[COL_TRACK].pixel);
	XDrawLine(g->dpy, g->buf, g->gc, x0, g->slider_size_y, x1, g->slider_size_y);
	cx = x0 + (int)((double)(g->font_size - font_size_min) /
	                (font_size_max - font_size_min) * (x1 - x0));
	XSetForeground(g->dpy, g->gc, g->col[COL_KNOB].pixel);
	XFillArc(g->dpy, g->buf, g->gc, cx - g->knob_r, g->slider_size_y - g->knob_r,
	         g->knob_r * 2, g->knob_r * 2, 0, 360 * 64);

	XCopyArea(g->dpy, g->buf, g->win, g->gc, 0, 0, g->w, g->h, 0, 0);
	XFlush(g->dpy);
}

/* ---- setup ------------------------------------------------------------- */

/* (Re)allocate the whole palette from g->rgb. Called at startup and again on
 * every change from the colours panel, which is why it frees first — a drag
 * across the gradient would otherwise leak an allocation per pixel of travel. */
static void
theme_apply(struct grid *g)
{
	Visual *visual = DefaultVisual(g->dpy, g->screen);
	Colormap cmap = DefaultColormap(g->dpy, g->screen);
	int i;

	if (g->col_allocated) {
		for (i = 0; i < COL_COUNT; i++) {
			XftColorFree(g->dpy, visual, cmap, &g->col[i]);
		}
	}
	for (i = 0; i < COL_COUNT; i++) {
		XRenderColor rc;

		rc.red   = g->rgb[i][0] * 257;   /* 8-bit -> 16-bit, 255 -> 65535 */
		rc.green = g->rgb[i][1] * 257;
		rc.blue  = g->rgb[i][2] * 257;
		rc.alpha = 0xffff;
		if (!XftColorAllocValue(g->dpy, visual, cmap, &rc, &g->col[i])) {
			fprintf(stderr, "timegrid: cannot allocate colour %s\n",
			        palette[i].key);
			exit(1);
		}
	}
	g->col_allocated = 1;
}


int
main(int argc, char *argv[])
{
	struct grid g = {0};
	XSetWindowAttributes attrs;
	struct pollfd fds[2];
	char inotify_buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	char watch_dir[512];
	char *slash;
	int inotify_fd, dirty, x0, x1, bottom;

	setlocale(LC_CTYPE, "");   /* so Xutf8LookupString decodes properly */

	if (argc > 1) {
		snprintf(g.file_path, sizeof g.file_path, "%s", argv[1]);
	} else {
		snprintf(g.file_path, sizeof g.file_path, "%s/%s",
		         getenv("HOME") ? getenv("HOME") : ".", plans_path);
	}
	snprintf(watch_dir, sizeof watch_dir, "%s", g.file_path);
	if ((slash = strrchr(watch_dir, '/'))) {
		*slash = '\0';
		snprintf(g.file_name, sizeof g.file_name, "%s", slash + 1);
	} else {
		snprintf(watch_dir, sizeof watch_dir, ".");
		snprintf(g.file_name, sizeof g.file_name, "%s", g.file_path);
	}
	snprintf(g.dir_path, sizeof g.dir_path, "%s", watch_dir);
	mkdir_parents(g.dir_path);   /* before inotify: the watch needs it to exist */

	if (!(g.dpy = XOpenDisplay(NULL))) {
		fprintf(stderr, "timegrid: cannot open display\n");
		return 1;
	}
	g.screen = DefaultScreen(g.dpy);
	g.w = DisplayWidth(g.dpy, g.screen);
	g.screen_h = DisplayHeight(g.dpy, g.screen);

	/* Defaults first, then load: the file carries the text size and any
	 * colours that have been changed away from these. */
	for (int i = 0; i < COL_COUNT; i++) {
		hex_to_rgb(palette[i].hex, g.rgb[i]);
	}
	g.font_size = font_size_default;
	snprintf(g.themes_file, sizeof g.themes_file, "%s/%s",
	         getenv("HOME") ? getenv("HOME") : ".", themes_path);
	themes_load(&g);
	plans_load(&g);
	layout_apply(&g);
	if (!g.font) {
		fprintf(stderr, "timegrid: cannot load font %s\n", font_family);
		return 1;
	}
	g.pps = g.min_col_w[LVL_DAY] / level_secs[LVL_DAY];   /* open at day columns */
	g.view_left = (double)bucket_start(time(NULL), LVL_DAY);
	window_fit(&g);   /* sets g.h; the window does not exist yet */

	theme_apply(&g);
	picker_init(&g.picker, g.dpy, DefaultVisual(g.dpy, g.screen),
	            DefaultDepth(g.dpy, g.screen));
	picker_set_rgb(&g.picker, g.rgb[g.picker_element]);

	/* Override-redirect so dwm ignores the window entirely, then lowered:
	 * getting covered by tiled windows is the correct behaviour. */
	attrs.override_redirect = True;
	attrs.background_pixel = g.col[COL_BG].pixel;
	attrs.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
	                   Button1MotionMask | KeyPressMask |
	                   EnterWindowMask | LeaveWindowMask | StructureNotifyMask;
	g.win = XCreateWindow(g.dpy, RootWindow(g.dpy, g.screen), 0, g.win_top,
	                      g.w, g.h, 0,
	                      DefaultDepth(g.dpy, g.screen), InputOutput,
	                      DefaultVisual(g.dpy, g.screen),
	                      CWOverrideRedirect | CWBackPixel | CWEventMask, &attrs);

	g.gc = XCreateGC(g.dpy, g.win, 0, NULL);
	g.buf = XCreatePixmap(g.dpy, g.win, g.w, g.h, DefaultDepth(g.dpy, g.screen));
	g.draw = XftDrawCreate(g.dpy, g.buf, DefaultVisual(g.dpy, g.screen),
	                       DefaultColormap(g.dpy, g.screen));


	/* An input method, so typing a row name handles compose keys and dead
	 * keys rather than only the ASCII a naive keysym mapping would give.
	 * Both calls are allowed to fail — the key handler falls back. */
	XSetLocaleModifiers("");
	if ((g.xim = XOpenIM(g.dpy, NULL, NULL, NULL))) {
		g.xic = XCreateIC(g.xim, XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
		                  XNClientWindow, g.win, XNFocusWindow, g.win, NULL);
	}

	XMapWindow(g.dpy, g.win);
	XLowerWindow(g.dpy, g.win);

	if ((inotify_fd = inotify_init1(IN_NONBLOCK)) < 0) {
		fprintf(stderr, "timegrid: inotify_init1 failed\n");
		return 1;
	}
	/* Watch the directory, not the inode: editors write a temp file and
	 * rename over it, which silently kills a watch on the file itself. */
	inotify_add_watch(inotify_fd, watch_dir, IN_MOVED_TO | IN_CLOSE_WRITE);

	fds[0].fd = ConnectionNumber(g.dpy);
	fds[0].events = POLLIN;
	fds[1].fd = inotify_fd;
	fds[1].events = POLLIN;

	for (;;) {
		dirty = 0;
		XFlush(g.dpy);

		/* The only timer in the program, and it exists only while the
		 * shuttle knob is held off-centre. Idle stays at zero wakeups. */
		if (poll(fds, 2, g.shuttle != 0.0 ? shuttle_tick_ms : -1) < 0) {
			break;
		}
		if (g.shuttle != 0.0) {
			shuttle_advance(&g);
			dirty = 1;
		}

		while (XPending(g.dpy)) {
			XEvent ev;
			XNextEvent(g.dpy, &ev);
			if (XFilterEvent(&ev, None)) {
				continue;   /* consumed by the input method */
			}

			switch (ev.type) {
			case Expose:
				dirty = 1;
				break;
			case EnterNotify:
				/* dwm hands focus to the root window on an empty tag,
				 * which is exactly when this is being looked at, so
				 * there is no competitor to fight. */
				XSetInputFocus(g.dpy, g.win, RevertToPointerRoot, CurrentTime);
				break;
			case LeaveNotify:
				/* Keep focus while a name is being typed, or moving the
				 * pointer away mid-word would send the rest of it to
				 * whatever is underneath. */
				if (g.edit_mode == EDIT_NONE) {
					XSetInputFocus(g.dpy, RootWindow(g.dpy, g.screen),
					               RevertToPointerRoot, CurrentTime);
				}
				break;
			case ButtonPress:
				track_bounds(&g, &x0, &x1);
				bottom = g.margin_top + g.context_h +
				         g.row_h * (g.visible_count + 1);
				if (ev.xbutton.button == Button4) {
					zoom_at(&g, ZOOM_STEP, ev.xbutton.x);
					dirty = 1;
				} else if (ev.xbutton.button == Button5) {
					zoom_at(&g, 1.0 / ZOOM_STEP, ev.xbutton.x);
					dirty = 1;
				} else if (ev.xbutton.button == Button3) {
					/* Right-click a row name to hide it. Button 1 on the
					 * gutter is already taken by the reorder drag, and there
					 * is no menu bar to hang this off. */
					int vis = (ev.xbutton.y - g.margin_top -
					           g.context_h) / g.row_h - 1;

					if (ev.xbutton.x < g.gutter_w && vis >= 0 &&
					    vis < g.visible_count &&
					    ev.xbutton.y >= g.margin_top + g.context_h + g.row_h) {
						if (g.edit_mode == EDIT_CELL) {
							cell_commit(&g);
						}
						g.sel_active = 0;
						g.edit_mode = EDIT_NONE;
						row_set_hidden(&g, g.visible[vis], 1);
						window_fit(&g);
						dirty = 1;
					}
				} else if (ev.xbutton.button != Button1) {
					break;
				} else if (abs(ev.xbutton.y - g.slider_zoom_y) <= g.knob_r + 4) {
					/* Clicking the track jumps the knob there, so a
					 * click is a coarse seek and the drag refines it. */
					g.drag = DRAG_ZOOM;
					zoom_set_fraction(&g, (double)(ev.xbutton.x - x0) / (x1 - x0));
					dirty = 1;
				} else if (abs(ev.xbutton.y - g.slider_pos_y) <= g.knob_r + 4) {
					g.drag = DRAG_POS;
					shuttle_grab(&g, ev.xbutton.x);
					clock_gettime(CLOCK_MONOTONIC, &g.shuttle_tick);
					dirty = 1;
				} else if (abs(ev.xbutton.y - g.slider_size_y) <= g.knob_r + 4) {
					/* The track is measured from the gutter, and the gutter
					 * grows with the text — so this is the one slider that
					 * moves its own scale as you drag it, which oscillates.
					 * Freeze the bounds at the grab and drag against those. */
					g.drag = DRAG_SIZE;
					g.drag_x0 = x0;
					g.drag_x1 = x1;
					size_set_fraction(&g, (double)(ev.xbutton.x - x0) / (x1 - x0));
					dirty = 1;
				} else if (ev.xbutton.y >= colors_row_y(&g)) {
					/* The colours control and its panel. Checked before the
					 * hidden-rows block because it sits below it and both are
					 * "somewhere under the grid". */
					int cy = colors_row_y(&g);
					int sx = g.pad_x * 4, stride = g.row_h + 2;

					if (ev.xbutton.y < cy + g.row_h) {
						g.picker_open = !g.picker_open;
						window_fit(&g);
					} else if (!g.picker_open) {
						break;
					} else if (ev.xbutton.y < cy + g.row_h * 2) {
						int i = (ev.xbutton.x - sx) / stride;

						if (ev.xbutton.x >= sx && i >= 0 && i < COL_COUNT &&
						    (ev.xbutton.x - sx) % stride < g.row_h) {
							g.picker_element = i;
							picker_set_rgb(&g.picker, g.rgb[i]);
						}
					} else if (ev.xbutton.y < cy + g.row_h * 3) {
						int bx = bold_toggle_x(&g);

						if (ev.xbutton.x >= bx &&
						    ev.xbutton.x < bx + g.row_h * 3) {
							g.bold[g.picker_element] =
								!g.bold[g.picker_element];
							plans_save(&g);
						}
					} else if (themes_item_at(&g, ev.xbutton.x,
					                          ev.xbutton.y) >= 0) {
						int item = themes_item_at(&g, ev.xbutton.x,
						                          ev.xbutton.y);

						if (item == 0) {
							/* "save as": type a name, Return commits. */
							g.edit_mode = EDIT_THEME;
							g.input[0] = '\0';
							g.input_len = 0;
							XSetInputFocus(g.dpy, g.win, RevertToPointerRoot,
							               CurrentTime);
							if (g.xic) {
								XSetICFocus(g.xic);
							}
						} else {
							theme_recall(&g, item - 1);
							theme_apply(&g);
							picker_set_rgb(&g.picker, g.rgb[g.picker_element]);
							plans_save(&g);
						}
					} else if (picker_press(&g.picker, ev.xbutton.x,
					                        ev.xbutton.y)) {
						memcpy(g.rgb[g.picker_element], g.picker.rgb, 3);
						theme_apply(&g);
						g.drag = DRAG_COLOR;
					}
					dirty = 1;
				} else if (g.hidden_count > 0 &&
				           ev.xbutton.y >= hidden_row_y(&g)) {
					/* The hidden rows control: its first row is the header
					 * and toggles the list, the rows under it are the names,
					 * and clicking one puts that row back. Checked before the
					 * rows because an open list floats over them. */
					int item = (ev.xbutton.y - hidden_row_y(&g)) / g.row_h;

					if (item == 0) {
						g.menu_open = !g.menu_open;
					} else if (g.menu_open) {
						int i, listed = 0;

						for (i = 0; i < g.row_count; i++) {
							if (!g.row_hidden[i]) {
								continue;
							}
							if (++listed == item) {
								row_set_hidden(&g, i, 0);
								break;
							}
						}
					}
					g.sel_active = 0;
					window_fit(&g);
					dirty = 1;
				} else if (ev.xbutton.y >= bottom) {
					/* The "+" row. Grab focus explicitly: the pointer is
					 * here now, but the user is about to look at the
					 * keyboard rather than keep the pointer still. */
					g.edit_mode = EDIT_ROW;
					g.input[0] = '\0';
					g.input_len = 0;
					XSetInputFocus(g.dpy, g.win, RevertToPointerRoot, CurrentTime);
					if (g.xic) {
						XSetICFocus(g.xic);
					}
					dirty = 1;
				} else if (ev.xbutton.x < g.gutter_w &&
				           ev.xbutton.y >= g.margin_top + g.context_h + g.row_h &&
				           ev.xbutton.y < bottom) {
					/* Grab a row name to reorder. The gutter used to pan the
					 * grid along with everything else below the margin; the
					 * name column is a better use of it. */
					int vis = (ev.xbutton.y - g.margin_top -
					           g.context_h) / g.row_h - 1;

					if (vis >= 0 && vis < g.visible_count) {
						if (g.edit_mode == EDIT_CELL) {
							cell_commit(&g);
						}
						g.edit_mode = EDIT_NONE;
						g.sel_active = 0;
						g.drag = DRAG_ROW;
						g.drag_row = g.visible[vis];
						dirty = 1;
					}
				} else if (ev.xbutton.x >= g.gutter_w &&
				           ev.xbutton.y >= g.margin_top + g.context_h + g.row_h &&
				           ev.xbutton.y < bottom) {
					int vis = (ev.xbutton.y - g.margin_top -
					           g.context_h) / g.row_h - 1;
					int lvl = zoom_level(&g);
					time_t cell = bucket_start(
						(time_t)time_at_pixel(&g, ev.xbutton.x), lvl);
					int row, idx, in_sel = 0;

					if (vis < 0 || vis >= g.visible_count) {
						break;
					}
					row = g.visible[vis];
					idx = entry_find(&g, row, lvl, cell);
					/* Asked before anything below clears the selection. */
					if (g.sel_active) {
						int v0, v1;
						time_t t0, t1;

						selection_bounds(&g, &v0, &v1, &t0, &t1);
						in_sel = vis >= v0 && vis <= v1 &&
						         cell >= t0 && cell < t1;
					}
					/* Clicking away commits what is being typed, the same as
					 * tabbing away does — otherwise the click silently throws
					 * the half-written cell away. */
					if (g.edit_mode == EDIT_CELL) {
						cell_commit(&g);
						g.edit_mode = EDIT_NONE;
						idx = entry_find(&g, row, lvl, cell);
					}

					if (ev.xbutton.state & ControlMask) {
						/* Ctrl-drag pushes this cell and everything to
						 * the right of it along the timeline. */
						g.sel_active = 0;
						g.drag = DRAG_SHIFT;
						g.drag_row = row;
						g.shift_level = lvl;
						g.shift_from = cell;
					} else if (ev.xbutton.state & ShiftMask) {
						g.sel_active = 1;
						g.sel_level = lvl;
						g.sel_anchor_row = g.sel_cursor_row = vis;
						g.sel_anchor_t = g.sel_cursor_t = cell;
						g.drag = DRAG_SELECT;
					} else if ((ev.xbutton.state & Mod1Mask) && in_sel) {
						selection_set_color(&g);
					} else if (ev.xbutton.state & Mod1Mask) {
						/* Alt-click cycles plain -> red -> blue -> plain,
						 * keeping whatever text is already there. */
						char kept[MAX_LABEL];
						int next_color = idx >= 0 ?
							(g.entries[idx].color + 1) % 3 : CELL_RED;

						g.sel_active = 0;
						snprintf(kept, sizeof kept, "%s",
						         idx >= 0 ? g.entries[idx].text : "");
						entry_set(&g, row, lvl, cell, kept, next_color);
						plans_save(&g);
					} else {
						g.sel_active = 0;
						cell_edit_begin(&g, row, lvl, cell);
						XSetInputFocus(g.dpy, g.win, RevertToPointerRoot,
						               CurrentTime);
						if (g.xic) {
							XSetICFocus(g.xic);
						}
					}
					dirty = 1;
				} else if (ev.xbutton.y >= g.margin_top) {
					g.drag = DRAG_GRID;
					g.drag_x = ev.xbutton.x;
				}
				break;
			case ButtonRelease:
				if (ev.xbutton.button == Button1) {
					if (g.drag == DRAG_POS) {
						g.shuttle = 0.0;   /* springs back to centre */
						dirty = 1;
					}
					/* Write the text size back once, on release — saving on
					 * every motion event would rewrite the file per pixel. */
					if (g.drag == DRAG_SIZE) {
						plans_save(&g);
					}
					/* Row order is line order in the file, so saving is all
					 * the persistence a reorder needs. */
					if (g.drag == DRAG_ROW) {
						plans_save(&g);
						dirty = 1;
					}
					/* Same reasoning as the size slider: one write on
					 * release, not one per pixel of gradient dragged. */
					if (g.drag == DRAG_COLOR) {
						picker_release(&g.picker);
						plans_save(&g);
						dirty = 1;
					}
					g.drag = DRAG_NONE;
				}
				break;
			case MotionNotify:
				track_bounds(&g, &x0, &x1);
				if (g.drag == DRAG_GRID) {
					pan_by(&g, ev.xmotion.x - g.drag_x);
					g.drag_x = ev.xmotion.x;
				} else if (g.drag == DRAG_ZOOM) {
					zoom_set_fraction(&g, (double)(ev.xmotion.x - x0) / (x1 - x0));
				} else if (g.drag == DRAG_POS) {
					shuttle_grab(&g, ev.xmotion.x);
				} else if (g.drag == DRAG_SIZE) {
					size_set_fraction(&g, (double)(ev.xmotion.x - g.drag_x0) /
					                      (g.drag_x1 - g.drag_x0));
				} else if (g.drag == DRAG_SELECT) {
					int vis = (ev.xmotion.y - g.margin_top -
					           g.context_h) / g.row_h - 1;

					if (vis < 0) {
						vis = 0;
					}
					if (vis >= g.visible_count) {
						vis = g.visible_count - 1;
					}
					g.sel_cursor_row = vis;
					g.sel_cursor_t = bucket_start(
						(time_t)time_at_pixel(&g, ev.xmotion.x), g.sel_level);
				} else if (g.drag == DRAG_COLOR) {
					if (!picker_motion(&g.picker, ev.xmotion.x, ev.xmotion.y)) {
						break;
					}
					memcpy(g.rgb[g.picker_element], g.picker.rgb, 3);
					theme_apply(&g);
				} else if (g.drag == DRAG_ROW) {
					/* Reorder live, so the row visibly moves under the
					 * pointer rather than jumping on release. */
					int vis = (ev.xmotion.y - g.margin_top -
					           g.context_h) / g.row_h - 1;

					if (vis < 0) {
						vis = 0;
					}
					if (vis >= g.visible_count) {
						vis = g.visible_count - 1;
					}
					if (g.visible_count > 0 && g.visible[vis] != g.drag_row) {
						int target = g.visible[vis];

						rows_move(&g, g.drag_row, target);
						g.drag_row = target;
					}
				} else if (g.drag == DRAG_SHIFT) {
					/* Counted in buckets rather than pixels: columns are
					 * real durations, so February is narrower than March
					 * and a pixel delta would drift over a long drag.
					 * Every column is at least min_col_w wide, so the
					 * window width in pixels bounds the count. */
					time_t target = bucket_start(
						(time_t)time_at_pixel(&g, ev.xmotion.x),
						g.shift_level);
					time_t t = g.shift_from;
					int steps = 0;

					while (t < target && steps < g.w) {
						t = bucket_step(t, g.shift_level, +1);
						steps++;
					}
					while (t > target && -steps < g.w) {
						t = bucket_step(t, g.shift_level, -1);
						steps--;
					}
					if (steps == 0) {
						break;
					}
					row_shift_from(&g, g.drag_row, g.shift_level,
					               g.shift_from, steps);
					g.shift_from = t;
				} else {
					break;
				}
				dirty = 1;
				break;
			case KeyPress: {
				KeySym key = XkbKeycodeToKeysym(g.dpy, ev.xkey.keycode, 0, 0);

				if (g.edit_mode != EDIT_NONE) {
					char typed[32];
					KeySym sym = NoSymbol;
					Status status;
					int n;

					if (key == XK_Escape) {
						g.edit_mode = EDIT_NONE;
						dirty = 1;
						break;
					}
					if (key == XK_Return || key == XK_KP_Enter) {
						if (g.edit_mode == EDIT_ROW) {
							plans_load(&g);
							plans_add_row(&g, g.input);
							plans_load(&g);
						} else if (g.edit_mode == EDIT_THEME) {
							theme_store(&g, g.input);
						} else {
							cell_commit(&g);
						}
						window_fit(&g);
						g.edit_mode = EDIT_NONE;
						dirty = 1;
						break;
					}
					/* Tab and the arrows move the edit from cell to cell.
					 * Free to use for that because the text entry has no
					 * caret to move within. */
					if (g.edit_mode == EDIT_CELL &&
					    (key == XK_Tab || key == XK_Left || key == XK_Right ||
					     key == XK_Up || key == XK_Down)) {
						if (key == XK_Tab) {
							cell_move(&g, ev.xkey.state & ShiftMask ? -1 : +1, 0);
						} else if (key == XK_Right) {
							cell_move(&g, +1, 0);
						} else if (key == XK_Left) {
							cell_move(&g, -1, 0);
						} else if (key == XK_Up) {
							cell_move(&g, 0, -1);
						} else {
							cell_move(&g, 0, +1);
						}
						window_fit(&g);
						dirty = 1;
						break;
					}
					if (key == XK_BackSpace &&
					    (ev.xkey.state & (Mod1Mask | ControlMask))) {
						input_delete_word(&g);
						dirty = 1;
						break;
					}
					if (key == XK_BackSpace) {
						/* Step back over UTF-8 continuation bytes so one
						 * press deletes one character, not one byte. */
						while (g.input_len > 0 &&
						       (g.input[g.input_len - 1] & 0xC0) == 0x80) {
							g.input_len--;
						}
						if (g.input_len > 0) {
							g.input_len--;
						}
						g.input[g.input_len] = '\0';
						dirty = 1;
						break;
					}
					if (g.xic) {
						n = Xutf8LookupString(g.xic, &ev.xkey, typed,
						                      sizeof typed - 1, &sym, &status);
					} else {
						n = XLookupString(&ev.xkey, typed, sizeof typed - 1,
						                  &sym, NULL);
					}
					if (n > 0 && (unsigned char)typed[0] >= ' ' &&
					    g.input_len + n < MAX_LABEL) {
						memcpy(g.input + g.input_len, typed, n);
						g.input_len += n;
						g.input[g.input_len] = '\0';
						dirty = 1;
					}
					break;
				}

				if (g.sel_active &&
				    (key == XK_Delete || key == XK_BackSpace)) {
					selection_clear_text(&g);
					window_fit(&g);
				} else if (key == XK_Escape &&
				           (g.sel_active || g.menu_open || g.picker_open)) {
					g.sel_active = 0;
					g.menu_open = 0;
					g.picker_open = 0;
					window_fit(&g);
				} else if (key == XK_minus) {
					zoom_at(&g, 1.0 / ZOOM_STEP, (g.gutter_w + g.w) / 2);
				} else if (key == XK_plus || key == XK_equal) {
					zoom_at(&g, ZOOM_STEP, (g.gutter_w + g.w) / 2);
				} else if (key == XK_h) {
					pan_by(&g, (g.w - g.gutter_w) / 8);
				} else if (key == XK_l) {
					pan_by(&g, -(g.w - g.gutter_w) / 8);
				} else if (key == XK_0) {
					g.view_left = (double)bucket_start(time(NULL), zoom_level(&g));
				} else {
					break;
				}
				dirty = 1;
				break;
			}
			}
		}

		if (fds[1].revents & POLLIN) {
			ssize_t len = read(inotify_fd, inotify_buf, sizeof inotify_buf);
			for (char *p = inotify_buf; len > 0 && p < inotify_buf + len; ) {
				struct inotify_event *ie = (struct inotify_event *)p;
				if (ie->len && strcmp(ie->name, g.file_name) == 0) {
					dirty = 1;
				}
				p += sizeof(struct inotify_event) + ie->len;
			}
			if (dirty) {
				plans_load(&g);
				window_fit(&g);
			}
		}

		/* One place that acts on a changed text size, whether it came from the
		 * slider or from the file. Everything else only ever assigns
		 * g.font_size and marks the grid dirty. */
		if (dirty && g.font_size != g.layout_size) {
			layout_apply(&g);
			window_fit(&g);
		}
		if (dirty) {
			redraw(&g);
		}
	}
	return 0;
}
