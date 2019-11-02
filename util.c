/* See LICENSE file for copyright and license details. */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <X11/Xlib.h>

#include "util.h"

static int xerror_handler(Display *, XErrorEvent *);

static int xerror = 0;
static int (* handler)(Display *, XErrorEvent *) = NULL;

int
pscanf(const char *path, const char *fmt, ...)
{
	FILE *fp;
	va_list ap;
	int n;

	if (!(fp = fopen(path, "r"))) {
		return -1;
	}
	va_start(ap, fmt);
	n = vfscanf(fp, fmt, ap);
	va_end(ap);
	fclose(fp);

	return (n == EOF) ? -1 : n;
}

void
list_init(list_head *head, list_head *prev, list_head *next)
{
	next->prev = head;
	head->next = next;
	head->prev = prev;
	prev->next = head;
}

void
list_add(list_head *head, list_head *entry)
{
	list_init(entry, head, head->next);
}

void
list_add_tail(list_head *head, list_head *entry)
{
	list_init(entry, head->prev, head);
}

void
list_del(list_head *head)
{
	list_head *prev = head->prev;
	list_head *next = head->next;

	next->prev = prev;
	prev->next = next;
}

void
xerror_begin()
{
	xerror = 0;
	handler = XSetErrorHandler(xerror_handler);
}

void
xerror_end()
{
	if (handler)
		XSetErrorHandler(handler);
	handler = NULL;
}

int
xerror_catch(Display *dpy)
{
	int err;

	XSync(dpy, 0);
	err = xerror;
	xerror = 0;

	return err;
}

int
xerror_handler(Display *dpy, XErrorEvent *err)
{
	(void)dpy;
	xerror = err->type;
	return 0;
}
