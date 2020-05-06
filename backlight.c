/* See LICENSE file for copyright and license details. */

#if defined(__linux)
# include <alloca.h>
#endif
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include <xcb/xcb.h>
#include <xcb/randr.h>

#include "bspwmbar.h"
#include "util.h"

typedef struct {
	int32_t min;
	int32_t max;
	int32_t cur;
} backlight_t;

static bool init_atom(xcb_connection_t *);
static bool backlight_load(backlight_t *, xcb_connection_t *);
static bool backlight_load_info(xcb_connection_t *, xcb_randr_output_t, backlight_t *);
static void backlight_set(xcb_connection_t *, xcb_randr_output_t, int32_t);

static xcb_atom_t atom_backlight;
static xcb_randr_output_t output_cache;

void
backlight(draw_context_t *dc, module_option_t *opts)
{
	backlight_t backlight = { 0 };
	uint32_t blightness = 0;

	if (!backlight_load(&backlight, xcb_connection()))
		return;

	blightness = (backlight.cur - backlight.min) * 100 / (backlight.max - backlight.min);

	sprintf(buf, "%s%d%s", opts->backlight.prefix, blightness, opts->backlight.suffix);
	draw_text(dc, buf);
}

bool
backlight_load(backlight_t *backlight, xcb_connection_t *xcb)
{
	size_t i;
	xcb_randr_get_screen_resources_reply_t *screen_reply;
	xcb_randr_output_t *outputs;

	xcb_screen_t *scr = xcb_setup_roots_iterator(xcb_get_setup(xcb)).data;

	screen_reply = xcb_randr_get_screen_resources_reply(xcb, xcb_randr_get_screen_resources(xcb, scr->root), NULL);
	outputs = xcb_randr_get_screen_resources_outputs(screen_reply);

	for (i = 0; i < screen_reply->num_outputs; i++) {
		if (backlight_load_info(xcb, outputs[i], backlight)) {
			output_cache = outputs[i];
			break;
		}
	}
	free(screen_reply);

	return true;
}

bool
backlight_load_info(xcb_connection_t *xcb, xcb_randr_output_t output, backlight_t *backlight)
{
	xcb_randr_get_output_property_reply_t *prop_reply;
	xcb_randr_query_output_property_reply_t *query_reply;
	int32_t cur, *values;

	/* get atom for backlight */
	if (!atom_backlight && !init_atom(xcb))
		return false;

	prop_reply = xcb_randr_get_output_property_reply(xcb, xcb_randr_get_output_property(xcb, output, atom_backlight, XCB_ATOM_NONE, 0, 4, 0, 0), NULL);
	if (!prop_reply)
		return false;
	cur = *((int32_t *)xcb_randr_get_output_property_data(prop_reply));
	free(prop_reply);

	query_reply = xcb_randr_query_output_property_reply(xcb, xcb_randr_query_output_property(xcb, output, atom_backlight), NULL);
	if (!query_reply)
		return false;
	if (query_reply->range && xcb_randr_query_output_property_valid_values_length(query_reply) == 2) {
		backlight->cur = cur;
		values = xcb_randr_query_output_property_valid_values(query_reply);

		backlight->min = values[0];
		backlight->max = values[1];
	}
	free(query_reply);

	return true;
}

void
backlight_set(xcb_connection_t *xcb, xcb_randr_output_t output, int32_t value)
{
	/* get atom for backlight */
	if (!atom_backlight && !init_atom(xcb))
		return;

	xcb_randr_change_output_property(xcb, output, atom_backlight, XCB_ATOM_INTEGER, 32, XCB_PROP_MODE_REPLACE, 1, (unsigned char *)&value);
}

void
backlight_ev(xcb_generic_event_t *ev)
{
	backlight_t backlight;
	xcb_button_press_event_t *button;
	int32_t cur, step;

	xcb_connection_t *xcb = xcb_connection();
	if (!backlight_load(&backlight, xcb))
		return;

	cur = (backlight.cur - backlight.min);
	step = (backlight.max - backlight.min) * 0.05 + 1;

	switch (ev->response_type & ~0x80) {
	case XCB_BUTTON_PRESS:
		button = (xcb_button_press_event_t *)ev;
		switch (button->detail) {
		case XCB_BUTTON_INDEX_4:
			cur = (cur / step) * step + step;
			if (cur > backlight.max)
				cur = backlight.max;
			backlight_set(xcb, output_cache, cur);
			break;
		case XCB_BUTTON_INDEX_5:
			cur = (cur / step) * step - step;
			if (cur < backlight.min)
				cur = backlight.min;
			backlight_set(xcb, output_cache, cur);
			break;
		}
		break;
	}
}

bool
init_atom(xcb_connection_t *xcb)
{
	xcb_intern_atom_reply_t *backlight_reply;

	backlight_reply = xcb_intern_atom_reply(xcb, xcb_intern_atom(xcb, true, strlen("Backlight"), "Backlight"), NULL);
	if (!backlight_reply)
		return false;

	atom_backlight = backlight_reply->atom;
	free(backlight_reply);

	return true;
}
