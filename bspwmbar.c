/* See LICENSE file for copyright and license details. */

#define _XOPEN_SOURCE 700

#include <alloca.h>
#include <X11/Xatom.h>
#include <X11/Xft/Xft.h>
#include <X11/Xproto.h>
#include <X11/extensions/Xrandr.h>
#include <locale.h>
#include <signal.h>
#include <stdint.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <time.h>

#include "bspwmbar.h"
#include "config.h"

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

typedef enum {
	DA_RIGHT = 0,
	DA_LEFT,
	/* currently not supported the below */
	DA_CENTER
} DrawAlign;

typedef struct {
	FcPattern *pattern;
	FcFontSet *set;
	XftFont   *base;
} XFont;

typedef struct {
	int          fd;
	const Poller *poller;
} PollFD;

typedef struct {
	const Module *module;

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

typedef struct _DC {
	BarWindow xbar;
	GC gc;
	DrawAlign align;
	int x;
} DrawCtx;

typedef struct {
	int fd;
	Display *dpy;
	int scr;
	GC gc;
	XFont font;
	DrawCtx *dcs;
	int ndc;

	Label labels[LENGTH(modules)];
	int nlabel;
} Bspwmbar;

static Bspwmbar bar;
static TrayWindow tray;

/* cache Atom */
static Atom filter;
static Atom xembed_info;

static XftColor cols[LENGTH(colors)];
static XftFont **fcaches;
static int nfcache = 0;
static int fcachecap = 0;
static int celwidth = 0;

static int epfd = 0;
static struct epoll_event events[MAX_EVENTS];
static PollFD pollfds[LENGTH(pollers)];

XftColor *
getcolor(int index)
{
	return &cols[index];
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

void
windowtitle(DC dc, const char *suffix)
{
	char *title = NULL;
	Window active = get_active_window(bar.dpy, bar.scr);

	if (active && (title = (char *)get_window_title(bar.dpy, active))) {
		size_t i = 0;
		FcChar32 dst;
		strncpy(buf, title, sizeof(buf));

		for (size_t len = 0; i < strlen(title) && len < TITLE_MAXSZ; len++)
			i += FcUtf8ToUcs4((FcChar8 *)&title[i], &dst, strlen(title) - i);
		if (i < strlen(buf))
			strncpy(&buf[i], suffix, strlen(suffix) + 1);

		drawtext(dc, buf);
		XFree(title);
	}
}

static XftFont *
getfont(FcChar32 rune)
{
	FcResult result;
	FcPattern *pat, *match;
	FcCharSet *charset;
	FcFontSet *fsets[] = { NULL };
	int i, idx;

	/* Lookup character index with default font. */
	idx = XftCharIndex(bar.dpy, bar.font.base, rune);
	if (idx)
		return bar.font.base;

	/* fallback on font cache */
	for (i = 0; i < nfcache; i++) {
		if ((idx = XftCharIndex(bar.dpy, fcaches[i], rune)))
			return fcaches[i];
	}

	/* find font when not found */
	if (i >= nfcache) {
		if (!bar.font.set)
			bar.font.set = FcFontSort(0, bar.font.pattern, 1, 0, &result);
		fsets[0] = bar.font.set;

		if (nfcache >= fcachecap) {
			fcachecap += 8;
			fcaches = realloc(fcaches, fcachecap * sizeof(XftFont *));
		}

		pat = FcPatternDuplicate(bar.font.pattern);
		charset = FcCharSetCreate();

		/* find font that contains rune and scalable */
		FcCharSetAddChar(charset, rune);
		FcPatternAddCharSet(pat, FC_CHARSET, charset);
		FcPatternAddBool(pat, FC_SCALABLE, 1);

		FcConfigSubstitute(0, pat, FcMatchPattern);
		FcDefaultSubstitute(pat);

		match = FcFontSetMatch(0, fsets, 1, pat, &result);
		FcPatternDestroy(pat);

		fcaches[nfcache] = XftFontOpenPattern(bar.dpy, match);
		FcPatternDestroy(match);
		FcCharSetDestroy(charset);

		if (!fcaches[nfcache])
			die("XftFontOpenPattern(): failed seeking fallback font\n");

		i = nfcache++;
	}

	return fcaches[i];
}

static void
loadfonts(const char *patstr)
{
	FcPattern *pat = FcNameParse((FcChar8 *)patstr);
	if (!pat)
		die("loadfonts(): failed parse pattern: %s\n", patstr);

	FcConfigSubstitute(NULL, pat, FcMatchPattern);
	XftDefaultSubstitute(bar.dpy, bar.scr, pat);

	FcResult result;
	FcPattern *match = FcFontMatch(NULL, pat, &result);
	if (!match) {
		FcPatternDestroy(match);
		die("loadfonts(): failed parse pattern: %s\n", patstr);
	}

	if (!(bar.font.base = XftFontOpenPattern(bar.dpy, match))) {
		FcPatternDestroy(pat);
		FcPatternDestroy(match);
		die("loadfonts(): failed open font: %s\n", patstr);
	}
	FcPatternDestroy(match);

	bar.font.pattern = pat;
}

int
getdrawwidth(const char *str, XGlyphInfo *extents)
{
	FcChar32 rune = 0;
	int width = 0, len = 0;
	XftFont *font;
	for (unsigned int i = 0; i < strlen(str); i += len) {
		len = FcUtf8ToUcs4((FcChar8 *)&str[i], &rune, strlen(str) - i);
		font = getfont(rune);
		XftTextExtentsUtf8(bar.dpy, font, (FcChar8 *)&str[i], len, extents);
		width += extents->x + extents->xOff;
	}
	return width;
}

static void
drawspace(DC dc, int num)
{
	DrawCtx *dctx = (DrawCtx *)dc;
	switch ((int)dctx->align) {
	case DA_LEFT:
		dctx->x += num;
		break;
	case DA_RIGHT:
		dctx->x -= num;
		break;
	}
}

static void
drawstring(DC dc, XftColor *color, const char *str)
{
	DrawCtx *dctx = (DrawCtx *)dc;
	XGlyphInfo extents = { 0 };
	FcChar32 rune = 0;
	int width = 0, len = 0;
	XftFont *font;

	if (dctx->align == DA_RIGHT)
		dctx->x -= getdrawwidth(str, &extents);

	for (unsigned int i = 0; i < strlen(str); i += len) {
		int len = FcUtf8ToUcs4((FcChar8 *)&str[i], &rune, strlen(str) - i);
		font = getfont(rune);
		int y = (BAR_HEIGHT - (font->ascent + font->descent) / 2);
		XftTextExtentsUtf8(bar.dpy, font, (FcChar8 *)&str[i], len, &extents);
		XftDrawStringUtf8(dctx->xbar.draw, color, font,
		                  dctx->x + width + extents.x, y,
		                  (FcChar8 *)&str[i], len);
		width += extents.x + extents.xOff;
		i += len;
	}

	if (dctx->align == DA_LEFT)
		dctx->x += width;
}

void
drawtext(DC dc, const char *str)
{
	drawspace(dc, celwidth);
	drawstring(dc, &cols[FGCOLOR], str);
	drawspace(dc, celwidth);
}

static void
drawcpu(DC dc, CoreInfo *a, int nproc)
{
	DrawCtx *dctx = (DrawCtx *)dc;
	int maxh = bar.font.base->ascent;
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
		XSetForeground(bar.dpy, dctx->gc, cols[ALTBGCOLOR].pixel);
		XFillRectangle(bar.dpy, dctx->xbar.win, dctx->gc, dctx->x - width, basey, width,
		               maxh);

		XSetForeground(bar.dpy, dctx->gc, fg.pixel);
		XFillRectangle(bar.dpy, dctx->xbar.win, dctx->gc, dctx->x - width,
		               basey + (maxh - height), width, height);
		dctx->x -= width + 1;
	}
	drawstring(dc, &cols[FGCOLOR], "cpu: ");
}

static void
drawmem(DC dc, size_t memused)
{
	DrawCtx *dctx = (DrawCtx *)dc;
	int width = 5;
	int maxh = bar.font.base->ascent;
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

		XSetForeground(bar.dpy, dctx->gc, fg.pixel);
		XFillRectangle(bar.dpy, dctx->xbar.win, dctx->gc, dctx->x - width,
		               basey, width, maxh);
		dctx->x -= width + 1;
	}
	drawstring(dc, &cols[FGCOLOR], "mem: ");
}

