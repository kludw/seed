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
| `full/` | Programs for the rest of the language: interfaces, function values and `sortBy`, `defer`, channels, `select`, `group`/`spawn` with Erlang-style failure, integers wider than 64 bits, `s.chars()`, `os.crash`, the core packages' error messages, enum patterns, crash traces (`trace_*`: where every kind of crash happened, and the calls that led there). `full/NAME/` directories are programs made of packages (`pkg_basic`, `trace_pkg`). | `make test-full` |
| `full/errors/` | The pedantic checks: unused names and imports, shadowing, switch coverage, missing returns, unreachable code, naming rules, `implements`, unhandled errors, formatting, enum patterns and so on. `full/errors/NAME/` directories are package errors: cycles, visibility (`pub` names, fields, methods, struct literals), unknown and wrongly named packages, natives outside the standard library, tests outside `tests/`. | `make test-full` |
| `testcmd/` | Projects for `seed test` (`test_cmd.sh`): `NAME.out` is the exact stdout of `seed test testcmd/NAME` (run in `tests/`), `NAME.exit` its exit code. | `make test-full` |
| `core/` | More programs in the same subset as `lang/` (written for the retired bootstrap compiler seed0): crashes, exit codes, equality, narrowing, value copies, an interpreter; `core/errors/` has more programs that must be rejected. | `make test` |
| `lsp/` | The language server (`seed lsp`): session scripts `NAME.lsp` with their transcripts `NAME.out`, the documents they work on (`proj/`, `programs/`, `edits/`) and `driver/`, which frames the messages and checks the answers (`test_lsp.sh` has the details). | `make test-lsp` |
| `gdb/` | The program that `test_gdb.sh` debugs: `seed build -g`, then a scripted gdb session (a breakpoint at a Seed line, the locals through `runtime/seed-gdb.py`, `step`, `bt`, `next`, `finish`); skipped without gdb. | `make test-full` |
| `fmt/` | Golden tests for `seed fmt`: `NAME.sd` must format to `NAME.expected` (`tests/fmt/run.sh <seed>`, or `make test-fmt`). | `make test-fmt` |
| `spec/` | Spec conformance tests, one or a few rules each, named after the section of `SPEC.md`, `docs/grammar.md`, `docs/builtins.md` or `docs/formatting.md` they pin (`null_narrowing_not`, `select_evaluation_order`, ...); `spec/errors/` has the programs that must be rejected (`fmt_*` are unformatted programs that must fail at the first line that differs), and `spec/NAME/` directories are programs made of packages. `spec/AUDIT.md` maps every spec section to the tests that cover it. | `make test-full` |
| `spec/testcmd/` | `seed test` projects for the rules about tests (run by `test_cmd.sh` in `spec/`, like `testcmd/`); `NAME.err`, if present, is text that must appear in stderr. | `make test-full` |
| `spec/pending/` | Spec tests that fail because of an implementation bug (listed in `spec/AUDIT.md`). They are not run by any target; move a test to `spec/` (or `spec/errors/`) once its bug is fixed: `tests/run.sh "build/seed2 c" tests/spec/pending`. | none |

The `lang/` and `core/` programs are written to pass the compiler's pedantic checks: they
are formatted (4-space indentation, gofmt-style layout), have no unused names
and no shadowing, and handle every error.

## File format

The format is defined in the header of `run.sh`. For every `NAME.sd`:

- `NAME.out` is the exact expected stdout, including the final newline.
- `NAME.exit` is the expected exit code (default 0). Errors returned from
  `main` exit with 1 and crashes exit with 2.
- `NAME.err` holds lines that must appear in stderr, in that order, each
  within a line of it: `error: disk full`, or a crash's message and the
  lines of its trace (`    at get (tests/full/trace_index.sd:14:14)`). The
  `trace_*` tests list the lines that both gcc and clang builds print: a
  function that the C compiler inlined has no line of its own.
- `NAME.leaks` (only for `tests/leaks.sh`) says why the program is expected
  to leak. There are none: programs that crash, and those whose goroutines
  crash or are stopped, must not leak either (the cleanup frames release
  what the unwound frames held).

For every `errors/NAME.sd`, compiling must fail, and stderr must contain the
text after `// expect-error: ` on the file's first line. When the second
line is `// expect-errors: N`, stderr must also report exactly `N` errors
(lines with `: error: `): those tests check that one mistake does not
produce a cascade of follow-up errors.

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
tests/run.sh "build/seed2 c" tests/full tests/spec
```

The first argument is a command that takes a `.sd` path and writes C to
stdout. `run.sh` compiles that C with `$CC` (default `cc`), runs the program
with stdin from `/dev/null`, compares the results and prints a pass/fail
summary. It exits non-zero if anything failed.

`tests/leaks.sh` takes the same arguments and runs every program under
macOS `leaks --atExit`, or elsewhere built with LeakSanitizer
(`-fsanitize=leak -DSD_LEAK_CHECK`) (`make test-leaks`); every program must be free of
leaks, also the ones that crash (only one with a `NAME.leaks` file would be
reported as expected to leak). `full/crash_frames.sd` crashes and stops
goroutines at many kinds of points for this check.

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
  in `testcmd/`, crash traces through `seed run` and `seed run -g` (every
  frame) and `seed build -g`, `seed run` with arguments, `seed build` without `-o`, the
  formatting error, `seed` found through `$PATH` as a symbolic link
  without `$SEED_ROOT`, concurrent runs of programs with the same name,
  that no temporary directory is left behind, the runtime cache (and its
  `$TMPDIR` fallback when the root is read-only), stdout on a terminal
  (`testcmd/tty_flush.sd` under `script`: line-buffered, and flushed before
  reading stdin), and `make install` / `make uninstall` into a
  temporary `PREFIX`. For `spec/` it also runs the `spec/testcmd/`
  projects, `seed test` without a directory, `seed run` with `--`, `seed fmt
  --check` (its output and exit codes) and `seed fmt` on a file with syntax
  errors, `seed build` of a single file (with and without `-o`), `seed ast`,
  and `spec/io_stdin_stop_point.sd` with a stdin that stays open (reading
  stdin is a stop point).
- The standard library's own tests are run by `make test-lib`
  (`seed test lib`).
