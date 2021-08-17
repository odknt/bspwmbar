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
#include "module.h"
#include "draw.h"
#include "font.h"
#include "../config.h"

#if !defined(VERSION)
# define VERSION "v0.0.0-dev"
#endif

/* epoll max events */
#define MAX_EVENTS 10
/* check event and returns true if target is the label */
#define IS_LABEL_EVENT(l,e) (((l).x < (e)->event_x) && ((e)->event_x < (l).x + (l).width))

struct bb_label {
	int x;
	int width;
	union bb_module *module;
};

typedef void (* bb_loop_func)();

struct bb_label left_labels[LENGTH(left_modules)];
struct bb_label right_labels[LENGTH(right_modules)];

typedef int desktop_state_t;

struct bb_desktop {
	char name[NAME_MAXSZ];
	enum {
		STATE_FREE     = 0,
		STATE_FOCUSED  = 1 << 1,
		STATE_OCCUPIED = 1 << 2,
		STATE_URGENT   = 1 << 3,

		STATE_ACTIVE = 1 << 8
	} state;
};

struct bb_monitor {
	char name[NAME_MAXSZ];
	struct draw_context *dc;
	struct bb_desktop *desktops;
	int ndesktop; /* num of desktops */
	int cdesktop; /* cap of desktops */
	uint8_t is_active;
};

struct bb_bspwmbar {
	/* xcb resources */
	xcb_connection_t *xcb;
	xcb_screen_t *scr;
	xcb_visualid_t visual;
	xcb_colormap_t cmap;

	/* font */
	struct bb_font_manager *fm;

	/* draw context */
	struct bb_draw_context **dcs;
	int ndc;

	/* base color */
	struct bb_color *fg;
	struct bb_color *bg;
};

/* temporary buffer */
char buf[1024];

static struct bb_bspwmbar bar;
static struct bb_systray *tray;
static struct bb_poll_manager *pm;
static struct bb_poll_option xfd = { 0 };

static struct bb_color **cols;
static int ncol, colcap;
static int celwidth = 0;

/* EWMH */
static xcb_ewmh_connection_t ewmh;
static xcb_atom_t xembed_info;

/* Window title cache */
static char *wintitle = NULL;

/* private functions */
static struct bb_window *bb_create_window(xcb_connection_t *, xcb_screen_t *, int16_t, int16_t, uint16_t, uint16_t, struct bb_color *);
static void bb_destroy_window(xcb_connection_t *, struct bb_window *);
static struct bb_draw_context *dc_init(xcb_connection_t *, xcb_screen_t *, struct bb_font_manager *, int, int, int, int);
static enum bb_poll_result xcb_event_notify(xcb_generic_event_t *);
static void color_load_hex(const char *, struct bb_color *);
static bool color_load_name(const char *, struct bb_color *);
static void signal_handler(int);
static xcb_window_t get_active_window(uint8_t scrno);
static char *get_window_title(xcb_connection_t *, xcb_window_t);
static void render_labels(struct bb_draw_context *, size_t, struct bb_label *);
static void windowtitle_update(xcb_connection_t *, uint8_t);
static void calculate_systray_item_positions(struct bb_label *, union bb_module *);
static void calculate_label_positions(struct bb_draw_context *, size_t, struct bb_label *, int);
static void render();
static bool bb_init(xcb_connection_t *, xcb_screen_t *);
static void bb_destroy();
static void poll_init();
static void poll_loop(void (*)());
static enum bb_poll_result xev_handle();
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
color_load_hex(const char *colstr, struct bb_color *color)
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
color_load_name(const char *colstr, struct bb_color *color)
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

struct bb_color *
bb_color_load(const char *colstr)
{
	int i;
	/* find color caches */
	for (i = 0; i < ncol; i++)
		if (!strncmp(cols[i]->name, colstr, strlen(cols[i]->name)))
			return cols[i];

	if (ncol >= colcap) {
		colcap += 5;
		cols = realloc(cols, sizeof(struct bb_color *) * colcap);
	}
	cols[ncol] = calloc(1, sizeof(struct bb_color));

	if (colstr[0] == '#' && strlen(colstr) > 6)
		color_load_hex(colstr, cols[ncol]);
	else
		color_load_name(colstr, cols[ncol]);

	if (!cols[ncol])
		die("bb_color_load(): failed to load color: %s", colstr);

	return cols[ncol++];
}

