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

/* common libraries */
#include <sys/socket.h>
#include <sys/un.h>
#include <locale.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* X11 */
#include <X11/Xatom.h>
#include <X11/Xft/Xft.h>
#include <X11/Xproto.h>
#include <X11/extensions/Xrandr.h>
#include <X11/extensions/Xdbe.h>

/* local headers */
#include "bspwmbar.h"
#include "systray.h"
#include "config.h"

/* bspwm commands */
#define SUBSCRIBE_REPORT "subscribe\0report"
/* epoll max events */
#define MAX_EVENTS 10

/* temporary buffer */
char buf[1024];
static XftCharFontSpec glyph_caches[1024];

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
    DrawAlign align;

    int x, width;
} Label;

typedef int WsState;

typedef struct {
    char name[NAME_MAXSZ];
    WsState state;
} Desktop;

typedef struct {
    char name[NAME_MAXSZ];
    Desktop *desktops;
    int ndesktop; /* num of desktops */
    int cdesktop; /* cap of desktops */
    Bool is_active;
} Monitor;

typedef struct {
    Window win;
    Monitor monitor;

    int x, y, width, height;
} BarWindow;

struct _DC {
    XdbeSwapInfo swapinfo;
    BarWindow    xbar;

    GC        gc;
    XftDraw   *draw;
    Drawable  buf;
    DrawAlign align;

    int left_x, right_x;

    Label labels[LENGTH(left_modules) + LENGTH(right_modules)];
    int   nlabel;
};

typedef struct {
    int     fd;
    Display *dpy;
    int     scr;
    XFont   font;
    DC      *dcs;
    int     ndc;
} Bspwmbar;

static Bspwmbar bar;
static SystemTray tray;

static XVisualInfo *visinfo;
static XftColor cols[LENGTH(colors)];
static XftFont **fcaches;
static int nfcache = 0;
static int fcachecap = 0;
static int celwidth = 0;
static int xdbe_support = 0;

/* Atom caches */
static Atom filter;
static Atom xembed_info;

/* Window title cache */
static char *wintitle = NULL;

/* polling fd */
static int pfd = 0;
#if defined(__linux)
static struct epoll_event events[MAX_EVENTS];
#elif defined(__OpenBSD__)
static struct kevent events[MAX_EVENTS];
#endif
static list_head pollfds;

static int error_handler(Display *dpy, XErrorEvent *err);
static int dummy_error_handler(Display *dpy, XErrorEvent *err);

/**
 * get_color() - get XftColor pointer by index of color caches.
 * @index: index of the color.
 *
 * Return: XftColor *
 */
XftColor *
get_color(int index)
{
    return &cols[index];
}

/**
 * load_xft_color() - get XftColor by color name.
 * @dpy: display pointer.
 * @scr: screen number.
 * @colstr: color name.
 *
 * The returned XftColor must call XftColorFree() after used.
 *
 * Return: XftColor
 */
static XftColor
load_xft_color(Display *dpy, int scr, const char *colstr)
{
    Colormap cmap = DefaultColormap(dpy, scr);
    XftColor color;

    XftColorAllocName(dpy, DefaultVisual(dpy, scr), cmap, colstr, &color);
    return color;
}

/**
 * load_colors() - load colors for bspwmbar.
 * @dpy: display pointer.
 * @scr: screen number.
 */
static void
load_colors(Display *dpy, int scr)
{
    for (size_t i = 0; i < LENGTH(colors); i++)
        cols[i] = load_xft_color(dpy, scr, colors[i]);
}

/**
 * free_colors() - free loaded colors.
 * @dpy: display pointer.
 * @scr: screen number.
 */
static void
free_colors(Display *dpy, int scr)
{
    Colormap cmap = DefaultColormap(dpy, scr);
    for (size_t i = 0; i < LENGTH(colors); i++)
        XftColorFree(dpy, DefaultVisual(dpy, scr), cmap, &cols[i]);
}

/**
 * ws_state() - parse char to bspwm desktop state.
 * @s: desktop state char.
 *
 * Retrun: WsState
 * 'o'         - STATE_OCCUPIED
 * 'u'         - STATE_URGENT
 * 'F','U','O' - STATE_ACTIVE
 * not match   - STATE_FREE
 */
