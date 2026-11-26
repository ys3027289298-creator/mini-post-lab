CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: bloom pyramid

bloom: src/bloom.c
	$(CC) $(CFLAGS) $< -o $@ -lm

pyramid: src/pyramid.c
	$(CC) $(CFLAGS) $< -o $@ -lm

test: bloom pyramid
	./bloom
	./pyramid

clean:
	rm -f bloom pyramid
