CC ?= cc
# Flags for compiling generated C (the compiler stages and test programs).
# compiler/main.sd has the same flags for `seed build`, `seed run` and
# `seed test` (cFlags, cLibs).
SEED_CFLAGS = -std=c11 -O2 -w
SEED_LIBS = -lm -lpthread

# Where `make install` puts the `seed` link.
PREFIX ?= /usr/local

# The compiler, in Seed. The order is fixed (sorted) so that every stage
# sees the declarations in the same order.
SEED_SRC = $(sort $(wildcard compiler/*.sd))
# The runtime that the compiler pastes into every generated program.
RUNTIME_SRC = $(sort $(wildcard runtime/*.h runtime/seed_*.c))
# The standard library packages the compiler imports are compiled into it
# (their natives are pasted into the generated C), so they are inputs too.
LIB_SRC = $(sort $(wildcard lib/*/*.sd lib/*/*.c))
# Everything the compiler's generated C depends on.
BOOT_INPUTS = $(SEED_SRC) $(RUNTIME_SRC) $(LIB_SRC)
SHA256 = $(shell command -v sha256sum >/dev/null 2>&1 && echo sha256sum || echo shasum -a 256)

.PHONY: all test test-full test-leaks test-fmt test-lib test-examples verify release install uninstall clean

all: build/seed2

# Bootstrap. bootstrap/seed.c is a snapshot of the compiler's own generated
# C (self-contained: runtime and natives included). It is built with the C
# compiler (seed-boot) and compiles the current compiler source (stage1),
# which compiles itself (stage2), which compiles itself again (stage3).
# stage2.c and stage3.c must be identical. `make release` refreshes the
# snapshot (compiler/README.md, "Adding a language feature").
build/seed-boot: bootstrap/seed.c
	@mkdir -p build
	$(CC) $(SEED_CFLAGS) -o $@ bootstrap/seed.c $(SEED_LIBS)

build/stage1.c: build/seed-boot $(BOOT_INPUTS)
	build/seed-boot c $(SEED_SRC) > $@

build/seed1: build/stage1.c
	$(CC) $(SEED_CFLAGS) -o $@ build/stage1.c $(SEED_LIBS)

build/stage2.c: build/seed1 $(BOOT_INPUTS)
	build/seed1 c $(SEED_SRC) > $@

build/seed2: build/stage2.c
	$(CC) $(SEED_CFLAGS) -o $@ build/stage2.c $(SEED_LIBS)

build/stage3.c: build/seed2 $(BOOT_INPUTS)
	build/seed2 c $(SEED_SRC) > $@

# The fixed point, and the snapshot: bootstrap/inputs.sha256 lists the
# inputs the snapshot was made from. While they are unchanged, stage3.c must
# be the snapshot byte for byte; once they change, the snapshot is only
# reported as out of date (`make release` refreshes it).
verify: build/stage2.c build/stage3.c
	cmp build/stage2.c build/stage3.c
	@echo "fixed point: stage2.c and stage3.c are identical"
	@if $(SHA256) $(BOOT_INPUTS) | cmp -s - bootstrap/inputs.sha256; then \
	  cmp build/stage3.c bootstrap/seed.c && echo "snapshot: bootstrap/seed.c is stage3.c (the compiler has not changed)"; \
	else \
	  echo "snapshot: the compiler, runtime or lib changed since bootstrap/seed.c was made; run make release to refresh it"; \
	fi

# Writes the verified stage3.c to bootstrap/seed.c, with the list of its
# inputs.
release: verify
	cmp -s build/stage3.c bootstrap/seed.c || cp build/stage3.c bootstrap/seed.c
	$(SHA256) $(BOOT_INPUTS) > bootstrap/inputs.sha256
	@echo "bootstrap/seed.c updated"

test: build/seed2
	tests/run.sh "build/seed2 c" tests/lang tests/core

# The rest of the language, packages (tests/full/NAME/ directories) and the
# seed command (tests/test_cmd.sh).
test-full: build/seed2
	tests/run.sh "build/seed2 c" tests/full
	tests/test_cmd.sh build/seed2

# Every test program under macOS `leaks --atExit` (slow: about 3 minutes).
test-leaks: build/seed2
	tests/leaks.sh "build/seed2 c" tests/lang tests/core tests/full

# The tests of the standard library packages (lib/<pkg>/tests).
test-lib: build/seed2
	build/seed2 test lib

# The example programs: tests/examples.sh builds every examples/*.sd and
# checks wordcount on stdin, todo, and server with fetch (and curl) as
# clients; examples/run.sh builds every example (projects too), compares
# the output of the others with examples/expected/, runs their tests and
# checks that they are formatted.
test-examples: build/seed2
	tests/examples.sh build/seed2
	examples/run.sh build/seed2

# seed fmt. --verify runs the formatter's own checks on every .sd file under
# compiler/, tests/ and lib/ (files with syntax errors are skipped): formatting the
# formatted text changes nothing, and the tokens (ignoring newlines and
# trailing commas), comments and syntax tree are unchanged. Then the golden
# tests in tests/fmt, and a check that the compiler and the test programs
# are formatted.
FMT_CHECKED = $(SEED_SRC) $(wildcard tests/lang/*.sd tests/core/*.sd tests/full/*.sd)

# Package directories (tests/full/NAME/, tests/testcmd/) and the standard
# library are verified too.
FMT_VERIFIED = $(SEED_SRC) $(wildcard tests/*/*.sd tests/*/errors/*.sd) $(shell find tests/full tests/testcmd lib -mindepth 2 -name '*.sd')

test-fmt: build/seed2
	@build/seed2 fmt --verify $(FMT_VERIFIED)
	tests/fmt/run.sh build/seed2
	@build/seed2 fmt --check $(FMT_CHECKED) && echo "seed fmt --check: the compiler and the test programs are formatted"

# A link $(PREFIX)/bin/seed to build/seed2. The compiler finds runtime/ and
# lib/ through the link, so it works from anywhere while this tree stays.
install: build/seed2
	mkdir -p "$(PREFIX)/bin"
	ln -sf "$(abspath build/seed2)" "$(PREFIX)/bin/seed"
	@echo "installed $(PREFIX)/bin/seed -> $(abspath build/seed2)"

# Removes the link, but only if it is the one `make install` made.
uninstall:
	@if [ "$$(readlink "$(PREFIX)/bin/seed")" = "$(abspath build/seed2)" ]; then \
	  rm -f "$(PREFIX)/bin/seed" && echo "removed $(PREFIX)/bin/seed"; \
	elif [ -e "$(PREFIX)/bin/seed" ] || [ -L "$(PREFIX)/bin/seed" ]; then \
	  echo "$(PREFIX)/bin/seed is not a link to $(abspath build/seed2); left alone" >&2; exit 1; \
	else \
	  echo "nothing to remove: no $(PREFIX)/bin/seed"; \
	fi

clean:
	rm -rf build
