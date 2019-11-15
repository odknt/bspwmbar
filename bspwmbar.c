/* See LICENSE file for copyright and license details. */

#if defined(__linux)
# define _XOPEN_SOURCE 700
# include <alloca.h>
# include <sys/epoll.h>
# include <sys/timerfd.h>
# include <sys/un.h>
#elif defined(__OpenBSD__)
# include <sys/types.h>
# include <sys/event.h>
# include <sys/time.h>
#endif

/* common libraries */
#include <sys/socket.h>
#include <sys/un.h>
#include <locale.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <unistd.h>

/* XCB */
#include <xcb/xcb.h>
#include <xcb/xcb_util.h>
#include <xcb/xcb_ewmh.h>
#include <xcb/xcb_icccm.h>
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
#include "systray.h"
#include "config.h"

#if !defined(VERSION)
# define VERSION "v0.0.0-dev"
#endif

/* bspwm commands */
#define SUBSCRIBE_REPORT "subscribe\0report"
/* epoll max events */
#define MAX_EVENTS 10
/* convert color for cairo */
#define CONVCOL(x) (double)((x) / 255.0)
/* check event and returns true if target is the label */
#define IS_LABEL_EVENT(l,e) (((l).x < (e)->event_x) && ((l).x + (l).width))

struct _color_t {
	char *name;
	uint16_t red;
	uint16_t green;
	uint16_t blue;
	uint32_t pixel;
};

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

typedef enum {
	DA_RIGHT = 0,
	DA_LEFT,
	/* currently not supported the below */
	DA_CENTER
} draw_align_t;

typedef struct {
	module_option_t *option;
	draw_align_t align;
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
	monitor_t monitor;

	int x, y, width, height;
} window_t;

struct _draw_context_t {
	window_t xbar;

	xcb_visualtype_t *visual;
	xcb_gcontext_t gc;
	xcb_drawable_t buf;
	draw_align_t align;
	cairo_t *cr;

	int left_x, right_x;

	label_t labels[LENGTH(left_modules) + LENGTH(right_modules)];
	int nlabel;
};

