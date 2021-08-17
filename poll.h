/* See LICENSE file for copyright and license details. */

#ifndef BSPWMBAR_POLL_H
#define BSPWMBAR_POLL_H

#if defined(__linux)

#include <errno.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <sys/un.h>

#elif defined(__OpenBSD__)

#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>

#endif

#include <stdbool.h>

#include "util.h"

#define MAX_EVENTS 10

#if defined(__linux)
#define bb_poll_event_data(ev) ev.data.ptr
#elif defined(__OpenBSD__)
#define bb_poll_event_data(ev) ev.udata
#endif

enum bb_poll_result {
	PR_NOOP   =  0,
	PR_UPDATE,
	PR_REINIT,
	PR_FAILED
};

typedef int (* bb_poll_init_func)();
typedef void (* bb_poll_deinit_func)(int);
typedef enum bb_poll_result (* bb_poll_update_func)();
struct bb_poll_option {
	int fd;
	bb_poll_init_func init; /* initialize and return fd */
	bb_poll_deinit_func deinit; /* close fd and cleanup resources */
	bb_poll_update_func handler; /* event handler for fd */

	list_head head;
};

struct bb_poll_manager {
	int pfd;
	list_head fds;
	bool stopped;

#if defined(__linux)
	struct bb_poll_option timer;
	struct itimerspec interval;
	struct epoll_event events[MAX_EVENTS];
#elif defined(__OpenBSD__)
	struct kevent events[MAX_EVENTS];
	struct timespec tspec;
#endif
};

struct bb_poll_manager *bb_poll_manager_new();
void bb_poll_manager_destroy(struct bb_poll_manager *);
void bb_poll_manager_add(struct bb_poll_manager *, struct bb_poll_option *);
void bb_poll_manager_del(struct bb_poll_manager *, struct bb_poll_option *);
void bb_poll_manager_stop(struct bb_poll_manager *);
int bb_poll_manager_wait_events(struct bb_poll_manager *);
void bb_poll_manager_timer_reset(struct bb_poll_manager *);

#endif /* BSPWMBAR_POLL_H */
