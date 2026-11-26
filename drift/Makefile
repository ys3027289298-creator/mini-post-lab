CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra

.PHONY: all run data clean

all: drift_lab

drift_lab: src/drift_lab.c
	$(CC) $(CFLAGS) $< -o $@ -lm

run: drift_lab
	mkdir -p out
	./drift_lab | tee out/results.txt

data: run
	python3 -I tools/analyze.py

clean:
	rm -f drift_lab
	rm -rf out report/data.js
