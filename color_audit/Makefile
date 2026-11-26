CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: color_audit

color_audit: src/color_audit.c
	$(CC) $(CFLAGS) $< -o $@ -lm

test: color_audit
	./color_audit

clean:
	rm -f color_audit
