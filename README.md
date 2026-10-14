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

You need a C compiler (`cc`) and `make`. Seed has been tested on macOS (arm64).

```
make                                        # builds build/seed2, the seed toolchain
build/seed2 run examples/todo.sd add buy milk
build/seed2 run examples/todo.sd list
echo "the cat and the dog and the bird" | build/seed2 run examples/wordcount.sd 3
build/seed2 build -o wordcount examples/wordcount.sd
```

`make install PREFIX=$HOME/.local` (being added) installs the toolchain under `PREFIX` and puts `seed` on your `PATH` as `$PREFIX/bin/seed`. Until then, use `build/seed2` wherever this document says `seed`, or link to it: `seed` finds its standard library and runtime from its own path, through symbolic links.

To learn the language, start with [the guide](docs/guide/README.md).

## The toolchain

There is one binary, `seed`:

| Command | What it does |
|---|---|
| `seed run <dir \| file.sd> [args...]` | Compiles a program and runs it with the arguments. |
| `seed build [-o out] <dir \| file.sd>` | Compiles a program into one executable. |
| `seed test [dir...]` | Runs the `test` blocks of every package under the directories. |
| `seed fmt [--check] <file.sd>...` | Formats files in place, or reports the ones that aren't formatted. |
| `seed c <dir \| file.sd...>` | Prints the generated C (for testing the compiler). |
| `seed ast <file.sd>...` | Prints the syntax tree (for debugging the parser). |

A program is a single `.sd` file or the `package main` of a directory, together with the packages it imports. The compiler checks it, generates one self-contained C file (the runtime included), and compiles that with `cc`.

The Makefile has the development targets:

| Target | What it does |
|---|---|
| `make` | Bootstraps the compiler: `build/seed2`. |
| `make verify` | Checks the fixed point: the compiler compiled by itself generates exactly the same C again. |
| `make test` | The language tests in `tests/lang` and `tests/seed0`. |
| `make test-full` | The features outside the bootstrap subset, packages and `seed test`. |
| `make test-lib` | The standard library's tests. |
| `make test-fmt` | The formatter's self-checks and golden tests. |
| `make test-examples` | Builds `examples/` and runs the ones that need no person. |
| `make test-leaks` | Every test program under macOS `leaks` (slow). |
| `docs/guide/check.sh` | Every example of the guide, against its expected output. |

## The repository

| Path | Contents |
|---|---|
| [SPEC.md](SPEC.md) | The language specification. |
| [DECISIONS.md](DECISIONS.md) | Decisions made during implementation without the author, for review. |
| [docs/](docs/README.md) | The guide and the reference documents (index in [docs/README.md](docs/README.md)). |
| [compiler/](compiler/README.md) | The compiler, written in Seed: lexer, parser, checker, C generator, formatter, `seed` command line. |
| [runtime/](runtime/README.md) | The runtime in C (integers, strings, lists, maps, goroutines, channels, crashes), pasted into every program. |
| [lib/](lib/README.md) | The standard library: one directory per package, in Seed with native functions in C. |
| [seed0/](seed0/README.md) | The bootstrap compiler, written in C, for a subset of Seed (being retired). |
| `bootstrap/` | The C snapshot of the compiler that replaces `seed0` (being added). |
| [tests/](tests/README.md) | Conformance tests: programs with their expected output, and programs the compiler must reject. |
| `examples/` | Example programs: `wordcount`, `todo`, an HTTP `server` and a concurrent `fetch`. |
| [editors/vscode/](editors/vscode/README.md) | Syntax highlighting for VS Code. |

The documentation:

- [The guide](docs/guide/README.md) teaches the language by example, in 13 chapters, for programmers who know Go or another C-like language.
- [SPEC.md](SPEC.md), [docs/grammar.md](docs/grammar.md), [docs/builtins.md](docs/builtins.md), [docs/stdlib.md](docs/stdlib.md) and [docs/formatting.md](docs/formatting.md) are the reference.
- [compiler/README.md](compiler/README.md), [runtime/README.md](runtime/README.md) and [lib/README.md](lib/README.md) describe the implementation.

## Status

**What works:** the whole language as specified: enums with data and exhaustive `switch`, nullable types with flow-sensitive narrowing, errors with `try` and error guards, structs, `mut` methods, interfaces with `implements`, function values, integers that grow without overflowing, goroutines with `group`, `spawn`, channels and `select`, `defer`, packages with `pub`, `test` blocks with `assert`, and the formatter. The standard library has all 15 planned packages (`errors fmt io os fs path strings math rand time encoding json crypto net http`). The compiler compiles itself to a fixed point and every test suite passes.

**Known limitations:**

- Narrowing a nullable variable by assigning it a non-null value (`if (x == null) { x = "default" }`) is specified but still being implemented.
- A crash, or a goroutine stopped by its group, leaks the memory held by the locals of the unwound frames. Normal execution leaks nothing.
- Each goroutine is an OS thread, so tens of thousands of simultaneous goroutines hit OS limits.
- Deadlock detection is global: it fires only when every goroutine is blocked. A goroutine stuck while others keep running waits forever, and defers do not run on a deadlock.
- When a group fails, the code of the group block itself is not stopped, only the goroutines it started.
- File I/O and `os.output` are not stop points, so a goroutine blocked in them is stopped only when the call returns.
- A `select` case can only receive into a new variable.
- Big integers use quadratic algorithms, so converting a number with millions of digits to text is slow.
- Only tested on macOS (arm64, Apple clang). The runtime is C11 and POSIX, but Linux is untested.
- The parser reports at most one syntax error per statement.
- There is no language server and no debugger; generated programs are C, so `lldb` works on them.

Bugs found in passing are recorded in [compiler/BUGS_FIXED.md](compiler/BUGS_FIXED.md) and [lib/COMPILER_BUGS.md](lib/COMPILER_BUGS.md).

## How the bootstrap works

The compiler is written in Seed, so something has to compile it the first time:

1. **seed0** ([seed0/](seed0/README.md), [docs/seed0.md](docs/seed0.md)) is a small compiler written in C for a subset of Seed. It has fixed-size integers, no interfaces and no concurrency, and few checks.
2. The real compiler ([compiler/](compiler/README.md)) is written in that subset. `seed0` compiles it to `build/stage1.c`, which becomes `build/seed1`.
3. `seed1` compiles the compiler again (`build/stage2.c`, `build/seed2`), now with the full language and all checks. `seed2` compiling itself gives `build/stage3.c`, which must be byte-for-byte identical to `stage2.c`: the **fixed point** (`make verify`).
4. The generated C of the compiler is committed as a snapshot, `bootstrap/seed.c`. From then on, `make` builds the compiler from the snapshot with `cc`, and `seed0` is deleted.
5. A new language feature follows "teach, snapshot, use": teach the compiler the feature without using it, take a new snapshot, then use the feature in the compiler's own source.

Steps 1 to 3 are how `make` works today. Step 4, the snapshot that retires `seed0`, is being finished.
