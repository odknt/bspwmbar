PREFIX    ?= /usr/local
DESTDIR   ?=
MANPREFIX ?= $(PREFIX)/share/man

PKG_CONFIG ?= pkg-config

DEPS ?= x11 xft xrandr xext fontconfig alsa
MODS ?= cpu memory disk thermal datetime alsa

INCS = `$(PKG_CONFIG) --cflags $(DEPS)`
LDLIBS = `$(PKG_CONFIG) --libs $(DEPS)`

CFLAGS  += $(INCS) -std=c99 -pedantic -Wall -Wextra
LDFLAGS += $(LDLIBS)

# debug flags
DCFLAGS  = $(CFLAGS) -fsanitize=address -fno-omit-frame-pointer -g
DLDFLAGS = $(LDFLAGS) -fsanitize=address
# release flags
RCFLAGS  = $(CFLAGS) -Os -DNDEBUG
RLDFLAGS = $(LDFLAGS) -s
