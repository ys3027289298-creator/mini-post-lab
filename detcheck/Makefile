CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: detcheck

detcheck: src/plugin.c src/harness.c src/plugin.h
	$(CC) $(CFLAGS) src/plugin.c src/harness.c -o $@ -lm -pthread

test: detcheck
	./detcheck

clean:
	rm -f detcheck
