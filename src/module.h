/* See LICENSE file for copyright and license details. */

#ifndef BSPWMBAR_MODULE_H
#define BSPWMBAR_MODULE_H

#include "draw.h"
#include "poll.h"
#include "bspwm.h"

union bb_module;

typedef void (* bb_module_handler_func)(struct bb_draw_context *, union bb_module *);
typedef void (* bb_event_handler_func)(xcb_generic_event_t *);

/* Module */
#define MODULE_BASE \
	bb_module_handler_func func; \
	bb_event_handler_func handler; \
	char *prefix; \
	char *suffix

struct bb_module_volume {
	MODULE_BASE;

	char *muted;
	char *unmuted;
};

struct bb_module_desktop {
	MODULE_BASE;

	char *focused;
	char *unfocused;
	char *fg;
	char *fg_free;
};

struct bb_module_text {
	MODULE_BASE;

	char *label;
	char *fg;
};

struct bb_module_graph {
	MODULE_BASE;

	char *cols[4];
};

struct bb_module_title {
	MODULE_BASE;

	unsigned int maxlen;
	char *ellipsis;
};

struct bb_module_datetime {
	MODULE_BASE;

	char *format;
};

struct bb_module_thermal {
	MODULE_BASE;

	char *sensor;
};

struct bb_module_filesystem {
	MODULE_BASE;

	char *mountpoint;
};

struct bb_module_any {
	MODULE_BASE;
};

struct bb_module_systray {
	MODULE_BASE;

	int iconsize;
};

struct bb_module_battery {
	MODULE_BASE;

	char *prefix_1;
	char *prefix_2;
	char *prefix_3;
	char *prefix_4;
	char *path;
};

struct bb_module_backlight {
	MODULE_BASE;
};

union bb_module {
	struct bb_module_any any;
	struct bb_module_systray tray;
	struct bb_module_datetime date;
	struct bb_module_filesystem fs;
	struct bb_module_volume vol;
	struct bb_module_desktop desk;
	struct bb_module_text text;
	struct bb_module_graph cpu;
	struct bb_module_graph mem;
	struct bb_module_title title;
	struct bb_module_thermal thermal;
	struct bb_module_battery battery;
	struct bb_module_backlight backlight;
};

/* modules */
void text(struct bb_draw_context *, union bb_module *);
void desktops(struct bb_draw_context *, union bb_module *);
void windowtitle(struct bb_draw_context *, union bb_module *);
void filesystem(struct bb_draw_context *, union bb_module *);
void thermal(struct bb_draw_context *, union bb_module *);
void volume(struct bb_draw_context *, union bb_module *);
void datetime(struct bb_draw_context *, union bb_module *);
void cpugraph(struct bb_draw_context *, union bb_module *);
void memgraph(struct bb_draw_context *, union bb_module *);
void systray(struct bb_draw_context *, union bb_module *);
void battery(struct bb_draw_context *, union bb_module *);
void backlight(struct bb_draw_context *, union bb_module *);

/* handler */
void volume_ev(xcb_generic_event_t *);
void backlight_ev(xcb_generic_event_t *);

#endif /* BSPWMBAR_MODULE_H */
