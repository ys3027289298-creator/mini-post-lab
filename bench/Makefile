CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: bench

bench: src/bench.c
	$(CC) $(CFLAGS) $< -o $@ -lm

run: bench
	(taskset -c 0 ./bench 2>/dev/null || ./bench) | tee bench_out.txt

analyze: run
	python3 -I analyze.py bench_out.txt

clean:
	rm -f bench bench_out.txt summary.json tradeoff.html
