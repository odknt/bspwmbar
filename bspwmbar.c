#define _XOPEN_SOURCE
#define _XOPEN_SOURCE_EXTENDED

#include <X11/Xft/Xft.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrandr.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <xcb/xcb.h>

#include "bspwmbar.h"
#include "config.h"
#include "util.h"

/* bspwm commands */
#define SUBSCRIBE_REPORT "subscribe\0report"
/* epoll max events */
#define MAX_EVENTS 10
#define MINCW 3

char buf[1024];

static Bool thermal_found;

static char ascii_table[] = " !\"#$%&'()*+,-./0123456789:;<=>?"
                            "@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_"
                            "`abcdefghijklmnopqrstuvwxyz{|}~";

enum {
	STATE_FREE     = 0,
	STATE_FOCUSED  = 1 << 1,
	STATE_OCCUPIED = 1 << 2,
	STATE_URGENT   = 1 << 3,

	STATE_ACTIVE = 1 << 8
};

XColor bgcol;
XftColor xftcols[LENGTH(colors)];

typedef int WsState;

typedef struct {
	char name[NAME_MAXSZ];
	WsState state;
} Workspace;

typedef struct {
	char name[NAME_MAXSZ];
	Workspace workspaces[WS_MAXSZ];
	int nworkspaces;
	Bool is_active;
} Monitor;

typedef struct {
	Window win;
	Pixmap pixmap;
	XftDraw *draw;
	Monitor monitor;

	int x, y, width, height;
} BarWindow;

typedef struct {
	int fd;
	Display *dpy;
	int scr;
	GC gc;
	XftFont *fonts[FONT_MAXSZ];
	int nfont;
	BarWindow *xbars;
	int nxbar;
	int baseline;
} Bspwmbar;

static XColor
getcolor(Display *dpy, int scr, const char *colstr)
{
	Colormap cmap = DefaultColormap(dpy, scr);
	XColor color;

	XAllocNamedColor(dpy, cmap, colstr, &color, &color);
	return color;
}

static XftColor
getxftcolor(Display *dpy, int scr, const char *colstr)
{
	Colormap cmap = DefaultColormap(dpy, scr);
	XftColor color;

	XftColorAllocName(dpy, DefaultVisual(dpy, scr), cmap, colstr, &color);
	return color;
}

static void
load_colors(Display *dpy, int scr)
{
	for (size_t i = 0; i < LENGTH(colors); i++) {
		xftcols[i] = getxftcolor(dpy, scr, colors[i]);
	}
	bgcol = getcolor(dpy, scr, colors[BGCOLOR]);
}

static WsState
ws_state(char s)
{
	WsState state = STATE_FREE;
	if (s == 'o')
		state = STATE_OCCUPIED;
	if (s == 'u')
		state = STATE_URGENT;
	if (s == 'F' || s == 'U' || s == 'O')
		return state | STATE_ACTIVE;
	return state;
}

static void
barwindow_init(Display *dpy, int scr, int x, int y, int width, int height,
               BarWindow *xw)
{
	XSetWindowAttributes wattrs;
	XClassHint *hint;

	xw->pixmap = XCreatePixmap(dpy, RootWindow(dpy, scr), width, height,
	                           DefaultDepth(dpy, scr));

	wattrs.override_redirect = 1;
	wattrs.background_pixmap = xw->pixmap;
	wattrs.event_mask        = ExposureMask;

	xw->win = XCreateWindow(dpy, RootWindow(dpy, scr), x, y, width, height, 0,
	                        DefaultDepth(dpy, scr), CopyFromParent,
	                        DefaultVisual(dpy, scr),
	                        CWOverrideRedirect | CWBackPixmap | CWEventMask,
	                        &wattrs);

	xw->draw = XftDrawCreate(dpy, xw->win, DefaultVisual(dpy, scr),
	                         DefaultColormap(dpy, scr));

	hint            = XAllocClassHint();
	hint->res_class = "Bspwmbar";
	hint->res_name  = "Bspwmbar";
	XSetClassHint(dpy, xw->win, hint);
	XFree(hint);

	xw->x      = x;
	xw->y      = y;
	xw->width  = width;
	xw->height = height;
}

static unsigned char *
get_window_prop(Display *dpy, Window win, char *property)
{
	Atom type, filter;
	int format, status;
	unsigned long nitem, bytes_after;
	unsigned char *prop;

	filter = XInternAtom(dpy, property, 1);
	status = XGetWindowProperty(dpy, win, filter, 0, TITLE_MAXSZ, 0,
	                            AnyPropertyType, &type, &format, &nitem,
	                            &bytes_after, &prop);
	if (status != Success)
		return NULL;

	return prop;
}

