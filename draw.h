/* See LICENSE file for copyright and license details. */

#ifndef BSPWMBAR_DRAW_H
#define BSPWMBAR_DRAW_H

#include <sys/shm.h>
#include <xcb/xcb.h>
#include <xcb/shm.h>
#include <xcb/xcb_image.h>

#include <cairo/cairo.h>

#include "util.h"
#include "window.h"
#include "font.h"

struct bb_color {
	char *name;
	uint16_t red;
	uint16_t green;
	uint16_t blue;
	uint32_t pixel;
};

struct bb_pixmap {
	xcb_pixmap_t pixmap;
	xcb_shm_segment_info_t shm_info;
};

struct bb_graph_item {
	struct bb_color *fg;
	struct bb_color *bg;
	double val;
};

struct bb_graph_spec {
	char *prefix;
	char *suffix;
	int width;
	int height;
	int padding;
};

struct bb_draw_context {
	char *monitor_name;
	xcb_connection_t *xcb;

	xcb_visualtype_t *visual;
	xcb_gcontext_t gc;
	cairo_t *cr;
	struct bb_font_manager *fm;

	struct bb_window *win;
	struct bb_pixmap *buf;
	struct bb_pixmap *tmp;
	xcb_shm_segment_info_t shm_info;

	int16_t x;
	uint16_t width;
	struct bb_color *fgcolor;
	struct bb_color *bgcolor;
};

struct bb_draw_context *bb_draw_context_new(xcb_connection_t *, xcb_screen_t *, struct bb_window *, struct bb_font_manager *);
void bb_draw_context_destroy(struct bb_draw_context *);
void bb_draw_padding(struct bb_draw_context *, int);
void bb_draw_padding_em(struct bb_draw_context *, int);
void bb_draw_color_text(struct bb_draw_context *, struct bb_color *, const char *);
void bb_draw_text(struct bb_draw_context *, const char *);
void bb_draw_bargraph(struct bb_draw_context *, struct bb_graph_spec *spec, size_t, struct bb_graph_item *);
void bb_draw_clear(struct bb_draw_context *, struct bb_color *);

#endif /* BSPWMBAR_DRAW_H */
