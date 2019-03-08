#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bspwmbar.h"
#include "util.h"

static inline int
calc_used(MemInfo mem)
{
	return (mem.total - mem.available) / (double)mem.total * 100;
}

int
mem_perc()
{
	static time_t prevtime = { 0 };
	static MemInfo a = { 0 };
	FILE *fp;

	time_t curtime = time(NULL);
	if (curtime - prevtime < 1)
		return calc_used(a);
	prevtime = curtime;

	if (!(fp = fopen("/proc/meminfo", "r")))
		return 0;

	while (fgets(buf, sizeof(buf), fp)) {
		if (strncmp(buf, "MemTotal:", 9) == 0)
			sscanf(buf, "%*s %lu kB", &a.total);
		else if (strncmp(buf, "MemAvailable:", 8) == 0)
			sscanf(buf, "%*s %lu kB", &a.available);
	}
	fclose(fp);

	return calc_used(a);
}
