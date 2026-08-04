/* HSV colour picker — see picker.h. */

#include "picker.h"

#include <X11/Xutil.h>   /* XPutPixel and XDestroyImage are macros, and live here */

#include <math.h>
#include <stdlib.h>

/* ---- colour space ------------------------------------------------------- */

static void
hsv_to_rgb(double h, double s, double v, unsigned char rgb[3])
{
	double c = v * s;
	double hh = h / 60.0;
	double x = c * (1 - fabs(fmod(hh, 2) - 1));
	double m = v - c;
	double r = 0, g = 0, b = 0;
	int sector = (int)hh % 6;

	if (sector < 0) {
		sector += 6;
	}
	switch (sector) {
	case 0:  r = c; g = x; break;
	case 1:  r = x; g = c; break;
	case 2:  g = c; b = x; break;
	case 3:  g = x; b = c; break;
	case 4:  r = x; b = c; break;
	default: r = c; b = x; break;
	}
	rgb[0] = (unsigned char)((r + m) * 255 + 0.5);
	rgb[1] = (unsigned char)((g + m) * 255 + 0.5);
	rgb[2] = (unsigned char)((b + m) * 255 + 0.5);
}

/* Hue is left at 0 for a grey; picker_set_rgb is what decides to keep the old
 * one in that case. */
static void
rgb_to_hsv(const unsigned char rgb[3], double *h, double *s, double *v)
{
	double r = rgb[0] / 255.0, g = rgb[1] / 255.0, b = rgb[2] / 255.0;
	double max = r > g ? (r > b ? r : b) : (g > b ? g : b);
	double min = r < g ? (r < b ? r : b) : (g < b ? g : b);
	double d = max - min;

	*v = max;
	*s = max > 0.0 ? d / max : 0.0;
	*h = 0.0;
	if (d <= 0.0) {
		return;
	}
	if (max == r) {
		*h = 60 * fmod((g - b) / d, 6);
	} else if (max == g) {
		*h = 60 * ((b - r) / d + 2);
	} else {
		*h = 60 * ((r - g) / d + 4);
	}
	if (*h < 0) {
		*h += 360;
	}
}

/* Fit an 8-bit channel into whatever width this visual gives it. Truecolour at
 * depth 24 makes this the identity shift; 16-bit visuals want the top bits. */
static unsigned long
channel_pack(unsigned long mask, int value)
{
	int shift = 0, width = 0;
	unsigned long m = mask;

	if (!m) {
		return 0;
	}
	while (!(m & 1)) {
		m >>= 1;
		shift++;
	}
	while (m & 1) {
		m >>= 1;
		width++;
	}
	return (((unsigned long)value >> (8 - width)) << shift) & mask;
}

/* ---- the gradient ------------------------------------------------------- */

/* Rebuild the saturation/value square for the current hue. Cheap to call: it
 * returns immediately unless the hue or the size actually moved. */
static void
area_build(struct picker *p)
{
	XImage *img;
	GC gc;
	char *data;
	int bpp, px, py;

	if (p->area_pix && p->pix_w == p->area_w && p->pix_h == p->area_h &&
	    p->pix_hue == p->hue) {
		return;
	}
	if (p->area_w <= 0 || p->area_h <= 0) {
		return;
	}
	if (p->area_pix && (p->pix_w != p->area_w || p->pix_h != p->area_h)) {
		XFreePixmap(p->dpy, p->area_pix);
		p->area_pix = 0;
	}
	if (!p->area_pix) {
		p->area_pix = XCreatePixmap(p->dpy, DefaultRootWindow(p->dpy),
		                            p->area_w, p->area_h, p->depth);
	}

	bpp = p->depth > 16 ? 4 : (p->depth > 8 ? 2 : 1);
	if (!(data = calloc((size_t)p->area_w * p->area_h, bpp))) {
		return;
	}
	img = XCreateImage(p->dpy, p->visual, p->depth, ZPixmap, 0, data,
	                   p->area_w, p->area_h, bpp * 8, 0);
	if (!img) {
		free(data);
		return;
	}
	for (py = 0; py < p->area_h; py++) {
		double val = 1.0 - (double)py / (p->area_h - 1);

		for (px = 0; px < p->area_w; px++) {
			double sat = (double)px / (p->area_w - 1);
			unsigned char rgb[3];

			hsv_to_rgb(p->hue, sat, val, rgb);
			XPutPixel(img, px, py,
			          channel_pack(p->visual->red_mask, rgb[0]) |
			          channel_pack(p->visual->green_mask, rgb[1]) |
			          channel_pack(p->visual->blue_mask, rgb[2]));
		}
	}
	gc = XCreateGC(p->dpy, p->area_pix, 0, NULL);
	XPutImage(p->dpy, p->area_pix, gc, img, 0, 0, 0, 0, p->area_w, p->area_h);
	XFreeGC(p->dpy, gc);
	XDestroyImage(img);   /* frees data too */

	p->pix_w = p->area_w;
	p->pix_h = p->area_h;
	p->pix_hue = p->hue;
}

/* ---- interface ---------------------------------------------------------- */

