CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: bloom

bloom: src/bloom.c
	$(CC) $(CFLAGS) $< -o $@ -lm

test: bloom
	./bloom

clean:
	rm -f bloom
