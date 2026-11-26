CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: gamma bench

gamma: src/gamma.c
	$(CC) $(CFLAGS) $< -o $@ -lm

bench: src/bench.c
	$(CC) $(CFLAGS) $< -o $@ -lm

test: gamma bench
	./gamma
	./bench

clean:
	rm -f gamma bench
