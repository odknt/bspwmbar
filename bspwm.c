/* See LICENSE file for copyright and license details. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <unistd.h>

#include "bspwmbar.h"
#include "bspwm.h"
#include "module.h"
#include "util.h"

static void bspwm_parse(const char *);
static void bspwm_init();
static int bspwm_connect();
static void bspwm_disconnect(int);
static enum bb_poll_result bspwm_handle(int);

/* file descriptior for bspwm */
static list_head monitors;
static struct bb_poll_option pfd = { 0 };

void
bspwm_init()
{
	pfd.init = bspwm_connect;
	pfd.deinit = bspwm_disconnect;
	pfd.handler = bspwm_handle;
	poll_add(&pfd);
}

/**
 * bspwm_connect() - connect to bspwm socket.
 *
 * Return: file descripter or -1.
 */
int
bspwm_connect()
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
		if (xcb_parse_display(NULL, &sp, &dpyno, &scrno))
			snprintf(sock.sun_path, sizeof(sock.sun_path),
			         "/tmp/bspwm%s_%i_%i-socket", sp, dpyno, scrno);
		free(sp);
	}

	if (connect(fd, (struct sockaddr *)&sock, sizeof(sock)) == -1)
		return -1;

	/* subscribe bspwm report */
	send(fd, SUBSCRIBE_REPORT, LENGTH(SUBSCRIBE_REPORT), 0);

	return fd;
}

void
bspwm_disconnect(int fd)
{
	struct bspwm_monitor *mon;
	struct bspwm_desktop *desk;
	list_head *cur, *tmp, *cur2, *tmp2;

	close(fd);
	list_for_each_safe(&monitors, cur, tmp) {
		mon = list_entry(cur, struct bspwm_monitor, head);
		list_for_each_safe(&mon->desktops, cur2, tmp2) {
			desk = list_entry(cur2, struct bspwm_desktop, head);
			free(desk->name);
			free(desk);
		}
		free(mon->name);
		free(mon);
	}
}

/**
 * bspwm_desktop_state_parse() - parse char to bspwm desktop state.
 * @s: desktop state char.
 *
 * Retrun: DesktopState
 * 'o'         - STATE_OCCUPIED
 * 'u'         - STATE_URGENT
 * 'F','U','O' - STATE_ACTIVE
 * not match   - STATE_FREE
 */
enum bspwm_desktop_state
bspwm_desktop_state_parse(char s)
{
	enum bspwm_desktop_state state = BSPWM_DESKTOP_FREE;
	if ((s | 0x20) == 'o')
		state = BSPWM_DESKTOP_OCCUPIED;
	if ((s | 0x20) == 'u')
		state = BSPWM_DESKTOP_URGENT;
	if (s == 'F' || s == 'U' || s == 'O')
		return state | BSPWM_DESKTOP_FOCUSED;
	return state;
}

void
bspwm_parse(const char *report)
{
	int i, j;
	int len = strlen(report);
	char tok;
	list_head *cur;

	struct bspwm_monitor *curmon = NULL, *mon = NULL;
	struct bspwm_desktop *desktop = NULL, *desk = NULL;

	for (i = 0; i < len; i++) {
		switch (tok = report[i]) {
		case 'M':
		case 'm':
			curmon = NULL;
			for (j = ++i; j < len; j++)
				if (report[j] == ':')
					break;
			list_for_each(&monitors, cur) {
				mon = list_entry(cur, struct bspwm_monitor, head);
				if (!strncmp(mon->name, &report[i], j - i)) {
					curmon = mon;
					break;
				}
			}
			if (!curmon) {
				curmon = calloc(1, sizeof(struct bspwm_monitor));
				list_add_tail(&monitors, &curmon->head);
				curmon->name = strndup(&report[i], j - i);
				curmon->name[j - i] = '\0';
				list_head_init(&curmon->desktops);
			}
			curmon->is_active = (tok == 'M');
			i = j;
			break;
		case 'o':
		case 'O':
		case 'f':
		case 'F':
		case 'u':
		case 'U':
			desktop = NULL;
			for (j = ++i; j < len; j++)
				if (report[j] == ':')
					break;
			list_for_each(&curmon->desktops, cur) {
				desk = list_entry(cur, struct bspwm_desktop, head);
				if (!strncmp(desk->name, &report[i], j - i)) {
					desktop = desk;
					break;
				}
			}
			if (!desktop) {
				desktop = calloc(1, sizeof(struct bspwm_desktop));
				list_add_tail(&curmon->desktops, &desktop->head);
				desktop->name = strndup(&report[i], j - i);
				desktop->name[j - i] = '\0';
			}
			desktop->state = bspwm_desktop_state_parse(tok);
			i = j;
			break;
		case 'L':
		case 'T':
			i++; /* skip next char. */
			break;
		case 'G':
			/* skip current node flags. */
			while (report[i + 1] != ':' && report[i + 1] != '\n')
				i++;
			break;
		}
	}
}

/**
 * bspwm_handle() - bspwm event handling function.
 * @fd: a file descriptor for bspwm socket.
 *
 * This function expects call after bspwm_connect().
 * Read and parse bspwm report from fd.
 *
 * Return: poll_result_t
 *
 * success and not need more action - PR_NOOP
 * success and need rerendering     - PR_UPDATE
 * failed to read from fd           - PR_FAILED
 */
enum bb_poll_result
bspwm_handle(int fd)
{
	ssize_t len;

	if ((len = recv(fd, buf, sizeof(buf) - 1, 0)) > 0) {
		buf[len] = '\0';
		if (buf[0] == '\x07') {
			err("bspwm: %s", buf + 1);
			return PR_FAILED;
		}
		bspwm_parse(buf);
		return PR_UPDATE;
	}
	return PR_FAILED;
}

enum bspwm_desktop_state
bspwm_desktop_state(struct bspwm_desktop *desktop)
{
	return desktop->state;
}

void
draw_desktop(struct bb_draw_context *dc, struct bspwm_desktop *desktop, struct bb_module_desktop *opts)
{
	const char *ws;
	enum bspwm_desktop_state state = bspwm_desktop_state(desktop);

	struct bb_color *col;
	static struct bb_color *fg = NULL;
	static struct bb_color *fg_free = NULL;
	if (!fg)
		fg = opts->fg ? bb_color_load(opts->fg) : dc->fgcolor;
	if (!fg_free)
		fg_free = opts->fg_free ? bb_color_load(opts->fg_free) : dc->fgcolor;

	ws = (state & BSPWM_DESKTOP_FOCUSED) ? opts->focused : opts->unfocused;
	col = (state == BSPWM_DESKTOP_FREE) ? fg_free : fg;

	bb_draw_color_text(dc, col, ws);
}

/**
 * desktops() - render bspwm desktop states.
 * @dc: draw context.
 * @opts: module options.
 */
void
desktops(struct bb_draw_context *dc, union bb_module *opts)
{
	struct bspwm_monitor *mon = NULL;
	struct bspwm_desktop *desktop;
	list_head *cur;

	if (!pfd.fd)
		bspwm_init();
	if (!monitors.next)
		list_head_init(&monitors);

	list_for_each(&monitors, cur) {
		mon = list_entry(cur, struct bspwm_monitor, head);
		if (!strncmp(mon->name, dc->monitor_name, strlen(dc->monitor_name)))
			break;
	}
	if (!mon)
		return;

	list_for_each(&mon->desktops, cur) {
		desktop = list_entry(cur, struct bspwm_desktop, head);
		draw_desktop(dc, desktop, &opts->desk);
		if (&mon->desktops != cur->next)
			bb_draw_padding_em(dc, 1);
	}
}
