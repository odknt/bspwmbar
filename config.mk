VERSION ?= 0

PREFIX    ?= /usr/local
DESTDIR   ?=
MANPREFIX ?= $(PREFIX)/share/man

CFLAGS  += -std=c99 -pedantic -Wall -Wextra -I/usr/X11R6/include -I/usr/X11R6/include/freetype2
LDFLAGS += -L/usr/X11R6/lib
LDLIBS  += -lX11 -lfontconfig -lXft -lXrandr -lasound

CC ?= cc
