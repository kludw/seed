# Seed

Seed is a small, strongly typed programming language inspired by Go. There is one way to do each thing, nothing happens implicitly, there is no `nil` and there are no pointers, and the compiler is a pedantic friend that rejects anything doubtful and explains how to fix it. Goroutines and channels come built in, along with a standard library that covers files, JSON, HTTP and more, so a program needs no dependencies.

Seed is a proof of concept. Its compiler is written in Seed, compiles to C, and builds itself.

```
package main

import "fmt"

type Shape enum {
    Circle(float radius)
    Square(float side)
}

fn area(Shape s) float {
    switch (s) {
    case Shape.Circle(float r):
        return 3.14159 * r * r
    case Shape.Square(float side):
        return side * side
    }
}

// "c2" is a circle of radius 2, "s3" a square of side 3.
fn measure(string text, chan float areas) error {
    float size = try text.slice(1, text.len()).toFloat()
    Shape s = Shape.Square(size)
    if (text.startsWith("c")) {
        s = Shape.Circle(size)
    }
    areas.send(area(s))
    return null
}

fn main([]string args) error {
    chan float areas = chan float(args.len())
    try group {
        for (int i = 1; i < args.len(); i++) {
            spawn measure(args[i], areas)
        }
    }
    areas.close()
    float total = 0.0
    for (float? a = areas.receive(); a != null; a = areas.receive()) {
        total = total + a
    }
    fmt.println("total area: " + total.string())
    return null
}
```

```
$ seed run shapes.sd c1 s2.5 s3
total area: 18.39159
$ seed run shapes.sd c1 sx
error: string.toFloat: "x": invalid float
```

The `switch` must handle every case of the enum. `try` passes the error from `toFloat` up. Each `spawn` starts a goroutine, and the `group` waits for all of them; if one of them fails, the group stops the others and returns the error. `receive()` returns a `float?`, which is `null` once the channel is closed and empty, and the compiler only lets you add it to `total` after the null check.

## Design principles

- **One way to do things.** One loop (C-style `for`), one way to declare a variable (`int x = 5`, type first), no syntax sugar except `try` for passing errors up.
- **Nothing implicit.** No implicit conversions, no zero values, no `init()`, no hidden control flow. Values become text with `.string()`.
- **Values only.** There are no pointers: every value behaves like a copy (copy-on-write makes that cheap), and `mut` methods change the variable they are called on.
- **No nil.** A `T` is never null; a `T?` can be, and must be checked before it is used as a `T`.
- **Errors are values and must be handled.** Ignoring an error, or any result, does not compile.
- **Real enums** with data, and a `switch` over one must list every case.
- **Goroutines and channels, safely.** Goroutines share nothing (sending copies the value), only start inside a `group` that waits for them, and fail like Erlang processes: one failure stops its group.
- **No generics** beyond the built-in lists, maps, channels and nullable types.
- **`pub` instead of capital letters** for exports.
- **One style.** The formatter (`seed fmt`) is part of the language: unformatted code does not compile.
- **Everything built in.** One binary, a standard library, a test runner. No package manager, because there is nothing to download.

The full rules are in [SPEC.md](SPEC.md).

## Quick start

You need a C compiler (`cc`) and `make`. Seed is tested on macOS (arm64, Apple clang) and Linux (x86_64, glibc, GCC).

```
make                                        # builds build/seed2, the seed toolchain
build/seed2 run examples/todo.sd add buy milk
build/seed2 run examples/todo.sd list
echo "the cat and the dog and the bird" | build/seed2 run examples/wordcount.sd 3
build/seed2 build -o wordcount examples/wordcount.sd
```

`make install` puts `seed` on your `PATH`: it links `$PREFIX/bin/seed` (`PREFIX` is `/usr/local` unless you give another, as in `make install PREFIX=$HOME/.local`) to `build/seed2`, and `make uninstall` removes the link. `seed` finds its standard library and runtime from its own path, through the link, so it works from any directory as long as the repository stays where it is. Without installing, use `build/seed2` wherever this document says `seed`.

To learn the language, start with [the guide](docs/guide/README.md).

## The toolchain

There is one binary, `seed`:

