# Seed tests

Conformance tests for Seed, written from `SPEC.md`, `docs/grammar.md`,
`docs/builtins.md` and `docs/seed0.md`. Each test is a complete program whose
exact stdout (and exit code, where it isn't 0) is checked.

## Layout

| Directory | Contents | Compiler |
|---|---|---|
| `lang/` | Programs that stay inside the seed0 subset (no interfaces, function values, channels, `select`, `group`/`spawn`, `defer`, `test`/`assert`, user packages or `s.chars()`). | `seed0` and the full compiler |
| `lang/errors/` | Programs that seed0 must reject: an ignored error, `_` with a plain `T`, a `mut` method on a parameter, a `T?` used as `T`, syntax errors, unknown names and type mismatches. | `seed0` and the full compiler |
| `full/` | Programs for the features outside the subset: interfaces, function values and `sortBy`, `defer`, channels, `select`, `group`/`spawn` with Erlang-style failure, integers wider than 64 bits and `s.chars()`. | full compiler only |
| `full/errors/` | The full compiler's pedantic checks: unused names and imports, shadowing, switch coverage, missing returns, unreachable code, naming rules, `implements`, unhandled errors, formatting and so on. | full compiler only |
| `fmt/` | Golden tests for `seed fmt`: `NAME.sd` must format to `NAME.expected` (`tests/fmt/run.sh <seed>`, or `make test-fmt`). | full compiler |

The `lang/` programs are written to also pass the full compiler's checks: they
are formatted (4-space indentation, gofmt-style layout), have no unused names
and no shadowing, and handle every error.

## File format

The format is defined in the header of `run.sh`. For every `NAME.sd`:

- `NAME.out` is the exact expected stdout, including the final newline.
- `NAME.exit` is the expected exit code (default 0). Errors returned from
  `main` exit with 1 and crashes exit with 2.
- `NAME.err` is text that must appear somewhere in stderr, such as
  `error: disk full` or `crash:`.

For every `errors/NAME.sd`, compiling must fail, and stderr must contain the
text after `// expect-error: ` on the file's first line. Most error tests
expect `NAME.sd:LINE:`, the location of the offending line, because seed0
reports errors as `file:line:col: error: message`, while the wording of the
message is up to the compiler. Where a name is certain to appear in any
reasonable message (an unknown identifier, a missing enum case, a badly named
function), the test expects that name instead.

## Running

```
tests/run.sh build/seed0 tests/lang
tests/run.sh "<full compiler command>" tests/lang tests/full
```

The first argument is a command that takes a `.sd` path and writes C to
stdout. `run.sh` compiles that C with `$CC` (default `cc`), runs the program
with stdin from `/dev/null`, compares the results and prints a pass/fail
summary. It exits non-zero if anything failed.

## Notes

- The `fs_*` tests work in `$TMPDIR/seed_lang_fs_basic` and
  `$TMPDIR/seed_lang_fs_errors` (or under `/tmp` when `TMPDIR` is unset), and
  remove those directories again.
- `os_run*` tests run `true`, `false` and `sh` from `PATH`. `os_env` expects
  `PATH` to be set.
- Concurrency tests in `full/` have deterministic output. Only `main` prints,
  apart from deferred calls whose order the test forces.
- Float output avoids values whose text depends on where a formatter switches
  to exponent notation. Only values between 0.001 and 100000 and values with
  three-digit exponents (`1e+300`) are printed.
- seed0's 64-bit overflow crash is not tested, because the full compiler's
  integers grow instead. `full/bigint_*` tests the growing integers.
