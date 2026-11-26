CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: bloom bloomcast

bloom: src/bloom.c
	$(CC) $(CFLAGS) $< -o $@ -lm

bloomcast: src/bloomcast.c
	$(CC) $(CFLAGS) $< -o $@ -lm

test: bloom bloomcast
	./bloom
	./bloomcast

clean:
	rm -f bloom bloomcast