static Window
get_active_window(Display *dpy, int scr)
{
	unsigned char *prop;
	if (!(prop = get_window_prop(dpy, RootWindow(dpy, scr),
	                             "_NET_ACTIVE_WINDOW")))
		return 0;
	if (!strlen((const char *)prop))
		return 0;
	Window win = prop[0] + (prop[1] << 8) + (prop[2] << 16) + (prop[3] << 24);
	XFree(prop);
	return win;
}

static unsigned char *
get_window_title(Display *dpy, Window win)
{
	return get_window_prop(dpy, win, "_NET_WM_NAME");
}

void
bspwmbar_loadfonts(Bspwmbar *bar, const char **names, size_t len)
{
	XftFont *font;
	size_t i;
	for (i = 0; i < len; i++)
		if ((font = XftFontOpenName(bar->dpy, bar->scr, names[i])))
			bar->fonts[bar->nfont++] = font;
}

int
bspwmbar_getdrawwidth(Bspwmbar *bar, char *str, XGlyphInfo *extents)
{
	long rune = 0;
	int width = 0;
	for (unsigned int i = 0; i < strlen(str);) {
		int len = utf8decode(&str[i], &rune, UTF_SZ);
		for (int j = 0; j < bar->nfont; j++) {
			if (!XftCharExists(bar->dpy, bar->fonts[j], rune))
				continue;

			XftTextExtentsUtf8(bar->dpy, bar->fonts[j], (FcChar8 *)&str[i], len,
			                   extents);
			width += BIGGER(extents->width - extents->x, MINCW);
			break;
		}
		i += len;
	}
	return width;
}

int
bspwmbar_drawstring(Bspwmbar *bar, XftDraw *draw, XftColor *color,
                    const char *str, int x)
{
	XGlyphInfo extents = { 0 };
	long rune          = 0;
	int width          = 0;
	for (unsigned int i = 0; i < strlen(str);) {
		int len = utf8decode(&str[i], &rune, UTF_SZ);
		for (int j = 0; j < bar->nfont; j++) {
			if (!XftCharExists(bar->dpy, bar->fonts[j], rune))
				continue;
			XftTextExtentsUtf8(bar->dpy, bar->fonts[j], (FcChar8 *)&str[i], len,
			                   &extents);
			XftDrawStringUtf8(draw, color, bar->fonts[j], x + width,
			                  bar->baseline, (FcChar8 *)&str[i], len);
			width += BIGGER(extents.width - extents.x, MINCW);
			break;
		}
		i += len;
	}
	return width;
}

int
bspwmbar_drawfontstring(Bspwmbar *bar, XftDraw *draw, int fno, XftColor *color,
                        const char *str, int x)
{
	XGlyphInfo extents = { 0 };
	long rune          = 0;
	int width          = 0;
	for (unsigned int i = 0; i < strlen(str);) {
		int len = utf8decode(&str[i], &rune, UTF_SZ);
		XftTextExtentsUtf8(bar->dpy, bar->fonts[fno], (FcChar8 *)&str[i], len,
		                   &extents);
		XftDrawStringUtf8(draw, color, bar->fonts[fno], x + width,
		                  bar->baseline, (FcChar8 *)&str[i], len);
		width += extents.width - extents.x;
		i += len;
	}
	return width;
}

static int
bspwmbar_drawcpu(Bspwmbar *bar, XftDraw *draw, CoreInfo *a, int nproc, int x)
{
	char *ramp;
	XGlyphInfo extents;
	int pos = x - bspwmbar_getdrawwidth(bar, "▁", &extents);
	for (int i = nproc - 1; i >= 0; i--) {
		int avg     = (int)a[i].loadavg;
		XftColor fg = xftcols[4];
		if (avg < 10) {
			ramp = "▁";
		} else if (avg < 30) {
			ramp = "▂";
		} else if (avg < 50) {
			ramp = "▃";
			fg   = xftcols[5];
		} else if (avg < 60) {
			ramp = "▄";
			fg   = xftcols[5];
		} else if (avg < 70) {
			ramp = "▅";
			fg   = xftcols[6];
		} else if (avg < 80) {
			ramp = "▆";
			fg   = xftcols[6];
		} else if (avg < 90) {
			ramp = "▇";
			fg   = xftcols[7];
		} else {
			ramp = "█";
			fg   = xftcols[7];
		}
		bspwmbar_drawfontstring(bar, draw, CPUFONT, &xftcols[ALTFGCOLOR], "█",
		                        pos);
		pos -= bspwmbar_drawfontstring(bar, draw, CPUFONT, &fg, ramp, pos);
	}
	pos -= bspwmbar_getdrawwidth(bar, "cpu:", &extents);
	bspwmbar_drawstring(bar, draw, &xftcols[FGCOLOR], "cpu:", pos);
	return x - pos;
}

