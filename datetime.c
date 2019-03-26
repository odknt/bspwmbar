/* See LICENSE file for copyright and license details. */

#include <alloca.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <X11/Xlib.h>

#include "util.h"

static const char *prefix = " ";

char *
datetime(const char *fmt)
{
	time_t timer = time(NULL);
	struct tm *tptr = localtime(&timer);

	int size = SMALLER(strlen(fmt) + strlen(prefix), 128);
	char *format = alloca(size);
	snprintf(format, size, "%s%s", prefix, fmt);
	strftime(buf, sizeof(buf), format, tptr);

	return buf;
}
