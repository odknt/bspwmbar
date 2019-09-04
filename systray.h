/* See LICENSE file for copyright and license details. */

#ifndef SYSTRAY_H_
#define SYSTRAY_H_

#include <X11/Xlib.h>

#include "util.h"

typedef struct {
    unsigned long version;
    unsigned long flags;
} XEmbedInfo;

typedef struct _TrayItem {
    Window win;
    XEmbedInfo info;
    int x;

    list_head head;
} TrayItem;

/**
 * SystemTray - An opaque pointer for struct _SystemTray.
 */
typedef struct _SystemTray *SystemTray;

SystemTray systray_new(Display *, Window);
int systray_handle(SystemTray, XEvent);
void systray_destroy(SystemTray);
void systray_remove_item(SystemTray, Window);
Window systray_get_window(SystemTray);
Display *systray_get_display(SystemTray);
list_head *systray_get_items(SystemTray);

#endif /* SYSTRAY_H_ */
