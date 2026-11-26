CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: xplat_test

xplat_test: src/xplat_test.c
	$(CC) $(CFLAGS) $< -o $@ -lm

test: xplat_test
	./xplat_test

clean:
	rm -f xplat_test
