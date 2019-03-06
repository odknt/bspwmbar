#include <math.h>
#include <sys/statvfs.h>
#include <time.h>

#include "bspwmbar.h"
#include "util.h"

static inline int
calc_used(struct statvfs mp)
{
	return (mp.f_blocks - mp.f_bavail) / (double)mp.f_blocks * 100 + 0.5;
}

int
disk_perc()
{
	static struct statvfs mp;
	static time_t prevtime;

	time_t curtime = time(NULL);
	if (curtime - prevtime < 1)
		return calc_used(mp);
	prevtime = curtime;

	if (statvfs("/", &mp))
		return -1;
	return calc_used(mp);
}