static void
bspwm_parse(char *report)
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
			for (j = 0; j < bar.ndc; j++)
				if (!strncmp(bar.dcs[j].xbar.monitor.name, name,
				             strlen(name)))
					curmon = &bar.dcs[j].xbar.monitor;
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
logo(DC dc, const char *args)
{
	DrawCtx *dctx = (DrawCtx *)dc;
	int pad = 0;

	switch ((int)dctx->align) {
	case DA_LEFT:
		pad = celwidth;
		break;
	case DA_RIGHT:
		pad = -celwidth;
		break;
	}
	dctx->x += pad;
	drawstring(dc, &cols[LOGOCOLOR], args);
	dctx->x += pad;
}

void
float_right(DC dc, const char *arg)
{
	(void)arg;

	DrawCtx *dctx = (DrawCtx *)dc;
	dc->x = dctx->xbar.width - celwidth;
	dc->align = DA_RIGHT;
}

static void
render_label(DrawCtx *dc)
{
	int x = 0, width = 0, pad = 0;

	for (int j = 0; j < bar.nlabel; j++) {
		x = dc->x; width = 0; pad = 0;

		bar.labels[j].module->func(dc, bar.labels[j].module->arg);
		switch ((int)dc->align) {
		case DA_LEFT:
			width = dc->x - x;
			pad = celwidth;
			break;
		case DA_RIGHT:
			width = x - dc->x;
			pad = -celwidth;
			break;
		}
		x = dc->x;
		if (width) {
			width += celwidth;
			x = dc->x + pad;
		}
		bar.labels[j].width = width;
		bar.labels[j].x = x;
	}
}

