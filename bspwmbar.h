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

typedef struct {
	const char *muted;
	const char *unmuted;
} VolumeOption;

typedef struct {
	const char *active;
	const char *inactive;
} DesktopOption;

typedef struct {
	const char *prefix;
	const char *suffix;
	union {
		const char *arg;
		const VolumeOption vol;
		const DesktopOption desk;
	};
} Option;

/* Draw context */
typedef struct _DC *DC;
typedef void (* ModuleHandler)(DC, Option);
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
	Option opts;
	XEventHandler handler;
} Module;

XftColor *getcolor(int);
void draw_text(DC, const char *);
void draw_bargraph(DC, const char *, GraphItem *, int);

/* handler */
void volume_ev(XEvent);

/* modules */
void logo(DC, Option);
void desktops(DC, Option);
void windowtitle(DC, Option);
void filesystem(DC, Option);
void thermal(DC, Option);
void volume(DC, Option);
void datetime(DC, Option);
void cpugraph(DC, Option);
void memgraph(DC, Option);
void systray(DC, Option);

#endif