static WsState
ws_state(char s)
{
    WsState state = STATE_FREE;
    if ((s | 0x20) == 'o')
        state = STATE_OCCUPIED;
    if ((s | 0x20) == 'u')
        state = STATE_URGENT;
    if (s == 'F' || s == 'U' || s == 'O')
        return state | STATE_ACTIVE;
    return state;
}

/**
 * get_window_prop() - get window property.
 * @dpy: display pointer.
 * @win: target window.
 * @property: property name.
 *
 * Return: Property value as (unsigned char *).
 *         The value needs free by call free() after used.
 */
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

/**
 * set_window_prop() - set window property.
 * @dpy: display pointer.
 * @win: window.
 * @type: Atom type.
 * @property: property name.
 * @mode: operation mode. Prease see XChangeProperty(3).
 * @propvalue: property values array.
 * @nvalue: length of propvalue.
 */
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

/**
 * get_visual_info() - get XVisualInfo object.
 * @dpy: display pointer.
 *
 * This function needs Xdbe support.
 *
 * Return: XVisualInfo *
 */
XVisualInfo *
get_visual_info(Display *dpy)
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

/**
 * dc_init() - initialize DC.
 * @dc: DC.
 * @scr: screen number.
 * @x: window position x.
 * @y: window position y.
 * @width: window width.
 * @height: window height.
 */
static void
dc_init(DC dc, Display *dpy, int scr, int x, int y, int width,
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
        vis = get_visual_info(dpy)->visual;
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

    /* set class hint */
    hint = XAllocClassHint();
    hint->res_class = "Bspwmbar";
    hint->res_name = "Bspwmbar";
    XSetClassHint(dpy, xw->win, hint);
    XFree(hint);

    xw->x = x;
    xw->y = y;
    xw->width = width;
    xw->height = height;

    /* create labels from modules */
    dc->nlabel = LENGTH(left_modules) + LENGTH(right_modules);
    for (int i = 0; i < (int)LENGTH(left_modules); i++) {
        dc->labels[i].align = DA_LEFT;
        dc->labels[i].module = &left_modules[i];
    }
    int nlabel = LENGTH(left_modules);
    for (int i = 0; nlabel < dc->nlabel; i++, nlabel++) {
        dc->labels[nlabel].align = DA_RIGHT;
        dc->labels[nlabel].module = &right_modules[i];
    }

    /* send window rendering request */
    XClearWindow(dpy, xw->win);
    XLowerWindow(dpy, xw->win);
    XMapWindow(dpy, xw->win);
}

/**
 * dc_get_x() - get next rendering position of DC.
 * @dc: DC.
 *
 * Return: int
 */
static int
dc_get_x(DC dc)
{
    if (dc->align == DA_LEFT)
        return dc->left_x;
    return dc->xbar.width - dc->right_x;
}

/**
 * dc_move_x() - move rendering position by x.
 * @dc: DC.
 * @x: distance of movement.
 */
static void
dc_move_x(DC dc, int x)
{
    if (dc->align == DA_LEFT)
        dc->left_x += x;
    else if (dc->align == DA_RIGHT)
        dc->right_x += x;
}

/**
 * get_active_window() - get active window.
 * @dpy: display pointer.
 * @scr: screen number.
 *
 * Return: Window
 */
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

/**
 * get_window_title() - get title of specified win.
 * @dpy: display pointer.
 * @win: window.
 *
 * Return: unsigned char *
 *         The return value needs free after used.
 */
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

/**
 * windowtitle_update() - update windowtitle() returns value.
 * @dpy: display pointer.
 * @scr: screen number.
 */
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

/**
 * windowtitle() - active window title render function.
 * @dc: DC.
 * @suffix: suffix when substituted long title.
 */
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

    draw_text(dc, buf);
}

/**
 * get_font() - finds a font that renderable specified rune.
 * @rune: FcChar32
 *
 * Return: XftFont *
 */
static XftFont *
get_font(FcChar32 rune)
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

        FcConfigSubstitute(NULL, pat, FcMatchPattern);
        XftDefaultSubstitute(bar.dpy, bar.scr, pat);

        match = FcFontSetMatch(NULL, fsets, 1, pat, &result);
        FcPatternDestroy(pat);
        FcCharSetDestroy(charset);
        if (!match)
            die("no fonts contain glyph: 0x%x\n", rune);

        fcaches[nfcache] = XftFontOpenPattern(bar.dpy, match);
        FcPatternDestroy(match);

        if (!fcaches[nfcache])
            die("XftFontOpenPattern(): failed seeking fallback font\n");

        i = nfcache++;
    }

    return fcaches[i];
}

