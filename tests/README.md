# Seed tests

Conformance tests for Seed, written from `SPEC.md`, `docs/grammar.md`,
`docs/builtins.md` and the subset of the retired bootstrap compiler seed0
(`docs/seed0.md`). Each test is a complete program whose
exact stdout (and exit code, where it isn't 0) is checked.

## Layout

| Directory | Contents | Run by |
|---|---|---|
| `lang/` | Programs that stay inside the subset of the retired bootstrap compiler seed0 (no interfaces, function values, channels, `select`, `group`/`spawn`, `defer`, `test`/`assert`, user packages or `s.chars()`). | `make test` |
| `lang/errors/` | Programs that must be rejected: an ignored error, `_` with a plain `T`, a `mut` method on a parameter, a `T?` used as `T`, syntax errors, unknown names and type mismatches. | `make test` |
| `full/` | Programs for the rest of the language: interfaces, function values and `sortBy`, `defer`, channels, `select`, `group`/`spawn` with Erlang-style failure, integers wider than 64 bits, `s.chars()`, `os.crash`, the core packages' error messages, enum patterns. `full/NAME/` directories are programs made of packages (`pkg_basic`). | `make test-full` |
| `full/errors/` | The pedantic checks: unused names and imports, shadowing, switch coverage, missing returns, unreachable code, naming rules, `implements`, unhandled errors, formatting, enum patterns and so on. `full/errors/NAME/` directories are package errors: cycles, visibility (`pub` names, fields, methods, struct literals), unknown and wrongly named packages, natives outside the standard library, tests outside `tests/`. | `make test-full` |
| `testcmd/` | Projects for `seed test` (`test_cmd.sh`): `NAME.out` is the exact stdout of `seed test testcmd/NAME` (run in `tests/`), `NAME.exit` its exit code. | `make test-full` |
| `core/` | More programs in the same subset as `lang/` (written for the bootstrap compiler seed0, as `tests/seed0/`): crashes, exit codes, equality, narrowing, value copies, an interpreter; `core/errors/` has more programs that must be rejected. | `make test` |
| `fmt/` | Golden tests for `seed fmt`: `NAME.sd` must format to `NAME.expected` (`tests/fmt/run.sh <seed>`, or `make test-fmt`). | `make test-fmt` |

The `lang/` and `core/` programs are written to pass the compiler's pedantic checks: they
are formatted (4-space indentation, gofmt-style layout), have no unused names
and no shadowing, and handle every error.

## File format

The format is defined in the header of `run.sh`. For every `NAME.sd`:

- `NAME.out` is the exact expected stdout, including the final newline.
- `NAME.exit` is the expected exit code (default 0). Errors returned from
  `main` exit with 1 and crashes exit with 2.
- `NAME.err` is text that must appear somewhere in stderr, such as
  `error: disk full` or `crash:`.
- `NAME.leaks` (only for `tests/leaks.sh`) says why the program is expected
  to leak although it exits normally: a goroutine crashes inside a group,
  or is stopped while it waits, which leaks the locals of its unwound
  frames.

For every `errors/NAME.sd`, compiling must fail, and stderr must contain the
text after `// expect-error: ` on the file's first line.

A directory `NAME/` with a `NAME.out` next to it is a program made of
packages: the compile command gets the directory (`seed c full/pkg_basic`),
whose `.sd` files are package main, and whose subdirectories are the
packages it imports. A directory `errors/NAME/` must fail to compile, with
the expected text on the first line of its `main.sd`. Most error tests
expect `NAME.sd:LINE:`, the location of the offending line, because seed0
reported errors as `file:line:col: error: message`, while the wording of the
message is up to the compiler. Where a name is certain to appear in any
reasonable message (an unknown identifier, a missing enum case, a badly named
function), the test expects that name instead.

## Running

```
tests/run.sh "build/seed2 c" tests/lang tests/core
tests/run.sh "build/seed2 c" tests/full
```

The first argument is a command that takes a `.sd` path and writes C to
stdout. `run.sh` compiles that C with `$CC` (default `cc`), runs the program
with stdin from `/dev/null`, compares the results and prints a pass/fail
summary. It exits non-zero if anything failed.

`tests/leaks.sh` takes the same arguments and runs every program under
macOS `leaks --atExit` (`make test-leaks`); programs that crash (and those
with a `NAME.leaks` file) are expected to leak the locals of the frames the
crash unwinds.

## Notes

- The `fs_*` tests work in `$TMPDIR/seed_lang_fs_basic` and
  `$TMPDIR/seed_lang_fs_errors` (or under `/tmp` when `TMPDIR` is unset), and
  remove those directories again.
- `os_run*` tests run `true`, `false` and `sh` from `PATH`. `os_env` expects
  `PATH` to be set.
- Concurrency tests in `full/` have deterministic output. Only `main` prints,
  apart from deferred calls whose order the test forces (or that print the
  same line). Results that arrive in any order are sorted or summed first.
- `chan_deadlock` and `group_deadlock` check the runtime's deadlock crash
  (`crash: deadlock: all goroutines are blocked`, exit code 2).
  `select_timeout` and `group_cancel_receiver` use timers of a few
  milliseconds (and an hour-long sleep that must be stopped).
- Float output avoids values whose text depends on where a formatter switches
  to exponent notation. Only values between 0.001 and 100000 and values with
  three-digit exponents (`1e+300`) are printed.
- Integers grow instead of overflowing: `full/bigint_*` and `full/int_*`
  test them (`full/int_no_overflow` has the programs that crashed with
  seed0's 64-bit ints).
- Every program is compiled with the pedantic checks and the formatting
  check (always on), so every test program, apart from `errors/`, is
  formatted and passes them.
- `make test-full` also runs `tests/test_cmd.sh`: the `seed test` projects
  in `testcmd/`, `seed run` with arguments, `seed build` without `-o`, the
  formatting error, `seed` found through `$PATH` as a symbolic link
  without `$SEED_ROOT`, concurrent runs of programs with the same name,
  that no temporary directory is left behind, the runtime cache (and its
  `$TMPDIR` fallback when the root is read-only), and `make install` /
  `make uninstall` into a temporary `PREFIX`.
- The standard library's own tests are run by `make test-lib`
  (`seed test lib`).
