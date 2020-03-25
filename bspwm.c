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
#include "util.h"

struct _bspwm_desktop_t {
	char *name;
	bspwm_desktop_state_t state;

	list_head head;
};

struct _bspwm_monitor_t {
	char *name;
	bool is_active;

	/* cache of number of desktops */
	list_head desktops;

	list_head head;
};

typedef struct {
	list_head monitors;
} bspwm_t;

static void bspwm_init();
static int bspwm_connect();
static int bspwm_disconnect();
static int bspwm_send(const char*, size_t);
static void bspwm_parse(const char *);
static poll_result_t bspwm_handle(int);

/* file descriptior for bspwm */
static poll_fd_t pfd = { 0 };
static bspwm_t *bspwm = NULL;

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

	return fd;
}

int
bspwm_disconnect()
{
	bspwm_monitor_t *mon;
	bspwm_desktop_t *desk;
	list_head *cur, *tmp, *cur2, *tmp2;

	close(pfd.fd);
	list_for_each_safe(&bspwm->monitors, cur, tmp) {
		mon = list_entry(cur, bspwm_monitor_t, head);
		list_for_each_safe(&mon->desktops, cur2, tmp2) {
			desk = list_entry(cur2, bspwm_desktop_t, head);
			free(desk->name);
			free(desk);
		}
		free(mon->name);
		free(mon);
	}
	free(bspwm);
	return 0;
}

void
bspwm_init()
{
	bspwm = calloc(1, sizeof(bspwm_t));

	list_head_init(&bspwm->monitors);

	pfd.fd = bspwm_connect();
	pfd.init = bspwm_connect;
	pfd.deinit = bspwm_disconnect;
	pfd.handler = bspwm_handle;
	poll_add(&pfd);

	/* subscribe bspwm report */
	bspwm_send(SUBSCRIBE_REPORT, LENGTH(SUBSCRIBE_REPORT));
}

/**
 * bspwm_send() - send specified command to bspwm.
 * @bspwm: bspwm.
 * @cmd: bspwm command.
 * @len: length of cmd.
 *
 * Return: sent bytes length.
 */
int
bspwm_send(const char *cmd, size_t len)
{
	return send(pfd.fd, cmd, len, 0);
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
bspwm_desktop_state_t
bspwm_desktop_state_parse(char s)
{
	bspwm_desktop_state_t state = BSPWM_DESKTOP_FREE;
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

	bspwm_monitor_t *curmon = NULL, *mon = NULL;
	bspwm_desktop_t *desktop = NULL, *desk = NULL;

	for (i = 0; i < len; i++) {
		switch (tok = report[i]) {
		case 'M':
		case 'm':
			curmon = NULL;
			for (j = ++i; j < len; j++)
				if (report[j] == ':')
					break;
			list_for_each(&bspwm->monitors, cur) {
				mon = list_entry(cur, bspwm_monitor_t, head);
				if (!strncmp(mon->name, &report[i], j - i)) {
					curmon = mon;
					break;
				}
			}
			if (!curmon) {
				curmon = calloc(1, sizeof(bspwm_monitor_t));
				list_add_tail(&bspwm->monitors, &curmon->head);
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
				desk = list_entry(cur, bspwm_desktop_t, head);
				if (!strncmp(desk->name, &report[i], j - i)) {
					desktop = desk;
					break;
				}
			}
			if (!desktop) {
				desktop = calloc(1, sizeof(bspwm_desktop_t));
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
poll_result_t
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

bspwm_desktop_state_t
bspwm_desktop_state(bspwm_desktop_t *desktop)
{
	return desktop->state;
}

void
draw_desktop(draw_context_t *dc, bspwm_desktop_t *desktop, module_desktop_t *opts)
{
	const char *ws;
	bspwm_desktop_state_t state = bspwm_desktop_state(desktop);

	color_t *col;
	static color_t *fg = NULL, *fg_free = NULL;
	if (!fg)
		fg = opts->fg ? color_load(opts->fg) : color_default_fg();
	if (!fg_free)
		fg_free = opts->fg_free ? color_load(opts->fg_free) : color_default_fg();

	ws = (state & BSPWM_DESKTOP_FOCUSED) ? opts->focused : opts->unfocused;
	col = (state == BSPWM_DESKTOP_FREE) ? fg_free : fg;

	draw_color_text(dc, col, ws);
}

/**
 * desktops() - render bspwm desktop states.
 * @dc: draw context.
 * @opts: module options.
 */
void
desktops(draw_context_t *dc, module_option_t *opts)
{
	const char *name = draw_context_monitor_name(dc);
	bspwm_monitor_t *mon = NULL;
	bspwm_desktop_t *desktop;
	list_head *cur;

	if (!bspwm)
		bspwm_init();

	list_for_each(&bspwm->monitors, cur) {
		mon = list_entry(cur, bspwm_monitor_t, head);
		if (!strncmp(mon->name, name, strlen(name)))
			break;
	}
	if (!mon)
		return;

	list_for_each(&mon->desktops, cur) {
		desktop = list_entry(cur, bspwm_desktop_t, head);
		draw_desktop(dc, desktop, &opts->desk);
		if (&mon->desktops != cur->next)
			draw_padding_em(dc, 1);
	}
}