static int
bspwmbar_drawmem(Bspwmbar *bar, XftDraw *draw, size_t memused, int x)
{
	int pos = x;
	XGlyphInfo extents;
	for (size_t i = 10; i > 0; i--) {
		XftColor fg = xftcols[ALTFGCOLOR];
		if (memused >= 90)
			fg = xftcols[7];
		else if (i <= 3 && memused >= i * 10)
			fg = xftcols[4];
		else if (i <= 6 && memused >= i * 10)
			fg = xftcols[5];
		else if (i <= 9 && memused >= i * 10)
			fg = xftcols[6];
		pos -= bspwmbar_drawfontstring(bar, draw, MEMFONT, &fg, "█", pos);
	}
	pos -= bspwmbar_getdrawwidth(bar, "mem:", &extents);
	bspwmbar_drawstring(bar, draw, &xftcols[FGCOLOR], "mem:", pos);
	return x - pos;
}

void
bspwmbar_parse(Bspwmbar *bar, char *report)
{
	int i, j, nws = 0, name_len;
	int len = strlen(report);
	char tok, name[NAME_MAXSZ];
	Monitor *curmon = NULL;

	for (i = 0; i < len; i++) {
		switch (tok = report[i]) {
			case 'M':
			case 'm':
				nws = 0;
				for (j = ++i; j < len; j++)
					if (report[j] == ':')
						break;
				name_len = SMALLER(j - i, NAME_MAXSZ - 1);
				strncpy(name, &report[i], name_len);
				name[name_len] = '\0';
				i              = j;
				for (j = 0; j < bar->nxbar; j++)
					if (!strncmp(bar->xbars[j].monitor.name, name,
					             strlen(name)))
						curmon = &bar->xbars[j].monitor;
				if (curmon)
					curmon->is_active = (tok == 'M') ? 1 : 0;
				break;
			case 'o':
			case 'O':
			case 'f':
			case 'F':
			case 'u':
			case 'U':
				nws++;
				for (j = ++i; j < len; j++)
					if (report[j] == ':')
						break;
				if (curmon)
					curmon->workspaces[nws - 1].state = ws_state(tok);
				i = j;
				break;
			case 'T':
				i++; // skip next char.
				break;
			case 'G':
				if (curmon)
					curmon->nworkspaces = nws;
				break;
		}
	}
}

void
bspwmbar_render(Bspwmbar *bar)
{
	XftColor col;
	XftColor fg        = xftcols[FGCOLOR];
	XftColor altfg     = xftcols[ALTFGCOLOR];
	XGlyphInfo extents = { 0 };

	time_t t = time(NULL);
	uintmax_t temp;
	Bool temp_loaded;

	if (thermal_found && pscanf(THERMAL_PATH, "%ju", &temp) != -1)
		temp_loaded = 1;

	/* padding width */
	int pad = bspwmbar_getdrawwidth(bar, "a", &extents);

	Window win        = get_active_window(bar->dpy, bar->scr);
	char *title       = NULL;
	Bool title_suffix = 0;
	if (win) {
		title      = (char *)get_window_title(bar->dpy, win);
		size_t idx = utf8npos(title, TITLE_MAXSZ, strlen(title));
		if (idx < strlen(title))
			title_suffix = 1;
		title[idx] = '\0';
	}

	CoreInfo *cores;
	int ncore = cpu_perc(&cores);
	int mem   = mem_perc();
	int disk  = disk_perc();

	AlsaInfo alsa    = alsa_info();
	const char *mark = (alsa.unmuted) ? "墳" : "婢";

	for (int i = 0; i < bar->nxbar; i++) {
		BarWindow *xw = &bar->xbars[i];
		int x         = 0, width;

		XClearWindow(bar->dpy, xw->win);

		/* render logo */
		x     = pad * 2;
		width = bspwmbar_drawstring(bar, xw->draw, &xftcols[LOGOCOLOR], "", x);
		x += width + pad;

		/* render workspaces */
		for (int j = 0; j < xw->monitor.nworkspaces; j++) {
			char *ws;
			x += pad;
			if (xw->monitor.workspaces[j].state & STATE_ACTIVE)
				ws = "";
			else
				ws = "";
			col = (xw->monitor.workspaces[j].state == STATE_FREE) ? altfg : fg;
			x += bspwmbar_drawstring(bar, xw->draw, &col, ws, x);
		}

		/* render title */
		x += pad * 2;
		if (title)
			x += bspwmbar_drawstring(bar, xw->draw, &fg, title, x);
		if (title_suffix)
			x += bspwmbar_drawstring(bar, xw->draw, &fg, "…", x);

		x = xw->width;
		/* render time */
		if (strftime(buf, sizeof(buf), " %H:%M", localtime(&t))) {
			x -= bspwmbar_getdrawwidth(bar, buf, &extents) + pad * 2;
			bspwmbar_drawstring(bar, xw->draw, &fg, buf, x);
		}

		/* render temperature */
		if (thermal_found && temp_loaded) {
			sprintf(buf, " %ld℃", temp / 1000);
			x -= bspwmbar_getdrawwidth(bar, buf, &extents) + pad * 2;
			bspwmbar_drawstring(bar, xw->draw, &fg, buf, x);
		}

		/* render disk usage */
		if (disk != -1) {
			sprintf(buf, " %d％", disk);
			x -= bspwmbar_getdrawwidth(bar, buf, &extents) + pad * 2;
			bspwmbar_drawstring(bar, xw->draw, &fg, buf, x);
		}

		/* render volume */
		sprintf(buf, "%s %ld％", mark, alsa.volume);
		x -= bspwmbar_getdrawwidth(bar, buf, &extents) + pad * 2;
		bspwmbar_drawstring(bar, xw->draw, &fg, buf, x);

		/* render mem */
		x -= pad * 3;
		x -= bspwmbar_drawmem(bar, xw->draw, mem, x);

		/* render cpu */
		x -= pad;
		x -= bspwmbar_drawcpu(bar, xw->draw, cores, ncore, x);
	}

	if (title)
		XFree(title);

	XFlush(bar->dpy);
}

