VERSION ?= 0

PREFIX    ?= /usr/local
DESTDIR   ?=
MANPREFIX ?= $(PREFIX)/share/man

CFLAGS  += -std=c99 -pedantic -Wall -Wextra -I/usr/include/freetype2
LDFLAGS +=
LDLIBS  += -lX11 -lfontconfig -lXft -lXrandr -lasound

CC ?= cc
