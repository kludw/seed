CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall

.PHONY: all test0 clean

all: build/seed0

# The runtime is pasted into every generated program, so it is embedded in
# seed0 as a C string.
build/runtime.inc: seed0/runtime.c
	@mkdir -p build
	sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/"/' -e 's/$$/\\n"/' seed0/runtime.c > $@

build/seed0: seed0/seed0.c build/runtime.inc
	$(CC) $(CFLAGS) -Ibuild -o $@ seed0/seed0.c

test0: build/seed0
	tests/run.sh build/seed0 tests/lang tests/seed0

clean:
	rm -rf build
