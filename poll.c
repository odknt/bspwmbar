/* See LICENSE file for copyright and license details. */

#include <stdlib.h>
#include <unistd.h>

#include "poll.h"
#include "util.h"

static enum bb_poll_result timer_reset(int);
static int bb_poll_create(void);
static int epoll_wait_ignore_eintr(int, struct epoll_event *, int, int);

struct bb_poll_manager *
bb_poll_manager_new()
{
	int pfd = bb_poll_create();
	if (!pfd)
		return NULL;

	struct bb_poll_manager *pm = calloc(1, sizeof(struct bb_poll_manager));
	list_head_init(&pm->fds);
	pm->pfd = pfd;

#if defined(__linux)
	/* initialize timer */
	pm->interval.it_interval.tv_sec = 1;
	pm->interval.it_value.tv_sec = 1;
	pm->timer.fd = timerfd_create(CLOCK_REALTIME, 0);
	timerfd_settime(pm->timer.fd, 0, &pm->interval, NULL);

	pm->timer.handler = timer_reset;
	bb_poll_manager_add(pm, &pm->timer);
#elif defined(__OpenBSD__)
	pm->tspec.tv_sec = 1;
#endif

	return pm;
}

void
bb_poll_manager_destroy(struct bb_poll_manager *pm)
{
	list_head *cur;
	list_head *tmp;
	list_for_each_safe(&pm->fds, cur, tmp)
		bb_poll_manager_del(pm, list_entry(cur, struct bb_poll_option, head));

	free(pm);
}

void
bb_poll_manager_add(struct bb_poll_manager *pm, struct bb_poll_option *popt)
{
	if (popt->init)
		popt->fd = popt->init();

#if defined(__linux)
	struct epoll_event ev = { 0 };

	ev.events = EPOLLIN;
	ev.data.fd = popt->fd;
	ev.data.ptr = popt;

	if (epoll_ctl(pm->pfd, EPOLL_CTL_ADD, popt->fd, &ev) == -1)
		die("epoll_ctl(): failed to add to epoll\n");
#elif defined(__OpenBSD__)
	struct kevent ev = { 0 };

	EV_SET(&ev, popt->fd, EVFILT_READ, EV_ADD, 0, 0, popt);
	if (kevent(pm->pfd, &ev, 1, NULL, 0, NULL) == -1)
		die("EV_SET(): failed to add to kqueue\n");
#endif
	list_add_tail(&pm->fds, &popt->head);
}

void
bb_poll_manager_del(struct bb_poll_manager *pm, struct bb_poll_option *popt)
{
	if (popt->fd) {
#if defined(__linux)
		epoll_ctl(pm->pfd, EPOLL_CTL_DEL, popt->fd, NULL);
#elif defined(__OpenBSD__)
		struct kevent ev = { 0 };
		EV_SET(&ev, popt->fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
		kevent(pm->pfd, &ev, 1, NULL, 0, NULL);
#endif
		if (popt->deinit)
			popt->deinit(popt->fd);
	}
	list_del(&popt->head);
}

void
bb_poll_manager_stop(struct bb_poll_manager *pm)
{
	if (pm->pfd > 0) {
		close(pm->pfd);
		pm->pfd = 0;
	}
	pm->stopped = true;
}

int
bb_poll_manager_wait_events(struct bb_poll_manager *pm)
{
	if (pm->stopped)
		return -1;
#if defined(__linux)
	return epoll_wait_ignore_eintr(pm->pfd, pm->events, MAX_EVENTS, -1);
#elif defined(__OpenBSD__)
	pm->tspec.tv_sec = 1;
	return kevent(pm->pfd, NULL, 0, pm->events, MAX_EVENTS, &pm->tspec);
#else
	return -1;
#endif
}

void
bb_poll_manager_timer_reset(struct bb_poll_manager *pm)
{
#if defined(__linux)
	timerfd_settime(pm->timer.fd, 0, &pm->interval, NULL);
#endif
}

int
bb_poll_create(void)
{
	int pfd;

#if defined(__linux)
	/* epoll */
	if ((pfd = epoll_create1(0)) == -1) {
		err("epoll_create1(): Failed to create epoll fd\n");
		return 0;
	}
#elif defined(__OpenBSD__)
	if (!(pfd = kqueue())) {
		err("kqueue(): Failed to create kqueue fd\n");
		return 0;
	}
#endif

	return pfd;
}

#if defined(__linux)
enum bb_poll_result
timer_reset(int fd)
{
	uint64_t tcnt;
	if (read(fd, &tcnt, sizeof(uint64_t)) < 0)
		return PR_FAILED;
	return PR_UPDATE;
}

/*
 * epoll_wait_ignore_eintr()
 *
 * epoll_wait() wrapper to ignore EINTR errno
 * EINTR errno can be set when the process was interrupted
 * for example on breakpoint stop or when system goes to sleep
 */
int
epoll_wait_ignore_eintr(int pfd, struct epoll_event *events, int maxevents, int timeout)
{
	int nfd;
	errno = 0;
	nfd = epoll_wait(pfd, events, maxevents, timeout);
	if (nfd != -1)
		return nfd;
	if (errno == EINTR)
		return 0;
	return -1;
}
#endif
