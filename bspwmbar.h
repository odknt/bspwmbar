/* See LICENSE file for copyright and license details. */

#ifndef BSPWMBAR_H_
#define BSPWMBAR_H_

#include <unistd.h>
#include <X11/Xlib.h>

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
	long volume;
	int  unmuted;
	long max, min;
	long oneper;
} AlsaInfo;

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

int cpu_perc(CoreInfo **);
int mem_perc();

char *filesystem(const char *);

int alsa_connect();
void alsa_disconnect();
int alsa_need_update();
char *thermal(const char *);

char *volume(const char *);
void volume_ev(XEvent);

char *datetime(const char *);

int systray_init(TrayWindow *);
void systray_destroy(TrayWindow *);
void systray_remove_item(TrayWindow *, Window);
int systray_handle(TrayWindow *, XEvent);

#endif