/**
 * load_fonts() - load fonts by specified fontconfig pattern string.
 * @patstr: pattern string.
 *
 * Return:
 * 0 - success
 * 1 - failure
 */
static int
load_fonts(const char *patstr)
{
    FcPattern *pat = FcNameParse((FcChar8 *)patstr);
    if (!pat)
        die("loadfonts(): failed parse pattern: %s\n", patstr);

    FcConfigSubstitute(NULL, pat, FcMatchPattern);
    XftDefaultSubstitute(bar.dpy, bar.scr, pat);

    FcResult result;
    FcPattern *match = FcFontMatch(NULL, pat, &result);
    if (!match) {
        FcPatternDestroy(pat);
        err("loadfonts(): no fonts match pattern: %s\n", patstr);
        return 1;
    }

    bar.font.base = XftFontOpenPattern(bar.dpy, match);
    FcPatternDestroy(match);
    if (!bar.font.base) {
        FcPatternDestroy(pat);
        err("loadfonts(): failed open font: %s\n", patstr);
        return 1;
    }

    bar.font.pattern = pat;
    return 0;
}

/**
 * get_base_line() - get text rendering baseline.
 *
 * Return: y offset.
 */
static int
get_baseline()
{
    return (BAR_HEIGHT - bar.font.base->height) / 2 + bar.font.base->ascent;
}

/**
 * dc_calc_render_pos() - calculate render position.
 * @dc: DC.
 * @glyphs: (in/out) XftCharFontSpec *.
 * @nglyph: lenght of glyphs.
 */
static void
dc_calc_render_pos(DC dc, XftCharFontSpec *glyphs, int nglyph)
{
    int x = dc_get_x(dc);
    for (int i = 0; i < nglyph; i++) {
        glyphs[i].x += x;
    }
}

/**
 * load_glyphs() - load XGlyphFontSpec from specified str.
 * @str: utf-8 string.
 * @glyphs: (out) XCharFontSpec *.
 * @nglyph: length of glyphs.
 * @width: (out) rendering width.
 *
 * Return: number of loaded glyphs.
 */
static int
load_glyphs(const char *str, XftCharFontSpec *glyphs, int nglyph, int *width)
{
    XGlyphInfo extents = { 0 };
    FcChar32 rune = 0;
    int i, len = 0;
    size_t offset = 0;
    int y = get_baseline();

    *width = 0;
    for (i = 0; offset < strlen(str) && i < nglyph; i++, offset += len) {
        len = FcUtf8ToUcs4((FcChar8 *)&str[offset], &rune, strlen(str) - i);
        glyphs[i].font = get_font(rune);
        glyphs[i].ucs4 = rune;
        glyphs[i].x = *width;
        glyphs[i].y = y;
        XftTextExtentsUtf8(bar.dpy, glyphs[i].font, (FcChar8 *)&str[offset],
                           len, &extents);
        *width += extents.x + extents.xOff;
    }

    return i;
}

/**
 * draw_padding() - pender padding.
 * @dc: DC.
 * @num: padding width.
 */
static void
draw_padding(DC dc, int num)
{
    switch ((int)dc->align) {
    case DA_LEFT:
        dc->left_x += num;
        break;
    case DA_RIGHT:
        if (!dc->right_x)
            num += celwidth;
        dc->right_x += num;
        break;
    }
}

/**
 * draw_string() - render string with the color.
 * @dc: DC.
 * @color: rendering text color.
 * @str: rendering text.
 */
static void
draw_string(DC dc, XftColor *color, const char *str)
{
    int width;
    int nglyph = load_glyphs(str, glyph_caches, sizeof(glyph_caches), &width);
    if (dc->align == DA_RIGHT)
        dc_move_x(dc, width);
    dc_calc_render_pos(dc, glyph_caches, nglyph);
    XftDrawCharFontSpec(dc->draw, color, glyph_caches, nglyph);
    if (dc->align == DA_LEFT)
        dc_move_x(dc, width);
}

