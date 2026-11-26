CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: lut_test

lut_test: src/lut_test.c src/lut.c src/lut.h
	$(CC) $(CFLAGS) src/lut_test.c src/lut.c -o lut_test -lm

lut_bench: analysis/lut_bench.c src/lut.c src/lut.h
	$(CC) $(CFLAGS) -Isrc analysis/lut_bench.c src/lut.c -o lut_bench -lm

bench: lut_bench
	./lut_bench | tee analysis/sweep.csv

test: lut_test
	./lut_test

clean:
	rm -f lut_test lut_bench
