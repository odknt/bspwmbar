#ifndef BSPWMBAR_H_
#define BSPWMBAR_H_

#include <unistd.h>

typedef struct {
	double user;
	double nice;
	double system;
	double idle;
	double iowait;
	double irq;
	double softirq;
	double sum;
	double loadavg;
} CoreInfo;

typedef struct {
	size_t total;
	size_t available;
} MemInfo;

typedef struct {
	long volume;
	int  unmuted;
} AlsaInfo;

int cpu_perc(CoreInfo **);
int mem_perc();
int disk_perc();

int alsa_connect();
void alsa_disconnect();
int alsa_need_update();
int thermal_val(const char *);
AlsaInfo alsa_info();

#endif
