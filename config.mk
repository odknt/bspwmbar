VERSION = 0

PREFIX    = /usr/local
MANPREFIX = $(PREFIX)/share/man

CFLAGS  = -std=c99 -pedantic -Wall -Wextra -g3 -I/usr/include/freetype2
LDFLAGS =
LDLIBS  = -lX11 -lfontconfig -lXft -lXrandr -lxcb -lasound

CC = cc