static int
bspwm_connect()
{
	struct sockaddr_un sock;
	int fd, dn = 0, sn = 0;
	char *sp, *host;

	sock.sun_family = AF_UNIX;
	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
		return -1;

	sp = getenv("BSPWM_SOCKET");
	if (sp) {
		snprintf(sock.sun_path, sizeof(sock.sun_path), "%s", sp);
	} else {
		if (xcb_parse_display(NULL, &host, &dn, &sn))
			snprintf(sock.sun_path, sizeof(sock.sun_path),
			         "/tmp/bspwm%s_%i_%i-socket", host, dn, sn);
		free(host);
	}
	if (connect(fd, (struct sockaddr *)&sock, sizeof(sock)) == -1)
		return -1;

	return fd;
}

int
bspwmbar_init(Bspwmbar *bar, Display *dpy, int scr)
{
	XRRScreenResources *xrr_res;
	XRRMonitorInfo *xrr_mon;
	XRROutputInfo *xrr_out;
	XGCValues gcv      = { 0 };
	XGlyphInfo extents = { 0 };
	int i, j, nmon;

	/* connect bspwm socket */
	if ((bar->fd = bspwm_connect()) == -1)
		die("bspwm_connect(): Failed to connect to the socket\n");

	/* get monitors */
	xrr_mon    = XRRGetMonitors(dpy, RootWindow(dpy, DefaultScreen(dpy)), 1,
                             &nmon);
	bar->xbars = (BarWindow *)malloc(sizeof(BarWindow) * nmon);
	bar->nxbar = nmon;

	/* create window per monitor */
	xrr_res = XRRGetScreenResources(dpy, RootWindow(dpy, DefaultScreen(dpy)));
	for (i = 0; i < xrr_res->noutput; i++) {
		xrr_out = XRRGetOutputInfo(dpy, xrr_res, xrr_res->outputs[i]);
		if (xrr_out->connection != RR_Connected)
			continue;
		for (j = 0; j < nmon; j++) {
			if (xrr_res->outputs[i] != xrr_mon[j].outputs[0])
				continue;
			barwindow_init(dpy, scr, xrr_mon[j].x, 24, xrr_mon[j].width, 24,
			               &bar->xbars[j]);
			strncpy(bar->xbars[j].monitor.name, xrr_out->name, NAME_MAXSZ);
		}
		XRRFreeOutputInfo(xrr_out);
	}
	XRRFreeScreenResources(xrr_res);
	XRRFreeMonitors(xrr_mon);

	/* initialize */
	bar->dpy   = dpy;
	bar->scr   = scr;
	bar->nfont = 0;

	bspwmbar_loadfonts(bar, font_names, LENGTH(font_names));
	bspwmbar_getdrawwidth(bar, ascii_table, &extents);
	bar->baseline = extents.y / 2 + (BAR_HEIGHT - extents.height) / 2;

	bar->gc = XCreateGC(dpy, RootWindow(dpy, scr), GCGraphicsExposures, &gcv);

	/* clear background */
	for (i = 0; i < bar->nxbar; i++) {
		XSetForeground(dpy, bar->gc, bgcol.pixel);
		XFillRectangle(dpy, bar->xbars[i].pixmap, bar->gc, 0, 0,
		               bar->xbars[i].width, bar->xbars[i].height);

		XMapWindow(dpy, bar->xbars[i].win);
		XSync(dpy, False);
	}

	return 0;
}

