CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: transluc

transluc: src/transluc.c
	$(CC) $(CFLAGS) $< -o $@ -lm

test: transluc
	./transluc

clean:
	rm -f transluc
