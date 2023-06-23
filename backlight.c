/* See LICENSE file for copyright and license details. */

#if defined(__linux)
# include <alloca.h>
#endif
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>

#include <xcb/xcb.h>
#include <xcb/randr.h>

#include "bspwmbar.h"
#include "util.h"

typedef struct {
	int32_t min;
	int32_t max;
	int32_t cur;
} backlight_t;

static bool backlight_load(backlight_t *, const char *dev);
static void backlight_set(int32_t);

void
backlight(draw_context_t *dc, module_option_t *opts)
{
	backlight_t backlight = { 0 };
	uint32_t blightness = 0;

	if (!backlight_load(&backlight, opts->backlight.device))
		return;

	blightness = (double)(backlight.cur - backlight.min) * 100 / (double)(backlight.max - backlight.min);

	sprintf(buf, "%s%d%s", opts->backlight.prefix, blightness, opts->backlight.suffix);
	draw_text(dc, buf);
}

#if defined(__linux)

/*
 * Backlight devices can be found under /sys/class/backlight/ on Linux
 *
 * Directory structure is as follows:
 *
 *   /sys/class/backlight/<device>/
 *     actual_brightness
 *     bl_power
 *     brightness
 *     max_brightness
 *     scale
 *     subsystem
 *     type
 *     uevent
 *     device/ (symlink) -> ../../../<device id>
 *     power/
 *       autosuspend_delay_ms
 *       control
 *       runtime_active_time
 *       runtime_status
 *       runtime_suspend_time
 *     subsystem/ (symlink) -> ../../../../../class/backlight
 *
 */

static char *bdev = NULL;
static char *mbdev = NULL;
static int bfd = -1;
static int mbfd = -1;

bool
backlight_load(backlight_t *backlight, const char *dev)
{
	if (dev == NULL)
		return false;

	char bbuf[4] = { 0 };
	char mbbuf[4] = { 0 };

	if (bdev == NULL) {
		bdev = malloc(strlen(dev) + strlen("/brightness") + 1);
		sprintf(bdev, "%s/brightness", dev);
	}

	if ((bfd = open(bdev, O_RDWR)) < 0)
		return false;

	if (read(bfd, bbuf, sizeof(bbuf)) < 0) {
		close(bfd);
		return false;
	}

	bbuf[3] = '\0';
	backlight->cur = atoi(bbuf);
	close(bfd);

	if (mbdev == NULL) {
		mbdev = malloc(strlen(dev) + strlen("/max_brightness") + 1);
		sprintf(mbdev, "%s/max_brightness", dev);
	}

	if ((mbfd = open(mbdev, O_RDONLY)) < 0)
		return false;

	if (read(mbfd, mbbuf, sizeof(mbbuf)) < 0) {
		close(mbfd);
		return false;
	}

	mbbuf[3] = '\0';
	backlight->max = atoi(mbbuf);
	close(mbfd);

	backlight->min = 0;

	return true;
}

void
backlight_set(int32_t value)
{
	char strval[4] = { 0 };
	snprintf(strval, sizeof(strval), "%i", value);

	if ((bfd = open(bdev, O_RDWR)) < 0)
		return;

	(void)write(bfd, strval, sizeof(strval));
	close(bfd);
}

#elif defined(__OpenBSD__) // TODO

bool
backlight_load(backlight_t *backlight, const char *dev)
{
	(void)backlight;
	(void)dev;
	return false;
}

void
backlight_set(int32_t value)
{
	(void)value;
}

#elif defined(__FreeBSD__)
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/backlight.h>

static int fd = -1;

bool
backlight_load(backlight_t *backlight, const char *dev)
{
	if (dev == NULL)
		return false;

	struct backlight_props props;

	if (fd < 0 && (fd = open(dev, O_RDWR)) < 0)
		return false;

	if (ioctl(fd, BACKLIGHTGETSTATUS, &props) < 0)
	{
		close(fd);
		fd = -1;
		return false;
	}

	backlight->min = 0;
	backlight->max = BACKLIGHTMAXLEVELS;
	backlight->cur = props.brightness;

	return true;
}

void
backlight_set(int32_t value)
{
	if (fd < 0)
		return;

	struct backlight_props props = {
		.brightness = value,
	};

	if (ioctl(fd, BACKLIGHTUPDATESTATUS, &props) < 0) {
		close(fd);
		fd = -1;
	}
}

#endif

void
backlight_ev(xcb_generic_event_t *ev, module_option_t *opts)
{
	backlight_t backlight;
	xcb_button_press_event_t *button;
	double cur, step;

	if (!backlight_load(&backlight, opts->backlight.device))
		return;

	cur = (backlight.cur - backlight.min);
	step = (double)(backlight.max - backlight.min) * 0.05 + 1;

	switch (ev->response_type & ~0x80) {
	case XCB_BUTTON_PRESS:
		button = (xcb_button_press_event_t *)ev;
		switch (button->detail) {
		case XCB_BUTTON_INDEX_4:
			cur = (cur / step) * step + step;
			if (cur > backlight.max)
				cur = backlight.max;
			backlight_set((int32_t)cur);
			break;
		case XCB_BUTTON_INDEX_5:
			cur = (cur / step) * step - step;
			if (cur < backlight.min)
				cur = backlight.min;
			backlight_set((int32_t)cur);
			break;
		}
		break;
	}
}

