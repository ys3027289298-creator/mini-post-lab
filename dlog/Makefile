CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: dlog

dlog: src/dlog.c
	$(CC) $(CFLAGS) $< -o $@ -lm

test: dlog
	mkdir -p logs
	./dlog

clean:
	rm -f dlog
	rm -rf logs
