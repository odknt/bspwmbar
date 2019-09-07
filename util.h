/* See LICENSE file for copyright and license details. */

#ifndef BSPWMBAR_UTIL_H_
#define BSPWMBAR_UTIL_H_
#include <unistd.h>

/*
 * color settings by index of color map
 */
extern const char *colors[9];  // Set in utils.c
typedef enum {
    WHITE = 0,
    BLACK,
    GRAY,
    DARK_GRAY,
    LIGHT_GREEN,
    GREEN,
    ORANGE,
    RED,
    BLUE,
} Colors;

/* utility macros */
#define LENGTH(X)        (sizeof (X) / sizeof (X[0]))
#define BETWEEN(X, A, B) ((A) <= (X) && (X) <= (B))
#define SMALLER(A, B)    ((A) < (B) ? (A) : (B))
#define BIGGER(A, B)     ((A) > (B) ? (A) : (B))
#define DIVCEIL(n, d)    (((n) + ((d) - 1)) / (d))

#define die(...) { fprintf(stderr, __VA_ARGS__); exit(1); }
#define err(...) { fprintf(stderr, __VA_ARGS__); }

extern char buf[1024];

int pscanf(const char *, const char *, ...);

typedef struct _list_head {
    struct _list_head *prev, *next;
} list_head;

/* list */
#define list_head_init(ptr) { (ptr)->prev = (ptr); (ptr)->next = (ptr); }
#define list_for_each(head, pos) \
for (pos = (head)->next; pos != (head); pos = pos->next)
#define list_entry(ptr, type, member) \
((type *)((char *)(ptr)-(unsigned long)(&((type *)0)->member)))
#define list_empty(head) ((head)->next == (head))
#define list_count(head, num) \
for (list_head *pos = (head)->next; pos != (head); pos = pos->next) num++
#define list_for_each_safe(head, pos, n) \
for (pos = (head)->next, n = pos->next; pos != (head); pos = n, n = pos->next)

void list_init(list_head *head, list_head *prev, list_head *next);
void list_add(list_head *head, list_head *entry);
void list_add_tail(list_head *head, list_head *entry);
void list_del(list_head *head);

#endif
