/* See LICENSE file for copyright and license details. */

/* See LICENSE file for copyright and license details. */
/* See LICENSE file for copyright and license details. */#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>

#include "bspwmbar.h"
#include "util.h"

static char *format = " %d℃";

char *
thermal(const char *thermal_path)
{
	static time_t prevtime;
	static uintmax_t temp;
	static int thermal_found = -1;

	if (thermal_found == -1) {
		if (access(thermal_path, F_OK) != -1)
			thermal_found = 1;
		else
			thermal_found = 0;
	}
	if (!thermal_found)
		return NULL;

	time_t curtime = time(NULL);
	if (curtime - prevtime < 1) {
		sprintf(buf, format, temp / 1000);
		return buf;
	}
	prevtime = curtime;

	if (pscanf(thermal_path, "%ju", &temp) == -1) {
		return NULL;
	}

	sprintf(buf, format, temp / 1000);
	return buf;
}
