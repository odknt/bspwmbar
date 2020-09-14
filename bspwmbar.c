/* See LICENSE file for copyright and license details. */

#if defined(__linux)
# define _XOPEN_SOURCE 700
# include <alloca.h>
# include <errno.h>
# include <sys/epoll.h>
# include <sys/timerfd.h>
# include <sys/un.h>
#elif defined(__OpenBSD__)
# include <sys/types.h>
# include <sys/event.h>
# include <sys/time.h>
#endif

/* common libraries */
#include <locale.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <time.h>
#include <unistd.h>

/* XCB */
#include <xcb/xcb.h>
#include <xcb/shm.h>
#include <xcb/xcb_util.h>
#include <xcb/xcb_ewmh.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xcb_image.h>
#include <xcb/xcb_renderutil.h>
#include <xcb/randr.h>

#include <ft2build.h>
#include <fontconfig/fontconfig.h>
#include <fontconfig/fcfreetype.h>
#include <cairo/cairo.h>
#include <cairo/cairo-ft.h>
#include <cairo/cairo-xcb.h>
#include <harfbuzz/hb.h>
#include <harfbuzz/hb-ft.h>

/* local headers */
#include "bspwmbar.h"
#include "bspwm.h"
#include "systray.h"
#include "config.h"

#if !defined(VERSION)
# define VERSION "v0.0.0-dev"
#endif

/* epoll max events */
#define MAX_EVENTS 10
/* convert color for cairo */
#define CONVCOL(x) (double)((x) / 255.0)
/* check event and returns true if target is the label */
#define IS_LABEL_EVENT(l,e) (((l).x < (e)->event_x) && ((e)->event_x < (l).x + (l).width))

struct _color_t {
	char *name;
	uint16_t red;
	uint16_t green;
	uint16_t blue;
	uint32_t pixel;
};

typedef struct {
	xcb_pixmap_t pixmap;
	xcb_shm_segment_info_t shm_info;
} pixmap_t;

typedef struct {
	FT_Face face;
	cairo_font_face_t *cairo;
	hb_font_t *hb;
} font_t;

typedef struct {
	font_t *font;
	cairo_glyph_t glyph;
} glyph_font_spec_t;

static glyph_font_spec_t glyph_caches[1024];

typedef struct {
	module_option_t *option;
	color_t fg, bg;

	int x, width;
} label_t;

typedef int desktop_state_t;

typedef struct {
	char name[NAME_MAXSZ];
	enum {
		STATE_FREE     = 0,
		STATE_FOCUSED  = 1 << 1,
		STATE_OCCUPIED = 1 << 2,
		STATE_URGENT   = 1 << 3,

		STATE_ACTIVE = 1 << 8
	} state;
} desktop_t;

typedef struct {
	char name[NAME_MAXSZ];
	desktop_t *desktops;
	int ndesktop; /* num of desktops */
	int cdesktop; /* cap of desktops */
	uint8_t is_active;
} monitor_t;

typedef struct {
	xcb_window_t win;

	int x, y, width, height;
} window_t;

struct _draw_context_t {
	window_t xbar;
	char monitor_name[NAME_MAXSZ];

	xcb_visualtype_t *visual;
	xcb_gcontext_t gc;
	pixmap_t *buf;
	pixmap_t *tmp;
	xcb_shm_segment_info_t shm_info;
	cairo_t *cr;

	int x, width;

	label_t left_labels[LENGTH(left_modules)];
	label_t right_labels[LENGTH(right_modules)];
};

typedef struct {
	/* xcb resources */
	xcb_connection_t *xcb;
	xcb_screen_t *scr;
	xcb_visualid_t visual;
	xcb_colormap_t cmap;

	/* font */
	font_t font;
	double font_size;
	cairo_font_options_t *font_opt;
	FcPattern *pattern;
	FcFontSet *set;

	/* draw context */
	draw_context_t *dcs;
	int ndc;

	/* base color */
	color_t *fg, *bg;
} bspwmbar_t;

/* temporary buffer */
char buf[1024];

static bspwmbar_t bar;
static systray_t *tray;
static poll_fd_t xfd;
#if defined(__linux)
static poll_fd_t timer;
#endif

static FT_Int32 load_flag = FT_LOAD_COLOR | FT_LOAD_NO_BITMAP | FT_LOAD_NO_AUTOHINT;
static FT_Library ftlib;

static color_t **cols;
static int ncol, colcap;
static font_t *fcaches;
static int nfcache = 0;
static int fcachecap = 0;
static int celwidth = 0;
static int graph_maxh = 0;
static int graph_basey = 0;

/* EWMH */
static xcb_ewmh_connection_t ewmh;
static xcb_atom_t xembed_info;

/* Window title cache */
static char *wintitle = NULL;

/* polling fd */
static int pfd = 0;
#if defined(__linux)
static struct epoll_event events[MAX_EVENTS];
#elif defined(__OpenBSD__)
static struct kevent events[MAX_EVENTS];
#endif
static list_head pollfds;

/* private functions */
static void color_load_hex(const char *, color_t *);
static bool color_load_name(const char *, color_t *);
static void signal_handler(int);
static xcb_window_t get_active_window(uint8_t scrno);
static xcb_visualtype_t *xcb_visualtype_get(xcb_screen_t *);
static bool xcb_shm_support(xcb_connection_t *);
static void xcb_gc_color(xcb_connection_t *, xcb_gcontext_t, color_t *);
static char *get_window_title(xcb_connection_t *, xcb_window_t);
static FT_UInt get_font(FcChar32 rune, font_t **);
static bool load_fonts(const char *);
static void font_destroy(font_t font);
static size_t load_glyphs_from_hb_buffer(draw_context_t *, hb_buffer_t *, font_t *, int *, int, glyph_font_spec_t *, size_t);
static int load_glyphs(draw_context_t *, const char *, glyph_font_spec_t *, int, int *);
static bool xcb_create_pixmap_with_shm(xcb_connection_t *, xcb_screen_t *, xcb_pixmap_t, uint32_t, uint32_t, xcb_shm_segment_info_t *);
static pixmap_t *pixmap_new(xcb_connection_t *, xcb_screen_t *, uint32_t, uint32_t);
static void pixmap_free(pixmap_t *);
static bool dc_init(draw_context_t *, xcb_connection_t *, xcb_screen_t *, int, int, int, int);
static void dc_free(draw_context_t);
static int dc_get_x(draw_context_t *);
static void dc_move_x(draw_context_t *, int);
static void dc_calc_render_pos(draw_context_t *, glyph_font_spec_t *, int);
static void draw_padding(draw_context_t *, int);
static void draw_glyphs(draw_context_t *, color_t *, const glyph_font_spec_t *, int nglyph);
static void render_labels(draw_context_t *, label_t *, size_t);
static void windowtitle_update(xcb_connection_t *, uint8_t);
static void calculate_systray_item_positions(label_t *, module_option_t *);
static void calculate_label_positions(draw_context_t *, label_t *, size_t, int);
static void render();
static int get_baseline();
static bool bspwmbar_init(xcb_connection_t *, xcb_screen_t *);
static void bspwmbar_destroy();
static void poll_init();
static void poll_loop(void (*)());
static void poll_stop();
static poll_result_t xev_handle();
#if defined(__linux)
static poll_result_t timer_reset(int);
#endif
static bool is_change_active_window_event(xcb_property_notify_event_t *);
static void cleanup(xcb_connection_t *);
static void run();

