/* See LICENSE file for copyright and license details. */

#ifndef BSPWMBAR_H_
#define BSPWMBAR_H_

#include <stdint.h>
#include <xcb/xcb_event.h>

#include "util.h"
#include "bspwm.h"

typedef enum {
	PR_NOOP   =  0,
	PR_UPDATE,
	PR_REINIT,
	PR_FAILED
} poll_result_t;

typedef struct _color_t color_t;

typedef struct {
	char *prefix;
	char *suffix;

	double val;
	color_t *fg, *bg;
} graph_item_t;

typedef union _module_t module_t;
typedef module_t module_option_t;

/* Draw context */
typedef struct _draw_context_t draw_context_t;
typedef void (* module_handler_t)(draw_context_t *, module_option_t *);
typedef void (* event_handler_t)(xcb_generic_event_t *);

/* Poll */
typedef int (* poll_init_handler_t)();
typedef int (* poll_deinit_handler_t)();
typedef poll_result_t (* poll_update_handler_t)();
typedef struct {
	int fd;
	poll_init_handler_t init; /* initialize and return fd */
	poll_deinit_handler_t deinit; /* close fd and cleanup resources */
	poll_update_handler_t handler; /* event handler for fd */

	list_head head;
} poll_fd_t;

void poll_add(poll_fd_t *);
void poll_del(poll_fd_t *);

/* Module */
#define MODULE_BASE \
	module_handler_t func; \
	event_handler_t handler; \
	char *prefix; \
	char *suffix

typedef struct {
	MODULE_BASE;

	char *muted;
	char *unmuted;
} module_volume_t;

typedef struct {
	MODULE_BASE;

	char *focused;
	char *unfocused;
	char *fg;
	char *fg_free;
} module_desktop_t;

typedef struct {
	MODULE_BASE;

	char *label;
	char *fg;
} module_text_t;

typedef struct {
	MODULE_BASE;

	char *cols[4];
} module_graph_t;

typedef struct {
	MODULE_BASE;

	unsigned int maxlen;
	char *ellipsis;
} module_title_t;

typedef struct {
	MODULE_BASE;

	char *format;
} module_datetime_t;

typedef struct {
	MODULE_BASE;

	char *sensor;
} module_thermal_t;

typedef struct {
	MODULE_BASE;

	char *mountpoint;
} module_filesystem_t;

typedef struct {
	MODULE_BASE;
} module_any_t;

typedef struct {
	MODULE_BASE;

	int iconsize;
} module_systray_t;

typedef struct {
	MODULE_BASE;

	char *prefix_1;
	char *prefix_2;
	char *prefix_3;
	char *prefix_4;
	char *path;
} module_battery_t;

typedef struct {
	MODULE_BASE;
} module_backlight_t;

union _module_t {
	module_any_t any;
	module_systray_t tray;
	module_datetime_t date;
	module_filesystem_t fs;
	module_volume_t vol;
	module_desktop_t desk;
	module_text_t text;
	module_graph_t cpu;
	module_graph_t mem;
	module_title_t title;
	module_thermal_t thermal;
	module_battery_t battery;
	module_backlight_t backlight;
};

xcb_connection_t *xcb_connection();

color_t *color_load(const char *);
color_t *color_default_fg();
color_t *color_default_bg();

const char *draw_context_monitor_name(draw_context_t *);

void draw_text(draw_context_t *, const char *);
void draw_color_text(draw_context_t *, color_t *, const char *);
void draw_bargraph(draw_context_t *, const char *, graph_item_t *, int);
void draw_padding_em(draw_context_t *, double);

/* handler */
void volume_ev(xcb_generic_event_t *);
void backlight_ev(xcb_generic_event_t *);

/* modules */
void text(draw_context_t *, module_option_t *);
void desktops(draw_context_t *, module_option_t *);
void windowtitle(draw_context_t *, module_option_t *);
void filesystem(draw_context_t *, module_option_t *);
void thermal(draw_context_t *, module_option_t *);
void volume(draw_context_t *, module_option_t *);
void datetime(draw_context_t *, module_option_t *);
void cpugraph(draw_context_t *, module_option_t *);
void memgraph(draw_context_t *, module_option_t *);
void systray(draw_context_t *, module_option_t *);
void battery(draw_context_t *, module_option_t *);
void backlight(draw_context_t *, module_option_t *);

/* temporary buffer */
extern char buf[1024];

#endif
