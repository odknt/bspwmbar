.POSIX:

OBJ = bspwmbar.o util.o cpu.o memory.o disk.o alsa.o thermal.o datetime.o

include config.mk

all: bspwmbar

bspwmbar: $(OBJ)

$(OBJ): config.h util.h bspwmbar.h

.c.o: config.h
	$(CC) -o $@ -c $(CFLAGS) $<

config.h:
	cp config.def.h $@

config: config.def.h
	cp config.def.h config.h

clean:
	rm -f bspwmar $(OBJ)

run: bspwmbar
	./bspwmbar