void
picker_init(struct picker *p, Display *dpy, Visual *visual, int depth)
{
	p->dpy = dpy;
	p->visual = visual;
	p->depth = depth;
	p->area_pix = 0;
	p->pix_w = p->pix_h = 0;
	p->pix_hue = -1.0;
	p->drag = PICK_NONE;
	p->hue = p->sat = p->val = 0.0;
	p->rgb[0] = p->rgb[1] = p->rgb[2] = 0;
}

void
picker_free(struct picker *p)
{
	if (p->area_pix) {
		XFreePixmap(p->dpy, p->area_pix);
		p->area_pix = 0;
	}
}

void
picker_set_rgb(struct picker *p, const unsigned char rgb[3])
{
	double h, s, v;

	p->rgb[0] = rgb[0];
	p->rgb[1] = rgb[1];
	p->rgb[2] = rgb[2];
	rgb_to_hsv(rgb, &h, &s, &v);
	p->sat = s;
	p->val = v;
	if (s > 0.0) {
		p->hue = h;   /* a grey keeps whatever the strip was showing */
	}
}

int
picker_width(const struct picker *p)
{
	return p->area_w + p->gap + p->bar_w;
}

int
picker_height(const struct picker *p)
{
	return p->area_h;
}

void
picker_draw(struct picker *p, Drawable d, GC gc)
{
	int bar_x = p->x + p->area_w + p->gap;
	int i, mx, my;

	area_build(p);
	if (p->area_pix) {
		XCopyArea(p->dpy, p->area_pix, d, gc, 0, 0,
		          p->area_w, p->area_h, p->x, p->y);
	}

	/* The hue strip, a row of one-pixel bands. No cache: it never changes, and
	 * a few hundred rectangles is nothing next to the gradient. */
	for (i = 0; i < p->area_h; i++) {
		unsigned char rgb[3];

		hsv_to_rgb(360.0 * i / p->area_h, 1.0, 1.0, rgb);
		XSetForeground(p->dpy, gc,
		               channel_pack(p->visual->red_mask, rgb[0]) |
		               channel_pack(p->visual->green_mask, rgb[1]) |
		               channel_pack(p->visual->blue_mask, rgb[2]));
		XFillRectangle(p->dpy, d, gc, bar_x, p->y + i, p->bar_w, 1);
	}

	/* Markers are drawn black over white so they stay visible at both ends of
	 * the gradient, where a single colour would disappear. */
	mx = p->x + (int)(p->sat * (p->area_w - 1));
	my = p->y + (int)((1.0 - p->val) * (p->area_h - 1));
	XSetForeground(p->dpy, gc, WhitePixel(p->dpy, DefaultScreen(p->dpy)));
	XDrawArc(p->dpy, d, gc, mx - 5, my - 5, 10, 10, 0, 360 * 64);
	XSetForeground(p->dpy, gc, BlackPixel(p->dpy, DefaultScreen(p->dpy)));
	XDrawArc(p->dpy, d, gc, mx - 4, my - 4, 8, 8, 0, 360 * 64);

	my = p->y + (int)(p->hue / 360.0 * p->area_h);
	XSetForeground(p->dpy, gc, WhitePixel(p->dpy, DefaultScreen(p->dpy)));
	XDrawRectangle(p->dpy, d, gc, bar_x - 1, my - 2, p->bar_w + 1, 4);
	XSetForeground(p->dpy, gc, BlackPixel(p->dpy, DefaultScreen(p->dpy)));
	XDrawRectangle(p->dpy, d, gc, bar_x, my - 1, p->bar_w - 1, 2);
}

/* Clamp rather than reject: a drag that runs off the edge should pin to the
 * edge, not stop responding. */
static void
area_set(struct picker *p, int px, int py)
{
	double sat = (double)(px - p->x) / (p->area_w - 1);
	double val = 1.0 - (double)(py - p->y) / (p->area_h - 1);

	p->sat = sat < 0 ? 0 : (sat > 1 ? 1 : sat);
	p->val = val < 0 ? 0 : (val > 1 ? 1 : val);
	hsv_to_rgb(p->hue, p->sat, p->val, p->rgb);
}

static void
hue_set(struct picker *p, int py)
{
	double h = 360.0 * (py - p->y) / p->area_h;

	p->hue = h < 0 ? 0 : (h >= 360 ? 359.999 : h);
	hsv_to_rgb(p->hue, p->sat, p->val, p->rgb);
}

int
picker_press(struct picker *p, int px, int py)
{
	int bar_x = p->x + p->area_w + p->gap;

	if (py < p->y || py >= p->y + p->area_h) {
		return 0;
	}
	if (px >= p->x && px < p->x + p->area_w) {
		p->drag = PICK_AREA;
		area_set(p, px, py);
		return 1;
	}
	if (px >= bar_x && px < bar_x + p->bar_w) {
		p->drag = PICK_HUE;
		hue_set(p, py);
		return 1;
	}
	return 0;
}

int
picker_motion(struct picker *p, int px, int py)
{
	if (p->drag == PICK_AREA) {
		area_set(p, px, py);
		return 1;
	}
	if (p->drag == PICK_HUE) {
		hue_set(p, py);
		return 1;
	}
	return 0;
}

void
picker_release(struct picker *p)
{
	p->drag = PICK_NONE;
}
