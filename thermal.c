/* See LICENSE file for copyright and license details. */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>

#include "bspwmbar.h"
#include "util.h"

void
thermal(DC dc, Option opts)
{
	static time_t prevtime;
	static unsigned long temp;
	static int thermal_found = -1;

	if (thermal_found == -1) {
		if (access(opts.any.arg, F_OK) != -1)
			thermal_found = 1;
		else
			thermal_found = 0;
	}
	if (!thermal_found)
		return;

	time_t curtime = time(NULL);
	if (curtime - prevtime < 1)
		goto DRAW_THERMAL;
	prevtime = curtime;

	if (pscanf(opts.any.arg, "%ju", &temp) == -1)
		return;

DRAW_THERMAL:
	if (!opts.any.prefix)
		opts.any.prefix = "";
	if (!opts.any.suffix)
		opts.any.suffix = "";
	sprintf(buf, "%s%lu%s", opts.any.prefix, temp / 1000, opts.any.suffix);
	draw_text(dc, buf);
}
