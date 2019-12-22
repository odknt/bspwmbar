/* See LICENSE file for copyright and license details. */

#ifndef SYSTRAY_H_
#define SYSTRAY_H_

#include <xcb/xcb.h>

#include "util.h"

typedef struct {
	unsigned long version;
	unsigned long flags;
} xembed_info_t;

typedef struct _systray_item_t {
	xcb_window_t win;
	xembed_info_t info;
	int x;

	list_head head;
} systray_item_t;

typedef struct _systray_t systray_t;

systray_t *systray_new(xcb_connection_t *, xcb_screen_t *, xcb_window_t);
int systray_handle(systray_t *, xcb_generic_event_t *);
void systray_destroy(systray_t *);
void systray_remove_item(systray_t *, xcb_window_t);
xcb_window_t systray_get_window(systray_t *);
xcb_connection_t *systray_get_connection(systray_t *);
list_head *systray_get_items(systray_t *);
int systray_icon_size(systray_t *);
void systray_set_icon_size(systray_t *, int);

#endif /* SYSTRAY_H_ */
