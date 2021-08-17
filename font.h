/* See LICENSE file for copyright and license details. */

#ifndef BSPWMBAR_FONT_H
#define BSPWMBAR_FONT_H

#include <stdbool.h>
#include <cairo/cairo.h>
#include <cairo/cairo-ft.h>
#include <cairo/cairo-xcb.h>
#include <harfbuzz/hb.h>
#include <harfbuzz/hb-ft.h>

struct bb_font {
	FT_Face face;
	cairo_font_face_t *cairo;
	hb_font_t *hb;
};

struct bb_font_manager {
	FT_Library ftlib;
	struct bb_font font;
	double font_size;

	cairo_font_options_t *font_opt;
	FcPattern *pattern;

	struct bb_font *fcaches;
	int nfcache;
	int fcachecap;

	double dpi;
	FT_UInt32 load_flag;
	uint16_t celwidth;
};

struct bb_font_manager *bb_font_manager_new(double);
void bb_font_manager_destroy(struct bb_font_manager *);
void bb_font_manager_font_destroy(struct bb_font_manager *, struct bb_font *);
bool bb_font_manager_load_fonts(struct bb_font_manager *, const char *);
struct bb_font *bb_font_manager_find_font(struct bb_font_manager *, FcChar32);

#endif /* BSPWMBAR_FONT_H */
