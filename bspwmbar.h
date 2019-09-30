/* See LICENSE file for copyright and license details. */

#ifndef BSPWMBAR_H_
#define BSPWMBAR_H_

#include <X11/Xlib.h>

#include "util.h"

typedef enum {
	PR_NOOP   =  0,
	PR_UPDATE,
	PR_REINIT,
	PR_FAILED
} PollResult;

typedef struct _Color *Color;

typedef struct {
	double val;
	Color fg, bg;
} GraphItem;

typedef struct {
	char *muted;
	char *unmuted;
} VolumeOption;

typedef struct {
	char *active;
	char *inactive;
} DesktopOption;

typedef struct {
	char *label;
	char *fg;
} TextOption;

typedef struct {
	char *cols[4];
} GraphOption;

typedef struct {
	const char *prefix;
	const char *suffix;
	union {
		const char *arg;
		const VolumeOption vol;
		const DesktopOption desk;
		const TextOption text;
		const GraphOption cpu;
		const GraphOption mem;
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

Color color_load(const char *);
Color color_default_fg();
Color color_default_bg();

void draw_text(DC, const char *);
void draw_bargraph(DC, const char *, GraphItem *, int);

/* handler */
void volume_ev(XEvent);

/* modules */
void text(DC, Option);
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
