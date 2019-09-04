/* See LICENSE file for copyright and license details. */

#if defined(__linux)
# include <alloca.h>
#endif
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <X11/Xlib.h>

#include "bspwmbar.h"
#include "util.h"

static const char *prefix = "";

void
datetime(DC dc, const char *fmt)
{
    time_t timer = time(NULL);
    struct tm *tptr = localtime(&timer);

    int size = SMALLER(strlen(fmt) + strlen(prefix) + 1, 128);
    char *format = alloca(size);
    snprintf(format, size, "%s%s", prefix, fmt);
    strftime(buf, sizeof(buf), format, tptr);

    draw_text(dc, buf);
}
