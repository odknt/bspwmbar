/* See LICENSE file for copyright and license details. */

#ifndef BSPWMBAR_H_
#define BSPWMBAR_H_

#include <stdint.h>
#include <X11/Xlib.h>
#include <X11/Xft/Xft.h>

#if defined(__OpenBSD__)
# include <sys/sched.h>
#endif

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

typedef struct _CoreInfo *CoreInfo;

#if defined(__linux)
typedef struct {
	size_t total;
	size_t available;
} MemInfo;
#elif defined(__OpenBSD__)
typedef struct uvmexp MemInfo;
#endif

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

typedef struct {
	Display *dpy;
	Window win;
	list_head items;
} TrayWindow;

typedef struct _DC *DC;
typedef void (* ModuleHandler)(DC, const char *);
typedef void (* XEventHandler)(XEvent);

typedef struct {
	int fd;
	int (* init)(); /* initialize and return fd */
	int (* deinit)(); /* close fd and cleanup resources */
	PollResult (* handler)(int); /* event handler for fd */

	list_head head;
} PollFD;

typedef struct {
	ModuleHandler func;
	const char *arg;
	XEventHandler handler;
} Module;

XftColor *getcolor(int);
void drawtext(DC, const char *);
void draw_bargraph(DC, const char *, GraphItem *, int);
void poll_add(PollFD *);
void poll_del(PollFD *);

/* cpu.c */
int cpu_perc(double **);

/* mem.c */
double mem_perc();

/* systray.c */
int systray_init(TrayWindow *);
void systray_destroy(TrayWindow *);
void systray_remove_item(TrayWindow *, Window);
int systray_handle(TrayWindow *, XEvent);

/* modules for alignment */
void float_right(DC, const char *);

/* handler */
void volume_ev(XEvent);

/* modules */
void logo(DC, const char *);
void workspace(DC, const char *);
void windowtitle(DC, const char *);
void filesystem(DC, const char *);
void thermal(DC, const char *);
void volume(DC, const char *);
void datetime(DC, const char *);
void cpugraph(DC, const char *);
void memgraph(DC, const char *);
void systray(DC, const char *);

#endif
