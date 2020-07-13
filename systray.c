#if defined(__linux)
# include <alloca.h>
#elif defined(__OpenBSD__) || defined(__FreeBSD__)
# define __BSD_VISIBLE 1
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <xcb/xcb.h>
#include <xcb/xcb_util.h>

#include "bspwmbar.h"
#include "systray.h"

#define ATOM_SYSTRAY "_NET_SYSTEM_TRAY_S0"

#define XEMBED_EMBEDDED_NOTIFY        0
#define XEMBED_WINDOW_ACTIVATE        1
#define XEMBED_WINDOW_DEACTIVATE      2
#define XEMBED_REQUEST_FOCUS          3
#define XEMBED_FOCUS_IN               4
#define XEMBED_FOCUS_OUT              5
#define XEMBED_FOCUS_NEXT             6
#define XEMBED_FOCUS_PREV             7
/* 8-9 were used for XEMBED_GRAB_KEY/XEMBED_UNGRAB_KEY */
#define XEMBED_MODALITY_ON            10
#define XEMBED_MODALITY_OFF           11
#define XEMBED_REGISTER_ACCELERATOR   12
#define XEMBED_UNREGISTER_ACCELERATOR 13
#define XEMBED_ACTIVATE_ACCELERATOR   14

#define XEMBED_MAPPED (1 << 0)

enum {
	SYSTRAY_TIME,
	SYSTRAY_OPCODE,
	SYSTRAY_DATA1,
	SYSTRAY_DATA2,
	SYSTRAY_DATA3,
};

enum {
	SYSTRAY_REQUEST_DOCK,
	SYSTRAY_BEGIN_MESSAGE,
	SYSTRAY_CANCEL_MESSAGE,
};

struct _systray_t {
	xcb_connection_t *xcb;
	xcb_screen_t *scr;
	xcb_window_t win;
	int icon_size;
	list_head items;
};

/* functions */
static bool xembed_send(xcb_connection_t *, xcb_window_t, long, long, long, long);
static bool xembed_embedded_notify(systray_t *, xcb_window_t, long);
static int xembed_unembed_window(systray_t *, xcb_window_t);
static bool xembed_getinfo(systray_t *, xcb_window_t, xembed_info_t *);
static xcb_atom_t get_systray_atom(xcb_connection_t *);
static bool systray_set_selection_owner(systray_t *, xcb_atom_t);
static bool systray_get_ownership(systray_t *);
static systray_item_t *systray_append_item(systray_t *, xcb_window_t);
static systray_item_t *systray_find_item(systray_t *, xcb_window_t);

xcb_atom_t
get_systray_atom(xcb_connection_t *xcb)
{
	static xcb_atom_t systray_atom = 0;
	if (systray_atom)
		return systray_atom;
	systray_atom = xcb_atom_get(xcb, ATOM_SYSTRAY, false);
	return systray_atom;
}

bool
systray_set_selection_owner(systray_t *tray, xcb_atom_t atom)
{
	return xcb_request_check(tray->xcb, xcb_set_selection_owner(tray->xcb, tray->win, atom, XCB_TIME_CURRENT_TIME)) == NULL;
}

xcb_window_t
get_selection_owner(xcb_connection_t *xcb, xcb_atom_t atom)
{
	xcb_window_t win;
	xcb_get_selection_owner_reply_t *or;
	or = xcb_get_selection_owner_reply(xcb, xcb_get_selection_owner(xcb, atom), NULL);
	win = or->owner;
	free(or);
	return win;
}

bool
systray_get_ownership(systray_t *tray)
{
	xcb_atom_t atom = get_systray_atom(tray->xcb);
	if (get_selection_owner(tray->xcb, atom))
		return false;

	return systray_set_selection_owner(tray, atom);
}

/**
 * systray_new() - Initialize systray_t *object.
 * @xcb: A display pointer of win.
 * @win: A window for system tray.
 *
 * Return: A new system tray object.
 */
