/* See LICENSE file for copyright and license details. */

#include <xcb/xcb_renderutil.h>
#include <cairo/cairo-ft.h>
#include <cairo/cairo-xcb.h>

#include "draw.h"

/* convert color for cairo */
#define CONVCOL(x) (double)((x) / 255.0)

struct bb_glyph_font_spec {
	struct bb_font *font;
	cairo_glyph_t glyph;
};

static struct bb_glyph_font_spec glyph_caches[1024];

static xcb_visualtype_t *xcb_visualtype_get(xcb_screen_t *);
static void xcb_gc_color(xcb_connection_t *, xcb_gcontext_t, struct bb_color *);
static bool xcb_shm_support(xcb_connection_t *);
static size_t bb_draw_load_glyphs_from_hb_buffer(struct bb_draw_context *, hb_buffer_t *, struct bb_font *, int *, int, size_t, struct bb_glyph_font_spec *);
static int bb_draw_load_glyphs(struct bb_draw_context *, const char *, size_t, struct bb_glyph_font_spec *, int *);

static void bb_draw_calc_render_position(struct bb_draw_context *, size_t, struct bb_glyph_font_spec *);
static void bb_draw_glyphs(struct bb_draw_context *, struct bb_color *, size_t, const struct bb_glyph_font_spec *);

static struct bb_pixmap *bb_pixmap_new(xcb_connection_t *, xcb_screen_t *, uint32_t, uint32_t);
static void bb_pixmap_free(xcb_connection_t *, struct bb_pixmap *);
static bool xcb_create_pixmap_with_shm(xcb_connection_t *, xcb_screen_t *, xcb_pixmap_t, uint32_t, uint32_t, xcb_shm_segment_info_t *);

xcb_visualtype_t *
xcb_visualtype_get(xcb_screen_t *scr)
{
	xcb_visualtype_t *visual_type;
	xcb_depth_iterator_t depth_iter;
	depth_iter = xcb_screen_allowed_depths_iterator(scr);
	for (; depth_iter.rem; xcb_depth_next(&depth_iter)) {
		xcb_visualtype_iterator_t visual_iter;

		visual_iter = xcb_depth_visuals_iterator(depth_iter.data);
		for (; visual_iter.rem; xcb_visualtype_next(&visual_iter)) {
			if (scr->root_visual == visual_iter.data->visual_id) {
				visual_type = visual_iter.data;
				return visual_type;
			}
		}
	}
	return NULL;
}

