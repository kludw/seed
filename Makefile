CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall
# Flags for compiling generated C (the compiler stages and test programs).
# compiler/main.sd has the same flags for `seed build` and `seed run`.
SEED_CFLAGS = -std=c11 -O2 -w
SEED_LIBS = -lm

# The real compiler, in Seed. The order is fixed (sorted) so that every stage
# sees the declarations in the same order.
SEED_SRC = $(sort $(wildcard compiler/*.sd))

.PHONY: all test0 test test-strict verify clean

all: build/seed2

# The runtime is pasted into every generated program, so it is embedded in
# seed0 as a C string.
build/runtime.inc: seed0/runtime.c
	@mkdir -p build
	sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/"/' -e 's/$$/\\n"/' seed0/runtime.c > $@

build/seed0: seed0/seed0.c build/runtime.inc
	$(CC) $(CFLAGS) -Ibuild -o $@ seed0/seed0.c

test0: build/seed0
	tests/run.sh build/seed0 tests/lang tests/seed0

# Bootstrap: seed0 compiles the compiler (stage1), which compiles itself
# (stage2), which compiles itself again (stage3). stage2.c and stage3.c must
# be identical. The later stages run with --strict, so the compiler's own
# source must pass the pedantic checks (unused names, shadowing).
build/stage1.c: build/seed0 $(SEED_SRC)
	build/seed0 $(SEED_SRC) > $@

build/seed1: build/stage1.c
	$(CC) $(SEED_CFLAGS) -o $@ build/stage1.c $(SEED_LIBS)

build/stage2.c: build/seed1 $(SEED_SRC) compiler/runtime0.c
	build/seed1 c --strict $(SEED_SRC) > $@

build/seed2: build/stage2.c
	$(CC) $(SEED_CFLAGS) -o $@ build/stage2.c $(SEED_LIBS)

build/stage3.c: build/seed2 $(SEED_SRC) compiler/runtime0.c
	build/seed2 c --strict $(SEED_SRC) > $@

verify: build/stage2.c build/stage3.c
	cmp build/stage2.c build/stage3.c
	@echo "fixed point: stage2.c and stage3.c are identical"

test: build/seed2
	tests/run.sh "build/seed2 c" tests/lang tests/seed0

# tests/lang is written to pass the full compiler's pedantic checks too.
test-strict: build/seed2
	tests/run.sh "build/seed2 c --strict" tests/lang

clean:
	rm -rf build