struct bb_window *
bb_create_window(xcb_connection_t *xcb, xcb_screen_t *scr, int16_t x, int16_t y, uint16_t width, uint16_t height, struct bb_color *bg)
{
	xcb_configure_window_value_list_t winconf = { 0 };
	xcb_void_cookie_t cookie;
	struct bb_window *win;
	xcb_window_t xw;

	const uint32_t attrs[] = { bg->pixel, XCB_EVENT_MASK_NO_EVENT };

	xw = xcb_generate_id(xcb);
	cookie = xcb_create_window(xcb, XCB_COPY_FROM_PARENT, xw, scr->root, x, y,
	                           width, height, 0, XCB_COPY_FROM_PARENT,
	                           XCB_COPY_FROM_PARENT,
	                           XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, attrs);
	if (xcb_request_check(xcb, cookie))
		return NULL;

	win = calloc(1, sizeof(struct bb_window));
	win->xw = xw;
	win->x = x;
	win->y = y;
	win->width = width;
	win->height = height;

	/* set window state */
	xcb_atom_t states[] = { ewmh._NET_WM_STATE_STICKY, ewmh._NET_WM_STATE_ABOVE };
	xcb_ewmh_set_wm_state(&ewmh, xw, LENGTH(states), states);

	/* set window type */
	xcb_atom_t window_types[] = { ewmh._NET_WM_WINDOW_TYPE_DOCK };
	xcb_ewmh_set_wm_window_type(&ewmh, xw, LENGTH(window_types), window_types);

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
	xcb_ewmh_set_wm_strut(&ewmh, xw, 0, 0, y + height, 0);
	xcb_ewmh_set_wm_strut_partial(&ewmh, xw, strut_partial);

	/* set class hint */
	xcb_change_property(xcb, XCB_PROP_MODE_REPLACE, xw, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, 8, "bspwmbar");
	xcb_change_property(xcb, XCB_PROP_MODE_REPLACE, xw, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, 17, "bspwmbar\0bspwmbar");

	/* send window rendering request */
	winconf.stack_mode = XCB_STACK_MODE_BELOW;
	xcb_configure_window_aux(xcb, xw, XCB_CONFIG_WINDOW_STACK_MODE, &winconf);

	return win;
}

void
bb_destroy_window(xcb_connection_t *xcb, struct bb_window *win)
{
	xcb_destroy_window(xcb, win->xw);
	free(win);
}

struct bb_draw_context *
dc_init(xcb_connection_t *xcb, xcb_screen_t *scr, struct bb_font_manager *fm, int x, int y, int width, int height)
{
	struct bb_window *win = bb_create_window(xcb, scr, x, y, width, height, bb_color_load(BGCOLOR));
	if (!win)
		return NULL;
	xcb_map_window(xcb, win->xw);

	struct bb_draw_context *dc = bb_draw_context_new(xcb, scr, win, fm);
	dc->fgcolor = bar.fg;
	dc->bgcolor = bar.bg;
	return dc;
}

xcb_window_t
get_active_window(uint8_t scrno)
{
	xcb_window_t win;
	if (xcb_ewmh_get_active_window_reply(&ewmh, xcb_ewmh_get_active_window(&ewmh, scrno), &win, NULL))
		return win;
	return 0;
}

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
windowtitle(struct bb_draw_context *dc, union bb_module *opts)
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

	bb_draw_text(dc, buf);
}

/**
 * text() - render the specified text.
 * @dc: draw context.
 * @opts: module options.
 */
void
text(struct bb_draw_context *dc, union bb_module *opts)
{
	struct bb_color *fg = dc->fgcolor;
	if (opts->text.fg)
		fg = bb_color_load(opts->text.fg);
	bb_draw_color_text(dc, fg, opts->text.label);
}

void
render_labels(struct bb_draw_context *dc, size_t nlabel, struct bb_label *labels)
{
	size_t i;
	int oldx = 0;

	for (i = 0; i < nlabel; i++) {
		if (!labels[i].module->any.func)
			continue;

		oldx = dc->x;

		bb_draw_padding(dc, celwidth);
		labels[i].module->any.func(dc, labels[i].module);
		bb_draw_padding(dc, celwidth);
		labels[i].width = dc->x - oldx;
		labels[i].x = oldx;
		if (labels[i].width == celwidth * 2) {
			dc->x = oldx;
			labels[i].width = 0;
			continue;
		}
		dc->width += labels[i].width;
	}
}