xcb_connection_t *
xcb_connection()
{
	return bar.xcb;
}

/**
 * color_load_hex() - load a color from hex string
 * @colstr: hex string (#RRGGBB)
 * @color: (out) loaded color.
 */
void
color_load_hex(const char *colstr, color_t *color)
{
	char red[] = { colstr[1], colstr[2], '\0' };
	char green[] = { colstr[3], colstr[4], '\0' };
	char blue[] = { colstr[5], colstr[6], '\0' };

	color->name = strdup(colstr);
	color->red = strtol(red, NULL, 16);
	color->green = strtol(green, NULL, 16);
	color->blue = strtol(blue, NULL, 16);
	color->pixel = 0xff000000 | color->red << 16 | color->green << 8 | color->blue;
}

/**
 * color_load_name() - load a named color.
 * @colstr: color name.
 * @color: (out) loaded color.
 *
 * Return: bool
 */
bool
color_load_name(const char *colstr, color_t *color)
{
	xcb_alloc_named_color_cookie_t col_cookie;
	xcb_alloc_named_color_reply_t *col_reply;

	col_cookie = xcb_alloc_named_color(bar.xcb, bar.cmap, strlen(colstr), colstr);
	if (!(col_reply = xcb_alloc_named_color_reply(bar.xcb, col_cookie, NULL)))
		return false;

	color->name = strdup(colstr);
	color->red = col_reply->visual_red;
	color->green = col_reply->visual_green;
	color->blue = col_reply->visual_blue;
	color->pixel = col_reply->pixel;

	free(col_reply);
	return true;
}

/**
 * color_load() - get color_t from color name.
 * @colstr: color name.
 *
 * Return: color_t *
 */
color_t *
color_load(const char *colstr)
{
	int i;
	/* find color caches */
	for (i = 0; i < ncol; i++)
		if (!strncmp(cols[i]->name, colstr, strlen(cols[i]->name)))
			return cols[i];

	if (ncol >= colcap) {
		colcap += 5;
		cols = realloc(cols, sizeof(color_t *) * colcap);
	}
	cols[ncol] = calloc(1, sizeof(color_t));

	if (colstr[0] == '#' && strlen(colstr) > 6)
		color_load_hex(colstr, cols[ncol]);
	else
		color_load_name(colstr, cols[ncol]);

	if (!cols[ncol])
		die("color_load(): failed to load color: %s", colstr);

	return cols[ncol++];
}

/**
 * color_default_fg() - returns default fg color.
 *
 * Return: Color
 */
color_t *
color_default_fg()
{
	return bar.fg;
}

/**
 * color_default_bg() - returns default bg color.
 *
 * Return: Color
 */
color_t *
color_default_bg()
{
	return bar.bg;
}

/**
 * xcb_visualtype_get() - get visualtype
 * @scr: xcb_screen_t *
 *
 * Return: xcb_visual_type *
 */
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

/**
 * xcb_shm_support() - check shm extension is usable on the X server.
 * @xcb: xcb connection.
 *
 * Return: bool
 */
bool
xcb_shm_support(xcb_connection_t *xcb)
{
	xcb_shm_query_version_reply_t *version_reply;
	bool supported = (version_reply = xcb_shm_query_version_reply(xcb, xcb_shm_query_version(xcb), NULL)) && version_reply->shared_pixmaps;
	free(version_reply);
	return supported;
}

/**
 * xcb_create_pixmap_with_shm() - create a new pixmap by using shm extension.
 * @xcb: xcb connection.
 * @scr: screen.
 * @pixmap: pixmap id.
 * @width: width of pixmap.
 * @height: height of pixmap.
 * @info: (out) shm segment information of the pixmap.
 *
 * Return: bool
 */
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

/**
 * pixmap_new() - create a new pixmap.
 * @xcb: xcb connection.
 * @scr: screen.
 * @dc: draw context.
 * @pixmap: pixmap id.
 * @width: width of new pixmap.
 * @height: height of new pixmap.
 */
pixmap_t *
pixmap_new(xcb_connection_t *xcb, xcb_screen_t *scr, uint32_t width, uint32_t height)
{
	xcb_shm_segment_info_t shm_info = { 0 };
	xcb_pixmap_t xcb_pixmap = xcb_generate_id(xcb);
	bool res = false;

	if (xcb_shm_support(xcb))
		res = xcb_create_pixmap_with_shm(xcb, scr, xcb_pixmap, width, height, &shm_info);
	else
		res = xcb_request_check(xcb, xcb_create_pixmap(xcb, scr->root_depth, xcb_pixmap, scr->root, width, height));

	if (!res)
		return NULL;

	pixmap_t *pixmap = calloc(1, sizeof(pixmap_t));
	pixmap->pixmap = xcb_pixmap;
	pixmap->shm_info = shm_info;

	return pixmap;
}

/**
 * pixmap_free() - free pixmap_t
 * @pixmap: pixmap pointer.
 */
void
pixmap_free(pixmap_t *pixmap)
{
	if (pixmap->shm_info.shmseg) {
		/* if using shm */
		xcb_shm_detach(bar.xcb, pixmap->shm_info.shmseg);
		shmdt(pixmap->shm_info.shmaddr);
	}
	xcb_free_pixmap(bar.xcb, pixmap->pixmap);
	free(pixmap);
}

/**
 * dc_init() - initialize DC.
 * @dc: draw context.
 * @xcb: xcb connection.
 * @scr: screen number.
 * @x: window position x.
 * @y: window position y.
 * @width: window width.
 * @height: window height.
 *
 * Return: bool
 */
