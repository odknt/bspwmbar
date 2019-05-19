.POSIX:

PREFIX    ?= /usr/local
DESTDIR   ?=
MANPREFIX ?= $(PREFIX)/share/man

OBJ = bspwmbar.o util.o systray.o $(MODS:=.o)

BINPREFIX=$(PREFIX)/bin

all: optimized

bspwmbar: $(OBJ)
	$(CC) -o $@ $(OBJ) $(CFLAGS) $(LDLIBS) $(LDFLAGS)

$(OBJ): config.h util.h bspwmbar.h

.c.o: config.h
	$(CC) -o $@ -c $(CFLAGS) $<

config.h:
	cp config.def.h $@

config: config.def.h
	cp config.def.h config.h
.PHONY: config

clean:
	rm -f bspwmar $(OBJ)
.PHONY: clean

optimized: bspwmbar
.PHONY: optimized

debug: clean bspwmbar
.PHONY: debug

run: bspwmbar
	./bspwmbar

install:
	mkdir -p $(DESTDIR)$(BINPREFIX)
	cp bspwmbar $(DESTDIR)$(BINPREFIX)/
.PHONY: install

uninstall:
	rm -f $(DESTDIR)$(BINPREFIX)/bspwmbar
.PHONY: uninstall
