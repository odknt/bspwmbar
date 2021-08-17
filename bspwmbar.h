/* See LICENSE file for copyright and license details. */

#ifndef BSPWMBAR_BSPWMBAR_H
#define BSPWMBAR_BSPWMBAR_H

#include <stdint.h>
#include <xcb/xcb_event.h>

#include "util.h"
#include "bspwm.h"
#include "draw.h"
#include "poll.h"

xcb_connection_t *xcb_connection();

struct bb_color *bb_color_load(const char *);
void poll_add(struct bb_poll_option *);

/* temporary buffer */
extern char buf[1024];

#endif /* BSPWMBAR_BSPWMBAR_H */
