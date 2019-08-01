/* See LICENSE file for copyright and license details. */

#if defined(__linux)
# define _XOPEN_SOURCE 700
# include <alloca.h>
# include <sys/epoll.h>
# include <sys/timerfd.h>
# include <sys/un.h>
#elif defined(__OpenBSD__)
# include <sys/types.h>
# include <sys/event.h>
# include <sys/time.h>
#endif
#include <sys/socket.h>
#include <sys/un.h>
#include <X11/Xatom.h>
#include <X11/Xft/Xft.h>
#include <X11/Xproto.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/Xdbe.h>
#include <locale.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bspwmbar.h"
#include "config.h"

/* bspwm commands */
#define SUBSCRIBE_REPORT "subscribe\0report"
/* epoll max events */
#define MAX_EVENTS 10

char buf[1024];
char *wintitle = NULL;

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
	Monitor monitor;

	int x, y, width, height;
} BarWindow;

typedef struct _DC {
	XdbeSwapInfo swapinfo;
	BarWindow xbar;
	GC gc;
	XftDraw *draw;
	Drawable buf;
	DrawAlign align;
	int x;

	Label labels[LENGTH(modules)];
	int nlabel;
} DrawCtx;

typedef struct {
	int fd;
	Display *dpy;
	int scr;
	XFont font;
	DrawCtx *dcs;
	int ndc;
} Bspwmbar;

static Bspwmbar bar;
static TrayWindow tray;

/* cache Atom */
static Atom filter;
static Atom xembed_info;

static XVisualInfo *visinfo;
static XftColor cols[LENGTH(colors)];
static XftFont **fcaches;
static int nfcache = 0;
static int fcachecap = 0;
static int celwidth = 0;
static int xdbe_support = 0;

static int pfd = 0;
#if defined(__linux)
static struct epoll_event events[MAX_EVENTS];
#elif defined(__OpenBSD__)
static struct kevent events[MAX_EVENTS];
#endif
static list_head pollfds;

static int error_handler(Display *dpy, XErrorEvent *err);
static int dummy_error_handler(Display *dpy, XErrorEvent *err);

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

XVisualInfo *
get_visualinfo(Display *dpy)
{
	if (visinfo)
		return visinfo;

	int nscreen = 1;
	Drawable screens[] = { DefaultRootWindow(dpy) };
	XdbeScreenVisualInfo *info = XdbeGetVisualInfo(dpy, screens, &nscreen);
	if (!info || nscreen < 1 || info->count < 1)
		die("XdbeScreenVisualInfo(): does not supported\n");

	XVisualInfo vistmpl;
	vistmpl.visualid = info->visinfo[0].visual;
	vistmpl.screen = 0;
	vistmpl.depth = info->visinfo[0].depth;
	XdbeFreeVisualInfo(info);

	int nmatch;
	int mask = VisualIDMask | VisualScreenMask | VisualDepthMask;
	visinfo = XGetVisualInfo(dpy, mask, &vistmpl, &nmatch);

	if (!visinfo || nmatch < 1)
		die("couldn't match a Visual with double buffering\n");
	return visinfo;
}

