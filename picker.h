/* An HSV colour picker: a saturation/value area you drag in, and a hue strip
 * beside it.
 *
 * This knows nothing about timegrid. It is handed a display, a rectangle and a
 * colour; it draws itself into any Drawable and reports the colour back as you
 * drag. Nothing here would need changing to drop it into another program.
 *
 * The caller owns the layout: set x, y and the three size fields before
 * drawing, and ask picker_width/picker_height how much room that needs.
 */

#ifndef PICKER_H
#define PICKER_H

#include <X11/Xlib.h>

enum { PICK_NONE, PICK_AREA, PICK_HUE };

struct picker {
	/* Layout, set by the caller. */
	int x, y;
	int area_w, area_h;   /* the saturation/value square */
	int bar_w;            /* the hue strip beside it */
	int gap;              /* between the two */

	unsigned char rgb[3];

	/* HSV is what the picker actually manipulates, and it is kept alongside
	 * rgb rather than derived on demand. A grey has no meaningful hue, so
	 * round-tripping through rgb every drag step would make the hue strip jump
	 * to red whenever saturation touched zero. */
	double hue, sat, val;

	Display *dpy;
	Visual *visual;
	int depth;

	/* The gradient lives in a server-side Pixmap, rebuilt only when the hue or
	 * the size changes. Pushing a full XImage every redraw would put the
	 * gradient on the wire for every mouse move. */
	Pixmap area_pix;
	int pix_w, pix_h;
	double pix_hue;

	int drag;             /* PICK_NONE / PICK_AREA / PICK_HUE */
};

void picker_init(struct picker *p, Display *dpy, Visual *visual, int depth);
void picker_free(struct picker *p);

/* Load a colour in. Updates hue/sat/val to match, keeping the existing hue if
 * the colour is a grey. */
void picker_set_rgb(struct picker *p, const unsigned char rgb[3]);

int picker_width(const struct picker *p);
int picker_height(const struct picker *p);

void picker_draw(struct picker *p, Drawable d, GC gc);

/* Returns 1 if the point was inside the picker and the press was taken. */
int picker_press(struct picker *p, int px, int py);

/* Returns 1 if the drag moved and the colour changed. */
int picker_motion(struct picker *p, int px, int py);

void picker_release(struct picker *p);

#endif
