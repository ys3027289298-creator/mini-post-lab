CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: bench

bench: src/bench.c
	$(CC) $(CFLAGS) $< -o $@ -lm -lpthread

calibrate: bench
	./bench calibrate 20000
	BENCH_SYNC=spin ./bench calibrate 20000

test: bench
	./bench run sep 320 180 50
	./bench run mrt 320 180 50
	./bench run split 320 180 50

clean:
	rm -f bench bench_dbg
