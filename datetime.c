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
datetime(draw_context_t *dc, module_option_t *opts)
{
	time_t timer = time(NULL);
	struct tm *tptr = localtime(&timer);

	if (!opts->date.format)
		die("datetime(): arg is required for datetime");
	if (!opts->date.prefix)
		opts->date.prefix = "";
	if (!opts->date.suffix)
		opts->date.suffix = "";
	int size = SMALLER(strlen(opts->date.format) + strlen(opts->date.prefix) +
	                   strlen(opts->date.prefix) + 1, 128);
	char *format = alloca(size);
	snprintf(format, size, "%s%s%s", opts->date.prefix, opts->date.format,
	         opts->date.suffix);
	strftime(buf, sizeof(buf), format, tptr);

	draw_text(dc, buf);
}
