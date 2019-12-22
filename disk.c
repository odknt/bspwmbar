/* See LICENSE file for copyright and license details. */

#include <math.h>
#include <stdio.h>
#include <sys/statvfs.h>
#include <time.h>

#include "bspwmbar.h"
#include "util.h"

/* functions */
static inline int calc_used(struct statvfs);
static int disk_perc(const char *);

int
calc_used(struct statvfs mp)
{
	return (mp.f_blocks - mp.f_bavail) / (double)mp.f_blocks * 100 + 0.5;
}

static int
disk_perc(const char *mpoint)
{
	static struct statvfs mp;
	static time_t prevtime;

	time_t curtime = time(NULL);
	if (curtime - prevtime < 1)
		return calc_used(mp);
	prevtime = curtime;

	if (statvfs(mpoint, &mp))
		return -1;
	return calc_used(mp);
}

void
filesystem(draw_context_t *dc, module_option_t *opts)
{
	int perc = disk_perc(opts->fs.mountpoint);
	if (!opts->fs.prefix)
		opts->fs.prefix = "";
	if (!opts->fs.suffix)
		opts->fs.suffix = "";
	sprintf(buf, "%s%d%s", opts->fs.prefix, perc, opts->fs.suffix);
	draw_text(dc, buf);
}