typedef struct {
	/* bspwm socket fd */
	int fd;

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

static bspwmbar_t bar;
static systray_t *tray;
static poll_fd_t bfd, xfd;
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
static void xcb_gc_color(xcb_connection_t *, xcb_gcontext_t, color_t *);
static char *get_window_title(xcb_connection_t *, xcb_window_t);
static FT_UInt get_font(FcChar32 rune, font_t **);
static bool load_fonts(const char *);
static void font_destroy(font_t font);
static size_t load_glyphs_from_hb_buffer(draw_context_t *, hb_buffer_t *, font_t *, int *, int, glyph_font_spec_t *, size_t);
static int load_glyphs(draw_context_t *, const char *, glyph_font_spec_t *, int, int *);
static void dc_init(draw_context_t *, xcb_connection_t *, xcb_screen_t *, int, int, int, int);
static void dc_free(draw_context_t);
static int dc_get_x(draw_context_t *);
static void dc_move_x(draw_context_t *, int);
static void dc_calc_render_pos(draw_context_t *, glyph_font_spec_t *, int);
static void draw_padding(draw_context_t *, int);
static void draw_string(draw_context_t *, color_t *, const char *);
static void draw_glyphs(draw_context_t *, color_t *, const glyph_font_spec_t *, int nglyph);
static void render_label(draw_context_t *);
static int bspwm_connect();
static int bspwm_send(char *, int);
static void bspwm_parse(char *);
static desktop_state_t bspwm_desktop_state(char);
static void windowtitle_update(xcb_connection_t *, uint8_t);
static void render();
static int get_baseline();
static int bspwmbar_init(xcb_connection_t *, xcb_screen_t *);
static void bspwmbar_destroy();
static void poll_init();
static void poll_loop(void (*)());
static void poll_stop();
static poll_result_t bspwm_handle(int);
static poll_result_t xev_handle();
#if defined(__linux)
static poll_result_t timer_reset(int);
#endif
static bool is_change_active_window_event(xcb_property_notify_event_t *);
static void cleanup(xcb_connection_t *);
static void run();

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
 * bspwm_desktop_state() - parse char to bspwm desktop state.
 * @s: desktop state char.
 *
 * Retrun: DesktopState
 * 'o'         - STATE_OCCUPIED
 * 'u'         - STATE_URGENT
 * 'F','U','O' - STATE_ACTIVE
 * not match   - STATE_FREE
 */
desktop_state_t
bspwm_desktop_state(char s)
{
	desktop_state_t state = STATE_FREE;
	if ((s | 0x20) == 'o')
		state = STATE_OCCUPIED;
	if ((s | 0x20) == 'u')
		state = STATE_URGENT;
	if (s == 'F' || s == 'U' || s == 'O')
		return state | STATE_ACTIVE;
	return state;
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
 * dc_init() - initialize DC.
 * @dc: draw context.
 * @xcb: xcb connection.
 * @scr: screen number.
 * @x: window position x.
 * @y: window position y.
 * @width: window width.
 * @height: window height.
 */
void
dc_init(draw_context_t *dc, xcb_connection_t *xcb, xcb_screen_t *scr, int x,
        int y, int width, int height)
{
	xcb_configure_window_value_list_t winconf = { 0 };
	xcb_create_gc_value_list_t gcv = { 0 };
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
	dc->buf = xw->win;
	dc->visual = xcb_visualtype_get(scr);

	surface = cairo_xcb_surface_create(xcb, dc->buf, dc->visual, xw->width, xw->height);
	dc->cr = cairo_create(surface);
	cairo_surface_destroy(surface);

	gcv.graphics_exposures = 1;
	dc->gc = xcb_generate_id(xcb);
	xcb_create_gc_aux(xcb, dc->gc, dc->buf, XCB_GC_GRAPHICS_EXPOSURES, &gcv);

	/* set class hint */
	xcb_change_property(xcb, XCB_PROP_MODE_REPLACE, xw->win, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, 8, "bspwmbar");
	xcb_change_property(xcb, XCB_PROP_MODE_REPLACE, xw->win, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, 17, "bspwmbar\0bspwmbar");

	/* create labels from modules */
	dc->nlabel = LENGTH(left_modules) + LENGTH(right_modules);
	for (i = 0; i < (int)LENGTH(left_modules); i++) {
		dc->labels[i].align = DA_LEFT;
		dc->labels[i].option = &left_modules[i];
	}
	int rmlen = LENGTH(right_modules), nlabel = i;
	for (i = rmlen - 1; i >= 0; i--, nlabel++) {
		dc->labels[nlabel].align = DA_RIGHT;
		dc->labels[nlabel].option = &right_modules[i];
	}

	/* send window rendering request */
	winconf.stack_mode = XCB_STACK_MODE_BELOW;
	xcb_configure_window_aux(xcb, xw->win, XCB_CONFIG_WINDOW_STACK_MODE, &winconf);
	xcb_map_window(xcb, xw->win);
}

/**
 * dc_free() - free resources of DC.
 * dc: draw context.
 */
void
dc_free(draw_context_t dc)
{
	xcb_free_gc(bar.xcb, dc.gc);
	xcb_destroy_window(bar.xcb, dc.xbar.win);
	cairo_destroy(dc.cr);
	free(dc.xbar.monitor.desktops);
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
	if (dc->align == DA_LEFT)
		return dc->left_x;
	return dc->xbar.width - dc->right_x;
}

/**
 * dc_move_x() - move rendering position by x.
 * @dc: draw context.
 * @x: distance of movement.
 */
void
dc_move_x(draw_context_t *dc, int x)
{
	if (dc->align == DA_LEFT)
		dc->left_x += x;
	else if (dc->align == DA_RIGHT)
		dc->right_x += x;
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
 * draw_padding() - pender padding.
 * @dc: DC.
 * @num: padding width.
 */
void
draw_padding(draw_context_t *dc, int num)
{
	switch ((int)dc->align) {
	case DA_LEFT:
		dc->left_x += num;
		break;
	case DA_RIGHT:
		if (!dc->right_x)
			num += celwidth;
		dc->right_x += num;
		break;
	}
}

/**
 * draw_string() - render string with the color.
 * @dc: DC.
 * @color: rendering text color.
 * @str: rendering text.
 */
void
draw_string(draw_context_t *dc, color_t *color, const char *str)
{
	int width;
	size_t nglyph = load_glyphs(dc, str, glyph_caches, sizeof(glyph_caches), &width);
	if (dc->align == DA_RIGHT)
		dc_move_x(dc, width);
	dc_calc_render_pos(dc, glyph_caches, nglyph);
	draw_glyphs(dc, color, glyph_caches, nglyph);
	if (dc->align == DA_LEFT)
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
	draw_padding(dc, celwidth);
	draw_string(dc, bar.fg, str);
	draw_padding(dc, celwidth);
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

	draw_padding(dc, celwidth);
	int width = (celwidth + 1) * nitem;
	if (dc->align == DA_RIGHT)
		dc->right_x += width;
	int x = dc_get_x(dc) + celwidth;
	draw_string(dc, bar.fg, label);
	draw_padding(dc, celwidth);
	for (int i = 0; i < nitem; i++) {
		xcb_gc_color(bar.xcb, dc->gc, items[i].bg);
		rect.x = x - celwidth;
		rect.y = graph_basey;
		rect.width = celwidth;
		rect.height = graph_maxh;
		xcb_poly_fill_rectangle(bar.xcb, dc->buf, dc->gc, 1, &rect);

		if (items[i].val < 0)
			goto CONTINUE;

		xcb_gc_color(bar.xcb, dc->gc, items[i].fg);
		rect.width = celwidth;
		rect.height = SMALLER(BIGGER(graph_maxh * items[i].val, 1), graph_maxh);;
		rect.x = x - celwidth;
		rect.y = graph_basey + (graph_maxh - rect.height);
		xcb_poly_fill_rectangle(bar.xcb, dc->buf, dc->gc, 1, &rect);
	CONTINUE:
		x += celwidth + 1;
	}
	if (dc->align == DA_LEFT)
		dc_move_x(dc, width);
}

/**
 * bspwm_parse() - parse bspwm reported string.
 * @report: bspwm reported string.
 */
void
bspwm_parse(char *report)
{
	int i, j, name_len, nws = 0;
	int len = strlen(report);
	char tok, name[NAME_MAXSZ];
	monitor_t *curmon = NULL;

	for (i = 0; i < len; i++) {
		switch (tok = report[i]) {
		case 'M':
		case 'm':
			nws = 0;
			for (j = ++i; j < len; j++)
				if (report[j] == ':')
					break;
			name_len = SMALLER(j - i, NAME_MAXSZ - 1);
			strncpy(name, &report[i], name_len);
			name[name_len] = '\0';
			i = j;
			for (j = 0; j < bar.ndc; j++)
				if (!strncmp(bar.dcs[j].xbar.monitor.name, name, strlen(name)))
					curmon = &bar.dcs[j].xbar.monitor;
			if (curmon)
				curmon->is_active = (tok == 'M') ? 1 : 0;
			break;
		case 'o':
		case 'O':
		case 'f':
		case 'F':
		case 'u':
		case 'U':
			for (j = ++i; j < len; j++)
				if (report[j] == ':')
					break;
			if (nws + 1 >= curmon->cdesktop) {
				curmon->cdesktop += 5;
				curmon->desktops = realloc(curmon->desktops, sizeof(desktop_t) * curmon->cdesktop);
			}
			curmon->desktops[nws++].state = bspwm_desktop_state(tok);
			i = j;
			break;
		case 'L':
		case 'T':
			i++; /* skip next char. */
			break;
		case 'G':
			if (curmon)
				curmon->ndesktop = nws;
			/* skip current node flags. */
			while (report[i + 1] != ':' && report[i + 1] != '\n')
				i++;
			break;
		}
	}
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
	draw_padding(dc, celwidth);
	draw_string(dc, fg, opts->text.label);
	draw_padding(dc, celwidth);
}

/**
 * render_label() - render all labels
 * @dc: DC.
 */
void
render_label(draw_context_t *dc)
{
	int x = 0, width = 0;
	for (int j = 0; j < dc->nlabel; j++) {
		x = dc_get_x(dc); width = 0;

		dc->align = dc->labels[j].align;
		dc->labels[j].option->any.func(dc, dc->labels[j].option);
		if (dc->align == DA_LEFT)
			width = dc_get_x(dc) - x;
		else if (dc->align == DA_RIGHT)
			width = x - dc_get_x(dc);
		x = dc_get_x(dc);
		if (width)
			width += celwidth;
		dc->labels[j].width = width;
		dc->labels[j].x = x;
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
	uint32_t values[] = { color->pixel, 0 };
	xcb_change_gc(xcb, gc, XCB_GC_FOREGROUND, values);
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

	for (int i = 0; i < bar.ndc; i++) {
		dc = &bar.dcs[i];
		dc->align = DA_LEFT;
		dc->left_x = 0;
		dc->right_x = 0;
		xw = &dc->xbar;
		rect.width = xw->width;
		rect.height = xw->height;

		xcb_gc_color(bar.xcb, dc->gc, bar.bg);
		xcb_poly_fill_rectangle(bar.xcb, xw->win, dc->gc, 1, &rect);

		/* render modules */
		draw_padding(dc, celwidth);
		render_label(dc);
	}
	xcb_flush(bar.xcb);
}

/**
 * bspwm_connect() - connect to bspwm socket.
 *
 * Return: file descripter or -1.
 */
int
bspwm_connect()
{
	struct sockaddr_un sock;
	int fd, dpyno = 0, scrno = 0;
	char *sp = NULL;

	sock.sun_family = AF_UNIX;
	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
		return -1;

	sp = getenv("BSPWM_SOCKET");
	if (sp) {
		snprintf(sock.sun_path, sizeof(sock.sun_path), "%s", sp);
	} else {
		if (xcb_parse_display(NULL, &sp, &dpyno, &scrno))
			snprintf(sock.sun_path, sizeof(sock.sun_path),
			         "/tmp/bspwm%s_%i_%i-socket", sp, dpyno, scrno);
		free(sp);
	}

	if (connect(fd, (struct sockaddr *)&sock, sizeof(sock)) == -1)
		return -1;

	return fd;
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
int
bspwmbar_init(xcb_connection_t *xcb, xcb_screen_t *scr)
{
	xcb_randr_get_monitors_reply_t *mon_reply;
	xcb_randr_get_screen_resources_reply_t *screen_reply;
	xcb_randr_get_output_info_reply_t *info_reply;
	xcb_randr_output_t *outputs;
	xcb_randr_get_crtc_info_reply_t *crtc_reply;
	int i, nmon = 0;

	/* connect bspwm socket */
	if ((bar.fd = bspwm_connect()) == -1) {
		err("bspwm_connect(): Failed to connect to the socket\n");
		return 1;
	}

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
			dc_init(&bar.dcs[nmon], xcb, scr, crtc_reply->x, crtc_reply->y, crtc_reply->width, BAR_HEIGHT);
			strncpy(bar.dcs[nmon++].xbar.monitor.name, (const char *)xcb_randr_get_output_info_name(info_reply), NAME_MAXSZ);
			free(crtc_reply);
		}
		free(info_reply);
	}
	free(screen_reply);
	free(mon_reply);

	/* load_fonts */
	if (!load_fonts(fontname))
		return 1;

	xcb_flush(xcb);
	return 0;
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
 * bspwm_send() - send specified command to bspwm.
 * @cmd: bspwm command.
 * @len: length of cmd.
 *
 * Return: sent bytes length.
 */
int
bspwm_send(char *cmd, int len)
{
	return send(bar.fd, cmd, len, 0);
}

/**
 * desktops() - render bspwm desktop states.
 * @dc: draw context.
 * @opts: module options.
 */
void
desktops(draw_context_t *dc, module_option_t *opts)
{
	static color_t *fg = NULL, *altfg = NULL;
	color_t *col;
	const char *ws;
	int cur, max = dc->xbar.monitor.ndesktop;

	if (!fg)
		fg = color_load(FGCOLOR);
	if (!altfg)
		altfg = color_load(ALTFGCOLOR);

	draw_padding(dc, celwidth);
	for (int i = 0, j = max - 1; i < max; i++, j--) {
		cur = (dc->align == DA_RIGHT) ? j : i;
		draw_padding(dc, celwidth / 2.0 + 0.5);
		ws = (dc->xbar.monitor.desktops[cur].state & STATE_ACTIVE) ? opts->desk.active : opts->desk.inactive;
		col = (dc->xbar.monitor.desktops[cur].state == STATE_FREE) ? altfg : fg;
		draw_string(dc, col, ws);
		draw_padding(dc, celwidth / 2.0 + 0.5);
	}
	draw_padding(dc, celwidth);
}

/**
 * systray() - render systray.
 * @dc: draw context.
 * @opts: dummy.
 */
void
systray(draw_context_t *dc, module_option_t *opts)
{
	list_head *pos;
	int x;
	xcb_configure_window_value_list_t values;
	uint32_t mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;
	(void)opts;

	if (!systray_icon_size(tray))
		systray_set_icon_size(tray, opts->tray.iconsize);

	if (list_empty(systray_get_items(tray)))
		return;

	if (systray_get_window(tray) != dc->xbar.win)
		return;

	draw_padding(dc, celwidth);

	list_for_each(systray_get_items(tray), pos) {
		systray_item_t *item = list_entry(pos, systray_item_t, head);
		if (!item->info.flags)
			continue;
		draw_padding(dc, opts->tray.iconsize);
		x = dc_get_x(dc);
		if (item->x != x) {
			values.x = x;
			values.y = (BAR_HEIGHT - opts->tray.iconsize) / 2;
			values.width = opts->tray.iconsize;
			values.height = opts->tray.iconsize;
			if (!xcb_request_check(bar.xcb, xcb_configure_window_aux(bar.xcb, item->win, mask, &values)))
				item->x = x;
		}
		draw_padding(dc, celwidth);
	}
	draw_padding(dc, celwidth);
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
 * bspwm_handle() - bspwm event handling function.
 * @fd: a file descriptor for bspwm socket.
 *
 * This function expects call after bspwm_connect().
 * Read and parse bspwm report from fd.
 *
 * Return: PollResult
 *
 * success and not need more action - PR_NOOP
 * success and need rerendering     - PR_UPDATE
 * failed to read from fd           - PR_FAILED
 */
poll_result_t
bspwm_handle(int fd)
{
	size_t len;
	xcb_window_t win;
	xcb_change_window_attributes_value_list_t attrs;
	uint32_t mask = XCB_CW_EVENT_MASK;

	attrs.event_mask = XCB_EVENT_MASK_PROPERTY_CHANGE;

	if ((len = recv(fd, buf, sizeof(buf) - 1, 0)) > 0) {
		buf[len] = '\0';
		if (buf[0] == '\x07') {
			err("bspwm: %s", buf + 1);
			return PR_FAILED;
		}
		bspwm_parse(buf);
		if ((win = get_active_window(0)))
			xcb_change_window_attributes_aux(bar.xcb, win, mask, &attrs);
		return PR_UPDATE;
	}
	return PR_NOOP;
}

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
			/* handle evnent */
			for (int j = 0; j < dc->nlabel; j++) {
				if (!dc->labels[j].option->any.handler)
					continue;
				if (IS_LABEL_EVENT(dc->labels[j], button)) {
					dc->labels[j].option->any.handler(event);
					res = PR_UPDATE;
					break;
				}
			}
			break;
		case XCB_PROPERTY_NOTIFY:
			prop = (xcb_property_notify_event_t *)event;
			if (prop->atom == xembed_info) {
				systray_handle(tray, event);
			} else if (is_change_active_window_event(prop) || prop->atom == ewmh._NET_WM_NAME) {
				windowtitle_update(bar.xcb, 0);
				res = PR_UPDATE;
			}
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
	while ((nfd = epoll_wait(pfd, events, MAX_EVENTS, -1)) != -1) {
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

	if (bspwmbar_init(xcb, scr)) {
		err("bspwmbar_init(): Failed to init bspwmbar\n");
		goto CLEANUP;
	}

	/* subscribe bspwm report */
	if (bspwm_send(SUBSCRIBE_REPORT, LENGTH(SUBSCRIBE_REPORT)) == -1) {
		err("bspwm_send(): Failed to send command to bspwm\n");
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

	/* polling bspwm report */
	bfd.fd = bar.fd;
	bfd.handler = bspwm_handle;
	poll_add(&bfd);

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
