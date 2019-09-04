/* See LICENSE file for copyright and license details. */

#ifndef BSPWMBAR_H_
#define BSPWMBAR_H_

#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>

#include "util.h"

typedef enum {
    PR_NOOP   =  0,
    PR_UPDATE,
    PR_REINIT,
    PR_FAILED
} PollResult;

typedef struct {
    double val;
    int    colorno;
} GraphItem;

/* Draw context */
typedef struct _DC *DC;
typedef void (* ModuleHandler)(DC, const char *);
typedef void (* XEventHandler)(XEvent);

/* Poll */
typedef int (* PollInitHandler)();
typedef int (* PollDeinitHandler)();
typedef PollResult (* PollUpdateHandler)();
typedef struct {
    int fd;
    PollInitHandler init; /* initialize and return fd */
    PollDeinitHandler deinit; /* close fd and cleanup resources */
    PollUpdateHandler handler; /* event handler for fd */

    list_head head;
} PollFD;

void poll_add(PollFD *);
void poll_del(PollFD *);

/* Module */
typedef struct {
    ModuleHandler func;
    const char *arg;
    XEventHandler handler;
} Module;

XftColor *getcolor(int);
void draw_text(DC, const char *);
void draw_bargraph(DC, const char *, GraphItem *, int);

/* handler */
void volume_ev(XEvent);

/* modules */
void logo(DC, const char *);
void desktops(DC, const char *);
void windowtitle(DC, const char *);
void filesystem(DC, const char *);
void thermal(DC, const char *);
void battery(DC, const char *);
void brightness(DC, const char *);
void volume(DC, const char *);
void datetime(DC, const char *);
void cpugraph(DC, const char *);
void memgraph(DC, const char *);
void systray(DC, const char *);
void wireless_network(DC, const char *);

#endif
