PREFIX    ?= /usr/local
DESTDIR   ?=
MANPREFIX ?= $(PREFIX)/share/man

PKG_CONFIG ?= pkg-config

DEPS ?= x11 xft xrandr xext fontconfig alsa
MODS ?= cpu.o memory.o disk.o thermal.o datetime.o alsa.o

INCS = `$(PKG_CONFIG) --cflags $(DEPS)`
LIBS = `$(PKG_CONFIG) --libs $(DEPS)`

CFLAGS  += $(INCS) -std=c99 -pedantic -Wall -Wextra
LDFLAGS += $(LIBS)

# debug flags
DCFLAGS  = -fsanitize=address -fno-omit-frame-pointer -g
DLDFLAGS = -fsanitize=address
# release flags
RCFLAGS  = -Os -DNDEBUG
RLDFLAGS = -s
