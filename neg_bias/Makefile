CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: neg_bias

neg_bias: src/neg_bias.c
	$(CC) $(CFLAGS) $< -o $@ -lm

run: neg_bias
	mkdir -p out
	./neg_bias | tee out/results.txt

clean:
	rm -f neg_bias
