.POSIX:

include config.mk

MODSOBJ = $(addsuffix .o,$(MODS))
OBJ     = bspwmbar.o util.o $(MODSOBJ)

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

optimized: CFLAGS+= -Os -DNDEBUG
optimized: LDFLAGS+= -s
optimized: bspwmbar
.PHONY: optimized

debug: CFLAGS+= -fsanitize=address -fno-omit-frame-pointer -g
debug: LDFLAGS+= -fsanitize=address
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
