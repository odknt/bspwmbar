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

#if defined(__linux)
typedef struct {
	size_t total;
	size_t available;
} MemInfo;
#elif defined(__OpenBSD__)
typedef struct uvmexp MemInfo;
#endif

/* functions */
static inline double calc_used(MemInfo);
static double mem_perc();

static const char *deffgcols[4] = {
	"#449f3d", /* success color */
	"#2f8419", /* normal color */
	"#f5a70a", /* warning color */
	"#ed5456", /* critical color */
};

double
calc_used(MemInfo mem)
{
#if defined(__linux)
	return (double)(mem.total - mem.available) / mem.total;
#elif defined(__OpenBSD__)
	return (double)mem.active / mem.npages;
#endif
}

double
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
memgraph(draw_context_t *dc, module_option_t *opts)
{
	graph_item_t items[10];
	color_t *fgcols[4];
	color_t *bgcol;
	double used = mem_perc();
	int i;

	bgcol = color_load("#555555");
	for (i = 0; i < 4; i++) {
		if (opts->mem.cols[i])
			fgcols[i] = color_load(opts->cpu.cols[i]);
		else
			fgcols[i] = color_load(deffgcols[i]);
	}

	for (int i = 0; i < 10; i++) {
		items[i].bg = bgcol;
		items[i].val = (used > ((double)i / 10)) ? 1 : -1;
		if (i < 3)
			items[i].fg = fgcols[0];
		else if (i < 6)
			items[i].fg = fgcols[1];
		else if (i < 8)
			items[i].fg = fgcols[2];
		else
			items[i].fg = fgcols[3];
	}
	if (!opts->mem.prefix)
		opts->mem.prefix = "";
	draw_bargraph(dc, opts->mem.prefix, items, 10);
}
