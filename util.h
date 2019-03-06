#ifndef UTIL_H_
#define BSPWMBAR_UTIL_H_
#include <unistd.h>

/* utility macros */
#define LENGTH(X)        (sizeof (X) / sizeof (X[0]))
#define BETWEEN(X, A, B) ((A) <= (X) && (X) <= (B))
#define SMALLER(A, B)    ((A) < (B) ? (A) : (B))
#define BIGGER(A, B)    ((A) > (B) ? (A) : (B))
#define DIVCEIL(n, d)    (((n) + ((d) - 1)) / (d))

/* utf8 */
#define UTF_SZ      4
#define UTF_INVALID 0xFFFD

extern char buf[1024];

size_t utf8decode(const char *, long *, size_t);
size_t utf8npos(const char *, size_t, size_t);
void die(const char *, ...);
int pscanf(const char *, const char *, ...);
const char *bprintf(const char *, ...);

#endif