void
calculate_systray_item_positions(struct bb_label *label, union bb_module *opts)
{
	xcb_configure_window_value_list_t values;
	uint32_t mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT;
	struct bb_systray_item *item;
	list_head *pos;
	int x;

	if (!tray->icon_size)
		tray->icon_size = opts->tray.iconsize;

	if (list_empty(&tray->items))
		return;

	/* fix the position for padding */
	x = label->x + celwidth;
	list_for_each(&tray->items, pos) {
		item = list_entry(pos, struct bb_systray_item, head);
		if (!item->info.flags)
			continue;
		if (item->x != x) {
			values.x = x;
			values.y = (BAR_HEIGHT - opts->tray.iconsize) / 2;
			values.width = opts->tray.iconsize;
			values.height = opts->tray.iconsize;
			if (!xcb_request_check(bar.xcb, xcb_configure_window_aux(bar.xcb, item->win, mask, &values)))
				item->x = x;
			if (!item->mapped)
				item->mapped = xcb_request_check(bar.xcb, xcb_map_window(bar.xcb, item->win)) == NULL;
		}
		x += opts->tray.iconsize + celwidth;
	}
}

void
calculate_label_positions(struct bb_draw_context *dc, size_t nlabel, struct bb_label *labels, int offset)
{
	size_t i;
	for (i = 0; i < nlabel; i++) {
		labels[i].x += offset;

		if (tray->win == dc->win->xw && labels[i].module->any.func == systray)
			calculate_systray_item_positions(&labels[i], labels[i].module);
	}
}

/**
 * render() - rendering all modules.
 */
void
render()
{
	struct bb_draw_context *dc;
	struct bb_window *win;
	xcb_rectangle_t rect = { 0 };
	int i;

	for (i = 0; i < bar.ndc; i++) {
		dc = bar.dcs[i];
		win = dc->win;
		rect.width = win->width;
		rect.height = win->height;

		bb_draw_clear(dc, bar.bg);
		xcb_poly_fill_rectangle(bar.xcb, dc->buf->pixmap, dc->gc, 1, &rect);

		/* render left modules */
		xcb_poly_fill_rectangle(bar.xcb, dc->tmp->pixmap, dc->gc, 1, &rect);
		dc->x = dc->width = 0;
		render_labels(dc, LENGTH(left_modules), left_labels);
		xcb_copy_area(bar.xcb, dc->tmp->pixmap, dc->buf->pixmap, dc->gc, 0, 0, celwidth, 0, dc->width, win->height);
		calculate_label_positions(dc, LENGTH(left_modules), left_labels, celwidth);

		/* render right modules */
		xcb_poly_fill_rectangle(bar.xcb, dc->tmp->pixmap, dc->gc, 1, &rect);
		dc->x = dc->width = 0;
		render_labels(dc, LENGTH(right_modules), right_labels);
		xcb_copy_area(bar.xcb, dc->tmp->pixmap, dc->buf->pixmap, dc->gc, 0, 0, win->width - dc->width - celwidth, 0, dc->width, win->height);
		calculate_label_positions(dc, LENGTH(right_modules), right_labels, win->width - dc->width - celwidth);

		/* copy pixmap to window */
		xcb_copy_area(bar.xcb, dc->buf->pixmap, win->xw, dc->gc, 0, 0, 0, 0, win->width, win->height);
	}
	xcb_flush(bar.xcb);
}

/**
 * bb_init() - initialize bspwmbar.
 * @dpy: display pointer.
 * @scr: screen number.
 *
 * Return:
 * 0 - success
 * 1 - failure
 */
