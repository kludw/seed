# seed0 (retired)

`seed0` was the bootstrap compiler: about 3,900 lines of C (`seed0/seed0.c`,
with its own runtime `seed0/runtime.c`) that compiled a subset of Seed to C.
Its only job was to compile the real compiler (`compiler/*.sd`) the first
time, following SPEC.md's implementation plan:

1. Write seed0, a small C compiler for a subset of Seed.
2. Write the real compiler in that subset and compile it with seed0.
3. Let the compiler compile itself until its output reproduces itself.
4. Commit a snapshot of the generated C to `bootstrap/` and delete seed0.

All four steps are done. The compiler is now built from
`bootstrap/seed.c`, its own generated C (compiler/README.md, "Building"),
and seed0 is gone; it is in the git history (`seed0/` before the commit
that added `bootstrap/`).

What remains of it:

- The subset it compiled: no interfaces, function values, channels,
  `select`, `group`/`spawn`, `defer`, `test`/`assert`, user packages or
  `s.chars()`, and 64-bit ints that crash on overflow. The test programs it
  had to pass are still run with the real compiler (`tests/lang` and
  `tests/core`, which was `tests/seed0`).
- Its float formatter is the reference for the runtime's
  (`runtime/tests/ref_float.c`).
- Some of its choices became the language's: error messages of the core
  packages, `file:line:col: error: message`, crash and exit codes.
