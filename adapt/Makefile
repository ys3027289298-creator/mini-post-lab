CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: adapt

adapt: src/adapt.c
	$(CC) $(CFLAGS) $< -o $@ -lm

test: adapt
	./adapt

clean:
	rm -f adapt