bool
bb_init(xcb_connection_t *xcb, xcb_screen_t *scr)
{
	xcb_randr_get_monitors_reply_t *mon_reply;
	xcb_randr_get_screen_resources_reply_t *screen_reply;
	xcb_randr_get_output_info_reply_t *info_reply;
	xcb_randr_output_t *outputs;
	xcb_randr_get_crtc_info_reply_t *crtc_reply;
	size_t i, nmon = 0;
	double dpi = (((double)scr->height_in_pixels * 25.4) / (double)scr->height_in_millimeters);

	/* initialize */
	bar.xcb = xcb;
	bar.scr = scr;
	bar.cmap = scr->default_colormap;
	bar.fg = bb_color_load(FGCOLOR);
	bar.bg = bb_color_load(BGCOLOR);
	bar.fm = bb_font_manager_new(dpi);

	/* get monitors */
	mon_reply = xcb_randr_get_monitors_reply(xcb, xcb_randr_get_monitors(xcb, scr->root, 1), NULL);
	bar.dcs = calloc(mon_reply->nMonitors, sizeof(struct bb_draw_context *));
	bar.ndc = mon_reply->nMonitors;

	/* create window per monitor */
	screen_reply = xcb_randr_get_screen_resources_reply(xcb, xcb_randr_get_screen_resources(xcb, scr->root), NULL);
	outputs = xcb_randr_get_screen_resources_outputs(screen_reply);
	for (i = 0; i < screen_reply->num_outputs; i++) {
		info_reply = xcb_randr_get_output_info_reply(xcb, xcb_randr_get_output_info(xcb, outputs[i], XCB_TIME_CURRENT_TIME), NULL);
		if (info_reply->crtc != XCB_NONE) {
			crtc_reply = xcb_randr_get_crtc_info_reply(xcb, xcb_randr_get_crtc_info(xcb, info_reply->crtc, XCB_TIME_CURRENT_TIME), NULL);
			if ((bar.dcs[nmon] = dc_init(xcb, scr, bar.fm, crtc_reply->x, crtc_reply->y, crtc_reply->width, BAR_HEIGHT)))
				bar.dcs[nmon++]->monitor_name = strndup((const char *)xcb_randr_get_output_info_name(info_reply), xcb_randr_get_output_info_name_length(info_reply));
			free(crtc_reply);
		}
		free(info_reply);
	}
	free(screen_reply);
	free(mon_reply);

	if (!nmon)
		return false;

	/* load_fonts */
	if (!bb_font_manager_load_fonts(bar.fm, fontname))
		return false;
	celwidth = bar.fm->celwidth;

	for (i = 0; i < LENGTH(left_modules); i++)
		left_labels[i].module = &left_modules[i];
	for (i = 0; i < LENGTH(right_modules); i++)
		right_labels[i].module = &right_modules[i];

	xcb_flush(xcb);
	return true;
}

/**
 * bb_destroy() - destroy all resources of bspwmbar.
 */
void
bb_destroy()
{
	int i;

	/* font resources */
	bb_font_manager_destroy(bar.fm);

	/* rendering resources */
	for (i = 0; i < bar.ndc; i++) {
		bb_destroy_window(bar.xcb, bar.dcs[i]->win);
		bb_draw_context_destroy(bar.dcs[i]);
	}
	free(bar.dcs);
}

/**
 * systray() - render systray.
 * @dc: draw context.
 * @opts: dummy.
 */