/**
 * draw_colored_text() - render colored text.
 * @dc: DC.
 * @int: color_number.
 * @str: rendering text.
 */
void
draw_colored_text(DC dc, int color_number, const char *str)
{
    draw_padding(dc, celwidth);
    draw_string(dc, &cols[color_number], str);
    draw_padding(dc, celwidth);
}

/**
 * draw_text() - render text.
 * @dc: DC.
 * @str: rendering text.
 */
void
draw_text(DC dc, const char *str)
{
    draw_padding(dc, celwidth);
    draw_string(dc, &cols[FGCOLOR], str);
    draw_padding(dc, celwidth);
}

/**
 * draw_bargraph() - render bar graph.
 * @dc: DC.
 * @label: label of the graph.
 * @items: items of the Graph.
 * @nitem: number of items.
 */
void
draw_bargraph(DC dc, const char *label, GraphItem *items, int nitem)
{
    int maxh = bar.font.base->ascent;
    int basey = (BAR_HEIGHT - bar.font.base->ascent) / 2;

    draw_padding(dc, celwidth);
    int width = (celwidth + 1) * nitem;
    if (dc->align == DA_RIGHT)
        dc->right_x += width;
    int x = dc_get_x(dc) + celwidth;
    draw_string(dc, &cols[FGCOLOR], label);
    draw_padding(dc, celwidth);
    for (int i = 0; i < nitem; i++) {
        XSetForeground(bar.dpy, dc->gc, cols[ALTBGCOLOR].pixel);
        XFillRectangle(bar.dpy, dc->buf, dc->gc, x - celwidth,
                       basey, celwidth, maxh);

        if (items[i].val < 0)
            goto CONTINUE;

        int height = SMALLER(BIGGER(maxh * items[i].val, 1), maxh);
        XSetForeground(bar.dpy, dc->gc, cols[items[i].colorno].pixel);
        XFillRectangle(bar.dpy, dc->buf, dc->gc, x - celwidth,
                       basey + (maxh - height), celwidth, height);
    CONTINUE:
        x += celwidth + 1;
    }
    if (dc->align == DA_LEFT)
        dc_move_x(dc, width);
}

/**
 * bspwm_parse() - parse bspwm reported string.
 * @report: bspwm reported string.
 */
static void
bspwm_parse(char *report)
{
    int i, j, name_len, nws = 0;
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
                if (!strncmp(bar.dcs[j]->xbar.monitor.name, name, strlen(name)))
                    curmon = &bar.dcs[j]->xbar.monitor;
            if (curmon)
                curmon->is_active = (tok == 'M') ? 1 : 0;
            break;
        case 'o':
        case 'O':
        case 'f':
        case 'F':
        case 'u':
        case 'U':
            for (j = ++i; j < len; j++)
                if (report[j] == ':')
                    break;
            if (nws + 1 >= curmon->cdesktop) {
                curmon->cdesktop += 5;
                curmon->desktops = realloc(curmon->desktops,
                                           sizeof(Desktop) * curmon->cdesktop);
            }
            curmon->desktops[nws++].state = ws_state(tok);
            i = j;
            break;
        case 'L':
        case 'T':
            i++; /* skip next char. */
            break;
        case 'G':
            if (curmon)
                curmon->ndesktop = nws;
            /* skip current node flags. */
            while (report[i + 1] != ':' && report[i + 1] != '\n')
                i++;
            break;
        }
    }
}

/**
 * logo() - render the specified text.
 * @dc: DC
 * @args: rendering text.
 */
void
logo(DC dc, const char *args)
{
    draw_padding(dc, celwidth);
    draw_string(dc, &cols[LOGOCOLOR], args);
    draw_padding(dc, celwidth);
}

/**
 * render_label() - render all labels
 * @dc: DC.
 */
static void
render_label(DC dc)
{
    int x = 0, width = 0;
    for (int j = 0; j < dc->nlabel; j++) {
        x = dc_get_x(dc); width = 0;

        dc->align = dc->labels[j].align;
        dc->labels[j].module->func(dc, dc->labels[j].module->arg);
        if (dc->align == DA_LEFT)
            width = dc_get_x(dc) - x;
        else if (dc->align == DA_RIGHT)
        {
            if (j != dc->nlabel -1)
                // Draw vertical lines between the nodes, except for the last node on the right
                draw_string(dc, &cols[FGCOLOR], "| ");
            width = x - dc_get_x(dc);
        }
        x = dc_get_x(dc);
        if (width)
            width += celwidth;
        dc->labels[j].width = width;
        dc->labels[j].x = x;
    }
}

