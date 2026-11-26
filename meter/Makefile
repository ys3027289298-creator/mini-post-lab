CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: meter

meter: src/meter.c
	$(CC) $(CFLAGS) $< -o $@ -lm

test: meter
	./meter

clean:
	rm -f meter
