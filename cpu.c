/* See LICENSE file for copyright and license details. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(__linux)
# include <alloca.h>
# include <sys/sysinfo.h>
#elif defined(__OpenBSD__) || defined(__FreeBSD__)
# include <sys/types.h>
# include <sys/sched.h>
# include <sys/sysctl.h>
#endif
#if defined(__FreeBSD__)
# include <sys/resource.h>
#endif

#include "bspwmbar.h"
#include "util.h"

#if defined(__linux)
typedef struct {
	double user;
	double nice;
	double system;
	double idle;
	double iowait;
	double irq;
	double softirq;
	double sum;
} CoreInfo;
#elif defined(__OpenBSD__) || defined(__FreeBSD__)
typedef struct {
	uintmax_t states[CPUSTATES];
	uintmax_t sum;
	uintmax_t used;
} CoreInfo;
#endif

/* functions */
static int num_procs();
static int cpu_perc(double **);

static const char *deffgcols[4] = {
	"#449f3d", /* success color */
	"#2f8419", /* normal color */
	"#f5a70a", /* warning color */
	"#ed5456", /* critical color */
};
static double *loadavgs = NULL;

int
num_procs()
{
	static int nproc = 0;

	if (nproc)
		return nproc;

#if defined(__linux)
	nproc = get_nprocs();
	return nproc;
#elif defined(__OpenBSD__)
	int mibnproc[2] = { CTL_HW, HW_NCPU };
	size_t len = sizeof(nproc);

	if (sysctl(mibnproc, 2, &nproc, &len, NULL, 0) < 0)
		return -1;
	return nproc;
/* the above works for FreeBSD, however we are only able to get cpu usage for the entire cpu from sysctl */
#elif defined(__FreeBSD__)
	return 1;
#endif
}

int
cpu_perc(double **cores)
{
	static CoreInfo *a = NULL;
	static CoreInfo *b = NULL;
	static time_t prevtime;
	int i = 0;
	int nproc;

	if ((nproc = num_procs()) == -1)
		return 0;

	time_t curtime = time(NULL);
	if (curtime - prevtime < 1) {
		*cores = loadavgs;
		return nproc;
	}
	prevtime = curtime;

	if (a == NULL)
		a = (CoreInfo *)calloc(sizeof(CoreInfo), nproc);
	if (b == NULL)
		b = (CoreInfo *)calloc(sizeof(CoreInfo), nproc);
	if (loadavgs == NULL)
		loadavgs = (double *)calloc(sizeof(double), nproc);

	memcpy(b, a, sizeof(CoreInfo) * nproc);

#if defined(__linux)
	FILE *fp;
	if (!(fp = fopen("/proc/stat", "r")))
		return 0;

	while (fgets(buf, sizeof(buf), fp)) {
		if (strncmp(buf, "cpu ", 4) == 0)
			continue;
		if (strncmp(buf, "cpu", 3) != 0)
			break;
		sscanf(buf, "%*s %lf %lf %lf %lf %lf %lf %lf", &a[i].user, &a[i].nice,
		       &a[i].system, &a[i].idle, &a[i].iowait, &a[i].irq,
		       &a[i].softirq);
		b[i].sum = (b[i].user + b[i].nice + b[i].system + b[i].idle +
		            b[i].iowait + b[i].irq + b[i].softirq);
		a[i].sum = (a[i].user + a[i].nice + a[i].system + a[i].idle +
		            a[i].iowait + a[i].irq + a[i].softirq);
		double used =
		  (b[i].user + b[i].nice + b[i].system + b[i].irq + b[i].softirq) -
		  (a[i].user + a[i].nice + a[i].system + a[i].irq + a[i].softirq);
		loadavgs[i] = used / (b[i].sum - a[i].sum);
		i++;
	}
	fclose(fp);
#elif defined(__OpenBSD__)
	int mibcpu[3] = { CTL_KERN, 0, 0 };
	size_t miblen = 3;
	size_t len = sizeof(a[i].states);

	if (nproc == 1) {
		mibcpu[1] = KERN_CPTIME;
		miblen = 2;
	} else {
		mibcpu[1] = KERN_CPTIME2;
	}

	for (i = 0; i < nproc; i++) {
		mibcpu[2] = i;
		sysctl(mibcpu, miblen, &a[i].states, &len, NULL, 0);
		a[i].sum = (a[i].states[CP_USER] + a[i].states[CP_NICE] +
		            a[i].states[CP_SYS] + a[i].states[CP_INTR] +
		            a[i].states[CP_IDLE]);
		a[i].used = a[i].sum - a[i].states[CP_IDLE];
		loadavgs[i] = (double)(a[i].used - b[i].used) / (a[i].sum - b[i].sum);
	}
#elif defined(__FreeBSD__)
	size_t len = sizeof(a[i].states);

	sysctlbyname("kern.cp_time", &a[0].states, &len, NULL, 0);
	a[0].sum = (a[0].states[CP_USER] + a[0].states[CP_NICE] +
				a[0].states[CP_SYS] + a[0].states[CP_INTR] +
				a[0].states[CP_IDLE]);
	a[0].used = a[0].sum - a[0].states[CP_IDLE];
	loadavgs[i] = (double)(a[0].used - b[0].used) / (a[0].sum - b[0].sum);
#endif

	*cores = loadavgs;
	return nproc;
}

void
cpugraph(draw_context_t *dc, module_option_t *opts)
{
	color_t *fgcols[4];
	color_t *bgcol;
	double *vals = NULL;
	int i, ncore = cpu_perc(&vals);

	bgcol = color_load("#555555");
	for (i = 0; i < 4; i++) {
		if (opts->cpu.cols[i])
			fgcols[i] = color_load(opts->cpu.cols[i]);
		else
			fgcols[i] = color_load(deffgcols[i]);
	}

	graph_item_t *items = (graph_item_t *)alloca(sizeof(graph_item_t) * ncore);
	for (int i = 0; i < ncore; i++) {
		items[i].bg = bgcol;
		items[i].val = vals[i];
		if (vals[i] < 0.3) {
			items[i].fg = fgcols[0];
		} else if (vals[i] < 0.6) {
			items[i].fg = fgcols[1];
		} else if (vals[i] < 0.8) {
			items[i].fg = fgcols[2];
		} else {
			items[i].fg = fgcols[3];
		}
	}

	if (!opts->cpu.prefix)
		opts->cpu.prefix = "";
	draw_bargraph(dc, opts->cpu.prefix, items, ncore);
}
