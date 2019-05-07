/* See LICENSE file for copyright and license details. */

#define _XOPEN_SOURCE 700

#include <alloca.h>
#include <X11/Xatom.h>
#include <X11/Xft/Xft.h>
#include <X11/Xproto.h>
#include <X11/extensions/Xrandr.h>
#include <locale.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <time.h>

typedef struct {
	char *(* func)(const char *);
	const char *arg;
	void (* handler)(XEvent);
} Module;

#include "bspwmbar.h"
#include "config.h"
#include "util.h"

/* bspwm commands */
#define SUBSCRIBE_REPORT "subscribe\0report"
/* epoll max events */
#define MAX_EVENTS 10

char buf[1024];

static char ascii_table[] =
	" !\"#$%&'()*+,-./0123456789:;<=>?"
	"@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_"
	"`abcdefghijklmnopqrstuvwxyz{|}~";

static char *_net_wm_states[] = {
	"_NET_WM_STATE_STICKY",
	"_NET_WM_STATE_ABOVE"
};
static char *_net_wm_window_types[] = { "_NET_WM_WINDOW_TYPE_DOCK" };

enum {
	STATE_FREE     = 0,
	STATE_FOCUSED  = 1 << 1,
	STATE_OCCUPIED = 1 << 2,
	STATE_URGENT   = 1 << 3,

	STATE_ACTIVE = 1 << 8
};

static int epfd = 0;
static XftColor cols[LENGTH(colors)];
static TrayWindow tray;
static XftFont **fcaches;
static int nfcache = 0;
static int fcachecap = 0;

typedef struct {
	FcPattern *pattern;
	FcFontSet *set;
	XftFont   *base;
} XFont;

typedef struct {
	Module *module;

	int x, width;
} Label;

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
	XftDraw *draw;
	Monitor monitor;

	int x, y, width, height;
} BarWindow;

typedef struct {
	int fd;
	Display *dpy;
	int scr;
	GC gc;
	XFont font;
	BarWindow *xbars;
	int nxbar;

	Label labels[LENGTH(modules)];
	int nlabel;
} Bspwmbar;

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
	for (size_t i = 0; i < LENGTH(colors); i++)
		cols[i] = getxftcolor(dpy, scr, colors[i]);
}

