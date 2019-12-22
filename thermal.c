/* See LICENSE file for copyright and license details. */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>

#include "bspwmbar.h"
#include "util.h"

void
thermal(draw_context_t *dc, module_option_t *opts)
{
	static time_t prevtime;
	static unsigned long temp;
	static int thermal_found = -1;

	if (thermal_found == -1) {
		if (access(opts->thermal.sensor, F_OK) != -1)
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

	if (pscanf(opts->thermal.sensor, "%ju", &temp) == -1)
		return;

DRAW_THERMAL:
	if (!opts->thermal.prefix)
		opts->thermal.prefix = "";
	if (!opts->thermal.suffix)
		opts->thermal.suffix = "";
	sprintf(buf, "%s%lu%s", opts->thermal.prefix, temp / 1000,
	        opts->thermal.suffix);
	draw_text(dc, buf);
}