static void
render()
{
	XGlyphInfo extents = { 0 };

	/* padding width */
	if (!celwidth)
		celwidth = getdrawwidth("a", &extents);

	CoreInfo *cores;
	int ncore = cpu_perc(&cores);
	int mem = mem_perc();

	for (int i = 0; i < bar.ndc; i++) {
		DC dc = (DC)&bar.dcs[i];
		BarWindow *xw = &bar.dcs[i].xbar;
		dc->align = DA_LEFT;
		dc->x = 0;

		XClearWindow(bar.dpy, xw->win);

		/* render modules */
		dc->x += celwidth;
		render_label(dc);

		/* render mem */
		dc->x -= celwidth;
		drawmem(dc, mem);
		dc->x -= celwidth;

		/* render cpu */
		dc->x -= celwidth;
		drawcpu((DC)dc, cores, ncore);
		dc->x -= celwidth;

		/* render tray items */
		if (xw->win == tray.win) {
			dc->x -= celwidth;
			TrayItem *item = tray.items;
			for (; item; item = item->next) {
				if (!item->info.flags)
					continue;
				dc->x -= 16;
				XMoveResizeWindow(tray.dpy, item->win, dc->x, 4, 16, 16);
				dc->x -= celwidth;
			}
		}
	}

	XSync(bar.dpy, bar.scr);
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

static int
bspwmbar_init(Display *dpy, int scr)
{
	XRRScreenResources *xrr_res;
	XRRMonitorInfo *xrr_mon;
	XRROutputInfo *xrr_out;
	XGCValues gcv = { 0 };
	XGlyphInfo extents = { 0 };
	int i, j, nmon;

	/* connect bspwm socket */
	if ((bar.fd = bspwm_connect()) == -1)
		die("bspwm_connect(): Failed to connect to the socket\n");

	/* get monitors */
	xrr_mon = XRRGetMonitors(dpy, RootWindow(dpy, scr), 1,
                             &nmon);
	bar.dcs = (DrawCtx *)malloc(sizeof(DrawCtx) * nmon);
	bar.ndc = nmon;

	/* create window per monitor */
	xrr_res = XRRGetScreenResources(dpy, RootWindow(dpy, scr));
	for (i = 0; i < xrr_res->noutput; i++) {
		xrr_out = XRRGetOutputInfo(dpy, xrr_res, xrr_res->outputs[i]);
		if (xrr_out->connection == RR_Connected) {
			for (j = 0; j < nmon; j++) {
				if (xrr_res->outputs[i] != xrr_mon[j].outputs[0])
					continue;
				barwindow_init(dpy, scr, xrr_mon[j].x, 0, xrr_mon[j].width,
				               BAR_HEIGHT, &bar.dcs[j].xbar);
				strncpy(bar.dcs[j].xbar.monitor.name, xrr_out->name,
				        NAME_MAXSZ);
			}
		}
		XRRFreeOutputInfo(xrr_out);
	}
	XRRFreeScreenResources(xrr_res);
	XRRFreeMonitors(xrr_mon);

	/* initialize */
	bar.dpy = dpy;
	bar.scr = scr;

	/* init labels */
	bar.nlabel = LENGTH(modules);
	for (i = 0; i < bar.nlabel; i++)
		bar.labels[i].module = &modules[i];

	loadfonts(fontname);
	getdrawwidth(ascii_table, &extents);

	bar.gc = XCreateGC(dpy, RootWindow(dpy, scr), GCGraphicsExposures, &gcv);

	/* clear background */
	for (i = 0; i < bar.ndc; i++) {
		bar.dcs[i].gc = bar.gc;
		XClearWindow(dpy, bar.dcs[i].xbar.win);
		XLowerWindow(dpy, bar.dcs[i].xbar.win);
		XMapWindow(dpy, bar.dcs[i].xbar.win);
	}
	XSync(dpy, 0);

	return 0;
}

static void
bspwmbar_destroy()
{
	int i;

	close(bar.fd);

	XftFontClose(bar.dpy, bar.font.base);
	FcPatternDestroy(bar.font.pattern);
	FcFontSetDestroy(bar.font.set);
	for (i = 0; i < nfcache; i++)
		XftFontClose(bar.dpy, fcaches[i]);
	free(fcaches);

	for (i = 0; i < bar.ndc; i++) {
		XftDrawDestroy(bar.dcs[i].xbar.draw);
		XDestroyWindow(bar.dpy, bar.dcs[i].xbar.win);
	}
	free(bar.dcs);
	XFreeGC(bar.dpy, bar.gc);
}

static int
bspwm_send(char *cmd, int len)
{
	return send(bar.fd, cmd, len, 0);
}

void
workspace(DC dc, const char *args)
{
	(void)args;
	XftColor col;
	DrawCtx *dctx = (DrawCtx *)dc;
	BarWindow *xw = (BarWindow *)dc;
	const char *ws;
	int cur, max = xw->monitor.nworkspaces;

	drawspace(dc, celwidth);
	for (int i = 0, j = max - 1; i < max; i++, j--) {
		cur = (dctx->align == DA_RIGHT) ? j : i;
		drawspace(dc, celwidth / 2);
		ws = (xw->monitor.workspaces[cur].state & STATE_ACTIVE)
		     ? workspace_chars[0]
		     : workspace_chars[1];
		col = (xw->monitor.workspaces[cur].state == STATE_FREE)
		      ? cols[ALTFGCOLOR]
		      : cols[FGCOLOR];
		drawstring(dc, &col, ws);
		drawspace(dc, celwidth / 2);
	}
	drawspace(dc, celwidth);
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

static void
poll_add(PollFD *pollfd)
{
	struct epoll_event ev;

	ev.events = EPOLLIN;
	ev.data.fd = pollfd->fd;
	ev.data.ptr = (void *)pollfd;

	if (epoll_ctl(epfd, EPOLL_CTL_ADD, pollfd->fd, &ev) == -1)
		die("epoll_ctl(): Failed to add to epoll fd\n");
}

static void
poll_del(PollFD *pollfd)
{
	if (pollfd->poller && pollfd->poller->deinit)
		pollfd->poller->deinit();
	if (pollfd->fd) {
		epoll_ctl(epfd, EPOLL_CTL_DEL, pollfd->fd, NULL);
		close(pollfd->fd);
	}
}

static void
poll_init()
{
	for (unsigned long i = 0; i < LENGTH(pollers); i++) {
		pollfds[i].fd = pollers[i].init();
		pollfds[i].poller = &pollers[i];
		if (!pollfds[i].fd)
			die("poll_init(): pollers[%d].init() returns NULL\n", i);
		poll_add(&pollfds[i]);
	}
}

static void
poll_deinit()
{
	for (unsigned long i = 0; i < LENGTH(pollfds); i++)
		poll_del(&pollfds[i]);
}

static PollResult
timer_reset(int fd)
{
	uint64_t tcnt;
	read(fd, &tcnt, sizeof(uint64_t));
	return 1;
}

static PollResult
bspwm_handle(int fd)
{
	size_t len;
	Window win;
	XSetWindowAttributes attrs;
	attrs.event_mask = PropertyChangeMask;

	if ((len = recv(fd, buf, sizeof(buf) - 1, 0)) > 0) {
		buf[len] = '\0';
		if (buf[0] == '\x07') {
			fprintf(stderr, "bspwm: %s", buf + 1);
			return PR_FAILED;
		}
		bspwm_parse(buf);
		if ((win = get_active_window(bar.dpy, bar.scr)))
			XChangeWindowAttributes(bar.dpy, win, CWEventMask,
			                        &attrs);
		return PR_UPDATE;
	}
	return PR_NOOP;
}

static PollResult
xev_handle()
{
	XEvent event;
	PollResult res = PR_NOOP;

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
					res = PR_UPDATE;
					break;
				}
			}
			break;
		case PropertyNotify:
			if (event.xproperty.atom == xembed_info)
				systray_handle(&tray, event);
			else if (event.xproperty.atom == filter)
				res = PR_UPDATE;
			break;
		case ClientMessage:
			systray_handle(&tray, event);
			res = PR_UPDATE;
			break;
		case DestroyNotify:
			systray_remove_item(&tray, event.xdestroywindow.window);
			res = PR_UPDATE;
			break;
		}
	}
	return res;
}

