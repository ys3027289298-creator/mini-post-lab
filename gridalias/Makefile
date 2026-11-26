PY ?= python3
SCRIPT := src/grid_alias.py

.PHONY: all test plot clean

all: test

test:
	$(PY) -S $(SCRIPT)

plot:
	$(PY) -S $(SCRIPT) --plot

clean:
	rm -f src/*.png
	rm -rf src/__pycache__
