/* See LICENSE file for copyright and license details. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(__OpenBSD__)
# include <sys/sysctl.h>
#endif

#include "bspwmbar.h"
#include "util.h"

static inline int
calc_used(MemInfo mem)
{
#if defined(__linux)
	return (mem.total - mem.available) / (double)mem.total * 100;
#elif defined(__OpenBSD__)
	return mem.active * 100 / mem.npages;
#endif
}

int
mem_perc()
{
	static time_t prevtime = { 0 };
	static MemInfo a = { 0 };

	time_t curtime = time(NULL);
	if (curtime - prevtime < 1)
		return calc_used(a);
	prevtime = curtime;

#if defined(__linux)
	FILE *fp;
	if (!(fp = fopen("/proc/meminfo", "r")))
		return 0;

	while (fgets(buf, sizeof(buf), fp)) {
		if (strncmp(buf, "MemTotal:", 9) == 0)
			sscanf(buf, "%*s %lu kB", &a.total);
		else if (strncmp(buf, "MemAvailable:", 8) == 0)
			sscanf(buf, "%*s %lu kB", &a.available);
	}
	fclose(fp);
#elif defined(__OpenBSD__)
	int mib[] = { CTL_VM, VM_UVMEXP };
	size_t len = sizeof(a);
	if (sysctl(mib, 2, &a, &len, NULL, 0) < 0)
		return 0;
#endif
	return calc_used(a);
}

void
memgraph(DC dc, const char *arg)
{
	(void)arg;
	drawmem(dc, mem_perc());
}
