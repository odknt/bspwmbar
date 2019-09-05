#if defined(__linux)
# include <alloca.h>
#elif defined(__OpenBSD__) || defined(__FreeBSD__)
# define __BSD_VISIBLE 1
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include "bspwmbar.h"
#include "systray.h"

#define ATOM_SYSTRAY "_NET_SYSTEM_TRAY_S"

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

struct _SystemTray {
    Display *dpy;
    Window win;
    list_head items;
};

static Atom
get_systray_atom(Display *dpy)
{
    static Atom systray_atom = 0;
    if (systray_atom)
        return systray_atom;
    size_t len = strlen(ATOM_SYSTRAY) + sizeof(int) + 1;
    char *atomstr = (char *)alloca(len);
    snprintf(atomstr, len, ATOM_SYSTRAY "%d", DefaultScreen(dpy));
    systray_atom = XInternAtom(dpy, atomstr, 1);
    return systray_atom;
}

static void
set_selection_owner(SystemTray tray, Atom atom)
{
    XSetSelectionOwner(tray->dpy, atom, tray->win, CurrentTime);
}

static int
systray_get_ownership(SystemTray tray)
{
    Atom atom = get_systray_atom(tray->dpy);
    if (XGetSelectionOwner(tray->dpy, atom) != None)
        return -1;

    set_selection_owner(tray, atom);
    return 0;
}

/**
 * systray_new() - Initialize SystemTray object.
 * @dpy: A display pointer of win.
 * @win: A window for system tray.
 *
 * Return: A new system tray object.
 */
SystemTray
systray_new(Display *dpy, Window win)
{
    SystemTray tray = (SystemTray)calloc(1, sizeof(struct _SystemTray));
    XSetWindowAttributes wattrs;
    list_head_init(&tray->items);
    tray->dpy = dpy;
    tray->win = win;


    if (systray_get_ownership(tray)) {
        free(tray);
        return NULL;
    }

    wattrs.event_mask = ClientMessage;
    XChangeWindowAttributes(tray->dpy, tray->win, CWEventMask, &wattrs);

    XEvent ev = { 0 };
    ev.xclient.type = ClientMessage;
    ev.xclient.message_type = XInternAtom(tray->dpy, "MANAGER", 0);
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = CurrentTime;
    ev.xclient.data.l[1] = get_systray_atom(tray->dpy);
    ev.xclient.data.l[2] = tray->win;
    ev.xclient.data.l[3] = 0;
    ev.xclient.data.l[4] = 0;

    XSendEvent(tray->dpy, tray->win, 0, StructureNotifyMask, &ev);

    return tray;
}

static int
xembed_send(Display *dpy, Window win, long message, long d1, long d2, long d3)
{
    XEvent ev = { 0 };
    ev.xclient.type = ClientMessage;
    ev.xclient.window = win;
    ev.xclient.message_type = XInternAtom(dpy, "_XEMBED", 0);
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = CurrentTime;
    ev.xclient.data.l[1] = message;
    ev.xclient.data.l[2] = d1;
    ev.xclient.data.l[3] = d2;
    ev.xclient.data.l[4] = d3;

    XSendEvent(dpy, win, 0, NoEventMask, &ev);
    XSync(dpy, 0);

    return 0;
}

static int
xembed_embedded_notify(SystemTray tray, Window win, long version)
{
    return xembed_send(tray->dpy, win, XEMBED_EMBEDDED_NOTIFY, 0, tray->win,
                       version);
}

static int
xembed_unembed_window(SystemTray tray, Window child)
{
    XUnmapWindow(tray->dpy, child);
    XReparentWindow(tray->dpy, child, DefaultRootWindow(tray->dpy), 0, 0);
    XSync(tray->dpy, 0);

    return 0;
}

