.POSIX:

VERSION != git describe --tags

DESTDIR   ?=
PREFIX    ?= /usr/local
MANPREFIX := $(PREFIX)/share/man
BINPREFIX := $(PREFIX)/bin
OBJDIR    := .objects

OS != uname -s | tr [A-Z] [a-z]

DEPS = xcb xcb-ewmh xcb-util xcb-randr xcb-shm xcb-renderutil cairo harfbuzz fontconfig
MODS = bspwm cpu memory disk thermal datetime battery backlight volume

include targets/$(OS).mk

DEPCFLAGS  != pkg-config --cflags $(DEPS)
DEPLDFLAGS != pkg-config --libs $(DEPS)

CFLAGS  += -Wall -Wextra -pedantic -pipe -fstack-protector-strong -fno-plt -D_XOPEN_SOURCE=700 $(DEPCFLAGS)
LDFLAGS += $(DEPLDFLAGS)

OBJ := bspwmbar.o util.o systray.o draw.o poll.o font.o $(MODS:=.o)
OBJ := $(OBJ:%=$(OBJDIR)/%)

VPATH := src/

all: CFLAGS  += -Os -DNDEBUG
all: LDFLAGS += -s
all: bspwmbar
.PHONY: all

debug: CC      := clang
debug: CFLAGS  += -g -fsanitize=address
debug: LDFLAGS += -fsanitize=address
debug: bspwmbar
.PHONY: debug

bspwmbar: config.h util.h bspwmbar.h $(OBJ)
	$(CC) -o $@ $(OBJ) $(CFLAGS) $(LDFLAGS) -DVERSION='"$(VERSION)"'

$(OBJDIR)/%.o: %.c
	@mkdir -p $(OBJDIR)/
	$(CC) -c -o $@ $^ $(CFLAGS)

config.h:
	cp config.def.h $@

config: config.def.h
	cp config.def.h config.h
.PHONY: config

clean:
	rm -f bspwmar $(OBJ)
.PHONY: clean

run: bspwmbar
	./bspwmbar

install:
	mkdir -p $(DESTDIR)$(BINPREFIX)
	cp bspwmbar $(DESTDIR)$(BINPREFIX)/
.PHONY: install

uninstall:
	rm -f $(DESTDIR)$(BINPREFIX)/bspwmbar
.PHONY: uninstall