static void
poll_loop(void (* handler)())
{
	int i, nfd, need_render;
	PollFD *pollfd;

	/* timer for rendering at one sec interval */
	struct itimerspec interval = { {1, 0}, {1, 0} };
	/* initialize timer */
	int tfd = timerfd_create(CLOCK_REALTIME, 0);
	timerfd_settime(tfd, 0, &interval, NULL);

	Poller poller = { NULL, NULL, timer_reset };
	PollFD timer = { tfd, &poller };
	poll_add(&timer);

	/* polling X11 event for modules */
	Poller xpoll = { NULL, NULL, xev_handle };
	PollFD xfd = { ConnectionNumber(bar.dpy), &xpoll };
	poll_add(&xfd);

	/* polling fd */
	while ((nfd = epoll_wait(epfd, events, MAX_EVENTS, -1)) != -1) {
		need_render = 0;
		for (i = 0; i < nfd; i++) {
			pollfd = (PollFD *)events[i].data.ptr;
			switch ((int)pollfd->poller->handler(pollfd->fd)) {
			case PR_UPDATE:
				need_render = 1;
				break;
			case PR_REINIT:
				poll_del(pollfd);
				pollfd->fd = pollfd->poller->init();
				poll_add(pollfd);
				break;
			}
		}
		if (need_render) {
			/* force render after interval */
			timerfd_settime(tfd, 0, &interval, NULL);
			handler();
		}
	}
}

