CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra
PY ?= python3

all: build/readback_sim build/analytic_check build/clip_tail build/signed_delta

build/readback_sim: src/readback_sim.c
	$(CC) $(CFLAGS) $< -o $@ -lm

build/analytic_check: src/analytic_check.c
	$(CC) $(CFLAGS) $< -o $@ -lm

build/clip_tail: src/clip_tail.c
	$(CC) $(CFLAGS) $< -o $@ -lm

build/signed_delta: src/signed_delta.c
	$(CC) $(CFLAGS) $< -o $@ -lm

# the C tools write analysis/out/... relative to CWD, so run at repo root
ROOT := $(abspath $(CURDIR)/..)

run: all
	cd $(ROOT) && ./analysis/build/readback_sim && \
	  ./analysis/build/analytic_check && ./analysis/build/clip_tail && \
	  ./analysis/build/signed_delta && \
	  $(PY) -I analysis/src/render_charts.py

charts:
	$(PY) -I src/render_charts.py

clean:
	rm -f build/* out/*.csv out/*.svg
.PHONY: all run charts clean