void
bspwmbar_destroy(Bspwmbar *bar)
{
	int i;

	close(bar->fd);

	for (i = 0; i < bar->nfont; i++)
		XftFontClose(bar->dpy, bar->fonts[i]);
	free(bar->fonts);
	for (i = 0; i < bar->nxbar; i++) {
		XftDrawDestroy(bar->xbars[i].draw);
		XDestroyWindow(bar->dpy, bar->xbars[i].win);
	}
	free(bar->xbars);
}

int
bspwmbar_send(Bspwmbar *bar, char *cmd, int len)
{
	return send(bar->fd, cmd, len, 0);
}

int
main(int argc, char *argv[])
{
	char buf[1024];
	Bspwmbar bar;
	// pthread_t pt;
	struct epoll_event ev, events[MAX_EVENTS];
	struct epoll_event xev, aev;
	Display *dpy;
	int epfd, xfd, nfd, i, len;
	XEvent event;

	(void)(argc);
	(void)(argv);

	setlocale(LC_ALL, "");

	if (access(THERMAL_PATH, F_OK) != -1)
		thermal_found = 1;

	if (!(dpy = XOpenDisplay(NULL)))
		die("XOpenDisplay(): Failed to open display\n");

	load_colors(dpy, DefaultScreen(dpy));

	if (bspwmbar_init(&bar, dpy, DefaultScreen(dpy)))
		die("bspwmbar_init(): Failed to init bspwmbar\n");

	if (bspwmbar_send(&bar, SUBSCRIBE_REPORT, LENGTH(SUBSCRIBE_REPORT)) == -1)
		die("bspwmbar_send(): Failed to send command to bspwm\n");

	/* epoll */
	if ((epfd = epoll_create1(0)) == -1)
		die("epoll_create1(): Failed to create epoll fd\n");

	ev.events  = EPOLLIN;
	ev.data.fd = bar.fd;
	if (epoll_ctl(epfd, EPOLL_CTL_ADD, bar.fd, &ev) == -1)
		die("epoll_ctl(): Failed to add to epoll fd\n");

	/* polling X11 event */
	xfd         = ConnectionNumber(bar.dpy);
	xev.events  = EPOLLIN;
	xev.data.fd = xfd;
	if (epoll_ctl(epfd, EPOLL_CTL_ADD, xfd, &xev) == -1)
		die("epoll_ctl(): Failed to add to epoll xfd\n");

	/* event */
	Window win;
	XSetWindowAttributes attrs;
	attrs.event_mask = PropertyChangeMask;

	int afd     = alsa_connect();
	aev.events  = EPOLLIN;
	aev.data.fd = afd;

	if (epoll_ctl(epfd, EPOLL_CTL_ADD, afd, &aev) == -1)
		die("epoll_ctl(): Failed to add to epoll afd\n");

	/* main loop */
	while ((nfd = epoll_wait(epfd, events, MAX_EVENTS, 1000)) != -1) {
		for (i = 0; i < nfd; i++) {
			if (events[i].data.fd == bar.fd) {
				/* for BSPWM */
				if ((len = recv(bar.fd, buf, sizeof(buf) - 1, 0)) > 0) {
					buf[len] = '\0';
					if (buf[0] == '\x07') {
						fprintf(stderr, "bspwm: %s", buf + 1);
						goto CLEANUP;
					}
					bspwmbar_parse(&bar, buf);

					if ((win = get_active_window(bar.dpy, bar.scr)))
						XChangeWindowAttributes(bar.dpy, win, CWEventMask,
						                        &attrs);
				}
			} else if (events[i].data.fd == xfd) {
				/* for X11 events */
				while (XPending(bar.dpy))
					XNextEvent(bar.dpy, &event);
			} else if (events[i].data.fd == afd) {
				if (!alsa_need_update())
					continue;
			}
		}
		bspwmbar_render(&bar);
	}
CLEANUP:

	alsa_disconnect();
	bspwmbar_destroy(&bar);
	if (XCloseDisplay(dpy))
		die("XCloseDisplay(): Failed to close display\n");

	return 0;
}
