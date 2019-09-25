/* See LICENSE file for copyright and license details. */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
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
		if (access(opts.arg, F_OK) != -1)
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

	if (pscanf(opts.arg, "%ju", &temp) == -1)
		return;

DRAW_THERMAL:
	if (!opts.prefix)
		opts.prefix = "";
	if (!opts.suffix)
		opts.suffix = "";
	sprintf(buf, "%s%lu%s", opts.prefix, temp / 1000, opts.suffix);
	draw_text(dc, buf);
}
