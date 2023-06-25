/* See LICENSE file for copyright and license details. */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/soundcard.h>
#include <xcb/xcb.h>

#include "util.h"
#include "bspwmbar.h"

typedef struct {
	int8_t lvol;
	int8_t rvol;
} mixer_t;

static bool mixer_load(const char *, mixer_t *);
static void mixer_set(int8_t);

void
mixer(draw_context_t *dc, module_option_t *opts)
{
	mixer_t mixer;

	if (!mixer_load(opts->mixer.device, &mixer))
		return;

	sprintf(buf, "%s%i%s", opts->mixer.prefix, BIGGER(mixer.lvol, mixer.rvol), opts->mixer.suffix);
	draw_text(dc, buf);
}

static char *device = NULL;
static int fd = -1;

bool
mixer_load(const char *devname, mixer_t *mixer)
{
	int vol;

	if (device == NULL) {
		if ((device = strdup(devname)) == NULL || *device == '\0')
			device = "/dev/mixer";
	}

	if (fd < 0 && (fd = open(device, O_RDWR)) < 0) {
		return false;
	}

	if (ioctl(fd, SOUND_MIXER_READ_VOLUME, &vol) < 0) {
		close(fd);
		fd = -1;
		return false;
	}

	mixer->lvol = vol & 0x7F;
	mixer->rvol = (vol >> 8) & 0x7F;

	return true;
}

void
mixer_set(int8_t vol)
{
	if (fd < 0)
		return;

	int rl = vol | (vol << 8);
	if (ioctl(fd, SOUND_MIXER_WRITE_VOLUME, &rl) < 0) {
		close(fd);
		fd = -1;
		return;
	}
}

void
mixer_ev(xcb_generic_event_t *ev, module_option_t *opts)
{
	mixer_t mixer;
	xcb_button_press_event_t *button;
	double cur;
	const double step = 6;

	if (!mixer_load(opts->mixer.device, &mixer))
		return;

	cur = BIGGER(mixer.lvol, mixer.rvol);

	switch (ev->response_type & ~0x80) {
	case XCB_BUTTON_PRESS:
		button = (xcb_button_press_event_t *)ev;
		switch (button->detail) {
		case XCB_BUTTON_INDEX_4:
			cur = (cur / step) * step + step;
			if (cur > 100)
				cur = 100;
			mixer_set((int8_t)cur);
			break;
		case XCB_BUTTON_INDEX_5:
			cur = (cur / step) * step - step;
			if (cur < 0)
				cur = 0;
			mixer_set((int8_t)cur);
			break;
		}
		break;
	}
}