/**
 * render() - rendering all modules.
 */
static void
render()
{
    //XGlyphInfo extents = { 0 };

    /* padding width */
    if (!celwidth) {
        //XftTextExtentsUtf8(bar.dpy, bar.font.base, (FcChar8 *)" ", strlen(" "),
        //                   &extents);
        //celwidth = extents.x + extents.xOff;
        celwidth = 6;
    }

    for (int i = 0; i < bar.ndc; i++) {
        DC dc = bar.dcs[i];
        BarWindow *xw = &bar.dcs[i]->xbar;
        dc->align = DA_LEFT;
        dc->left_x = 0;
        dc->right_x = 0;

        XClearWindow(bar.dpy, xw->win);

        /* render modules */
        draw_padding(dc, celwidth);
        render_label(dc);

        /* swap buffer */
        if (xdbe_support)
            XdbeSwapBuffers(bar.dpy, &dc->swapinfo, 1);
    }
    XFlush(bar.dpy);
}

/**
 * parse_display() - parse DISPLAY environment variable string.
 * @name: display name format string.
 * @host: (out) host name. Memory for the new string is obtained with malloc(3),
 *              and must be freed with free().
 * @dpy: (out) display server number.
 * @scr: (out) screen number.
 */
static int
parse_display(char *name, char **host, int *dpy, int *scr)
{
    char *colon;
    int hostlen = 0;
    *host = NULL; *dpy = 0; *scr = 0;
    if (!(colon = strrchr(name, ':')))
        return 1;
    hostlen = (colon - name);
    if (hostlen < 0)
        return 1;
    ++colon;
    *host = strndup(name, hostlen);
    sscanf(colon, "%d.%d", dpy, scr);
    return 0;
}

/**
 * bspwm_connect() - connect to bspwm socket.
 * @dpy: Display pointer.
 *
 * Return: file descripter or -1.
 */
static int
bspwm_connect(Display *dpy)
{
    struct sockaddr_un sock;
    int fd, dpyno = 0, scrno = 0;
    char *sp = NULL;

    sock.sun_family = AF_UNIX;
    if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1)
        return -1;

    sp = getenv("BSPWM_SOCKET");
    if (sp) {
        snprintf(sock.sun_path, sizeof(sock.sun_path), "%s", sp);
    } else {
        if (parse_display(DisplayString(dpy), &sp, &dpyno, &scrno))
            return -1;
        snprintf(sock.sun_path, sizeof(sock.sun_path),
                 "/tmp/bspwm%s_%i_%i-socket", sp, dpyno, scrno);
        free(sp);
    }

    if (connect(fd, (struct sockaddr *)&sock, sizeof(sock)) == -1)
        return -1;

    return fd;
}

/**
 * bspwmbar_init() - initialize bspwmbar.
 * @dpy: display pointer.
 * @scr: screen number.
 *
 * Return:
 * 0 - success
 * 1 - failure
 */
