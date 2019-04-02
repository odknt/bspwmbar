.POSIX:

OBJ = bspwmbar.o util.o cpu.o memory.o disk.o alsa.o thermal.o datetime.o systray.o

include config.mk

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
	mkdir -p $(PREFIX)/bin/
	cp bspwmbar $(PREFIX)/bin/
.PHONY: install

uninstall:
	rm -f $(PREFIX)/bin/bspwmbar
.PHONY: uninstall