void
systray(struct bb_draw_context *dc, union bb_module *opts)
{
	list_head *pos;
	(void)opts;

	if (!tray->icon_size)
		tray->icon_size = opts->tray.iconsize;

	if (list_empty(&tray->items))
		return;

	if (tray->win != dc->win->xw)
		return;

	/* render spaces for iconsize */
	list_for_each(&tray->items, pos) {
		struct bb_systray_item *item = list_entry(pos, struct bb_systray_item, head);
		if (!item->info.flags)
			continue;
		bb_draw_padding(dc, opts->tray.iconsize);
		if (&tray->items != pos->next)
			bb_draw_padding(dc, celwidth);
	}
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

enum bb_poll_result
xcb_event_notify(xcb_generic_event_t *event)
{
	size_t i;
	xcb_button_press_event_t *button = (xcb_button_press_event_t *)event;
	for (i = 0; i < LENGTH(left_modules); i++) {
		if (!left_labels[i].module->any.handler)
			continue;
		if (IS_LABEL_EVENT(left_labels[i], button)) {
			left_labels[i].module->any.handler(event);
			return PR_UPDATE;
		}
	}
	for (i = 0; i < LENGTH(right_modules); i++) {
		if (!right_labels[i].module->any.handler)
			continue;
		if (IS_LABEL_EVENT(right_labels[i], button)) {
			right_labels[i].module->any.handler(event);
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
enum bb_poll_result
xev_handle()
{
	xcb_generic_event_t *event;
	xcb_button_press_event_t *button;
	xcb_property_notify_event_t *prop;
	xcb_window_t win;
	enum bb_poll_result res = PR_NOOP;
	struct bb_draw_context *dc;

	xcb_change_window_attributes_value_list_t attrs;
	uint32_t mask = XCB_CW_EVENT_MASK;
	attrs.event_mask = XCB_EVENT_MASK_PROPERTY_CHANGE;

	/* for X11 events */
	while ((event = xcb_poll_for_event(bar.xcb))) {
		switch (event->response_type & ~0x80) {
		case XCB_SELECTION_CLEAR:
			bb_systray_handle(tray, event);
			break;
		case XCB_EXPOSE:
			res = PR_UPDATE;
			break;
		case XCB_BUTTON_PRESS:
			dc = NULL;
			button = (xcb_button_press_event_t *)event;
			for (int j = 0; j < bar.ndc; j++)
				if (bar.dcs[j]->win->xw == button->event)
					dc = bar.dcs[j];
			if (!dc)
				break;
			/* notify evnent to modules */
			xcb_event_notify(event);
			break;
		case XCB_PROPERTY_NOTIFY:
			prop = (xcb_property_notify_event_t *)event;
			if (prop->atom == xembed_info) {
				bb_systray_handle(tray, event);
				res = PR_UPDATE;
			} else if (is_change_active_window_event(prop) || prop->atom == ewmh._NET_WM_NAME) {
				windowtitle_update(bar.xcb, 0);
				res = PR_UPDATE;
			}
			if (is_change_active_window_event(prop) && (win = get_active_window(0)))
				xcb_change_window_attributes_aux(bar.xcb, win, mask, &attrs);
			break;
		case XCB_CLIENT_MESSAGE:
			bb_systray_handle(tray, event);
			res = PR_UPDATE;
			break;
		case XCB_UNMAP_NOTIFY:
			win = ((xcb_unmap_notify_event_t *)event)->event;
			bb_systray_remove_item(tray, win);
			res = PR_UPDATE;
			break;
		case XCB_DESTROY_NOTIFY:
			win = ((xcb_destroy_notify_event_t *)event)->event;
			bb_systray_remove_item(tray, win);
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
poll_loop(bb_loop_func handler)
{
	int i, nfd;
	struct bb_poll_option *popt;

	/* polling X11 event for modules */
	xfd.fd = xcb_get_file_descriptor(bar.xcb);
	xfd.handler = xev_handle;
	bb_poll_manager_add(pm, &xfd);

	/* polling fd */
	nfd = 0;
	do {
		for (i = 0; i < nfd; i++) {
			popt = bb_poll_event_data(pm->events[i]);
			switch ((int)popt->handler(popt->fd)) {
			case PR_UPDATE:
				bb_poll_manager_timer_reset(pm);
				windowtitle_update(bar.xcb, 0);
				handler();
				break;
			case PR_REINIT:
				bb_poll_manager_del(pm, popt);
				popt->fd = popt->init();
				bb_poll_manager_add(pm, popt);
				break;
			case PR_FAILED:
				bb_poll_manager_stop(pm);
				break;
			}
		}
	} while ((nfd = bb_poll_manager_wait_events(pm)) != -1);
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
		bb_poll_manager_stop(pm);
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

	/* deinit modules */
	bb_poll_manager_destroy(pm);

	if (tray)
		bb_systray_destroy(tray);
	for (i = 0; i < ncol; i++) {
		free(cols[i]->name);
		free(cols[i]);
	}

	free(cols);
	bb_destroy();
	xcb_ewmh_connection_wipe(&ewmh);
	xcb_disconnect(xcb);
	FcFini();
}

void
poll_init()
{
	pm = bb_poll_manager_new();
}

void
poll_add(struct bb_poll_option *popt)
{
	bb_poll_manager_add(pm, popt);
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

	/* get active widnow title */
	windowtitle_update(xcb, 0);

	if (!bb_init(xcb, scr)) {
		err("bb_init(): Failed to init bspwmbar\n");
		goto CLEANUP;
	}

	/* tray initialize */
	if (!(tray = bb_systray_new(xcb, scr, bar.dcs[0]->win->xw))) {
		err("bb_systray_new(): Selection already owned by other window\n");
		goto CLEANUP;
	}

	/* wait PropertyNotify events of root window */
	attrs.event_mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
	xcb_change_window_attributes_aux(bar.xcb, scr->root, mask, &attrs);

	/* polling X11 event for modules */
	attrs.event_mask = XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_EXPOSURE;
	for (int i = 0; i < bar.ndc; i++)
		xcb_change_window_attributes_aux(bar.xcb, bar.dcs[i]->win->xw, mask, &attrs);

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
