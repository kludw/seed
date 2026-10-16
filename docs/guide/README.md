# The Seed guide

Seed is a small, strongly typed language in the family of Go and C. It is built on a few firm ideas: there is one way to do each thing, nothing happens implicitly, there is no `nil` and there are no pointers, and the compiler is a strict friend that rejects anything doubtful and explains how to fix it.

This guide is for programmers who already know Go or another C-like language. It teaches the whole language by example. The precise rules are in the reference documents: [SPEC.md](../../SPEC.md), [grammar.md](../grammar.md), [builtins.md](../builtins.md), [stdlib.md](../stdlib.md) and [formatting.md](../formatting.md).

## Chapters

1. [Getting started](01-getting-started.md): building the toolchain, hello world, `seed run`, `seed build`, `seed fmt`.
2. [Basics](02-basics.md): types, literals, declarations, constants, operators, strings and printing.
3. [Control flow](03-control-flow.md): `if`, the one `for`, `switch`, `break` and `continue`.
4. [Functions and errors](04-functions-and-errors.md): multiple results, errors as values, `try`, `_`, `defer`, crashes and exit codes.
5. [Null safety](05-null-safety.md): `T?` and how the compiler narrows it.
6. [Lists and maps](06-lists-and-maps.md): every method, value semantics and copy-on-write.
7. [Structs, methods and `mut`](07-structs-and-methods.md): default values of fields, why there are no pointers, and how to model shared state.
8. [Enums](08-enums.md): enums with data, exhaustive `switch`, trees without pointers.
9. [Interfaces and function values](09-interfaces-and-functions.md): `implements`, `error` as an interface, `sortBy`, function literals.
10. [Concurrency](10-concurrency.md): goroutines that share nothing, channels, `select`, groups and Erlang-style failure.
11. [Packages and visibility](11-packages.md): `pub`, imports, package layout, tests.
12. [The compiler is your friend](12-the-compiler.md): the pedantic checks, reading error messages, formatting.
13. [Seed for Go programmers](13-for-go-programmers.md): the differences side by side, and why.
14. [Crashes and debugging](14-crashes-and-debugging.md): reading a crash's trace, and `seed build -g` with gdb or lldb.

## The examples

Every complete program is in [examples/](examples/), with its exact output in a `.out` file next to it; the project of chapter 11 is the directory [examples/11_shop/](examples/11_shop/), with the output of the program (`main.out`) and of its tests (`test.out`). The programs that the compiler must reject are in [examples/errors/](examples/errors/), each with the exact error message in a `.err` file, and the formatting example of chapter 12 is in [examples/fmt/](examples/fmt/). To check all of them against the current compiler:

```
make
docs/guide/check.sh
```

The script compiles every example (the pedantic checks are always on), runs it, compares its output and exit code, runs the tests of the chapter 11 project, and checks that every example is formatted.
