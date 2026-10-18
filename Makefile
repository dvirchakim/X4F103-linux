CC ?= gcc
CFLAGS ?= -O2 -Wall
LDFLAGS ?= -lft4222

replay: src/replay.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f replay frames.bin

.PHONY: clean
