CC     ?= cc
CFLAGS ?= -O2 -w
B      := build

.PHONY: all bootstrap verify release selfhost hello clean

all: verify hello

# ---- Phase 1: bootstrap from C (done once, before the first release) ----

$(B)/seed0: seed0.c | $(B)
	$(CC) $(CFLAGS) -o $@ $<

# stage 1: the C compiler compiles the Seed compiler
$(B)/stage1.c: $(B)/seed0 seed.sd
	$(B)/seed0 < seed.sd > $@
$(B)/seed1: $(B)/stage1.c
	$(CC) $(CFLAGS) -o $@ $<

# stage 2: the Seed compiler compiles itself
$(B)/stage2.c: $(B)/seed1 seed.sd
	$(B)/seed1 < seed.sd > $@
$(B)/seed2: $(B)/stage2.c
	$(CC) $(CFLAGS) -o $@ $<

# stage 3: ...and again, using the self-built compiler
$(B)/stage3.c: $(B)/seed2 seed.sd
	$(B)/seed2 < seed.sd > $@

bootstrap: $(B)/seed2

# Fixed point: a compiler built by itself produces identical output.
verify: $(B)/stage2.c $(B)/stage3.c
	cmp $(B)/stage2.c $(B)/stage3.c
	@echo "✓ fixed point: seed compiles itself"

# ---- Phase 2: after release, the C bootstrap is retired ----

# Snapshot the generated C of the current compiler. Commit bootstrap/seed.c.
release: verify
	mkdir -p bootstrap
	cp $(B)/stage3.c bootstrap/seed.c
	@echo "✓ bootstrap/seed.c updated — seed0.c is no longer needed"

# Build the compiler with no hand-written C at all: only the snapshot + seed.sd.
selfhost: | $(B)
	$(CC) $(CFLAGS) -o $(B)/seed-old bootstrap/seed.c
	$(B)/seed-old < seed.sd > $(B)/seed-new.c
	$(CC) $(CFLAGS) -o $(B)/seed $(B)/seed-new.c
	@echo "✓ built build/seed from bootstrap/seed.c + seed.sd"

# ---- Using the language ----

hello: $(B)/seed2 examples/hello.sd
	$(B)/seed2 < examples/hello.sd > $(B)/hello.c
	$(CC) $(CFLAGS) -o $(B)/hello $(B)/hello.c
	$(B)/hello

$(B):
	mkdir -p $(B)

clean:
	rm -rf $(B)
