/* See LICENSE file for copyright and license details. */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>

#include "bspwmbar.h"
#include "util.h"

static char *format = " %d%%";


void
brightness(DC dc, const char *backlight)
{
    static time_t prevtime;
    static uintmax_t _brightness, brightness, max_brightness;

    char bpath[128], mpath[128];
    snprintf(bpath, 128, "/sys/class/backlight/%s/brightness", backlight);
    snprintf(mpath, 128, "/sys/class/backlight/%s/max_brightness", backlight);

    time_t curtime = time(NULL);
    if (curtime - prevtime < 1)
        goto DRAW_BATTERY;
    prevtime = curtime;

    if (pscanf(bpath, "%ju", &_brightness) == -1)
        return;

    if (pscanf(mpath, "%ju", &max_brightness) == -1)
        return;

    brightness = 100 * _brightness / max_brightness;

DRAW_BATTERY:
    sprintf(buf, format, brightness);
    draw_text(dc, buf);
}
