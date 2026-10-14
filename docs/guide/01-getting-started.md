# 1. Getting started

## Building the toolchain

Seed has one toolchain binary, `seed`, which compiles, runs, builds and formats programs. It is written in Seed itself, and the repository carries a C snapshot of it, so all you need is a C compiler (`cc`, such as clang or gcc) and `make`:

```
git clone <the seed repository>
cd seed
make
```

`make` compiles the snapshot `bootstrap/seed.c` with `cc` into `build/seed-boot`, uses it to compile the compiler's current source (`build/seed1`), and then lets that compiler compile itself into `build/seed2`. `build/seed2` is the `seed` command.

`seed` turns your program into C and calls `cc` to compile it, so `cc` must stay installed. It also needs the runtime and the standard library in the repository's `runtime/` and `lib/` directories, which it finds from its own path. To run it as plain `seed` from anywhere, install a link to it on your `PATH`:

```
make install                        # links /usr/local/bin/seed to build/seed2
make install PREFIX=$HOME/.local    # or $HOME/.local/bin/seed
```

The link keeps working as long as the repository stays where it is; `make uninstall` removes it.

The rest of this guide writes `seed`.

## Hello, world

Save this as `hello.sd`:

```seed
package main

import "fmt"

fn main([]string args) error {
    fmt.println("Hello, world!")
    return null
}
```

```
$ seed run hello.sd
Hello, world!
```

A few things are already different from Go:

- `main` always has the same signature. It receives the command-line arguments and returns an `error`. Returning `null` means success; returning an error prints it and exits with code 1 (see [chapter 4](04-functions-and-errors.md)).
- Parameters, like every declaration in Seed, write the type first: `[]string args`.
- `null` is Seed's "nothing". Only a few types can hold it, and `error` is one of them (see [chapter 5](05-null-safety.md)).
- `fmt.println` takes exactly one string. There are no format strings.

## Arguments

`args[0]` is the program itself, and the real arguments follow it. With `seed run`, everything after the program's file (or directory) is passed to it; a `--` in front of the arguments is allowed and skipped:

```seed
// Greets the name given on the command line, or the world.
//
//     seed run 01_greet.sd -- Ada
package main

import "fmt"

fn main([]string args) error {
    // args[0] is the program itself.
    string name = "world"
    if (args.len() > 1) {
        name = args[1]
    }
    fmt.println("Hello, " + name + "!")
    return null
}
```

```
$ seed run 01_greet.sd -- Ada
Hello, Ada!
$ seed run 01_greet.sd
Hello, world!
```

## The commands

| Command | What it does |
|---|---|
| `seed run <file.sd or dir> [args...]` | Compiles the program into a temporary executable and runs it. The exit code is the program's. |
| `seed build [-o out] <file.sd or dir>` | Compiles the program into an executable (named after the file or directory unless `-o` names it): a single binary with no dependencies. |
| `seed test [dir...]` | Runs the tests of the packages under the directories (see [chapter 11](11-packages.md)). |
| `seed fmt file.sd...` | Rewrites the files in the one canonical layout. |
| `seed fmt --check file.sd...` | Changes nothing, and reports the files that are not formatted (exit code 1). |
| `seed c <file.sd or dir>` | Writes the generated C program to stdout. |

A program is a single `.sd` file, or a directory: the `package main` in it and the packages it imports ([chapter 11](11-packages.md)). Compile errors are printed to stderr, and the command exits with code 1.

```
$ seed build -o greet 01_greet.sd
$ ./greet Ada
Hello, Ada!
```

## Formatting

There is exactly one layout for Seed code, and `seed fmt` produces it: 4-space indentation, braces on the same line, one statement per line, single spaces around binary operators. Code that isn't formatted does not compile, and the error tells you to run `seed fmt`. There is nothing to configure and nothing to argue about. Run it whenever you save:

```
$ seed fmt hello.sd
```

`seed fmt` takes files, not directories; for a project with packages, name them all: `seed fmt *.sd */*.sd */tests/*.sd`.

The rules are in [formatting.md](../formatting.md), and [chapter 12](12-the-compiler.md) shows the formatter at work.

## The current compiler

Seed is young. The language described in this guide is complete and implemented, and so is the standard library ([stdlib.md](../stdlib.md)): `errors`, `fmt`, `io`, `os`, `fs`, `path`, `strings`, `math`, `rand`, `time`, `encoding`, `json`, `crypto`, `net` and `http` are all written, and [lib/README.md](../../lib/README.md) has the notes on each package. The known limitations of the implementation are listed in the [top-level README](../../README.md#status).

Next: [Basics](02-basics.md).