static void
drawctx_init(DrawCtx *dc, Display *dpy, int scr, int x, int y, int width,
             int height)
{
	XGCValues gcv = { 0 };
	XSetWindowAttributes wattrs;
	XClassHint *hint;
	BarWindow *xw = &dc->xbar;

	wattrs.background_pixel = cols[BGCOLOR].pixel;
	wattrs.event_mask = NoEventMask;

	Visual *vis;
	if (xdbe_support)
		vis = get_visualinfo(dpy)->visual;
	else
		vis = DefaultVisual(dpy, scr);
	xw->win = XCreateWindow(dpy, RootWindow(dpy, scr), x, y, width, height, 0,
	                        CopyFromParent, CopyFromParent, vis,
	                        CWBackPixel | CWEventMask, &wattrs);

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

	/* create graphic context */
	if (xdbe_support)
		dc->buf = XdbeAllocateBackBufferName(dpy, xw->win, XdbeBackground);
	else
		dc->buf = xw->win;
	dc->gc = XCreateGC(dpy, dc->buf, GCGraphicsExposures, &gcv);
	dc->draw = XftDrawCreate(dpy, dc->buf, vis, DefaultColormap(dpy, scr));
	dc->swapinfo.swap_window = dc->xbar.win;
	dc->swapinfo.swap_action = XdbeBackground;

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

static void
windowtitle_update(Display *dpy, int scr)
{
	Window win;
	if ((win = get_active_window(dpy, scr))) {
		if (wintitle)
			XFree(wintitle);
		wintitle = (char *)get_window_title(dpy, win);
	} else {
		/* release wintitle when active window not found */
		XFree(wintitle);
		wintitle = NULL;
	}
}

void
windowtitle(DC dc, const char *suffix)
{
	if (!wintitle)
		return;

	size_t i = 0;
	FcChar32 dst;
	strncpy(buf, wintitle, sizeof(buf));
	for (size_t len = 0; i < strlen(wintitle) && len < TITLE_MAXSZ; len++)
		i += FcUtf8ToUcs4((FcChar8 *)&wintitle[i], &dst, strlen(wintitle) - i);
	if (i < strlen(buf))
		strncpy(&buf[i], suffix, sizeof(buf) - i);

	drawtext(dc, buf);
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

static int
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
		fprintf(stderr, "loadfonts(): failed parse pattern: %s\n", patstr);
		FcPatternDestroy(pat);
		return 1;
	}

	if (!(bar.font.base = XftFontOpenPattern(bar.dpy, match))) {
		die("loadfonts(): failed open font: %s\n", patstr);
		FcPatternDestroy(pat);
		FcPatternDestroy(match);
		return 1;
	}

	FcPatternDestroy(match);

	bar.font.pattern = pat;
	return 0;
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

	int y = (BAR_HEIGHT - bar.font.base->height) / 2 + bar.font.base->ascent;
	for (unsigned int i = 0; i < strlen(str); i += len) {
		int len = FcUtf8ToUcs4((FcChar8 *)&str[i], &rune, strlen(str) - i);
		font = getfont(rune);
		XftTextExtentsUtf8(bar.dpy, font, (FcChar8 *)&str[i], len, &extents);
		XftDrawStringUtf8(dctx->draw, color, font, dctx->x + width + extents.x,
		                  y, (FcChar8 *)&str[i], len);
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

void
drawcpu(DC dc, CoreInfo *a, int nproc)
{
	DrawCtx *dctx = (DrawCtx *)dc;
	int maxh = bar.font.base->ascent;
	int basey = (BAR_HEIGHT - bar.font.base->ascent) / 2;

	drawspace(dc, celwidth);
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
		XFillRectangle(bar.dpy, dctx->buf, dctx->gc, dctx->x - celwidth,
		               basey, celwidth, maxh);

		XSetForeground(bar.dpy, dctx->gc, fg.pixel);
		XFillRectangle(bar.dpy, dctx->buf, dctx->gc, dctx->x - celwidth,
		               basey + (maxh - height), celwidth, height);
		dctx->x -= celwidth + 1;
	}
	drawstring(dc, &cols[FGCOLOR], "cpu: ");
	drawspace(dc, celwidth);
}

void
drawmem(DC dc, int memused)
{
	DrawCtx *dctx = (DrawCtx *)dc;
	int maxh = bar.font.base->ascent;
	int basey = (BAR_HEIGHT - bar.font.base->ascent) / 2;

	drawspace(dc, celwidth);
	for (int i = 9; i >= 0; i--) {
		XftColor fg = cols[ALTBGCOLOR];
		if (i <= 2 && memused >= i * 10)
			fg = cols[4];
		else if (i <= 5 && memused >= i * 10)
			fg = cols[5];
		else if (i <= 7 && memused >= i * 10)
			fg = cols[6];
		else if (memused >= 90)
			fg = cols[7];

		XSetForeground(bar.dpy, dctx->gc, fg.pixel);
		XFillRectangle(bar.dpy, dctx->buf, dctx->gc, dctx->x - celwidth,
		               basey, celwidth, maxh);
		dctx->x -= celwidth + 1;
	}
	drawstring(dc, &cols[FGCOLOR], "mem: ");
	drawspace(dc, celwidth);
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
		case 'L':
		case 'T':
			i++; /* skip next char. */
			break;
		case 'G':
			if (curmon)
				curmon->nworkspaces = nws;
			/* skip current node flags. */
			while (report[i + 1] != ':' && report[i + 1] != '\n')
				i++;
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

	for (int j = 0; j < dc->nlabel; j++) {
		x = dc->x; width = 0; pad = 0;

		dc->labels[j].module->func(dc, dc->labels[j].module->arg);
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
		dc->labels[j].width = width;
		dc->labels[j].x = x;
	}
}

static void
render()
{
	XGlyphInfo extents = { 0 };

	/* padding width */
	if (!celwidth)
		celwidth = getdrawwidth("a", &extents);

	for (int i = 0; i < bar.ndc; i++) {
		DC dc = (DC)&bar.dcs[i];
		BarWindow *xw = &bar.dcs[i].xbar;
		dc->align = DA_LEFT;
		dc->x = 0;

		XClearWindow(bar.dpy, xw->win);

		/* render modules */
		dc->x += celwidth;
		render_label(dc);

		/* swap buffer */
		if (xdbe_support)
			XdbeSwapBuffers(bar.dpy, &bar.dcs[i].swapinfo, 1);
	}
	XFlush(bar.dpy);
}

static int
bspwm_connect(Display *dpy, int scr)
{
	struct sockaddr_un sock;
	int fd;
	char *sp = NULL, *host = NULL;

	sock.sun_family = AF_UNIX;
	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
		return -1;

	sp = getenv("BSPWM_SOCKET");
	if (sp) {
		snprintf(sock.sun_path, sizeof(sock.sun_path), "%s", sp);
	} else {
		snprintf(sock.sun_path, sizeof(sock.sun_path),
		         "/tmp/bspwm_%i_%i-socket", ConnectionNumber(dpy), scr);
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
	XGlyphInfo extents = { 0 };
	Window root = RootWindow(dpy, scr);
	int i, j, nmon;

	/* connect bspwm socket */
	if ((bar.fd = bspwm_connect(dpy, scr)) == -1)
		die("bspwm_connect(): Failed to connect to the socket\n");

	/* get monitors */
	xrr_mon = XRRGetMonitors(dpy, root, 1, &nmon);
	bar.dcs = (DrawCtx *)calloc(nmon, sizeof(DrawCtx));
	bar.ndc = nmon;

	/* create window per monitor */
	xrr_res = XRRGetScreenResources(dpy, root);
	for (i = 0; i < xrr_res->noutput; i++) {
		xrr_out = XRRGetOutputInfo(dpy, xrr_res, xrr_res->outputs[i]);
		if (xrr_out->connection == RR_Connected) {
			for (j = 0; j < nmon; j++) {
				if (xrr_res->outputs[i] != xrr_mon[j].outputs[0])
					continue;
				drawctx_init(&bar.dcs[j], dpy, scr, xrr_mon[j].x, 0,
				             xrr_mon[j].width, BAR_HEIGHT);
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

	if (loadfonts(fontname))
		return 1;
	getdrawwidth(ascii_table, &extents);

	/* clear background */
	for (i = 0; i < bar.ndc; i++) {
		XClearWindow(dpy, bar.dcs[i].xbar.win);
		XLowerWindow(dpy, bar.dcs[i].xbar.win);
		XMapWindow(dpy, bar.dcs[i].xbar.win);

		/* init labels */
		bar.dcs[i].nlabel = LENGTH(modules);
		for (j = 0; j < bar.dcs[i].nlabel; j++)
			bar.dcs[i].labels[j].module = &modules[j];
	}
	XFlush(dpy);

	return 0;
}

static void
bspwmbar_destroy()
{
	int i;

	close(bar.fd);

	XftFontClose(bar.dpy, bar.font.base);
	FcPatternDestroy(bar.font.pattern);
	if (bar.font.set)
		FcFontSetDestroy(bar.font.set);
	for (i = 0; i < nfcache; i++)
		XftFontClose(bar.dpy, fcaches[i]);
	free(fcaches);

	for (i = 0; i < bar.ndc; i++) {
		XFreeGC(bar.dpy, bar.dcs[i].gc);
		XftDrawDestroy(bar.dcs[i].draw);
		XDestroyWindow(bar.dpy, bar.dcs[i].xbar.win);
	}
	free(bar.dcs);
	if (xdbe_support)
		free(visinfo);
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
	const char *ws;
	int cur, max = dctx->xbar.monitor.nworkspaces;

	drawspace(dc, celwidth);
	for (int i = 0, j = max - 1; i < max; i++, j--) {
		cur = (dctx->align == DA_RIGHT) ? j : i;
		drawspace(dc, celwidth / 2);
		ws = (dctx->xbar.monitor.workspaces[cur].state & STATE_ACTIVE)
		     ? workspace_chars[0]
		     : workspace_chars[1];
		col = (dctx->xbar.monitor.workspaces[cur].state == STATE_FREE)
		      ? cols[ALTFGCOLOR]
		      : cols[FGCOLOR];
		drawstring(dc, &col, ws);
		drawspace(dc, celwidth / 2);
	}
	drawspace(dc, celwidth);
}

void
systray(DC dc, const char *arg)
{
	(void)arg;
	if (list_empty(&tray.items))
		return;

	DrawCtx *dctx = (DrawCtx *)dc;
	if (tray.win != dctx->xbar.win)
		return;

	XSetErrorHandler(dummy_error_handler);

	drawspace(dc, celwidth);
	list_head *pos;
	list_for_each(&tray.items, pos) {
		TrayItem *item = list_entry(pos, TrayItem, head);
		if (!item->info.flags)
			continue;
		drawspace(dc, TRAY_ICONSZ);
		if (item->x != dctx->x) {
			item->x = dctx->x;
			XMoveResizeWindow(tray.dpy, item->win, item->x,
			                  (BAR_HEIGHT - TRAY_ICONSZ) / 2, TRAY_ICONSZ,
			                  TRAY_ICONSZ);
		}
		drawspace(dc, celwidth);
	}
	drawspace(dc, celwidth);

	XSetErrorHandler(error_handler);
}

static void
polling_stop()
{
	if (pfd > 0)
		close(pfd);
}

static int
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
poll_add(PollFD *pollfd)
{
	(void)pollfd;
#if defined(__linux)
	struct epoll_event ev;

	ev.events = EPOLLIN;
	ev.data.fd = pollfd->fd;
	ev.data.ptr = (void *)pollfd;

	if (epoll_ctl(pfd, EPOLL_CTL_ADD, pollfd->fd, &ev) == -1)
		die("epoll_ctl(): failed to add to epoll\n");
#elif defined(__OpenBSD__)
	struct kevent ev = { 0 };

	EV_SET(&ev, pollfd->fd, EVFILT_READ, EV_ADD, 0, 0, pollfd);
	if (kevent(pfd, &ev, 1, NULL, 0, NULL) == -1)
		die("EV_SET(): failed to add to kqueue\n");
#endif
}

void
poll_del(PollFD *pollfd)
{
	if (pollfd->deinit)
		pollfd->deinit();
	if (pollfd->fd) {
#if defined(__linux)
		epoll_ctl(pfd, EPOLL_CTL_DEL, pollfd->fd, NULL);
#elif defined(__OpenBSD__)
		struct kevent ev = { 0 };
		EV_SET(&ev, pollfd->fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
		kevent(pfd, &ev, 1, NULL, 0, NULL);
#endif
		close(pollfd->fd);
	}
}

static void
poll_init()
{
	list_head_init(&pollfds);
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
	DrawCtx *dctx;

	/* for X11 events */
	while (XPending(bar.dpy)) {
		XNextEvent(bar.dpy, &event);
		switch (event.type) {
		case SelectionClear:
			systray_handle(&tray, event);
			break;
		case Expose:
			res = PR_UPDATE;
			break;
		case ButtonPress:
			dctx = NULL;
			for (int j = 0; j < bar.ndc; j++)
				if (bar.dcs[j].xbar.win == event.xbutton.window)
					dctx = &bar.dcs[j];
			if (!dctx)
				break;
			/* handle evnent */
			for (int j = 0; j < dctx->nlabel; j++) {
				if (!dctx->labels[j].module->handler)
					continue;
				if (dctx->labels[j].x < event.xbutton.x &&
				    event.xbutton.x < dctx->labels[j].x +
				    dctx->labels[j].width) {
					dctx->labels[j].module->handler(event);
					res = PR_UPDATE;
					break;
				}
			}
			break;
		case PropertyNotify:
			if (event.xproperty.atom == xembed_info) {
				systray_handle(&tray, event);
			} else if (event.xproperty.atom == filter) {
				windowtitle_update(bar.dpy, bar.scr);
				res = PR_UPDATE;
			}
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

#if defined(__linux)
	/* timer for rendering at one sec interval */
	struct itimerspec interval = { {1, 0}, {1, 0} };
	/* initialize timer */
	int tfd = timerfd_create(CLOCK_REALTIME, 0);
	timerfd_settime(tfd, 0, &interval, NULL);

	PollFD timer = { tfd, NULL, NULL, timer_reset, { 0 } };
	poll_add(&timer);
#elif defined(__OpenBSD__)
	/* dummy */
	(void)timer_reset;
#endif

	/* polling X11 event for modules */
	PollFD xfd = { ConnectionNumber(bar.dpy), NULL, NULL, xev_handle, { 0 } };
	poll_add(&xfd);

	/* polling fd */
#if defined(__linux)
	while ((nfd = epoll_wait(pfd, events, MAX_EVENTS, -1)) != -1) {
		need_render = 0;
#elif defined(__OpenBSD__)
	struct timespec tspec = { 0 };
	tspec.tv_sec = 1;
	while ((nfd = kevent(pfd, NULL, 0, events, MAX_EVENTS, &tspec)) != -1) {
		need_render = 0;
		if (!nfd)
			need_render = 1;
#endif
		for (i = 0; i < nfd; i++) {
#if defined(__linux)
			pollfd = (PollFD *)events[i].data.ptr;
#elif defined(__OpenBSD__)
			pollfd = (PollFD *)events[i].udata;
#endif
			switch ((int)pollfd->handler(pollfd->fd)) {
			case PR_UPDATE:
				need_render = 1;
				break;
			case PR_REINIT:
				poll_del(pollfd);
				pollfd->fd = pollfd->init();
				poll_add(pollfd);
				break;
			}
		}
		if (need_render) {
#if defined(__linux)
			/* force render after interval */
			timerfd_settime(tfd, 0, &interval, NULL);
#endif
			windowtitle_update(bar.dpy, bar.scr);
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

	/* Xdbe initialize */
	int major, minor;
	if (XdbeQueryExtension(dpy, &major, &minor))
		xdbe_support = 1;
#ifdef DISABLE_XDBE
	xdbe_support = 0;
#endif

	/* get active widnow title */
	windowtitle_update(dpy, DefaultScreen(dpy));

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

#if defined(__linux)
	/* epoll */
	if ((pfd = epoll_create1(0)) == -1)
		die("epoll_create1(): Failed to create epoll fd\n");
#elif defined(__OpenBSD__)
	if (!(pfd = kqueue()))
		die("kqueue(): Failed to create kqueue fd\n");
#endif

	/* polling bspwm report */
	PollFD bfd = { bar.fd, NULL, NULL, bspwm_handle, { 0 } };
	poll_add(&bfd);

	/* polling X11 event for modules */
	for (int i = 0; i < bar.ndc; i++)
		XSelectInput(bar.dpy, bar.dcs[i].xbar.win,
		             ButtonPressMask | ExposureMask);

	/* cache Atom */
	filter = XInternAtom(bar.dpy, "_NET_WM_NAME", 1);
	xembed_info = XInternAtom(bar.dpy, "_XEMBED_INFO", 1);

	/* polling initialize for modules */
	poll_init();

	/* main loop */
	poll_loop(render);

	if (wintitle)
		XFree(wintitle);

	systray_destroy(&tray);
	bspwmbar_destroy();
	free_colors(dpy, DefaultScreen(dpy));

	return 0;
}
