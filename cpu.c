/* See LICENSE file for copyright and license details. */

#include <alloca.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(__linux)
# include <sys/sysinfo.h>
#elif defined(__OpenBSD__)
# include <sys/types.h>
# include <sys/sched.h>
# include <sys/sysctl.h>
#endif

#include "bspwmbar.h"
#include "util.h"

#if defined(__linux)
struct _CoreInfo {
	double user;
	double nice;
	double system;
	double idle;
	double iowait;
	double irq;
	double softirq;
	double sum;
	double loadavg;
};
#elif defined(__OpenBSD__)
struct _CoreInfo {
	uintmax_t states[CPUSTATES];
	uintmax_t sum;
	uintmax_t used;
	double loadavg;
};
#endif

static int
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
#endif
}

int
cpu_perc(double **cores)
{
	static struct _CoreInfo *a = NULL;
	static struct _CoreInfo *b = NULL;
	static double *loadavgs = NULL;
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
		a = (struct _CoreInfo *)calloc(sizeof(struct _CoreInfo), nproc);
	if (b == NULL)
		b = (struct _CoreInfo *)calloc(sizeof(struct _CoreInfo), nproc);
	if (loadavgs == NULL)
		loadavgs = (double *)calloc(sizeof(double), nproc);

	memcpy(b, a, sizeof(struct _CoreInfo) * nproc);

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
	int miblen = 3;
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
		loadavgs[i] = (a[i].used - b[i].used) / (a[i].sum - b[i].sum);
	}
#endif

	*cores = loadavgs;
	return nproc;
}

void
cpugraph(DC dc, const char *arg)
{
	(void)arg;

	double *vals;
	int ncore = cpu_perc(&vals);

	GraphItem *items = (GraphItem *)alloca(sizeof(GraphItem) * ncore);
	for (int i = 0; i < ncore; i++) {
		items[i].val = vals[i];
		if (vals[i] < 0.3) {
			items[i].colorno = 4;
		} else if (vals[i] < 0.6) {
			items[i].colorno = 5;
		} else if (vals[i] < 0.8) {
			items[i].colorno = 6;
		} else {
			items[i].colorno = 7;
		}
	}

	draw_bargraph(dc, "cpu: ", items, ncore);
}
