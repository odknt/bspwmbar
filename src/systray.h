/* See LICENSE file for copyright and license details. */

#ifndef BSPWMBAR_SYSTRAY_H
#define BSPWMBAR_SYSTRAY_H

#include <stdbool.h>
#include <xcb/xcb.h>

#include "util.h"

typedef struct {
	unsigned long version;
	unsigned long flags;
} xembed_info_t;

struct bb_systray_item {
	xcb_window_t win;
	xembed_info_t info;
	int x;
	bool mapped;

	list_head head;
};

struct bb_systray {
	xcb_connection_t *xcb;
	xcb_screen_t *scr;
	xcb_window_t win;
	int icon_size;
	list_head items;
};

struct bb_systray *bb_systray_new(xcb_connection_t *, xcb_screen_t *, xcb_window_t);
int bb_systray_handle(struct bb_systray *, xcb_generic_event_t *);
void bb_systray_destroy(struct bb_systray *);
void bb_systray_remove_item(struct bb_systray *, xcb_window_t);

#endif /* BSPWMBAR_SYSTRAY_H */