struct bb_draw_context *
bb_draw_context_new(xcb_connection_t *xcb, xcb_screen_t *scr, struct bb_window *win, struct bb_font_manager *fm)
{
	xcb_create_gc_value_list_t gcv = { 0 };
	xcb_render_query_pict_formats_reply_t *pict_reply;
	xcb_render_pictforminfo_t *formats;
	struct bb_draw_context *dc;
	cairo_surface_t *surface;

	dc = calloc(1, sizeof(struct bb_draw_context));
	dc->xcb = xcb;
	dc->fm = fm;
	dc->win = win;
	dc->buf = bb_pixmap_new(xcb, scr, win->width, win->height);
	dc->tmp = bb_pixmap_new(xcb, scr, win->width, win->height);

	/* create graphic context */
	dc->visual = xcb_visualtype_get(scr);

	/* create cairo context */
	pict_reply = xcb_render_query_pict_formats_reply(xcb, xcb_render_query_pict_formats(xcb), NULL);
	formats = xcb_render_util_find_standard_format(pict_reply, XCB_PICT_STANDARD_RGB_24);
	surface = cairo_xcb_surface_create_with_xrender_format(xcb, scr, dc->tmp->pixmap, formats, win->width, win->height);
	dc->cr = cairo_create(surface);
	cairo_set_operator(dc->cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_surface(dc->cr, surface, 0, 0);
	cairo_surface_destroy(surface);
	free(pict_reply);

	/* create gc */
	gcv.graphics_exposures = 1;
	dc->gc = xcb_generate_id(xcb);
	xcb_create_gc_aux(xcb, dc->gc, dc->tmp->pixmap, XCB_GC_GRAPHICS_EXPOSURES, &gcv);

	return dc;
}

struct bb_pixmap *
bb_pixmap_new(xcb_connection_t *xcb, xcb_screen_t *scr, uint32_t width, uint32_t height)
{
	xcb_shm_segment_info_t shm_info = { 0 };
	xcb_pixmap_t xcb_pixmap = xcb_generate_id(xcb);
	xcb_generic_error_t *err = NULL;

	if (xcb_shm_support(xcb)) {
		if (!xcb_create_pixmap_with_shm(xcb, scr, xcb_pixmap, width, height, &shm_info))
			return NULL;
	} else {
		if ((err = xcb_request_check(xcb, xcb_create_pixmap_checked(xcb, scr->root_depth, xcb_pixmap, scr->root, width, height)))) {
			free(err);
			return NULL;
		}
	}

	struct bb_pixmap *pixmap = calloc(1, sizeof(struct bb_pixmap));
	pixmap->pixmap = xcb_pixmap;
	pixmap->shm_info = shm_info;

	return pixmap;
}

void
bb_draw_context_destroy(struct bb_draw_context *dc)
{
	if (dc->buf)
		bb_pixmap_free(dc->xcb, dc->buf);
	if (dc->tmp)
		bb_pixmap_free(dc->xcb, dc->tmp);
	if (dc->gc)
		xcb_free_gc(dc->xcb, dc->gc);
	if (dc->cr)
		cairo_destroy(dc->cr);
	if (dc->monitor_name)
		free(dc->monitor_name);
	free(dc);
}

void
bb_draw_clear(struct bb_draw_context *dc, struct bb_color *color)
{
	xcb_gc_color(dc->xcb, dc->gc, color);
}

size_t
bb_draw_load_glyphs_from_hb_buffer(struct bb_draw_context *dc, hb_buffer_t *buffer, struct bb_font *font, int *x, int y, size_t len, struct bb_glyph_font_spec *glyphs)
{
	cairo_text_extents_t extents;
	hb_glyph_info_t *infos;
	hb_glyph_position_t *pos;
	uint32_t i = 0, ninfo = 0, npos = 0;

	cairo_set_font_face(dc->cr, font->cairo);
	hb_buffer_guess_segment_properties(buffer);
	hb_shape(font->hb, buffer, NULL, 0);
	infos = hb_buffer_get_glyph_infos(buffer, &ninfo);
	pos = hb_buffer_get_glyph_positions(buffer, &npos);

	for (i = 0; i < ninfo && i < len; i++) {
		glyphs[i].font = font;
		glyphs[i].glyph.index = infos[i].codepoint;
		glyphs[i].glyph.x = *x;
		glyphs[i].glyph.y = y;

		if (pos[i].x_advance) {
			*x += pos[i].x_advance / 64;
		} else {
			cairo_glyph_extents(dc->cr, &glyphs[i].glyph, 1, &extents);
			*x += extents.x_advance;
		}
	}
	return i;
}

int
bb_draw_load_glyphs(struct bb_draw_context *dc, const char *str, size_t nglyph, struct bb_glyph_font_spec *glyphs, int *width)
{
	FcChar32 rune = 0;
	size_t i;
	int len = 0;
	size_t offset = 0, num = 0;
	struct bb_font *font = NULL;
	struct bb_font *prev = NULL;
	hb_buffer_t *buffer = NULL;

	buffer = hb_buffer_create();

	uint16_t y = dc->win->height - dc->fm->font_size / 2;
	*width = 0;
	for (i = 0; offset < strlen(str) && i < nglyph; i++, offset += len) {
		len = FcUtf8ToUcs4((FcChar8 *)&str[offset], &rune, strlen(str) - offset);
		if ((font = bb_font_manager_find_font(dc->fm, rune)) && prev && prev != font) {
			num += bb_draw_load_glyphs_from_hb_buffer(dc, buffer, prev, width, y, nglyph - num, &glyphs[num]);
			hb_buffer_clear_contents(buffer);
		}
		prev = font;
		hb_buffer_add_codepoints(buffer, &rune, 1, 0, 1);
	}
	if (prev && hb_buffer_get_length(buffer))
		num += bb_draw_load_glyphs_from_hb_buffer(dc, buffer, font, width, y, nglyph - num, &glyphs[num]);

	hb_buffer_destroy(buffer);
	return num;
}

void
bb_pixmap_free(xcb_connection_t *xcb, struct bb_pixmap *pixmap)
{
	if (!pixmap)
		return;
	if (pixmap->shm_info.shmseg) {
		/* if using shm */
		xcb_shm_detach(xcb, pixmap->shm_info.shmseg);
		shmdt(pixmap->shm_info.shmaddr);
	}
	xcb_free_pixmap(xcb, pixmap->pixmap);
	free(pixmap);
}

bool
xcb_shm_support(xcb_connection_t *xcb)
{
	xcb_shm_query_version_reply_t *version_reply;
	bool supported = (version_reply = xcb_shm_query_version_reply(xcb, xcb_shm_query_version(xcb), NULL)) && version_reply->shared_pixmaps;
	free(version_reply);
	return supported;
}

bool
xcb_create_pixmap_with_shm(xcb_connection_t *xcb, xcb_screen_t *scr, xcb_pixmap_t pixmap, uint32_t width, uint32_t height, xcb_shm_segment_info_t *info)
{
	info->shmid = shmget(IPC_PRIVATE, width * height * 4, IPC_CREAT | 0777);
	info->shmaddr = shmat(info->shmid, 0, 0);
	info->shmseg = xcb_generate_id(xcb);
	if (xcb_request_check(xcb, xcb_shm_attach(xcb, info->shmseg, info->shmid, 0))) {
		shmctl(info->shmid, IPC_RMID, 0);
		shmdt(info->shmaddr);
		return false;
	}
	shmctl(info->shmid, IPC_RMID, 0);
	if (xcb_request_check(xcb, xcb_shm_create_pixmap(xcb, pixmap, scr->root, width, height, scr->root_depth, info->shmseg, 0))) {
		shmdt(info->shmaddr);
		return false;
	}
	return true;
}

void
xcb_gc_color(xcb_connection_t *xcb, xcb_gcontext_t gc, struct bb_color *color)
{
	xcb_change_gc_value_list_t values = { .foreground = color->pixel };
	xcb_change_gc_aux(xcb, gc, XCB_GC_FOREGROUND, &values);
}

void
bb_draw_glyphs(struct bb_draw_context *dc, struct bb_color *color, size_t len, const struct bb_glyph_font_spec *specs)
{
	cairo_font_face_t *prev = NULL;
	size_t i;

	cairo_set_font_options(dc->cr, dc->fm->font_opt);
	cairo_set_font_size(dc->cr, dc->fm->font_size);
	cairo_set_source_rgb(dc->cr, CONVCOL(color->red), CONVCOL(color->green), CONVCOL(color->blue));
	for (i = 0; i < len; i++) {
		if (prev != specs[i].font->cairo) {
			prev = specs[i].font->cairo;
			cairo_set_font_face(dc->cr, prev);
		}
		cairo_show_glyphs(dc->cr, (cairo_glyph_t *)&specs[i].glyph, 1);
	}
}

void
bb_draw_padding(struct bb_draw_context *dc, int num)
{
	dc->x += num;
}

void
bb_draw_padding_em(struct bb_draw_context *dc, int num)
{
	dc->x += dc->fm->celwidth * num;
}

void
bb_draw_calc_render_position(struct bb_draw_context *dc, size_t nglyph, struct bb_glyph_font_spec *glyphs)
{
	for (size_t i = 0; i < nglyph; i++)
		glyphs[i].glyph.x += dc->x;
}

void
bb_draw_color_text(struct bb_draw_context *dc, struct bb_color *color, const char *str)
{
	int width;
	size_t nglyph = bb_draw_load_glyphs(dc, str, sizeof(glyph_caches), glyph_caches, &width);
	bb_draw_calc_render_position(dc, nglyph, glyph_caches);
	bb_draw_glyphs(dc, color, nglyph, glyph_caches);
	dc->x += width;
}

void
bb_draw_text(struct bb_draw_context *dc, const char *str)
{
	bb_draw_color_text(dc, dc->fgcolor, str);
}

void
bb_draw_bargraph(struct bb_draw_context *dc, struct bb_graph_spec *spec, size_t nitem, struct bb_graph_item *items)
{
	xcb_rectangle_t rect = { 0 };

	int16_t basey = (dc->win->height - spec->height) / 2;

	int width = (spec->width + spec->padding) * nitem;
	bb_draw_color_text(dc, dc->fgcolor, spec->prefix);
	int x = dc->x + spec->width;
	for (size_t i = 0; i < nitem; i++) {
		xcb_gc_color(dc->xcb, dc->gc, items[i].bg);
		rect.x = x - spec->width;
		rect.y = basey;
		rect.width = spec->width;
		rect.height = spec->height;
		xcb_poly_fill_rectangle(dc->xcb, dc->tmp->pixmap, dc->gc, 1, &rect);

		if (items[i].val < 0)
			goto CONTINUE;

		xcb_gc_color(dc->xcb, dc->gc, items[i].fg);
		rect.width = spec->width;
		rect.height = SMALLER(BIGGER(spec->height * items[i].val, 1), spec->height);;
		rect.x = x - spec->width;
		rect.y = basey + (spec->height - rect.height);
		xcb_poly_fill_rectangle(dc->xcb, dc->tmp->pixmap, dc->gc, 1, &rect);
	CONTINUE:
		x += spec->width + spec->padding;
	}
	dc->x += width;
	bb_draw_color_text(dc, dc->fgcolor, spec->suffix);
}
