/* See LICENSE file for copyright and license details. */

#if defined(__linux)
# include <alloca.h>
#endif
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <X11/Xlib.h>

#include "bspwmbar.h"
#include "util.h"

void
datetime(DC dc, Option opts)
{
	time_t timer = time(NULL);
	struct tm *tptr = localtime(&timer);

	if (!opts.arg)
		die("datetime(): arg is required for datetime");
	if (!opts.prefix)
		opts.prefix = "";
	if (!opts.suffix)
		opts.suffix = "";
	int size = SMALLER(strlen(opts.arg) + strlen(opts.prefix) +
	                   strlen(opts.prefix) + 1, 128);
	char *format = alloca(size);
	snprintf(format, size, "%s%s%s", opts.prefix, opts.arg, opts.suffix);
	strftime(buf, sizeof(buf), format, tptr);

	draw_text(dc, buf);
}