static void
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

	Display *dpy;
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

	if (bspwmbar_init(dpy, DefaultScreen(dpy)))
		die("bspwmbar_init(): Failed to init bspwmbar\n");

	/* tray initialize */
	tray.win = bar.dcs[0].xbar.win;
	tray.dpy = dpy;
	XSetErrorHandler(dummy_error_handler);
	if (systray_init(&tray))
		die("systray_init(): Selection already owned by other window\n");
	XSetErrorHandler(error_handler);

	/* subscribe bspwm report */
	if (bspwm_send(SUBSCRIBE_REPORT, LENGTH(SUBSCRIBE_REPORT)) == -1)
		die("bspwm_send(): Failed to send command to bspwm\n");

	/* epoll */
	if ((epfd = epoll_create1(0)) == -1)
		die("epoll_create1(): Failed to create epoll fd\n");

	/* polling bspwm report */
	Poller bpoll = { NULL, NULL, bspwm_handle };
	PollFD bfd = { bar.fd, &bpoll };
	poll_add(&bfd);

	/* polling X11 event for modules */
	for (int i = 0; i < bar.ndc; i++)
		XSelectInput(bar.dpy, bar.dcs[i].xbar.win, ButtonPressMask);

	/* cache Atom */
	filter = XInternAtom(bar.dpy, "_NET_WM_NAME", 1);
	xembed_info = XInternAtom(bar.dpy, "_XEMBED_INFO", 1);

	/* polling initialize for modules */
	poll_init();

	/* main loop */
	poll_loop(render);

	poll_deinit();
	systray_destroy(&tray);
	bspwmbar_destroy();
	free_colors(dpy, DefaultScreen(dpy));

	return 0;
}