bool
dc_init(draw_context_t *dc, xcb_connection_t *xcb, xcb_screen_t *scr, int x,
        int y, int width, int height)
{
	xcb_configure_window_value_list_t winconf = { 0 };
	xcb_create_gc_value_list_t gcv = { 0 };
	xcb_render_query_pict_formats_reply_t *pict_reply;
	xcb_render_pictforminfo_t *formats;
	cairo_surface_t *surface;
	window_t *xw = &dc->xbar;
	int i = 0;

	const uint32_t attrs[] = { bar.bg->pixel, XCB_EVENT_MASK_NO_EVENT };

	xw->win = xcb_generate_id(xcb);
	xw->x = x;
	xw->y = y;
	xw->width = width;
	xw->height = height;
	xcb_create_window(xcb, XCB_COPY_FROM_PARENT, xw->win, scr->root, x, y,
	                  width, height, 0, XCB_COPY_FROM_PARENT,
	                  XCB_COPY_FROM_PARENT,
	                  XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, attrs);

	/* set window state */
	xcb_atom_t states[] = { ewmh._NET_WM_STATE_STICKY, ewmh._NET_WM_STATE_ABOVE };
	xcb_ewmh_set_wm_state(&ewmh, xw->win, LENGTH(states), states);

	/* set window type */
	xcb_atom_t window_types[] = { ewmh._NET_WM_WINDOW_TYPE_DOCK };
	xcb_ewmh_set_wm_window_type(&ewmh, xw->win, LENGTH(window_types), window_types);

	/* set window strut */
	xcb_ewmh_wm_strut_partial_t strut_partial = {
		.left = 0,
		.right = 0,
		.top = y + height,
		.bottom = 0,
		.left_start_y = 0,
		.left_end_y = 0,
		.right_start_y = 0,
		.right_end_y = 0,
		.top_start_x = x,
		.top_end_x = x + width - 1,
		.bottom_start_x = 0,
		.bottom_end_x = 0,
	};
	xcb_ewmh_set_wm_strut(&ewmh, xw->win, 0, 0, y + height, 0);
	xcb_ewmh_set_wm_strut_partial(&ewmh, xw->win, strut_partial);

	/* create graphic context */
	dc->visual = xcb_visualtype_get(scr);

	/* create pixmap image for rendering */
	dc->buf = pixmap_new(xcb, scr, width, height);
	dc->tmp = pixmap_new(xcb, scr, width, height);

	/* create cairo context */
	pict_reply = xcb_render_query_pict_formats_reply(xcb, xcb_render_query_pict_formats(xcb), NULL);
	formats = xcb_render_util_find_standard_format(pict_reply, XCB_PICT_STANDARD_RGB_24);
	surface = cairo_xcb_surface_create_with_xrender_format(xcb, scr, dc->tmp->pixmap, formats, width, height);
	dc->cr = cairo_create(surface);
	cairo_set_operator(dc->cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_surface(dc->cr, surface, 0, 0);
	cairo_surface_destroy(surface);
	free(pict_reply);

	/* create gc */
	gcv.graphics_exposures = 1;
	dc->gc = xcb_generate_id(xcb);
	xcb_create_gc_aux(xcb, dc->gc, dc->tmp->pixmap, XCB_GC_GRAPHICS_EXPOSURES, &gcv);

	/* set class hint */
	xcb_change_property(xcb, XCB_PROP_MODE_REPLACE, xw->win, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, 8, "bspwmbar");
	xcb_change_property(xcb, XCB_PROP_MODE_REPLACE, xw->win, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, 17, "bspwmbar\0bspwmbar");

	/* create labels from modules */
	for (i = 0; i < (int)LENGTH(left_modules); i++)
		dc->left_labels[i].option = &left_modules[i];
	for (i = 0; i < (int)LENGTH(right_modules); i++)
		dc->right_labels[i].option = &right_modules[i];

	/* send window rendering request */
	winconf.stack_mode = XCB_STACK_MODE_BELOW;
	xcb_configure_window_aux(xcb, xw->win, XCB_CONFIG_WINDOW_STACK_MODE, &winconf);
	xcb_map_window(xcb, xw->win);

	return true;
}

/**
 * dc_free() - free resources of DC.
 * dc: draw context.
 */
void
dc_free(draw_context_t dc)
{
	xcb_free_gc(bar.xcb, dc.gc);
	pixmap_free(dc.buf);
	pixmap_free(dc.tmp);
	xcb_destroy_window(bar.xcb, dc.xbar.win);
	cairo_destroy(dc.cr);
}

const char *
draw_context_monitor_name(draw_context_t *dc)
{
	return dc->monitor_name;
}

/**
 * dc_get_x() - get next rendering position of DC.
 * @dc: draw context.
 *
 * Return: int
 */
int
dc_get_x(draw_context_t *dc)
{
	return dc->x;
}

/**
 * dc_move_x() - move rendering position by x.
 * @dc: draw context.
 * @x: distance of movement.
 */
void
dc_move_x(draw_context_t *dc, int x)
{
	dc->x += x;
}

/**
 * get_active_window() - get active window.
 * @dpy: display pointer.
 * @scr: screen number.
 *
 * Return: xcb_window_t
 */
xcb_window_t
get_active_window(uint8_t scrno)
{
	xcb_window_t win;
	if (xcb_ewmh_get_active_window_reply(&ewmh, xcb_ewmh_get_active_window(&ewmh, scrno), &win, NULL))
		return win;
	return 0;
}

/**
 * get_window_title() - get title of specified win.
 * @dpy: display pointer.
 * @win: window.
 *
 * Return: unsigned char *
 *         The return value needs free after used.
 */
char *
get_window_title(xcb_connection_t *xcb, xcb_window_t win)
{
	char *title = NULL;
	xcb_get_property_reply_t *reply = NULL;
	xcb_ewmh_get_utf8_strings_reply_t utf8_reply = { 0 };

	if (xcb_ewmh_get_wm_name_reply(&ewmh, xcb_ewmh_get_wm_name(&ewmh, win), &utf8_reply, NULL)) {
		title = strndup(utf8_reply.strings, utf8_reply.strings_len);
		xcb_ewmh_get_utf8_strings_reply_wipe(&utf8_reply);
	} else if ((reply = xcb_get_property_reply(xcb, xcb_get_property(xcb, 0, win, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 0, NAME_MAXSZ), NULL))) {
		title = strndup(xcb_get_property_value(reply), xcb_get_property_value_length(reply));
		free(reply);
	}
	return title;
}

/**
 * windowtitle_update() - update windowtitle() returns value.
 * @dpy: display pointer.
 * @scr: screen number.
 */
void
windowtitle_update(xcb_connection_t *xcb, uint8_t scrno)
{
	xcb_window_t win;
	if ((win = get_active_window(scrno))) {
		if (wintitle)
			free(wintitle);
		wintitle = get_window_title(xcb, win);
	} else {
		/* release wintitle when active window not found */
		free(wintitle);
		wintitle = NULL;
	}
}

/**
 * windowtitle() - active window title render function.
 * @dc: draw context.
 * @opts: module options.
 */
void
windowtitle(draw_context_t *dc, module_option_t *opts)
{
	if (!wintitle)
		return;

	FcChar32 dst;
	size_t i = 0, titlelen = strlen(wintitle);
	strncpy(buf, wintitle, sizeof(buf));
	for (size_t len = 0; i < titlelen && len < opts->title.maxlen; len++)
		i += FcUtf8ToUcs4((FcChar8 *)&wintitle[i], &dst, strlen(wintitle) - i);
	if (i < strlen(buf))
		strncpy(&buf[i], opts->title.ellipsis, sizeof(buf) - i);

	draw_text(dc, buf);
}

/**
 * get_font() - finds a font that renderable specified rune.
 * @rune: FcChar32
 *
 * Return: FT_Face
 */
FT_UInt
get_font(FcChar32 rune, font_t **font)
{
	FcResult result;
	FcFontSet *fonts;
	FcPattern *pat;
	FcCharSet *charset;
	FcChar8 *path;
	FT_Face face;
	int i, idx;

	/* Lookup character index with default font. */
	if ((idx = FT_Get_Char_Index(bar.font.face, rune))) {
		*font = &bar.font;
		return idx;
	}

	/* fallback on font cache */
	for (i = 0; i < nfcache; i++) {
		if ((idx = FT_Get_Char_Index(fcaches[i].face, rune))) {
			*font = &fcaches[i];
			return idx;
		}
	}

	/* find font when not found */
	if (i >= nfcache) {
		if (nfcache >= fcachecap) {
			fcachecap += 8;
			fcaches = realloc(fcaches, fcachecap * sizeof(font_t));
		}

		pat = FcPatternDuplicate(bar.pattern);
		charset = FcCharSetCreate();

		/* find font that contains rune and scalable */
		FcCharSetAddChar(charset, rune);
		FcPatternAddCharSet(pat, FC_CHARSET, charset);
		FcPatternAddBool(pat, FC_SCALABLE, 1);
		FcPatternAddBool(pat, FC_COLOR, 1);

		FcConfigSubstitute(NULL, pat, FcMatchPattern);
		FcDefaultSubstitute(pat);

		fonts = FcFontSort(NULL, pat, 1, NULL, &result);
		FcPatternDestroy(pat);
		FcCharSetDestroy(charset);
		if (!fonts)
			die("no fonts contain glyph: 0x%x\n", rune);

		/* Allocate memory for the new cache entry. */
		if (nfcache >= fcachecap) {
			fcachecap += 16;
			fcaches = realloc(fcaches, fcachecap * sizeof(font_t));
		}

		/* veirfy matched font */
		for (i = 0; i < fonts->nfont; i++) {
			pat = fonts->fonts[i];
			FcPatternGetString(pat, FC_FILE, 0, &path);
			if (FT_New_Face(ftlib, (const char *)path, 0, &face))
				die("FT_New_Face failed seeking fallback font: %s\n", path);
			if ((idx = FT_Get_Char_Index(face, rune))) {
				break;
			}
			FT_Done_Face(face);
			face = NULL;
		}
		FcFontSetDestroy(fonts);
		if (!face)
			return 0;

		fcaches[nfcache].face = face;
		fcaches[nfcache].cairo = cairo_ft_font_face_create_for_ft_face(fcaches[nfcache].face, load_flag);
		fcaches[nfcache].hb = hb_ft_font_create(face, NULL);

		i = nfcache++;
	}
	*font = &fcaches[i];

	return idx;
}

/**
 * load_fonts() - load fonts by specified fontconfig pattern string.
 * @patstr: pattern string.
 *
 * Return:
 * 0 - success
 * 1 - failure
 */
bool
load_fonts(const char *patstr)
{
	double dpi;
	FcChar8 *path;
	FcPattern *pat = FcNameParse((FcChar8 *)patstr);

	if (!pat)
		die("loadfonts(): failed parse pattern: %s\n", patstr);

	/* get dpi and set to pattern */
	dpi = (((double)bar.scr->height_in_pixels * 25.4) / (double)bar.scr->height_in_millimeters);
	FcPatternAddDouble(pat, FC_DPI, dpi);
	FcPatternAddBool(pat, FC_SCALABLE, 1);

	FcConfigSubstitute(NULL, pat, FcMatchPattern);
	FcDefaultSubstitute(pat);

	FcResult result;
	FcPattern *match = FcFontMatch(NULL, pat, &result);
	if (!match) {
		FcPatternDestroy(pat);
		err("loadfonts(): no fonts match pattern: %s\n", patstr);
		return false;
	}

	FcPatternGetString(match, FC_FILE, 0, &path);
	if (FT_New_Face(ftlib, (const char *)path, 0, &bar.font.face))
		die("FT_New_Face failed seeking fallback font: %s\n", path);
	FcPatternGetDouble(match, FC_PIXEL_SIZE, 0, &bar.font_size);
	FcPatternDestroy(match);

	if (!bar.font.face) {
		FcPatternDestroy(pat);
		err("loadfonts(): failed open font: %s\n", patstr);
		return false;
	}

	bar.font.cairo = cairo_ft_font_face_create_for_ft_face(bar.font.face, load_flag);
	bar.font.hb = hb_ft_font_create(bar.font.face, NULL);
	bar.pattern = pat;

	bar.font_opt = cairo_font_options_create();
	cairo_font_options_set_antialias(bar.font_opt, CAIRO_ANTIALIAS_SUBPIXEL);
	cairo_font_options_set_subpixel_order(bar.font_opt, CAIRO_SUBPIXEL_ORDER_RGB);
	cairo_font_options_set_hint_style(bar.font_opt, CAIRO_HINT_STYLE_SLIGHT);
	cairo_font_options_set_hint_metrics(bar.font_opt, CAIRO_HINT_METRICS_ON);

	/* padding width */
	celwidth = bar.font_size / 2 - 1;

	graph_maxh = bar.font_size - (int)bar.font_size % 2;
	graph_basey = (BAR_HEIGHT - graph_maxh) / 2;

	return true;
}

/**
 * get_base_line() - get text rendering baseline.
 *
 * Return: y offset.
 */
int
get_baseline()
{
	return (BAR_HEIGHT - bar.font_size / 2);
}

/**
 * dc_calc_render_pos() - calculate render position.
 * @dc: DC.
 * @glyphs: (in/out) XftCharFontSpec *.
 * @nglyph: lenght of glyphs.
 */
void
dc_calc_render_pos(draw_context_t *dc, glyph_font_spec_t *glyphs, int nglyph)
{
	int x = dc_get_x(dc);
	for (int i = 0; i < nglyph; i++) {
		glyphs[i].glyph.x += x;
	}
}

/**
 * load_glyphs_from_hb_buffer() - load glyphs from hb_buffer_t.
 * @dc: draw context.
 * @buffer: harfbuzz buffer.
 * @font: a font for rendering.
 * @x: (out) base x position.
 * @y: base y position.
 * @glyphs: (out) loaded glyphs.
 * @len: max length of glyphs.
 *
 * Return: size_t
 *   num of loaded glyphs.
 */
size_t
load_glyphs_from_hb_buffer(draw_context_t *dc, hb_buffer_t *buffer, font_t *font, int *x, int y, glyph_font_spec_t *glyphs, size_t len)
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

/**
 * load_glyphs() - load XGlyphFontSpec from specified str.
 * @dc: draw context.
 * @str: utf-8 string.
 * @glyphs: (out) XCharFontSpec *.
 * @nglyph: length of glyphs.
 * @width: (out) rendering width.
 *
 * Return: number of loaded glyphs.
 */
int
load_glyphs(draw_context_t *dc, const char *str, glyph_font_spec_t *glyphs, int nglyph, int *width)
{
	FcChar32 rune = 0;
	int i, y, len = 0;
	size_t offset = 0, num = 0;
	font_t *font = NULL, *prev = NULL;
	hb_buffer_t *buffer = NULL;

	buffer = hb_buffer_create();

	y = get_baseline();
	*width = 0;
	for (i = 0; offset < strlen(str) && i < nglyph; i++, offset += len) {
		len = FcUtf8ToUcs4((FcChar8 *)&str[offset], &rune, strlen(str) - offset);
		if (get_font(rune, &font) && prev && prev != font) {
			num += load_glyphs_from_hb_buffer(dc, buffer, prev, width, y, &glyphs[num], nglyph - num);
			hb_buffer_clear_contents(buffer);
		}
		prev = font;
		hb_buffer_add_codepoints(buffer, &rune, 1, 0, 1);
	}
	if (prev && hb_buffer_get_length(buffer))
		num += load_glyphs_from_hb_buffer(dc, buffer, font, width, y, &glyphs[num], nglyph - num);

	hb_buffer_destroy(buffer);

	return num;
}

/**
 * draw_padding_em() - render padding by em units.
 * @dc: DC.
 * @num: padding width.
 */
void
draw_padding_em(draw_context_t *dc, double em)
{
	draw_padding(dc, celwidth * em);
}

/**
 * draw_padding() - render padding.
 * @dc: DC.
 * @num: padding width.
 */
void
draw_padding(draw_context_t *dc, int num)
{
	dc->x += num;
}

/**
 * draw_color_text() - render text with color.
 * @dc: DC.
 * @color: rendering text color.
 * @str: rendering text.
 */
void
draw_color_text(draw_context_t *dc, color_t *color, const char *str)
{
	int width;
	size_t nglyph = load_glyphs(dc, str, glyph_caches, sizeof(glyph_caches), &width);
	dc_calc_render_pos(dc, glyph_caches, nglyph);
	draw_glyphs(dc, color, glyph_caches, nglyph);
	dc_move_x(dc, width);
}

/**
 * draw_glyphs() - draw text use loaded glyphs.
 * @dc: draw context.
 * @color: foreground color.
 * @specs: loaded glyphs.
 * @len: max length of specs.
 */
void
draw_glyphs(draw_context_t *dc, color_t *color, const glyph_font_spec_t *specs, int len)
{
	cairo_font_face_t *prev = NULL;
	int i;

	cairo_set_font_options(dc->cr, bar.font_opt);
	cairo_set_font_size(dc->cr, bar.font_size);
	cairo_set_source_rgb(dc->cr, CONVCOL(color->red), CONVCOL(color->green), CONVCOL(color->blue));
	for (i = 0; i < len; i++) {
		if (prev != specs[i].font->cairo) {
			prev = specs[i].font->cairo;
			cairo_set_font_face(dc->cr, prev);
		}
		cairo_show_glyphs(dc->cr, (cairo_glyph_t *)&specs[i].glyph, 1);
	}
}

/**
 * draw_text() - render text.
 * @dc: draw context.
 * @str: rendering text.
 */
void
draw_text(draw_context_t *dc, const char *str)
{
	draw_color_text(dc, bar.fg, str);
}

/**
 * draw_bargraph() - render bar graph.
 * @dc: draw context.
 * @label: label of the graph.
 * @items: items of the Graph.
 * @nitem: number of items.
 */
void
draw_bargraph(draw_context_t *dc, const char *label, graph_item_t *items, int nitem)
{
	xcb_rectangle_t rect = { 0 };

	int width = (celwidth + 1) * nitem;
	draw_color_text(dc, bar.fg, label);
	int x = dc_get_x(dc) + celwidth;
	for (int i = 0; i < nitem; i++) {
		xcb_gc_color(bar.xcb, dc->gc, items[i].bg);
		rect.x = x - celwidth;
		rect.y = graph_basey;
		rect.width = celwidth;
		rect.height = graph_maxh;
		xcb_poly_fill_rectangle(bar.xcb, dc->tmp->pixmap, dc->gc, 1, &rect);

		if (items[i].val < 0)
			goto CONTINUE;

		xcb_gc_color(bar.xcb, dc->gc, items[i].fg);
		rect.width = celwidth;
		rect.height = SMALLER(BIGGER(graph_maxh * items[i].val, 1), graph_maxh);;
		rect.x = x - celwidth;
		rect.y = graph_basey + (graph_maxh - rect.height);
		xcb_poly_fill_rectangle(bar.xcb, dc->tmp->pixmap, dc->gc, 1, &rect);
	CONTINUE:
		x += celwidth + 1;
	}
	dc_move_x(dc, width);
}

/**
 * text() - render the specified text.
 * @dc: draw context.
 * @opts: module options.
 */
void
text(draw_context_t *dc, module_option_t *opts)
{
	color_t *fg = bar.fg;
	if (opts->text.fg)
		fg = color_load(opts->text.fg);
	draw_color_text(dc, fg, opts->text.label);
}

/**
 * render_labels() - render all labels
 * @dc: DC.
 * @labels: label_t array.
 * @nlabel: length of labels.
 */
void
render_labels(draw_context_t *dc, label_t *labels, size_t nlabel)
{
	size_t i;
	int x = 0;

	for (i = 0; i < nlabel; i++) {
		if (!labels[i].option->any.func)
			continue;

		x = dc_get_x(dc);

		draw_padding(dc, celwidth);
		labels[i].option->any.func(dc, labels[i].option);
		draw_padding(dc, celwidth);
		labels[i].width = dc_get_x(dc) - x;
		labels[i].x = x;
		if (labels[i].width == celwidth * 2) {
			dc->x = x;
			labels[i].width = 0;
			continue;
		}
		dc->width += labels[i].width;
	}
}

/**
 * xcb_gc_color() - set foreground color to xcb_gcontext_t
 * @xcb: xcb connection.
 * @gc: xcb_gcontext_t
 * @color: foreground color.
 */
void
xcb_gc_color(xcb_connection_t *xcb, xcb_gcontext_t gc, color_t *color)
{
	xcb_change_gc_value_list_t values = { .foreground = color->pixel };
	xcb_change_gc_aux(xcb, gc, XCB_GC_FOREGROUND, &values);
}

/**
 * calculate_systray_item_positions() - calculate position of tray items.
 * @label: the label must has been made from systray module.
 * @opts: module option.
 */
void
calculate_systray_item_positions(label_t *label, module_option_t *opts)
{
	xcb_configure_window_value_list_t values;
	uint32_t mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;
	systray_item_t *item;
	list_head *pos;
	int x;

	if (!systray_icon_size(tray))
		systray_set_icon_size(tray, opts->tray.iconsize);

	if (list_empty(systray_get_items(tray)))
		return;

	/* fix the position for padding */
	x = label->x + celwidth;
	list_for_each(systray_get_items(tray), pos) {
		item = list_entry(pos, systray_item_t, head);
		if (!item->info.flags)
			continue;
		if (item->x != x) {
			values.x = x;
			values.y = (BAR_HEIGHT - opts->tray.iconsize) / 2;
			values.width = opts->tray.iconsize;
			values.height = opts->tray.iconsize;
			if (!xcb_request_check(bar.xcb, xcb_configure_window_aux(bar.xcb, item->win, mask, &values)))
				item->x = x;
		}
		x += opts->tray.iconsize + celwidth;
	}
}

/**
 * calculate_label_positions() - calculate position of labels.
 * @dc: draw_context_t
 * @labels: label_t array.
 * @nlabel: length of labels.
 * @offset: position offset.
 */
void
calculate_label_positions(draw_context_t *dc, label_t *labels, size_t nlabel, int offset)
{
	size_t i;
	for (i = 0; i < nlabel; i++) {
		labels[i].x += offset;

		if (systray_get_window(tray) == dc->xbar.win && labels[i].option->any.func == systray)
			calculate_systray_item_positions(&labels[i], labels[i].option);
	}
}

/**
 * render() - rendering all modules.
 */
void
render()
{
	draw_context_t *dc;
	window_t *xw;
	xcb_rectangle_t rect = { 0 };
	int i;

	for (i = 0; i < bar.ndc; i++) {
		dc = &bar.dcs[i];
		xw = &dc->xbar;
		rect.width = xw->width;
		rect.height = xw->height;

		xcb_gc_color(bar.xcb, dc->gc, bar.bg);
		xcb_poly_fill_rectangle(bar.xcb, dc->buf->pixmap, dc->gc, 1, &rect);

		/* render left modules */
		xcb_poly_fill_rectangle(bar.xcb, dc->tmp->pixmap, dc->gc, 1, &rect);
		dc->x = dc->width = 0;
		render_labels(dc, dc->left_labels, LENGTH(left_modules));
		xcb_copy_area(bar.xcb, dc->tmp->pixmap, dc->buf->pixmap, dc->gc, 0, 0, celwidth, 0, dc->width, xw->height);
		calculate_label_positions(dc, dc->left_labels, LENGTH(left_modules), celwidth);

		/* render right modules */
		xcb_poly_fill_rectangle(bar.xcb, dc->tmp->pixmap, dc->gc, 1, &rect);
		dc->x = dc->width = 0;
		render_labels(dc, dc->right_labels, LENGTH(right_modules));
		xcb_copy_area(bar.xcb, dc->tmp->pixmap, dc->buf->pixmap, dc->gc, 0, 0, xw->width - dc->width - celwidth, 0, dc->width, xw->height);
		calculate_label_positions(dc, dc->right_labels, LENGTH(right_modules), xw->width - dc->width - celwidth);

		/* copy pixmap to window */
		xcb_copy_area(bar.xcb, dc->buf->pixmap, xw->win, dc->gc, 0, 0, 0, 0, xw->width, xw->height);
	}
	xcb_flush(bar.xcb);
}

/**
 * bspwmbar_init() - initialize bspwmbar.
 * @dpy: display pointer.
 * @scr: screen number.
 *
 * Return:
 * 0 - success
 * 1 - failure
 */
bool
bspwmbar_init(xcb_connection_t *xcb, xcb_screen_t *scr)
{
	xcb_randr_get_monitors_reply_t *mon_reply;
	xcb_randr_get_screen_resources_reply_t *screen_reply;
	xcb_randr_get_output_info_reply_t *info_reply;
	xcb_randr_output_t *outputs;
	xcb_randr_get_crtc_info_reply_t *crtc_reply;
	int i, nmon = 0;

	/* initialize */
	bar.xcb = xcb;
	bar.scr = scr;
	bar.cmap = scr->default_colormap;
	bar.fg = color_load(FGCOLOR);
	bar.bg = color_load(BGCOLOR);

	/* get monitors */
	mon_reply = xcb_randr_get_monitors_reply(xcb, xcb_randr_get_monitors(xcb, scr->root, 1), NULL);
	bar.dcs = (draw_context_t *)calloc(mon_reply->nMonitors, sizeof(draw_context_t));
	bar.ndc = mon_reply->nMonitors;

	/* create window per monitor */
	screen_reply = xcb_randr_get_screen_resources_reply(xcb, xcb_randr_get_screen_resources(xcb, scr->root), NULL);
	outputs = xcb_randr_get_screen_resources_outputs(screen_reply);
	for (i = 0; i < screen_reply->num_outputs; i++) {
		info_reply = xcb_randr_get_output_info_reply(xcb, xcb_randr_get_output_info(xcb, outputs[i], XCB_TIME_CURRENT_TIME), NULL);
		if (info_reply->crtc != XCB_NONE) {
			crtc_reply = xcb_randr_get_crtc_info_reply(xcb, xcb_randr_get_crtc_info(xcb, info_reply->crtc, XCB_TIME_CURRENT_TIME), NULL);
			if (dc_init(&bar.dcs[nmon], xcb, scr, crtc_reply->x, crtc_reply->y, crtc_reply->width, BAR_HEIGHT))
				strncpy(bar.dcs[nmon++].monitor_name, (const char *)xcb_randr_get_output_info_name(info_reply), SMALLER(xcb_randr_get_output_info_name_length(info_reply), NAME_MAXSZ));
			free(crtc_reply);
		}
		free(info_reply);
	}
	free(screen_reply);
	free(mon_reply);

	if (!nmon)
		return false;

	/* load_fonts */
	if (!load_fonts(fontname))
		return false;

	xcb_flush(xcb);
	return true;
}

/**
 * font_destroy() - free all resources of font.
 * @font: font_t
 */
void
font_destroy(font_t font)
{
	cairo_font_face_destroy(font.cairo);
	hb_font_destroy(font.hb);
	FT_Done_Face(font.face);
}

/**
 * font_caches_destroy() - free all resources of font caches.
 */
void
font_caches_destroy()
{
	int i;
	for (i = 0; i < nfcache; i++)
		font_destroy(fcaches[i]);
	nfcache = 0;
	fcachecap = 0;
	if (fcaches)
		free(fcaches);
}

/**
 * bspwmbar_destroy() - destroy all resources of bspwmbar.
 */
void
bspwmbar_destroy()
{
	list_head *cur;
	int i;
	list_head *pos;

	list_for_each(&pollfds, cur)
		poll_del(list_entry(cur, poll_fd_t, head));

	/* font resources */
	cairo_font_options_destroy(bar.font_opt);
	font_destroy(bar.font);
	font_caches_destroy();
	FcPatternDestroy(bar.pattern);

	/* deinit modules */
	list_for_each(&pollfds, pos)
		poll_del(list_entry(pos, poll_fd_t, head));

	/* rendering resources */
	for (i = 0; i < bar.ndc; i++)
		dc_free(bar.dcs[i]);
	free(bar.dcs);
}

/**
 * systray() - render systray.
 * @dc: draw context.
 * @opts: dummy.
 */
void
systray(draw_context_t *dc, module_option_t *opts)
{
	list_head *pos, *base;
	(void)opts;

	if (!systray_icon_size(tray))
		systray_set_icon_size(tray, opts->tray.iconsize);

	if (list_empty(systray_get_items(tray)))
		return;

	if (systray_get_window(tray) != dc->xbar.win)
		return;

	/* render spaces for iconsize */
	base = systray_get_items(tray);
	list_for_each(base, pos) {
		systray_item_t *item = list_entry(pos, systray_item_t, head);
		if (!item->info.flags)
			continue;
		draw_padding(dc, opts->tray.iconsize);
		if (base != pos->next)
			draw_padding(dc, celwidth);
	}
}

/**
 * poll_stop() - stop polling to all file descriptor.
 */
void
poll_stop()
{
	if (pfd > 0)
		close(pfd);
}

/**
 * poll_add() - add the file descriptor to polling targets.
 * @pollfd: PollFD object.
 */
void
poll_add(poll_fd_t *pollfd)
{
	(void)pollfd;
#if defined(__linux)
	struct epoll_event ev;

	ev.events = EPOLLIN;
	ev.data.fd = pollfd->fd;
	ev.data.ptr = (void *)pollfd;

	if (epoll_ctl(pfd, EPOLL_CTL_ADD, pollfd->fd, &ev) == -1)
		die("epoll_ctl(): failed to add to epoll\n");
#elif defined(__OpenBSD__)
	struct kevent ev = { 0 };

	EV_SET(&ev, pollfd->fd, EVFILT_READ, EV_ADD, 0, 0, pollfd);
	if (kevent(pfd, &ev, 1, NULL, 0, NULL) == -1)
		die("EV_SET(): failed to add to kqueue\n");
#endif

	list_add_tail(&pollfds, &pollfd->head);
}

/**
 * poll_del() - delete the file descriptor from polling targets.
 * @pollfd: PollFD object.
 */
void
poll_del(poll_fd_t *pollfd)
{
	if (pollfd->deinit)
		pollfd->deinit();
	if (pollfd->fd) {
#if defined(__linux)
		epoll_ctl(pfd, EPOLL_CTL_DEL, pollfd->fd, NULL);
#elif defined(__OpenBSD__)
		struct kevent ev = { 0 };
		EV_SET(&ev, pollfd->fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
		kevent(pfd, &ev, 1, NULL, 0, NULL);
#endif
	}
	list_del(&pollfd->head);
}

/**
 * poll_init() - initialize poll.
 *
 * The function must be called before poll_add(), poll_del().
 */
void
poll_init()
{
	list_head_init(&pollfds);
}

#if defined(__linux)
/**
 * timer_reset() - PollUpdateHandler for timer.
 * @fd: timerfd.
 *
 * Return: PollResult
 *
 * always - PR_UPDATE
 */
poll_result_t
timer_reset(int fd)
{
	uint64_t tcnt;
	read(fd, &tcnt, sizeof(uint64_t));
	return PR_UPDATE;
}

#endif
/**
 * is_change_active_window_event() - check the event is change active window.
 *
 * ev: xcb_generic_event_t
 *
 * Return: bool
 */
bool
is_change_active_window_event(xcb_property_notify_event_t *ev)
{
	return (ev->window == bar.scr->root) && (ev->atom == ewmh._NET_ACTIVE_WINDOW);
}

poll_result_t
xcb_event_notify(xcb_generic_event_t *event, draw_context_t *dc)
{
	size_t i;
	xcb_button_press_event_t *button = (xcb_button_press_event_t *)event;
	for (i = 0; i < LENGTH(left_modules); i++) {
		if (!dc->left_labels[i].option->any.handler)
			continue;
		if (IS_LABEL_EVENT(dc->left_labels[i], button)) {
			dc->left_labels[i].option->any.handler(event);
			return PR_UPDATE;
		}
	}
	for (i = 0; i < LENGTH(right_modules); i++) {
		if (!dc->right_labels[i].option->any.handler)
			continue;
		if (IS_LABEL_EVENT(dc->right_labels[i], button)) {
			dc->right_labels[i].option->any.handler(event);
			return PR_UPDATE;
		}
	}
	return PR_NOOP;
}

/**
 * xev_handle() - X11 event handling
 *
 * Return: poll_result_t
 * PR_NOOP   - success and not need more action
 * PR_UPDATE - success and need rerendering
 */
poll_result_t
xev_handle()
{
	xcb_generic_event_t *event;
	xcb_button_press_event_t *button;
	xcb_property_notify_event_t *prop;
	xcb_window_t win;
	poll_result_t res = PR_NOOP;
	draw_context_t *dc;

	xcb_change_window_attributes_value_list_t attrs;
	uint32_t mask = XCB_CW_EVENT_MASK;
	attrs.event_mask = XCB_EVENT_MASK_PROPERTY_CHANGE;

	/* for X11 events */
	while ((event = xcb_poll_for_event(bar.xcb))) {
		switch (event->response_type & ~0x80) {
		case XCB_SELECTION_CLEAR:
			systray_handle(tray, event);
			break;
		case XCB_EXPOSE:
			res = PR_UPDATE;
			break;
		case XCB_BUTTON_PRESS:
			dc = NULL;
			button = (xcb_button_press_event_t *)event;
			for (int j = 0; j < bar.ndc; j++)
				if (bar.dcs[j].xbar.win == button->event)
					dc = &bar.dcs[j];
			if (!dc)
				break;
			/* notify evnent to modules */
			xcb_event_notify(event, dc);
			break;
		case XCB_PROPERTY_NOTIFY:
			prop = (xcb_property_notify_event_t *)event;
			if (prop->atom == xembed_info) {
				systray_handle(tray, event);
			} else if (is_change_active_window_event(prop) || prop->atom == ewmh._NET_WM_NAME) {
				windowtitle_update(bar.xcb, 0);
				res = PR_UPDATE;
			}
			if (is_change_active_window_event(prop) && (win = get_active_window(0)))
				xcb_change_window_attributes_aux(bar.xcb, win, mask, &attrs);
			break;
		case XCB_CLIENT_MESSAGE:
			systray_handle(tray, event);
			res = PR_UPDATE;
			break;
		case XCB_UNMAP_NOTIFY:
			win = ((xcb_unmap_notify_event_t *)event)->event;
			systray_remove_item(tray, win);
			res = PR_UPDATE;
			break;
		case XCB_DESTROY_NOTIFY:
			win = ((xcb_destroy_notify_event_t *)event)->event;
			systray_remove_item(tray, win);
			res = PR_UPDATE;
			break;
		}
		free(event);
	}
	return res;
}

#if defined(__linux)
/*
 * epoll_wait_ignore_eintr()
 *
 * epoll_wait() wrapper to ignore EINTR errno
 * EINTR errno can be set when the process was interrupted
 * for example on breakpoint stop or when system goes to sleep
 */
int
epoll_wait_ignore_eintr(int pfd, struct epoll_event *events, int maxevents, int timeout)
{
       int nfd;
       errno = 0;
       nfd = epoll_wait(pfd, events, maxevents, timeout);
       if (nfd != -1)
               return nfd;
       if (errno == EINTR)
               return 0;
       return -1;
}
#endif

/*
 * poll_loop() - polling loop
 * @handler: rendering function
 */
void
poll_loop(void (* handler)())
{
	int i, nfd, need_render;
	poll_fd_t *pollfd;

#if defined(__linux)
	/* timer for rendering at one sec interval */
	struct itimerspec interval = { {1, 0}, {1, 0} };
	/* initialize timer */
	int tfd = timerfd_create(CLOCK_REALTIME, 0);
	timerfd_settime(tfd, 0, &interval, NULL);

	timer.fd = tfd;
	timer.handler = timer_reset;
	poll_add(&timer);
#endif

	/* polling X11 event for modules */
	xfd.fd = xcb_get_file_descriptor(bar.xcb);
	xfd.handler = xev_handle;
	poll_add(&xfd);

	/* polling fd */
#if defined(__linux)
	while ((nfd = epoll_wait_ignore_eintr(pfd, events, MAX_EVENTS, -1)) != -1) {
		need_render = 0;
#elif defined(__OpenBSD__)
	struct timespec tspec = { 0 };
	tspec.tv_sec = 1;
	while ((nfd = kevent(pfd, NULL, 0, events, MAX_EVENTS, &tspec)) != -1) {
		need_render = 0;
		if (!nfd)
			need_render = 1;
#endif
		for (i = 0; i < nfd; i++) {
#if defined(__linux)
			pollfd = (poll_fd_t *)events[i].data.ptr;
#elif defined(__OpenBSD__)
			pollfd = (poll_fd_t *)events[i].udata;
#endif
			switch ((int)pollfd->handler(pollfd->fd)) {
			case PR_UPDATE:
				need_render = 1;
				break;
			case PR_REINIT:
				poll_del(pollfd);
				pollfd->fd = pollfd->init();
				poll_add(pollfd);
				break;
			case PR_FAILED:
				poll_stop();
				break;
			}
		}
		if (need_render) {
#if defined(__linux)
			/* force render after interval */
			timerfd_settime(tfd, 0, &interval, NULL);
#endif
			windowtitle_update(bar.xcb, 0);
			handler();
		}
	}
}

/**
 * @signal_handler - a signal handler.
 * @signum: signal number.
 *
 * The function stop polling if signum equals SIGINT or SIGTERM.
 */
void
signal_handler(int signum)
{
	switch (signum) {
	case SIGINT:
	case SIGTERM:
		poll_stop();
		break;
	}
}

/**
 * cleanup() - cleanup resources
 */
void
cleanup(xcb_connection_t *xcb)
{
	int i;
	if (wintitle)
		free(wintitle);

	if (tray)
		systray_destroy(tray);
	for (i = 0; i < ncol; i++) {
		free(cols[i]->name);
		free(cols[i]);
	}
	free(cols);
	bspwmbar_destroy();
	FT_Done_FreeType(ftlib);
	xcb_ewmh_connection_wipe(&ewmh);
	xcb_disconnect(xcb);
	FcFini();
}

void
run()
{
	struct sigaction act;
	xcb_connection_t *xcb;
	xcb_screen_t *scr;
	xcb_change_window_attributes_value_list_t attrs;
	uint32_t mask = XCB_CW_EVENT_MASK;

	sigemptyset(&act.sa_mask);
	act.sa_handler = &signal_handler;
	act.sa_flags = 0;
	sigaction(SIGTERM, &act, NULL);
	sigaction(SIGINT, &act, NULL);

	setlocale(LC_ALL, "");

	/* polling initialize for modules */
	poll_init();

	if (!(xcb = xcb_connect(NULL, NULL)))
		die("xcb_connect(): Failed to connect to X server\n");
	scr = xcb_setup_roots_iterator(xcb_get_setup(xcb)).data;
	if (!xcb_ewmh_init_atoms_replies(&ewmh, xcb_ewmh_init_atoms(xcb, &ewmh), NULL))
		die("xcb_ewmh_init_atoms(): Failed to initialize atoms\n");
	FT_Init_FreeType(&ftlib);

	/* get active widnow title */
	windowtitle_update(xcb, 0);

	if (!bspwmbar_init(xcb, scr)) {
		err("bspwmbar_init(): Failed to init bspwmbar\n");
		goto CLEANUP;
	}

	/* tray initialize */
	if (!(tray = systray_new(xcb, scr, bar.dcs[0].xbar.win))) {
		err("systray_new(): Selection already owned by other window\n");
		goto CLEANUP;
	}

#if defined(__linux)
	/* epoll */
	if ((pfd = epoll_create1(0)) == -1) {
		err("epoll_create1(): Failed to create epoll fd\n");
		goto CLEANUP;
	}
#elif defined(__OpenBSD__)
	if (!(pfd = kqueue())) {
		err("kqueue(): Failed to create kqueue fd\n");
		goto CLEANUP;
	}
#endif

	/* wait PropertyNotify events of root window */
	attrs.event_mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
	xcb_change_window_attributes_aux(bar.xcb, scr->root, mask, &attrs);

	/* polling X11 event for modules */
	attrs.event_mask = XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_EXPOSURE;
	for (int i = 0; i < bar.ndc; i++)
		xcb_change_window_attributes_aux(bar.xcb, bar.dcs[i].xbar.win, mask, &attrs);

	/* cache Atom */
	xembed_info = xcb_atom_get(bar.xcb, "_XEMBED_INFO", false);

	/* main loop */
	poll_loop(render);

CLEANUP:
	/* cleanup resources */
	cleanup(xcb);
}

int
main(int argc, char *argv[])
{
	int opt;
	while ((opt = getopt(argc, argv, ":v")) != -1) {
		switch (opt) {
		case 'v':
			die("bspwmbar version %s\n", VERSION);
		}
	}

	run();
}
