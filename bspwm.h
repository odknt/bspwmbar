/* See LICENSE file for copyright and license details. */

#ifndef BSPWMBAR_BSPWM_H
#define BSPWMBAR_BSPWM_H

#include "util.h"
#include "poll.h"

/* bspwm commands */
#define SUBSCRIBE_REPORT "subscribe\0report"

enum bspwm_desktop_state {
	BSPWM_DESKTOP_FREE     = 0,
	BSPWM_DESKTOP_FOCUSED  = 1 << 1,
	BSPWM_DESKTOP_OCCUPIED = 1 << 2,
	BSPWM_DESKTOP_URGENT   = 1 << 3,
};

struct bspwm_desktop {
	char *name;
	enum bspwm_desktop_state state;

	list_head head;
};

struct bspwm_monitor {
	char *name;
	bool is_active;

	/* cache of number of desktops */
	list_head desktops;

	list_head head;
};

enum bspwm_desktop_state
bspwm_desktop_state(struct bspwm_desktop *);

#endif /* BSPWMBAR_BSPWM_H */
