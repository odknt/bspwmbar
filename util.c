#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "util.h"

const unsigned char utfbyte[UTF_SZ + 1] = { 0x80, 0, 0xC0, 0xE0, 0xF0 };
const unsigned char utfmask[UTF_SZ + 1] = { 0xC0, 0x80, 0xE0, 0xF0, 0xF8 };

const long utfmin[UTF_SZ + 1] = { 0, 0, 0x80, 0x800, 0x10000 };
const long utfmax[UTF_SZ + 1] = { 0x10FFFF, 0x7F, 0x7FF, 0xFFFF, 0x10FFFF };

static long
utf8decodebyte(const char c, size_t *i)
{
	for (*i = 0; *i < (UTF_SZ + 1); ++(*i))
		if (((unsigned char)c & utfmask[*i]) == utfbyte[*i])
			return (unsigned char)c & ~utfmask[*i];
	return 0;
}

static size_t
utf8validate(long *u, size_t i)
{
	if (!BETWEEN(*u, utfmin[i], utfmax[i]) || BETWEEN(*u, 0xD800, 0xDFFF))
		*u = UTF_INVALID;
	for (i = 1; *u > utfmax[i]; ++i)
		;
	return i;
}

size_t
utf8decode(const char *c, long *u, size_t clen)
{
	size_t i, j, len, type;
	long udecoded;

	*u = UTF_INVALID;
	if (!clen)
		return 0;
	udecoded = utf8decodebyte(c[0], &len);
	if (!BETWEEN(len, 1, UTF_SZ))
		return 1;
	for (i = 1, j = 1; i < clen && j < len; ++i, ++j) {
		udecoded = (udecoded << 6) | utf8decodebyte(c[i], &type);
		if (type != 0)
			return j;
	}
	if (j < len)
		return 0;
	*u = udecoded;
	utf8validate(u, len);
	return len;
}

size_t
utf8npos(const char *c, size_t idx, size_t clen)
{
	size_t i, len;
	long u;
	for (i = 0, len = 0; i < clen && len < idx; len++) {
		i += utf8decode(&c[i], &u, UTF_SZ);
	}
	return i;
}

void
die(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);

	exit(1);
}

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

const char *
bprintf(const char *fmt, ...)
{
	va_list ap;
	int ret;

	va_start(ap, fmt);
	ret = vsnprintf(buf, sizeof(buf), fmt, ap);
	if ((size_t)ret > sizeof(buf))
		ret = -1;
	va_end(ap);

	return (ret < 0) ? NULL : buf;
}
