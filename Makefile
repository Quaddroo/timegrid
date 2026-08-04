CFLAGS  = -std=gnu99 -Wall -Wextra -O2 $(shell pkg-config --cflags x11 xft)
LDLIBS  = $(shell pkg-config --libs x11 xft) -lm

OBJ = timegrid.o picker.o

timegrid.bin: $(OBJ)
	$(CC) $(CFLAGS) $(OBJ) -o $@ $(LDLIBS)

timegrid.o: timegrid.c picker.h
picker.o: picker.c picker.h

# Needs static brotli/expat/png, which are not installed here. Note also that
# libX11.a still dlopen()s at runtime, so this is never fully static.
static: $(OBJ)
	$(CC) $(CFLAGS) -static $(OBJ) -o timegrid.bin $(shell pkg-config --static --libs x11 xft) -lm

clean:
	rm -f timegrid.bin $(OBJ)

.PHONY: static clean
