CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

all: errchain

errchain: src/errchain.c
	$(CC) $(CFLAGS) $< -o $@ -lm

run: errchain
	mkdir -p artifacts
	./errchain

test: errchain
	mkdir -p artifacts
	./errchain > artifacts/report.txt
	@test -s artifacts/sweep.csv && test -s artifacts/step_metrics.csv \
		&& test -s artifacts/verdict.csv && test -s artifacts/sweep_curves.svg \
		&& test -s artifacts/error_breakdown_heavy_R8.svg \
		&& echo "errchain artifacts OK"

clean:
	rm -f errchain
