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

Beyond an argument or two, the `flags` package parses them: `--name value`, `--name=value`, `-n 2`, bundled `-ln2`, `--` and a generated `--help`. Seed has no pointers, so a flag is not bound to a variable as with Go's `flag.Int(&n)`: a `flags.Spec` value declares the flags, parsing gives a `flags.Parsed` value, and the program reads each flag from it with a typed getter. `parseOrExit` handles `--help` and mistakes the way command-line tools do:

```seed
// Greets someone, as often as asked.
//
//     seed run 01_flags.sd -- --name Ada -n 2 -l
package main

import "flags"
import "fmt"

fn main([]string args) error {
    flags.Spec spec = flags.spec("greet", "", "Greets someone.")
    spec.addString("name", null, "world", "who to greet")
    spec.addInt("times", 'n', 1, "how many times")
    spec.addBool("loud", 'l', "shout")
    flags.Parsed p = spec.parseOrExit(args)
    string greeting = "Hello, " + p.getString("name") + "!"
    if (p.getBool("loud")) {
        greeting = greeting.toUpper()
    }
    for (int i = 0; i < p.getInt("times"); i++) {
        fmt.println(greeting)
    }
    return null
}
```

```
$ seed run 01_flags.sd -- --name Ada -n 2 -l
HELLO, ADA!
HELLO, ADA!
$ seed run 01_flags.sd -- --nmae Ada
greet: --nmae: unknown flag (did you mean --name?)
run 'greet --help' for usage
$ seed run 01_flags.sd -- --help
usage: greet [flags]

Greets someone.

flags:
      --name string   who to greet (default "world")
  -n, --times int     how many times (default 1)
  -l, --loud          shout
  -h, --help          show this help and exit
```

Asking for a flag that was not declared, or with the wrong type (`p.getInt("loud")`), crashes at once, so such a mistake shows up the first time the program runs. The details are in [the standard library reference](../stdlib.md#flags).

## The commands

| Command | What it does |
|---|---|
| `seed run [-g] <file.sd or dir> [args...]` | Compiles the program into a temporary executable and runs it. The exit code is the program's. |
| `seed build [-g] [-o out] <file.sd or dir>` | Compiles the program into an executable (named after the file or directory unless `-o` names it): a single binary with no dependencies. With `-g`, for a debugger: gdb and lldb then show the Seed source ([chapter 14](14-crashes-and-debugging.md)). |
| `seed test [-v] [--run text] [dir...]` | Runs the tests of the packages under the directories; `-v` lists each test as it runs, `--run` runs only the tests whose description contains the text (see [chapter 11](11-packages.md)). |
| `seed fmt <file.sd or dir>...` | Rewrites the files in the one canonical layout; a directory means every `.sd` file under it. |
| `seed fmt --check <file.sd or dir>...` | Changes nothing, and reports the files that are not formatted (exit code 1). |
| `seed c [-g] <file.sd or dir>` | Writes the generated C program to stdout. |
| `seed lsp` | The language server, for editors (see [Editors](#editors)). |

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

Given a directory, `seed fmt` formats every `.sd` file under it, in subdirectories too (hidden ones, such as `.git`, are skipped), so a whole project with its packages and tests is `seed fmt .`, as with `gofmt -w .`.

The rules are in [formatting.md](../formatting.md), and [chapter 12](12-the-compiler.md) shows the formatter at work.

## Editors

`seed lsp` is a language server: an editor that speaks the Language Server Protocol starts it and talks to it over stdin and stdout. It checks the file you are editing as you type, together with the rest of its package and the packages it imports (exactly as `seed build` or `seed test` would, with the editor's unsaved text), and gives the editor:

- the compiler's errors and hints, and a warning on the first line that `seed fmt` would change;
- formatting (the editor's **Format Document** runs the same formatter as `seed fmt`);
- go to definition and hover (the declaration, with its doc comment) for locals, parameters, functions, methods, types, fields, enum cases, constants and packages;
- completion after `pkg.` (the package's `pub` names), after `value.` (fields and methods) and after `Enum.` (cases), and otherwise the names in scope and the keywords;
- signature help: the parameters of the function or method being called, with the one you are typing;
- find all references and the highlights of a name in the file, across the files of its package, its tests and the packages of the workspace that import it;
- rename, of the same names, in every file that uses them (unsaved ones included); a new name that breaks the naming rules, is a keyword or is already taken is refused with the compiler's reason, and so is a name of the standard library;
- the outline of the file.

For VS Code, [editors/vscode/](../../editors/vscode/README.md) has syntax highlighting and a client that starts `seed lsp` (it needs `npm install` once; the README says how). Other editors need only to run `seed lsp` for `.sd` files: in Neovim, for example, `vim.lsp.start({name = "seed", cmd = {"seed", "lsp"}, root_dir = vim.fn.getcwd()})` from a `FileType` autocommand for the `seed` file type.

## The current compiler

Seed is young. The language described in this guide is complete and implemented, and so is the standard library ([stdlib.md](../stdlib.md)): `errors`, `fmt`, `io`, `os`, `fs`, `path`, `strings`, `math`, `rand`, `time`, `encoding`, `json`, `crypto`, `net` and `http` are all written, and [lib/README.md](../../lib/README.md) has the notes on each package. The known limitations of the implementation are listed in the [top-level README](../../README.md#status).

Next: [Basics](02-basics.md).
