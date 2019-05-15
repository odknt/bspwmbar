PREFIX    ?= /usr/local
DESTDIR   ?=
MANPREFIX ?= $(PREFIX)/share/man

PKG_CONFIG ?= pkg-config

DEPS ?= x11 xft xrandr fontconfig alsa
MODS ?= cpu memory disk thermal datetime systray alsa

INCS = $(shell $(PKG_CONFIG) --cflags $(DEPS))
LIBS = $(shell $(PKG_CONFIG) --libs $(DEPS))

CFLAGS  += $(INCS) -std=c99 -pedantic -Wall -Wextra
LDFLAGS += $(LIBS)

CC ?= cc