static int
xembed_getinfo(SystemTray tray, Window win, XEmbedInfo *info)
{
    Atom type, infoatom;
    int format, status;
    unsigned long nitem, bytes_after;
    unsigned char *data;

    infoatom = XInternAtom(tray->dpy, "_XEMBED_INFO", 0);
    status = XGetWindowProperty(tray->dpy, win, infoatom, 0, 2, 0, infoatom,
                                &type, &format, &nitem, &bytes_after, &data);
    XSync(tray->dpy, 0);
    if (status)
        return 1;

    unsigned long *long_data = (unsigned long *)data;
    info->version = long_data[0];
    info->flags = long_data[1];
    XFree(data);

    return 0;
}

static TrayItem *
systray_append_item(SystemTray tray, Window win)
{
    TrayItem *item = calloc(1, sizeof(TrayItem));
    item->win = win;

    list_add_tail(&tray->items, &item->head);

    return item;
}

static TrayItem *
systray_find_item(SystemTray tray, Window win)
{
    list_head *pos;
    list_for_each(&tray->items, pos) {
        TrayItem *item = list_entry(pos, TrayItem, head);
        if (item->win == win)
            return item;
    }
    return NULL;
}

void
systray_remove_item(SystemTray tray, Window win)
{
    list_head *pos;
    list_for_each(&tray->items, pos) {
        TrayItem *item = list_entry(pos, TrayItem, head);
        if (item->win == win) {
            list_del(pos);
            free(item);
            return;
        }
    }
}

int
systray_handle(SystemTray tray, XEvent ev)
{
    Atom atom;

    switch (ev.type) {
    case SelectionClear:
        atom = get_systray_atom(tray->dpy);
        if (ev.xselection.selection == atom)
            set_selection_owner(tray, get_systray_atom(tray->dpy));
        break;
    case ClientMessage:
        atom = XInternAtom(tray->dpy, "_NET_SYSTEM_TRAY_OPCODE", 0);
        if (ev.xclient.message_type != atom)
            return 1;

        Window win = 0;
        TrayItem *item;
        switch (ev.xclient.data.l[SYSTRAY_OPCODE]) {
        case SYSTRAY_REQUEST_DOCK:
            win = ev.xclient.data.l[SYSTRAY_DATA1];

            XSelectInput(tray->dpy, win, StructureNotifyMask | PropertyChangeMask);
            XReparentWindow(tray->dpy, win, tray->win, 0, 0);

            if (!(item = systray_append_item(tray, win)))
                return 1;

            xembed_getinfo(tray, win, &item->info);
            xembed_embedded_notify(tray, win, 0);
            break;
        }
        break;
    case PropertyNotify:
        if (ev.xproperty.state == PropertyNewValue) {
            if (!(item = systray_find_item(tray, ev.xproperty.window)))
                return 1;
            XEmbedInfo info = { 0 };
            xembed_getinfo(tray, ev.xproperty.window, &info);

            if (!(item->info.flags ^ info.flags))
                return 0;

            item->info.flags = info.flags;
            if (item->info.flags & XEMBED_MAPPED)
                XMapRaised(tray->dpy, item->win);
            else
                XUnmapWindow(tray->dpy, item->win);
        }
        break;
    }
    return 0;
}

list_head *
systray_get_items(SystemTray tray)
{
    return &tray->items;
}

Display *
systray_get_display(SystemTray tray)
{
    return tray->dpy;
}

Window
systray_get_window(SystemTray tray)
{
    return tray->win;
}

void
systray_destroy(SystemTray tray)
{
    if (!tray)
        return;
    Atom atom = get_systray_atom(tray->dpy);
    if (atom)
        XSetSelectionOwner(tray->dpy, atom, None, CurrentTime);

    list_head *pos, *tmp;
    list_for_each_safe(&tray->items, pos, tmp) {
        TrayItem *item = list_entry(pos, TrayItem, head);
        xembed_unembed_window(tray, item->win);
        list_del(pos);
        free(item);
    }
    free(tray);
}
