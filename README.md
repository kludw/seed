# Seed — a minimal self-hosting language

Seed compiles to C. The first compiler is written in C. Every compiler after that is written in Seed.

```
seed0.c      stage-0 compiler, written in C (temporary scaffolding)
seed.sd      the real compiler, written in Seed
examples/    Seed programs
bootstrap/   generated C snapshot of the last released compiler (committed)
```

## The language

```
var counter;                       // global (zero-initialised)

fn add(a, b) { return a + b; }     // every value is a 64-bit int

fn main() {
  var s = "hi\n";                  // strings are pointers to bytes
  while (load8(s) != 0) { putchar(load8(s)); s = s + 1; }
  if (add(1, 2) == 3) { counter = counter + 1; } else { exit(1); }
  return 0;
}
```

Builtins: `getchar putchar eputchar alloc load8 store8 exit`.

## Bootstrapping (done once)

```
make verify
```

```
seed0.c ──cc──▶ seed0
seed.sd ──seed0──▶ stage1.c ──cc──▶ seed1   (Seed compiler, built by C)
seed.sd ──seed1──▶ stage2.c ──cc──▶ seed2   (Seed compiler, built by itself)
seed.sd ──seed2──▶ stage3.c
                   stage2.c == stage3.c  ✓   fixed point
```

When stage 2 and stage 3 are byte-identical, the compiler reproduces itself exactly, so the language is self-hosting.

## After the first release: dogfooding

```
make release     # writes bootstrap/seed.c (commit it), then delete seed0.c
make selfhost    # builds the compiler from bootstrap/seed.c + seed.sd only
```

From here on, a C compiler is only used as a backend. The compiler's source is Seed.

### Adding a feature to the language

The rule: **the compiler can only use a feature after a compiler that supports it exists.** So each feature takes three steps:

1. **Teach the compiler the feature without using it.** For example, add `break` to `stmt()` in `seed.sd`. Run `make selfhost`. The old compiler (from `bootstrap/seed.c`) builds a new one that understands `break`.
2. **Snapshot.** Use the new compiler to rebuild itself, check the fixed point, and update `bootstrap/seed.c`.
3. **Use the feature.** Now `seed.sd` may use `break` (for example, to drop the `going` flags in `skipspace`).

Real compilers do the same: Go keeps an older Go toolchain for bootstrapping, Rust uses the previous beta, and Zig keeps a WASM snapshot of itself.
