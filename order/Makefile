CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: order

order: src/order.c
	$(CC) $(CFLAGS) $< -o $@ -lm

test: order
	./order

clean:
	rm -f order
