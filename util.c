/* See LICENSE file for copyright and license details. */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "util.h"

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
