/* See LICENSE file for copyright and license details. */

#ifndef BSPWMBAR_H_
#define BSPWMBAR_H_

#include <unistd.h>
#include <X11/Xlib.h>

#include "util.h"

typedef enum {
	PR_NOOP   =  0,
	PR_UPDATE,
	PR_REINIT,
	PR_FAILED
} PollResult;

typedef struct {
	double user;
	double nice;
	double system;
	double idle;
	double iowait;
	double irq;
	double softirq;
	double sum;
	double loadavg;
} CoreInfo;

typedef struct {
	size_t total;
	size_t available;
} MemInfo;

typedef struct {
	unsigned long version;
	unsigned long flags;
} XEmbedInfo;

typedef struct _TrayItem {
	struct _TrayItem *prev;
	struct _TrayItem *next;
	Window win;
	XEmbedInfo info;
} TrayItem;

typedef struct {
	Display *dpy;
	Window win;
	TrayItem *items;
} TrayWindow;

typedef struct {
	/* initialize and return fd */
	int (* init)();
	/* close fd and cleanup resources */
	int (* deinit)();
	/* event handler for fd */
	PollResult (* handler)(int);
} Poller;

typedef struct {
	char *(* func)(const char *);
	const char *arg;
	void (* handler)(XEvent);
} Module;

int cpu_perc(CoreInfo **);
int mem_perc();

char *filesystem(const char *);

int alsa_connect();
int alsa_disconnect();
PollResult alsa_update();
char *thermal(const char *);

char *volume(const char *);
void volume_ev(XEvent);

char *datetime(const char *);

int systray_init(TrayWindow *);
void systray_destroy(TrayWindow *);
void systray_remove_item(TrayWindow *, Window);
int systray_handle(TrayWindow *, XEvent);

#endif
