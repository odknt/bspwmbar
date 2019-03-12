VERSION = 0

PREFIX    = /usr/local
MANPREFIX = $(PREFIX)/share/man

CFLAGS  = -std=c99 -pedantic -Wall -Wextra -Os -I/usr/include/freetype2
LDFLAGS = -s
LDLIBS  = -lX11 -lfontconfig -lXft -lXrandr -lxcb -lasound

CC = cc