systray_t *
systray_new(xcb_connection_t *xcb, xcb_screen_t *scr, xcb_window_t win)
{
	xcb_client_message_event_t ev = { 0 };
	systray_t *tray = (systray_t *)calloc(1, sizeof(struct _systray_t));
	list_head_init(&tray->items);
	tray->xcb = xcb;
	tray->scr = scr;
	tray->win = win;

	if (!systray_get_ownership(tray)) {
		free(tray);
		return NULL;
	}

	ev.response_type = XCB_CLIENT_MESSAGE;
	ev.type = xcb_atom_get(tray->xcb, "MANAGER", false);
	ev.format = 32;
	ev.data.data32[0] = XCB_TIME_CURRENT_TIME;
	ev.data.data32[1] = get_systray_atom(tray->xcb);
	ev.data.data32[2] = tray->win;
	ev.data.data32[3] = 0;
	ev.data.data32[4] = 0;
	xcb_send_event(xcb, 0, tray->win, XCB_EVENT_MASK_STRUCTURE_NOTIFY, (const char *)&ev);

	return tray;
}

bool
xembed_send(xcb_connection_t *xcb, xcb_window_t win, long message, long d1, long d2, long d3)
{
	xcb_client_message_event_t ev = { 0 };

	ev.response_type = XCB_CLIENT_MESSAGE;
	ev.window = win;
	ev.type = xcb_atom_get(xcb, "_XEMBED", false);
	ev.format = 32;
	ev.data.data32[0] = XCB_TIME_CURRENT_TIME;
	ev.data.data32[1] = message;
	ev.data.data32[2] = d1;
	ev.data.data32[3] = d2;
	ev.data.data32[4] = d3;

	if (xcb_request_check(xcb, xcb_send_event(xcb, 0, win, XCB_EVENT_MASK_STRUCTURE_NOTIFY, (const char *)&ev)))
		return false;
	xcb_flush(xcb);

	return true;
}

bool
xembed_embedded_notify(systray_t *tray, xcb_window_t win, long version)
{
	return xembed_send(tray->xcb, win, XEMBED_EMBEDDED_NOTIFY, 0, tray->win, version);
}

int
xembed_unembed_window(systray_t *tray, xcb_window_t child)
{
	xcb_unmap_window(tray->xcb, child);
	xcb_reparent_window(tray->xcb, child, tray->scr->root, 0, 0);

	return 0;
}

bool
xembed_getinfo(systray_t *tray, xcb_window_t win, xembed_info_t *info)
{
	xcb_atom_t infoatom;
	xcb_get_property_reply_t *prop;
	uint32_t *xembed;

	if (!(infoatom = xcb_atom_get(tray->xcb, "_XEMBED_INFO", false)))
		return false;
	if (!(prop = xcb_get_property_reply(tray->xcb, xcb_get_property(tray->xcb, 0, win, infoatom, XCB_GET_PROPERTY_TYPE_ANY, 0, 8), NULL)))
		return false;
	xembed = (uint32_t *)xcb_get_property_value(prop);

	info->version = xembed[0];
	info->flags = xembed[1];

	free(prop);
	return true;
}

systray_item_t *
systray_append_item(systray_t *tray, xcb_window_t win)
{
	systray_item_t *item = calloc(1, sizeof(systray_item_t));
	item->win = win;

	list_add_tail(&tray->items, &item->head);

	return item;
}

void
systray_set_icon_size(systray_t *tray, int size)
{
	tray->icon_size = size;
}

int
systray_icon_size(systray_t *tray)
{
	return tray->icon_size;
}

systray_item_t *
systray_find_item(systray_t *tray, xcb_window_t win)
{
	list_head *pos;
	list_for_each(&tray->items, pos) {
		systray_item_t *item = list_entry(pos, systray_item_t, head);
		if (item->win == win)
			return item;
	}
	return NULL;
}