static int
bspwmbar_init(Display *dpy, int scr)
{
    XRRScreenResources *xrr_res;
    XRRMonitorInfo *xrr_mon;
    XRROutputInfo *xrr_out;
    Window root = RootWindow(dpy, scr);
    int i, j, nmon;

    /* connect bspwm socket */
    if ((bar.fd = bspwm_connect(dpy)) == -1) {
        err("bspwm_connect(): Failed to connect to the socket\n");
        return 1;
    }

    /* get monitors */
    xrr_mon = XRRGetMonitors(dpy, root, 1, &nmon);
    bar.dcs = (DC *)calloc(nmon, sizeof(DC));
    for (i = 0; i < nmon; i++)
        bar.dcs[i] = (DC)calloc(1, sizeof(struct _DC));
    bar.ndc = nmon;

    /* create window per monitor */
    xrr_res = XRRGetScreenResources(dpy, root);
    for (i = 0; i < xrr_res->noutput; i++) {
        xrr_out = XRRGetOutputInfo(dpy, xrr_res, xrr_res->outputs[i]);
        if (xrr_out->connection == RR_Connected) {
            for (j = 0; j < nmon; j++) {
                if (xrr_res->outputs[i] != xrr_mon[j].outputs[0])
                    continue;
                dc_init(bar.dcs[j], dpy, scr, xrr_mon[j].x, 0,
                             xrr_mon[j].width, BAR_HEIGHT);
                strncpy(bar.dcs[j]->xbar.monitor.name, xrr_out->name,
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

    /* load_fonts */
    if (load_fonts(fontname))
        return 1;

    XFlush(dpy);
    return 0;
}

/**
 * bspwmbar_destroy() - destroy all resources of bspwmbar.
 */
static void
bspwmbar_destroy()
{
    int i;

    close(bar.fd);

    /* font resources */
    XftFontClose(bar.dpy, bar.font.base);
    FcPatternDestroy(bar.font.pattern);
    if (bar.font.set)
        FcFontSetDestroy(bar.font.set);
    for (i = 0; i < nfcache; i++)
        XftFontClose(bar.dpy, fcaches[i]);
    free(fcaches);

    /* rendering resources */
    for (i = 0; i < bar.ndc; i++) {
        XFreeGC(bar.dpy, bar.dcs[i]->gc);
        XftDrawDestroy(bar.dcs[i]->draw);
        XDestroyWindow(bar.dpy, bar.dcs[i]->xbar.win);
        free(bar.dcs[i]->xbar.monitor.desktops);
        free(bar.dcs[i]);
    }
    free(bar.dcs);
    if (xdbe_support)
        free(visinfo);
}

/**
 * bspwm_send() - send specified command to bspwm.
 * @cmd: bspwm command.
 * @len: length of cmd.
 *
 * Return: sent bytes length.
 */
static int
bspwm_send(char *cmd, int len)
{
    return send(bar.fd, cmd, len, 0);
}

/**
 * desktops() - render bspwm desktop states.
 * @dc: DC.
 * @args: dummy.
 */
void
desktops(DC dc, const char *args)
{
    (void)args;
    XftColor col;
    const char *ws;
    int cur, max = dc->xbar.monitor.ndesktop;

    draw_padding(dc, celwidth);
    for (int i = 0, j = max - 1; i < max; i++, j--) {
        cur = (dc->align == DA_RIGHT) ? j : i;
        draw_padding(dc, celwidth / 2.0 + 0.5);
        ws = (dc->xbar.monitor.desktops[cur].state & STATE_ACTIVE)
             ? WS_ACTIVE
             : WS_INACTIVE;
        col = (dc->xbar.monitor.desktops[cur].state == STATE_FREE)
              ? cols[ALTFGCOLOR]
              : cols[FGCOLOR];
        draw_string(dc, &col, ws);
        draw_padding(dc, celwidth / 2.0 + 0.5);
    }
    draw_padding(dc, celwidth);
}

/**
 * systray() - render systray.
 * @dc: draw context.
 * @arg: dummy.
 */
void
systray(DC dc, const char *arg)
{
    (void)arg;
    if (list_empty(systray_get_items(tray)))
        return;

    if (systray_get_window(tray) != dc->xbar.win)
        return;

    XSetErrorHandler(dummy_error_handler);

    draw_padding(dc, celwidth);
    list_head *pos;
    list_for_each(systray_get_items(tray), pos) {
        TrayItem *item = list_entry(pos, TrayItem, head);
        if (!item->info.flags)
            continue;
        draw_padding(dc, TRAY_ICONSZ);
        if (item->x != dc_get_x(dc)) {
            item->x = dc_get_x(dc);
            XMoveResizeWindow(bar.dpy, item->win, item->x,
                              (BAR_HEIGHT - TRAY_ICONSZ) / 2, TRAY_ICONSZ,
                              TRAY_ICONSZ);
        }
        draw_padding(dc, celwidth);
    }
    draw_padding(dc, celwidth);

    XSetErrorHandler(error_handler);
}

/**
 * polling_stop() - stop polling to all file descriptor.
 */
static void
polling_stop()
{
    if (pfd > 0)
        close(pfd);
}

/**
 * error_handler() - X11 error handler.
 * @dpy: display pointer.
 * @err: XErrorEvent.
 *
 * Return: always 0.
 */
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
        err("Unknown Error Code: %d\n", err->type);
    }
    XGetErrorText(dpy, err->error_code, buf, sizeof(buf) - 1);
    err("XError: %s\n", buf);
    XGetErrorText(dpy, err->request_code, buf, sizeof(buf) - 1);
    err("  ErrorType: %d (%s)\n", err->type, buf);
    err("  MajorCode: %d (%s)\n", err->request_code, buf);
    err("  MinorCode: %d (%s)\n", err->minor_code, buf);
    err("  ResourceID: %ld\n", err->resourceid);
    err("  SerialNumer: %ld\n", err->serial);

    polling_stop();

    return 0;
}

/**
 * poll_add() - add the file descriptor to polling targets.
 * @pollfd: PollFD object.
 */
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

/**
 * poll_del() - delete the file descriptor from polling targets.
 * @pollfd: PollFD object.
 */
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

/**
 * poll_init() - initialize poll.
 *
 * The function must be called before poll_add(), poll_del().
 */
static void
poll_init()
{
    list_head_init(&pollfds);
}

#if defined(__linux)
/**
 * timer_reset() - PollUpdateHandler for timer.
 * @fd: timerfd.
 *
 * Return: PollResult
 *
 * always - PR_UPDATE
 */
static PollResult
timer_reset(int fd)
{
    uint64_t tcnt;
    read(fd, &tcnt, sizeof(uint64_t));
    return PR_UPDATE;
}

#endif
/**
 * bspwm_handle() - bspwm event handling function.
 * @fd: a file descriptor for bspwm socket.
 *
 * This function expects call after bspwm_connect().
 * Read and parse bspwm report from fd.
 *
 * Return: PollResult
 *
 * success and not need more action - PR_NOOP
 * success and need rerendering     - PR_UPDATE
 * failed to read from fd           - PR_FAILED
 */
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
            err("bspwm: %s", buf + 1);
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

/**
 * is_change_active_window_event() - check the event is change active window.
 *
 * ev: XEvent
 *
 * Return: int
 * 0 - false
 * 1 - true
 */
static int
is_change_active_window_event(XEvent ev)
{
    Window win = RootWindow(bar.dpy, bar.scr);
    Atom atom = XInternAtom(bar.dpy, "_NET_ACTIVE_WINDOW", 1);
    return (ev.xproperty.window == win) && (ev.xproperty.atom == atom);
}

/**
 * xev_handle() - X11 event handling
 *
 * Return: PollResult
 * PR_NOOP   - success and not need more action
 * PR_UPDATE - success and need rerendering
 */
static PollResult
xev_handle()
{
    XEvent event;
    PollResult res = PR_NOOP;
    DC dc;

    /* for X11 events */
    while (XPending(bar.dpy)) {
        XNextEvent(bar.dpy, &event);
        switch (event.type) {
        case SelectionClear:
            systray_handle(tray, event);
            break;
        case Expose:
            res = PR_UPDATE;
            break;
        case ButtonPress:
            dc = NULL;
            for (int j = 0; j < bar.ndc; j++)
                if (bar.dcs[j]->xbar.win == event.xbutton.window)
                    dc = bar.dcs[j];
            if (!dc)
                break;
            /* handle evnent */
            for (int j = 0; j < dc->nlabel; j++) {
                if (!dc->labels[j].module->handler)
                    continue;
                if (dc->labels[j].x < event.xbutton.x &&
                    event.xbutton.x < dc->labels[j].x +
                    dc->labels[j].width) {
                    dc->labels[j].module->handler(event);
                    res = PR_UPDATE;
                    break;
                }
            }
            break;
        case PropertyNotify:
            if (event.xproperty.atom == xembed_info) {
                systray_handle(tray, event);
            } else if (is_change_active_window_event(event) ||
                       event.xproperty.atom == filter) {
                windowtitle_update(bar.dpy, bar.scr);
                res = PR_UPDATE;
            }
            break;
        case ClientMessage:
            systray_handle(tray, event);
            res = PR_UPDATE;
            break;
        case DestroyNotify:
            systray_remove_item(tray, event.xdestroywindow.window);
            res = PR_UPDATE;
            break;
        }
    }
    return res;
}

/*
 * poll_loop() - polling loop
 * @handler: rendering function
 */
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
#endif

    /* polling X11 event for modules */
    PollFD xfd = { ConnectionNumber(bar.dpy), NULL, NULL, xev_handle, { 0 } };
    poll_add(&xfd);

    /* polling fd */
#if defined(__linux)
    int terminate = 0;  // Boolean if the code should terminate on epoll_wait -1 return status
    while (1) {
        nfd = epoll_wait(pfd, events, MAX_EVENTS, -1);
        /* When epoll_wait returns -1, an error has occured.
         * In the case of suspension this happons once, after that the code should continue.
         * In other cases this re-occurs and the loop should end. */
        if (nfd == -1) {
            if (terminate == 1)
                break;
            terminate = 1;
            continue;
        }
        terminate = 0;
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

/**
 * @signal_handler - a signal handler.
 * @signum: signal number.
 *
 * The function stop polling if signum equals SIGINT or SIGTERM.
 */
static void
signal_handler(int signum) {
    switch (signum) {
    case SIGINT:
    case SIGTERM:
        polling_stop();
        break;
    }
}

/**
 * dummy_error_handler() - a dummy X11 error handler.
 * @dpy: dummy.
 * @err: dummy.
 *
 * Return: Always 0.
 */
static int
dummy_error_handler(Display *dpy, XErrorEvent *err)
{
    (void)dpy;
    (void)err;
    return 0;
}

/**
 * cleanup() - cleanup resources
 */
static void
cleanup(Display *dpy)
{
    if (wintitle)
        XFree(wintitle);

    if (tray)
        systray_destroy(tray);
    free_colors(dpy, DefaultScreen(dpy));
    bspwmbar_destroy();
    XCloseDisplay(dpy);
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

#ifndef DISABLE_XDBE
    /* Xdbe initialize */
    int major, minor;
    if (XdbeQueryExtension(dpy, &major, &minor))
        xdbe_support = 1;
#endif

    /* get active widnow title */
    windowtitle_update(dpy, DefaultScreen(dpy));

    load_colors(dpy, DefaultScreen(dpy));

    if (bspwmbar_init(dpy, DefaultScreen(dpy))) {
        err("bspwmbar_init(): Failed to init bspwmbar\n");
        goto CLEANUP;
    }

    /* tray initialize */
    XSetErrorHandler(dummy_error_handler);
    if (!(tray = systray_new(dpy, bar.dcs[0]->xbar.win))) {
        err("systray_init(): Selection already owned by other window\n");
        goto CLEANUP;
    }
    XSetErrorHandler(error_handler);

    /* subscribe bspwm report */
    if (bspwm_send(SUBSCRIBE_REPORT, LENGTH(SUBSCRIBE_REPORT)) == -1) {
        err("bspwm_send(): Failed to send command to bspwm\n");
        goto CLEANUP;
    }

#if defined(__linux)
    /* epoll */
    if ((pfd = epoll_create1(0)) == -1) {
        err("epoll_create1(): Failed to create epoll fd\n");
        goto CLEANUP;
    }
#elif defined(__OpenBSD__)
    if (!(pfd = kqueue())) {
        err("kqueue(): Failed to create kqueue fd\n");
        goto CLEANUP;
    }
#endif

    /* polling bspwm report */
    PollFD bfd = { bar.fd, NULL, NULL, bspwm_handle, { 0 } };
    poll_add(&bfd);

    /* wait PropertyNotify events of root window */
    XSetWindowAttributes attrs;
    attrs.event_mask = PropertyChangeMask;
    XChangeWindowAttributes(bar.dpy, XRootWindow(bar.dpy, bar.scr), CWEventMask,
                            &attrs);

    /* polling X11 event for modules */
    for (int i = 0; i < bar.ndc; i++)
        XSelectInput(bar.dpy, bar.dcs[i]->xbar.win,
                     ButtonPressMask | ExposureMask);

    /* cache Atom */
    filter = XInternAtom(bar.dpy, "_NET_WM_NAME", 1);
    xembed_info = XInternAtom(bar.dpy, "_XEMBED_INFO", 1);

    /* polling initialize for modules */
    poll_init();

    /* main loop */
    poll_loop(render);

CLEANUP:
    /* cleanup resources */
    cleanup(dpy);

    return 0;
}
