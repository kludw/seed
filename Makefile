CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall
# Flags for compiling generated C (the compiler stages and test programs).
# compiler/main.sd has the same flags for `seed build` and `seed run`.
SEED_CFLAGS = -std=c11 -O2 -w
SEED_LIBS = -lm -lpthread

# The real compiler, in Seed. The order is fixed (sorted) so that every stage
# sees the declarations in the same order.
SEED_SRC = $(sort $(wildcard compiler/*.sd))
# The runtime that the compiler pastes into every generated program.
RUNTIME_SRC = $(wildcard runtime/*.h runtime/seed_*.c)
# The standard library packages the compiler imports are compiled into it
# (their natives are pasted into the generated C), so they are inputs too.
LIB_SRC = $(wildcard lib/*/*.sd lib/*/*.c)

.PHONY: all test0 test test-full test-leaks test-fmt test-lib test-examples verify clean

all: build/seed2

# The runtime is pasted into every generated program, so it is embedded in
# seed0 as a C string.
build/runtime.inc: seed0/runtime.c
	@mkdir -p build
	sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/"/' -e 's/$$/\\n"/' seed0/runtime.c > $@

build/seed0: seed0/seed0.c build/runtime.inc
	$(CC) $(CFLAGS) -Ibuild -o $@ seed0/seed0.c

# tests/seed0/only checks what only seed0 does: 64-bit ints that crash on
# overflow, and rejecting interfaces. The real compiler grows ints and has
# interfaces (tests/full).
test0: build/seed0
	tests/run.sh build/seed0 tests/lang tests/seed0 tests/seed0/only

# Bootstrap: seed0 compiles the compiler (stage1), which compiles itself
# (stage2), which compiles itself again (stage3). stage2.c and stage3.c must
# be identical. The real compiler always applies the pedantic checks (unused
# names, shadowing, formatting), so the compiler's own source must pass them.
build/stage1.c: build/seed0 $(SEED_SRC)
	build/seed0 $(SEED_SRC) > $@

build/seed1: build/stage1.c
	$(CC) $(SEED_CFLAGS) -o $@ build/stage1.c $(SEED_LIBS)

build/stage2.c: build/seed1 $(SEED_SRC) $(RUNTIME_SRC) $(LIB_SRC)
	build/seed1 c $(SEED_SRC) > $@

build/seed2: build/stage2.c
	$(CC) $(SEED_CFLAGS) -o $@ build/stage2.c $(SEED_LIBS)

build/stage3.c: build/seed2 $(SEED_SRC) $(RUNTIME_SRC) $(LIB_SRC)
	build/seed2 c $(SEED_SRC) > $@

verify: build/stage2.c build/stage3.c
	cmp build/stage2.c build/stage3.c
	@echo "fixed point: stage2.c and stage3.c are identical"

test: build/seed2
	tests/run.sh "build/seed2 c" tests/lang tests/seed0

# The features outside the seed0 subset, packages (tests/full/NAME/
# directories) and seed test (tests/test_cmd.sh).
test-full: build/seed2
	tests/run.sh "build/seed2 c" tests/full
	tests/test_cmd.sh build/seed2

# Every test program under macOS `leaks --atExit` (slow: about 3 minutes).
test-leaks: build/seed2
	tests/leaks.sh "build/seed2 c" tests/lang tests/seed0 tests/full

# The tests of the standard library packages (lib/<pkg>/tests).
test-lib: build/seed2
	build/seed2 test lib

# Builds every program in examples/ and runs the ones that need no person:
# wordcount on stdin, todo, and server with fetch (and curl) as clients.
test-examples: build/seed2
	tests/examples.sh build/seed2

# seed fmt. --verify runs the formatter's own checks on every .sd file under
# compiler/, tests/ and lib/ (files with syntax errors are skipped): formatting the
# formatted text changes nothing, and the tokens (ignoring newlines and
# trailing commas), comments and syntax tree are unchanged. Then the golden
# tests in tests/fmt, and a check that the compiler and the test programs
# are formatted.
FMT_CHECKED = $(SEED_SRC) $(wildcard tests/lang/*.sd tests/seed0/*.sd tests/full/*.sd)

# Package directories (tests/full/NAME/, tests/testcmd/) and the standard
# library are verified too.
FMT_VERIFIED = $(SEED_SRC) $(wildcard tests/*/*.sd tests/*/errors/*.sd) $(shell find tests/full tests/testcmd lib -mindepth 2 -name '*.sd')

test-fmt: build/seed2
	@build/seed2 fmt --verify $(FMT_VERIFIED)
	tests/fmt/run.sh build/seed2
	@build/seed2 fmt --check $(FMT_CHECKED) && echo "seed fmt --check: the compiler and the test programs are formatted"

clean:
	rm -rf build
