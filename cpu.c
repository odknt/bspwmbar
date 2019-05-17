/* See LICENSE file for copyright and license details. */

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
cpu_perc(CoreInfo **cores)
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
		*cores = a;
		return nproc;
	}
	prevtime = curtime;

	if (a == NULL)
		a = (CoreInfo *)calloc(sizeof(CoreInfo), nproc);
	if (b == NULL)
		b = (CoreInfo *)calloc(sizeof(CoreInfo), nproc);

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
		a[i].loadavg = used / (b[i].sum - a[i].sum) * 100 + 0.5;
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
		a[i].loadavg = (a[i].used - b[i].used) /
		               (a[i].sum - b[i].sum) * 100 + 5;
	}
#endif

	*cores = a;
	return nproc;
}

void
cpugraph(DC dc, const char *arg)
{
	(void)arg;

	CoreInfo *cores;
	int ncore = cpu_perc(&cores);
	drawcpu(dc, cores, ncore);
}