void
systray_remove_item(systray_t *tray, xcb_window_t win)
{
	list_head *pos;
	list_for_each(&tray->items, pos) {
		systray_item_t *item = list_entry(pos, systray_item_t, head);
		if (item->win == win) {
			list_del(pos);
			free(item);
			return;
		}
	}
}

int
systray_handle(systray_t *tray, xcb_generic_event_t *ev)
{
	xcb_atom_t atom;
	xcb_selection_clear_event_t *selection;
	xcb_client_message_event_t *client;
	xcb_property_notify_event_t *property;
	xcb_change_window_attributes_value_list_t attrs = { 0 };
	xcb_configure_window_value_list_t config = { 0 };
	systray_item_t *item;

	switch (ev->response_type & ~0x80) {
	case XCB_SELECTION_CLEAR:
		selection = (xcb_selection_clear_event_t *)ev;
		atom = get_systray_atom(tray->xcb);
		if (selection->selection == atom)
			systray_set_selection_owner(tray, get_systray_atom(tray->xcb));
		break;
	case XCB_CLIENT_MESSAGE:
		client = (xcb_client_message_event_t *)ev;
		atom = xcb_atom_get(tray->xcb, "_NET_SYSTEM_TRAY_OPCODE", true);
		if (client->type != atom)
			return 1;

		xcb_window_t win = 0;
		switch (client->data.data32[SYSTRAY_OPCODE]) {
		case SYSTRAY_REQUEST_DOCK:
			win = client->data.data32[SYSTRAY_DATA1];

			attrs.event_mask = XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE;
			xcb_change_window_attributes_aux(tray->xcb, win, XCB_CW_EVENT_MASK, &attrs);
			if (tray->icon_size) {
				config.width = tray->icon_size;
				config.height = tray->icon_size;
				xcb_configure_window_aux(tray->xcb, win, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, &config);
			}
			if (xcb_request_check(tray->xcb, xcb_reparent_window(tray->xcb, win, tray->win, 0, 0)))
				break;
			if (xcb_request_check(tray->xcb, xcb_map_window(tray->xcb, win)))
				break;

			/* notify to the window */
			xembed_embedded_notify(tray, win, 0);
			item = systray_append_item(tray, win);
			xembed_getinfo(tray, win, &item->info);

			break;
		}
		break;
	case XCB_PROPERTY_NOTIFY:
		property = (xcb_property_notify_event_t *)ev;
		if (property->state == XCB_PROPERTY_NEW_VALUE) {
			if (!(item = systray_find_item(tray, property->window)))
				return 1;
			xembed_info_t info = { 0 };
			xembed_getinfo(tray, property->window, &info);

			if (!(item->info.flags ^ info.flags))
				return 0;

			item->info.flags = info.flags;
			if (item->info.flags & XEMBED_MAPPED) {
				config.stack_mode = XCB_STACK_MODE_ABOVE;
				xcb_configure_window_aux(tray->xcb, property->window, XCB_CONFIG_WINDOW_STACK_MODE, &config);
			} else {
				xcb_unmap_window(tray->xcb, item->win);
			}
		}
		break;
	}
	return 0;
}

list_head *
systray_get_items(systray_t *tray)
{
	return &tray->items;
}

xcb_connection_t *
systray_get_connection(systray_t *tray)
{
	return tray->xcb;
}

xcb_window_t
systray_get_window(systray_t *tray)
{
	return tray->win;
}

void
systray_destroy(systray_t *tray)
{
	if (!tray)
		return;
	xcb_atom_t atom = get_systray_atom(tray->xcb);
	if (atom)
		xcb_set_selection_owner(tray->xcb, XCB_NONE, atom, XCB_TIME_CURRENT_TIME);

	list_head *pos, *tmp;
	list_for_each_safe(&tray->items, pos, tmp) {
		systray_item_t *item = list_entry(pos, systray_item_t, head);
		xembed_unembed_window(tray, item->win);
		list_del(pos);
		free(item);
	}
	free(tray);
}
