PREFIX    ?= /usr/local
DESTDIR   ?=
MANPREFIX ?= $(PREFIX)/share/man

PKG_CONFIG ?= pkg-config

DEPENDS= x11 xft xrandr fontconfig alsa

INCS = $(shell $(PKG_CONFIG) --cflags $(DEPENDS))
LIBS = $(shell $(PKG_CONFIG) --libs $(DEPENDS))

CFLAGS  += $(INCS) -std=c99 -pedantic -Wall -Wextra
LDFLAGS += $(LIBS)

CC ?= cc
