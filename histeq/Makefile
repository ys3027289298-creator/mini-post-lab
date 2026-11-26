CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: histeq

histeq: src/histeq.c
	$(CC) $(CFLAGS) $< -o $@ -lm

test: histeq
	./histeq

clean:
	rm -f histeq
