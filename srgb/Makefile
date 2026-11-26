CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: srgb

srgb: src/srgb.c
	$(CC) $(CFLAGS) $< -o $@ -lm

test: srgb
	./srgb

bench: srgb
	./srgb bench

clean:
	rm -f srgb

.PHONY: all test bench clean