static void
free_colors(Display *dpy, int scr)
{
	Colormap cmap = DefaultColormap(dpy, scr);
	for (size_t i = 0; i < LENGTH(colors); i++)
		XftColorFree(dpy, DefaultVisual(dpy, scr), cmap, &cols[i]);
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

static void
set_window_prop(Display *dpy, Window win, Atom type, char *property, int mode,
                void *propvalue, int nvalue)
{
	Atom prop;
	void *values;
	int format = 8;

	prop = XInternAtom(dpy, property, 1);
	switch (type) {
	case XA_ATOM:
		values = alloca(sizeof(Atom) * nvalue);
		Atom *atomvals = (Atom *)values;
		char **vals = (char **)propvalue;
		for (int i = 0; i < nvalue; i++)
			atomvals[i] = XInternAtom(dpy, vals[i], 1);
		format = 32;
		break;
	case XA_CARDINAL:
		values = propvalue;
		format = 32;
		break;
	default:
		return;
	}

	XChangeProperty(dpy, win, prop, type, format, mode, (unsigned char *)values,
	                nvalue);
}

static void
barwindow_init(Display *dpy, int scr, int x, int y, int width, int height,
               BarWindow *xw)
{
	XSetWindowAttributes wattrs;
	XClassHint *hint;

	wattrs.background_pixel = cols[BGCOLOR].pixel;
	wattrs.event_mask = NoEventMask;

	xw->win = XCreateWindow(dpy, RootWindow(dpy, scr), x, y, width, height, 0,
	                        CopyFromParent, CopyFromParent,
	                        CopyFromParent,
	                        CWBackPixel | CWEventMask,
	                        &wattrs);

	/* set window type */
	set_window_prop(dpy, xw->win, XA_ATOM, "_NET_WM_STATE", PropModeReplace,
	                _net_wm_states, LENGTH(_net_wm_states));
	set_window_prop(dpy, xw->win, XA_ATOM, "_NET_WM_WINDOW_TYPE",
	                PropModeReplace, _net_wm_window_types,
	                LENGTH(_net_wm_window_types));
	long strut[] = { 0, 0, height, 0 };
	set_window_prop(dpy, xw->win, XA_CARDINAL, "_NET_WM_STRUT", PropModeReplace,
	                strut, 4);
	long strut_partial[] = {
		0, 0, height, 0,
		0, 0, 0, 0,
		x, x + width - 1, 0, 0,
	};
	set_window_prop(dpy, xw->win, XA_CARDINAL, "_NET_WM_STRUT_PARTIAL",
	                PropModeReplace, strut_partial, 12);

	/* create draw context */
	xw->draw = XftDrawCreate(dpy, xw->win, DefaultVisual(dpy, scr),
	                         DefaultColormap(dpy, scr));

	hint = XAllocClassHint();
	hint->res_class = "Bspwmbar";
	hint->res_name = "Bspwmbar";
	XSetClassHint(dpy, xw->win, hint);
	XFree(hint);

	xw->x = x;
	xw->y = y;
	xw->width = width;
	xw->height = height;
}

static Window
get_active_window(Display *dpy, int scr)
{
	Window win = 0;
	unsigned char *prop;
	if (!(prop = get_window_prop(dpy, RootWindow(dpy, scr),
	                             "_NET_ACTIVE_WINDOW")))
		return 0;
	if (strlen((const char *)prop))
		win = prop[0] + (prop[1] << 8) + (prop[2] << 16) + (prop[3] << 24);
	XFree(prop);
	return win;
}

static unsigned char *
get_window_title(Display *dpy, Window win)
{
	unsigned char *title;
	if ((title = get_window_prop(dpy, win, "_NET_WM_NAME")))
		return title;
	if ((title = get_window_prop(dpy, win, "WM_NAME")))
		return title;
	return NULL;
}

static XftFont *
bspwmbar_getfont(Bspwmbar *bar, FcChar32 rune)
{
	FcResult result;
	FcPattern *pat, *match;
	FcCharSet *charset;
	FcFontSet *fsets[] = { NULL };
	int i, idx;

	/* Lookup character index with default font. */
	idx = XftCharIndex(bar->dpy, bar->font.base, rune);
	if (idx)
		return bar->font.base;

	/* fallback on font cache */
	for (i = 0; i < nfcache; i++) {
		if ((idx = XftCharIndex(bar->dpy, fcaches[i], rune)))
			return fcaches[i];
	}

	/* find font when not found */
	if (i >= nfcache) {
		if (!bar->font.set)
			bar->font.set = FcFontSort(0, bar->font.pattern, 1, 0, &result);
		fsets[0] = bar->font.set;

		if (nfcache >= fcachecap) {
			fcachecap += 8;
			fcaches = realloc(fcaches, fcachecap * sizeof(XftFont *));
		}

		pat = FcPatternDuplicate(bar->font.pattern);
		charset = FcCharSetCreate();

		/* find font that contains rune and scalable */
		FcCharSetAddChar(charset, rune);
		FcPatternAddCharSet(pat, FC_CHARSET, charset);
		FcPatternAddBool(pat, FC_SCALABLE, 1);

		FcConfigSubstitute(0, pat, FcMatchPattern);
		FcDefaultSubstitute(pat);

		match = FcFontSetMatch(0, fsets, 1, pat, &result);
		FcPatternDestroy(pat);

		fcaches[nfcache] = XftFontOpenPattern(bar->dpy, match);
		FcPatternDestroy(match);
		FcCharSetDestroy(charset);

		if (!fcaches[nfcache])
			die("XftFontOpenPattern(): failed seeking fallback font\n");

		i = nfcache++;
	}

	return fcaches[i];
}

void
bspwmbar_loadfonts(Bspwmbar *bar, const char *patstr)
{
	FcPattern *pat = FcNameParse((FcChar8 *)patstr);
	if (!pat)
		die("bspwmbar_loadfonts(): failed parse pattern: %s\n", patstr);

	FcConfigSubstitute(NULL, pat, FcMatchPattern);
	XftDefaultSubstitute(bar->dpy, bar->scr, pat);

	FcResult result;
	FcPattern *match = FcFontMatch(NULL, pat, &result);
	if (!match) {
		FcPatternDestroy(match);
		die("bspwmbar_loadfonts(): failed parse pattern: %s\n", patstr);
	}

	if (!(bar->font.base = XftFontOpenPattern(bar->dpy, match))) {
		FcPatternDestroy(pat);
		FcPatternDestroy(match);
		die("bspwmbar_loadfonts(): failed open font: %s\n", patstr);
	}

	bar->font.pattern = pat;
}

int
bspwmbar_getdrawwidth(Bspwmbar *bar, char *str, XGlyphInfo *extents)
{
	FcChar32 rune = 0;
	int width = 0, len = 0;
	XftFont *font;
	for (unsigned int i = 0; i < strlen(str); i += len) {
		len = FcUtf8ToUcs4((FcChar8 *)&str[i], &rune, strlen(str) - i);
		font = bspwmbar_getfont(bar, rune);
		XftTextExtentsUtf8(bar->dpy, font, (FcChar8 *)&str[i], len, extents);
		width += extents->x + extents->xOff;
	}
	return width;
}

int
bspwmbar_drawstring(Bspwmbar *bar, XftDraw *draw, XftColor *color,
                    const char *str, int x)
{
	XGlyphInfo extents = { 0 };
	FcChar32 rune = 0;
	int width = 0, len = 0;
	XftFont *font;

	XftTextExtentsUtf8(bar->dpy, bar->font.base, (FcChar8 *)str, strlen(str),
	                   &extents);
	for (unsigned int i = 0; i < strlen(str); i += len) {
		int len = FcUtf8ToUcs4((FcChar8 *)&str[i], &rune, strlen(str) - i);
		font = bspwmbar_getfont(bar, rune);
		int y = (BAR_HEIGHT - (font->ascent + font->descent) / 2);
		XftTextExtentsUtf8(bar->dpy, font, (FcChar8 *)&str[i], len, &extents);
		XftDrawStringUtf8(draw, color, font, x + width + extents.x, y,
		                  (FcChar8 *)&str[i], len);
		width += extents.x + extents.xOff;
		i += len;
	}
	return width;
}

static int
bspwmbar_drawcpu(Bspwmbar *bar, BarWindow *xw, CoreInfo *a, int nproc, int x)
{
	XGlyphInfo extents;
	int maxh = bar->font.base->ascent;
	int width = 5;
	int basey = maxh / 2;

	for (int i = nproc - 1; i >= 0; i--) {
		int avg = (int)a[i].loadavg;
		int height = BIGGER(maxh * ((double)avg / 100), 1);
		XftColor fg;
		if (avg < 30) {
			fg = cols[4];
		} else if (avg < 60) {
			fg = cols[5];
		} else if (avg < 80) {
			fg = cols[6];
		} else {
			fg = cols[7];
		}
		XSetForeground(bar->dpy, bar->gc, cols[ALTBGCOLOR].pixel);
		XFillRectangle(bar->dpy, xw->win, bar->gc, x - width, basey, width,
		               maxh);

		XSetForeground(bar->dpy, bar->gc, fg.pixel);
		XFillRectangle(bar->dpy, xw->win, bar->gc, x - width,
		               basey + (maxh - height), width, height);
		x -= width + 1;
	}
	int label_width = bspwmbar_getdrawwidth(bar, "cpu: ", &extents);
	bspwmbar_drawstring(bar, xw->draw, &cols[FGCOLOR], "cpu:",
	                    x - label_width);

	return (width + 1) * nproc + label_width;
}

static int
bspwmbar_drawmem(Bspwmbar *bar, BarWindow *xw, size_t memused, int x)
{
	XGlyphInfo extents;
	int width = 5;
	int maxh = bar->font.base->ascent;
	int basey = maxh / 2;

	for (size_t i = 10; i > 0; i--) {
		XftColor fg = cols[ALTBGCOLOR];
		if (i <= 3 && memused >= i * 10)
			fg = cols[4];
		else if (i <= 6 && memused >= i * 10)
			fg = cols[5];
		else if (i <= 8 && memused >= i * 10)
			fg = cols[6];
		else if (memused >= 90)
			fg = cols[7];

		XSetForeground(bar->dpy, bar->gc, fg.pixel);
		XFillRectangle(bar->dpy, xw->win, bar->gc, x - width, basey, width, maxh);
		x -= width + 1;
	}
	int label_width = bspwmbar_getdrawwidth(bar, "mem: ", &extents);
	bspwmbar_drawstring(bar, xw->draw, &cols[FGCOLOR], "mem:", x - label_width);
	return (width + 1) * 10 + label_width;
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
			i = j;
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
	XftColor fg = cols[FGCOLOR];
	XftColor altfg = cols[ALTFGCOLOR];
	XGlyphInfo extents = { 0 };

	/* padding width */
	int pad = bspwmbar_getdrawwidth(bar, "a", &extents);

	Window win = get_active_window(bar->dpy, bar->scr);
	char *title = NULL;
	Bool title_suffix = 0;
	if (win && (title = (char *)get_window_title(bar->dpy, win))) {
		size_t i = 0, len = 0;
		FcChar32 dst;
		for (; i < strlen(title) && len < TITLE_MAXSZ; len++)
			i += FcUtf8ToUcs4((FcChar8 *)&title[i], &dst, strlen(title) - i);
		if (i < strlen(title))
			title_suffix = 1;
		title[i] = '\0';
	}

	CoreInfo *cores;
	int ncore = cpu_perc(&cores);
	int mem = mem_perc();

	for (int i = 0; i < bar->nxbar; i++) {
		BarWindow *xw = &bar->xbars[i];
		int x = 0, width;

		XClearWindow(bar->dpy, xw->win);

		/* render logo */
		x = pad * 2;
		width = bspwmbar_drawstring(bar, xw->draw, &cols[LOGOCOLOR], "", x);
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
			bspwmbar_drawstring(bar, xw->draw, &fg, "…", x);

		x = xw->width - pad;
		for (int j = 0; j < bar->nlabel; j++) {
			char *tmp = bar->labels[j].module->func(bar->labels[j].module->arg);
			if (!tmp)
				continue;
			width = bspwmbar_getdrawwidth(bar, tmp, &extents) + pad;
			x -= width;
			bspwmbar_drawstring(bar, xw->draw, &fg, tmp, x);
			bar->labels[j].width = width;
			bar->labels[j].x = x -= pad;
		}

		/* render mem */
		x -= pad;
		x -= bspwmbar_drawmem(bar, xw, mem, x);
		x -= pad;

		/* render cpu */
		x -= pad;
		x -= bspwmbar_drawcpu(bar, xw, cores, ncore, x);
		x -= pad;

		/* render tray items */
		if (xw->win == tray.win) {
			x -= pad;
			TrayItem *item = tray.items;
			for (; item; item = item->next) {
				if (!item->info.flags)
					continue;
				x -= 16;
				XMoveResizeWindow(tray.dpy, item->win, x, 4, 16, 16);
				x -= pad;
			}
		}
	}

	if (title)
		XFree(title);

	XFlush(bar->dpy);
}

static int
parse_display(const char *dpy, char **host, int *dnum, int *snum)
{
	const char *dpystr;
	dnum = 0; snum = 0;

	if (dpy)
		dpystr = dpy;
	else
		dpystr = getenv("DISPLAY");

	sscanf(dpystr, "%s:%d.%d", buf, dnum, snum);
	*host = strndup(buf, strlen(buf));

	return 0;
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
		if (parse_display(NULL, &host, &dn, &sn))
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
	XGCValues gcv = { 0 };
	XGlyphInfo extents = { 0 };
	int i, j, nmon;

	/* connect bspwm socket */
	if ((bar->fd = bspwm_connect()) == -1)
		die("bspwm_connect(): Failed to connect to the socket\n");

	/* get monitors */
	xrr_mon = XRRGetMonitors(dpy, RootWindow(dpy, scr), 1,
                             &nmon);
	bar->xbars = (BarWindow *)malloc(sizeof(BarWindow) * nmon);
	bar->nxbar = nmon;

	/* create window per monitor */
	xrr_res = XRRGetScreenResources(dpy, RootWindow(dpy, scr));
	for (i = 0; i < xrr_res->noutput; i++) {
		xrr_out = XRRGetOutputInfo(dpy, xrr_res, xrr_res->outputs[i]);
		if (xrr_out->connection == RR_Connected) {
			for (j = 0; j < nmon; j++) {
				if (xrr_res->outputs[i] != xrr_mon[j].outputs[0])
					continue;
				barwindow_init(dpy, scr, xrr_mon[j].x, 0, xrr_mon[j].width,
				               BAR_HEIGHT, &bar->xbars[j]);
				strncpy(bar->xbars[j].monitor.name, xrr_out->name, NAME_MAXSZ);
			}
		}
		XRRFreeOutputInfo(xrr_out);
	}
	XRRFreeScreenResources(xrr_res);
	XRRFreeMonitors(xrr_mon);

	/* initialize */
	bar->dpy = dpy;
	bar->scr = scr;

	/* init labels */
	bar->nlabel = LENGTH(modules);
	for (i = 0; i < bar->nlabel; i++)
		bar->labels[i].module = &modules[i];

	bspwmbar_loadfonts(bar, fontname);
	bspwmbar_getdrawwidth(bar, ascii_table, &extents);

	bar->gc = XCreateGC(dpy, RootWindow(dpy, scr), GCGraphicsExposures, &gcv);

	/* clear background */
	for (i = 0; i < bar->nxbar; i++) {
		XClearWindow(dpy, bar->xbars[i].win);
		XLowerWindow(dpy, bar->xbars[i].win);
		XMapWindow(dpy, bar->xbars[i].win);
	}
	XSync(dpy, 0);

	return 0;
}

void
bspwmbar_destroy(Bspwmbar *bar)
{
	int i;

	close(bar->fd);

	XftFontClose(bar->dpy, bar->font.base);
	FcPatternDestroy(bar->font.pattern);
	FcFontSetDestroy(bar->font.set);
	for (i = 0; i < nfcache; i++)
		XftFontClose(bar->dpy, fcaches[i]);
	free(fcaches);

	for (i = 0; i < bar->nxbar; i++) {
		XftDrawDestroy(bar->xbars[i].draw);
		XDestroyWindow(bar->dpy, bar->xbars[i].win);
	}
	free(bar->xbars);
	XFreeGC(bar->dpy, bar->gc);
}

static int
bspwmbar_send(Bspwmbar *bar, char *cmd, int len)
{
	return send(bar->fd, cmd, len, 0);
}

static void
polling_stop()
{
	if (epfd > 0)
		close(epfd);
}

int
error_handler(Display *dpy, XErrorEvent *err)
{
	switch (err->type) {
	case Success:
	case BadWindow:
		switch (err->request_code) {
		case X_ChangeWindowAttributes:
		case X_GetProperty:
			return 0;
		}
		break;
	default:
		fprintf(stderr, "Unknown Error Code: %d\n", err->type);
	}
	XGetErrorText(dpy, err->error_code, buf, sizeof(buf) - 1);
	fprintf(stderr, "XError: %s\n", buf);
	XGetErrorText(dpy, err->request_code, buf, sizeof(buf) - 1);
	fprintf(stderr, "  MajorCode: %d (%s)\n", err->request_code, buf);
	fprintf(stderr, "  ResourceID: %ld\n", err->resourceid);
	fprintf(stderr, "  SerialNumer: %ld\n", err->serial);

	polling_stop();

	return 0;
}

void
signal_handler(int signum) {
	switch (signum) {
	case SIGINT:
	case SIGTERM:
		polling_stop();
		break;
	}
}

static int
dummy_error_handler(Display *dpy, XErrorEvent *err)
{
	(void)dpy;
	(void)err;
	return 0;
}

int
main(int argc, char *argv[])
{
	(void)(argc);
	(void)(argv);

	char buf[1024];
	Bspwmbar bar = { 0 };
	struct epoll_event ev, events[MAX_EVENTS];
	struct epoll_event xev, aev;
	Display *dpy;
	int xfd, nfd, i, len;
	XEvent event;
	struct sigaction act, oldact;

	act.sa_handler = &signal_handler;
	act.sa_flags = 0;
	sigaction(SIGTERM, &act, &oldact);
	sigaction(SIGINT, &act, &oldact);

	setlocale(LC_ALL, "");

	if (!(dpy = XOpenDisplay(NULL)))
		die("XOpenDisplay(): Failed to open display\n");
	XSetErrorHandler(error_handler);

	load_colors(dpy, DefaultScreen(dpy));

	if (bspwmbar_init(&bar, dpy, DefaultScreen(dpy)))
		die("bspwmbar_init(): Failed to init bspwmbar\n");

	/* tray initialize */
	tray.win = bar.xbars[0].win;
	tray.dpy = dpy;
	XSetErrorHandler(dummy_error_handler);
	if (systray_init(&tray))
		die("systray_init(): Selection already owned by other window\n");
	XSetErrorHandler(error_handler);

	/* subscribe bspwm report */
	if (bspwmbar_send(&bar, SUBSCRIBE_REPORT, LENGTH(SUBSCRIBE_REPORT)) == -1)
		die("bspwmbar_send(): Failed to send command to bspwm\n");

	/* epoll */
	if ((epfd = epoll_create1(0)) == -1)
		die("epoll_create1(): Failed to create epoll fd\n");

	ev.events = EPOLLIN;
	ev.data.fd = bar.fd;
	if (epoll_ctl(epfd, EPOLL_CTL_ADD, bar.fd, &ev) == -1)
		die("epoll_ctl(): Failed to add to epoll fd\n");

	/* polling X11 event */
	for (i = 0; i < bar.nxbar; i++)
		XSelectInput(bar.dpy, bar.xbars[i].win, ButtonPressMask);

	xfd = ConnectionNumber(bar.dpy);
	xev.events = EPOLLIN;
	xev.data.fd = xfd;
	if (epoll_ctl(epfd, EPOLL_CTL_ADD, xfd, &xev) == -1)
		die("epoll_ctl(): Failed to add to epoll xfd\n");

	/* event */
	Window win;
	XSetWindowAttributes attrs;
	attrs.event_mask = PropertyChangeMask;

	int afd = alsa_connect();
	aev.events = EPOLLIN;
	aev.data.fd = afd;

	if (epoll_ctl(epfd, EPOLL_CTL_ADD, afd, &aev) == -1)
		die("epoll_ctl(): Failed to add to epoll afd\n");

	Atom filter = XInternAtom(dpy, "_NET_WM_NAME", 1);
	Atom xembed_info = XInternAtom(dpy, "_XEMBED_INFO", 1);

	/* timerfd */
	struct itimerspec interval = { {1, 0}, {1, 0} };
	int tfd = timerfd_create(CLOCK_REALTIME, 0);
	timerfd_settime(tfd, 0, &interval, NULL);

	struct epoll_event tev;
	tev.events = EPOLLIN;
	tev.data.fd = tfd;

	if (epoll_ctl(epfd, EPOLL_CTL_ADD, tfd, &tev) == -1)
		die("epoll_ctl(): Failed to add to epoll afd\n");

	/* main loop */
	uint64_t tcnt;
	while ((nfd = epoll_wait(epfd, events, MAX_EVENTS, -1)) != -1) {
		int need_render = 0;
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
					need_render = 1;
				}
			} else if (events[i].data.fd == xfd) {
				/* for X11 events */
				while (XPending(bar.dpy)) {
					XNextEvent(bar.dpy, &event);
					switch (event.type) {
					case ButtonPress:
						for (int j = 0; j < bar.nlabel; j++) {
							if (!bar.labels[j].module->handler)
								continue;
							if (bar.labels[j].x < event.xbutton.x &&
							    event.xbutton.x < bar.labels[j].x +
							    bar.labels[j].width) {
								bar.labels[j].module->handler(event);
								need_render = 1;
								break;
							}
						}
						break;
					case PropertyNotify:
						if (event.xproperty.atom == xembed_info)
							systray_handle(&tray, event);
						else if (event.xproperty.atom == filter)
							need_render = 1;
						break;
					case ClientMessage:
						systray_handle(&tray, event);
						need_render = 1;
						break;
					case DestroyNotify:
						systray_remove_item(&tray, event.xdestroywindow.window);
						need_render = 1;
						break;
					}
				}
			} else if (events[i].data.fd == afd) {
				int res = alsa_need_update();
				if (res == -1) {
					/* reconnect to ALSA */
					alsa_disconnect();
					close(afd);
					afd = alsa_connect();
					aev.data.fd = afd;
					if (epoll_ctl(epfd, EPOLL_CTL_ADD, afd, &aev) == -1)
						die("epoll_ctl(): Failed to add to epoll afd\n");
				} else if (res == 1) {
					need_render = 1;
				}
			} else if (events[i].data.fd == tfd) {
				read(tfd, &tcnt, sizeof(uint64_t));
				need_render = 1;
			}
		}
		if (need_render) {
			/* force render after interval */
			timerfd_settime(tfd, 0, &interval, NULL);
			bspwmbar_render(&bar);
		}
	}
CLEANUP:

	close(tfd);
	alsa_disconnect();
	systray_destroy(&tray);
	bspwmbar_destroy(&bar);
	free_colors(dpy, DefaultScreen(dpy));

	return 0;
}
