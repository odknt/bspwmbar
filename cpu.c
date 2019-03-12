/* See LICENSE file for copyright and license details. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysinfo.h>
#include <time.h>

#include "bspwmbar.h"
#include "util.h"

int
cpu_perc(CoreInfo **cores)
{
	static CoreInfo *a = NULL;
	static CoreInfo *b = NULL;
	static time_t prevtime;
	int i = 0;
	FILE *fp;

	int nproc = get_nprocs();

	time_t curtime = time(NULL);
	if (curtime - prevtime < 1) {
		*cores = a;
		return nproc;
	}
	prevtime = curtime;

	if (a == NULL)
		a = (CoreInfo *)malloc(sizeof(CoreInfo) * nproc);
	if (b == NULL)
		b = (CoreInfo *)malloc(sizeof(CoreInfo) * nproc);

	memcpy(b, a, sizeof(CoreInfo) * nproc);

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

	*cores = a;
	return nproc;
}