| Command | What it does |
|---|---|
| `seed run [-g] <dir \| file.sd> [args...]` | Compiles a program and runs it with the arguments. |
| `seed build [-g] [-o out] <dir \| file.sd>` | Compiles a program into one executable; with `-g`, for gdb or lldb, which then show the Seed source ([the guide, chapter 14](docs/guide/14-crashes-and-debugging.md)). |
| `seed test [-v] [--run text] [dir...]` | Runs the `test` blocks of every package under the directories (`--run`: only the tests whose description contains the text; `-v`: list each test). |
| `seed fmt [--check] <file.sd \| dir>...` | Formats files (or every `.sd` file under a directory) in place, or reports the ones that aren't formatted. |
| `seed c [-g] <dir \| file.sd...>` | Prints the generated C (for testing the compiler). |
| `seed ast <file.sd>...` | Prints the syntax tree (for debugging the parser). |
| `seed doc [dir \| file.sd \| pkg[.Name[.member]]]` | Prints the public API of a package with its doc comments, or one name of it (`seed doc json.Value.get`); `seed doc --html -o out dir...` writes static HTML pages with an index and cross-links (`make docs` does it for the standard library, into `build/docs`); `seed doc --check dir...` lists the `pub` names without a doc comment ([the guide, chapter 11](docs/guide/11-packages.md#documentation)). |
| `seed lsp` | A language server on stdin and stdout, for editors: the compiler's errors as you type, formatting, go to definition, hover, completion, signature help, references, rename and the outline ([the guide, chapter 1](docs/guide/01-getting-started.md#editors)). |

A program is a single `.sd` file or the `package main` of a directory, together with the packages it imports. The compiler checks it, generates one self-contained C file (the runtime included), and compiles that with `cc`.

The Makefile has the development targets:

| Target | What it does |
|---|---|
| `make` | Bootstraps the compiler from the snapshot `bootstrap/seed.c`: `build/seed2`. |
| `make verify` | Checks the fixed point (the compiler compiled by itself generates exactly the same C again) and, while the compiler is unchanged, that the snapshot is that C. |
| `make release` | Verifies, then refreshes the snapshot `bootstrap/seed.c` from the current compiler. |
| `make install` | Links `$PREFIX/bin/seed` to `build/seed2` (`make uninstall` removes it). |
| `make test` | The language tests in `tests/lang` and `tests/core`. |
| `make test-full` | The features outside the bootstrap subset, packages, `seed test`, crash traces, and a gdb session (when gdb is installed). |
| `make test-lib` | The standard library's tests, and a check that every `pub` name in it has a doc comment. |
| `make test-fmt` | The formatter's self-checks and golden tests. |
| `make test-examples` | Builds every example and checks its output, its tests and its formatting. |
| `make test-lsp` | The language server: scripted sessions over stdin and stdout, and documents typed in a few bytes at a time. |
| `make test-doc` | `seed doc`: text output against goldens, the HTML of the standard library (every link resolves), `--check`. |
| `make docs` | The HTML documentation of the standard library, in `build/docs` (`seed doc --html lib -o build/docs`). |
| `make test-leaks` | Every test program under a leak checker: macOS `leaks`, LeakSanitizer on Linux (slow). |
| `docs/guide/check.sh` | Every example of the guide, against its expected output. |
| `runtime/tests/run.sh` | The runtime's own C tests, under the address, undefined-behaviour and thread sanitizers. |

## The repository

| Path | Contents |
|---|---|
| [SPEC.md](SPEC.md) | The language specification. |
| [DECISIONS.md](DECISIONS.md) | Decisions made during implementation without the author, for review. |
| [docs/](docs/README.md) | The guide and the reference documents (index in [docs/README.md](docs/README.md)). |
| [compiler/](compiler/README.md) | The compiler, written in Seed: lexer, parser, checker, C generator, formatter, `seed` command line. |
| [runtime/](runtime/README.md) | The runtime in C (integers, strings, lists, maps, goroutines, channels, crashes), pasted into every program. |
| [lib/](lib/README.md) | The standard library: one directory per package, in Seed with native functions in C. |
| `bootstrap/` | `seed.c`, the C snapshot of the compiler that `make` starts from, and `inputs.sha256`, the files it was made from. |
| [tests/](tests/README.md) | Conformance tests: programs with their expected output, and programs the compiler must reject. |
| [examples/](examples/README.md) | Twenty-two example programs, from `wordcount` to an expression language, a concurrent prime sieve, a key-value server with an append-only log, a JSON HTTP API, a CSV filter, a Markdown converter, a web crawler and a Lisp interpreter, with a checked runner. |
| [editors/vscode/](editors/vscode/README.md) | Syntax highlighting for VS Code, and a client for the language server. |

The documentation:

- [The guide](docs/guide/README.md) teaches the language by example, in 14 chapters, for programmers who know Go or another C-like language.
- [SPEC.md](SPEC.md), [docs/grammar.md](docs/grammar.md), [docs/builtins.md](docs/builtins.md), [docs/stdlib.md](docs/stdlib.md) and [docs/formatting.md](docs/formatting.md) are the reference.
- [compiler/README.md](compiler/README.md), [runtime/README.md](runtime/README.md) and [lib/README.md](lib/README.md) describe the implementation.
- [docs/first-impressions.md](docs/first-impressions.md) is the friction log of a Go programmer learning Seed from these documents, with what was fixed and what remains.
- [docs/dogfood.md](docs/dogfood.md) is the friction log of four realistic programs written the same way, with build times, binary sizes and a throughput comparison with Go.

## Status

**What works:** the whole language as specified: enums with data and exhaustive `switch`, nullable types with flow-sensitive narrowing, errors with `try` and error guards, structs (with default values of fields), `mut` methods, interfaces with `implements`, function values, integers that grow without overflowing, goroutines with `group`, `spawn`, channels and `select`, `defer`, packages with `pub`, `test` blocks with `assert`, and the formatter. The standard library has all 15 planned packages (`errors fmt io os fs path strings math rand time encoding json crypto net http`). The compiler compiles itself to a fixed point and every test suite passes.

**Known limitations:**

- Goroutines are scheduled M:N (one worker thread per CPU) and preempted cooperatively: one that computes for 10 ms while others wait yields at its next loop iteration or recursive call, but not inside a long runtime operation (sorting a huge list, multiplying huge integers) or a native call. Past about 24,000 simultaneous goroutines on Linux (the default `vm.max_map_count`), new goroutine stacks have no guard page, so their stack overflow is not detected (runtime/README.md, section 10).
- Deadlock detection is global: it fires only when every goroutine is blocked. A goroutine stuck while others keep running waits forever, and defers do not run on a deadlock.
- When a group fails, the code of the group block itself is not stopped, only the goroutines it started.
- Writing to stdout or stderr (`fmt`) is not a stop point, so a goroutine blocked writing to a full pipe there is stopped only when the write returns. File I/O that waits (opening or writing a FIFO, for example) is stopped.
- Tested on macOS (arm64) and Linux (x86_64, glibc). Other POSIX systems are untested.
- The parser reports one syntax error per statement (or member of a type); an error in an expression that spans lines may hide a second error on the next line.
- A crash shows where it happened and the calls that led there, but not the calls of functions that the C compiler inlined (`seed run -g` shows them). There is no debugger of Seed's own: `seed build -g` makes gdb and lldb follow the Seed source, with Seed's variables under their C names (`l_x`).

Bugs found in passing are recorded in [compiler/BUGS_FIXED.md](compiler/BUGS_FIXED.md) and [lib/COMPILER_BUGS.md](lib/COMPILER_BUGS.md).

## How the bootstrap works

The compiler is written in Seed, so something has to compile it the first time. That was **seed0**, a small compiler written in C for a subset of Seed; it is retired now ([docs/seed0.md](docs/seed0.md) tells its story). Today the compiler is built from a snapshot of its own generated C:

1. `bootstrap/seed.c` is the compiler's generated C, committed: self-contained (runtime and standard-library natives pasted in), so `cc` alone builds it into `build/seed-boot`.
2. `seed-boot` compiles the compiler's current source (`compiler/*.sd`) into `build/stage1.c`, which becomes `build/seed1`.
3. `seed1` compiles the compiler again (`build/stage2.c`, `build/seed2`). `seed2` compiling itself gives `build/stage3.c`, which must be byte-for-byte identical to `stage2.c`: the **fixed point** (`make verify`). While the compiler, runtime and library are unchanged since the snapshot, `stage3.c` must also be `bootstrap/seed.c` itself.
4. `make release` verifies and writes `stage3.c` to `bootstrap/seed.c`, so the next build starts from the newest compiler.

A new language feature follows "teach, snapshot, use": teach the compiler the feature without using it, take a new snapshot with `make release`, then use the feature in the compiler's own source ([compiler/README.md](compiler/README.md), "Adding a language feature").
