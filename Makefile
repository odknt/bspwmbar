.POSIX:

include config.mk

MODSOBJ = $(addsuffix .o,$(MODS))
OBJ     = bspwmbar.o util.o systray.o $(MODSOBJ)

BINPREFIX=$(PREFIX)/bin

all: optimized

bspwmbar: $(OBJ)

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

optimized:
	make bspwmbar CFLAGS="$(RCFLAGS)" LDFLAGS="$(RLDFLAGS)"
.PHONY: optimized

debug: clean
	make bspwmbar CFLAGS="$(DCFLAGS)" LDFLAGS="$(DLDFLAGS)"
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
