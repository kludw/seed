# Seed documentation

Start with the guide if you want to learn Seed; use the reference when you need the exact rule. The [top-level README](../README.md) has the pitch, the quick start and the project status.

## Learning

- [guide/](guide/README.md): the Seed guide, teaching the whole language by example in 14 chapters, with every example checked by [guide/check.sh](guide/check.sh).
  1. [Getting started](guide/01-getting-started.md): building the toolchain, hello world, `seed run`, `seed build`, `seed fmt`.
  2. [Basics](guide/02-basics.md): types, literals, declarations, constants, operators, strings and printing.
  3. [Control flow](guide/03-control-flow.md): `if`, the one `for`, `switch`, `break` and `continue`.
  4. [Functions and errors](guide/04-functions-and-errors.md): multiple results, errors as values, `try`, `_`, `defer`, crashes and exit codes.
  5. [Null safety](guide/05-null-safety.md): `T?` and how the compiler narrows it.
  6. [Lists and maps](guide/06-lists-and-maps.md): every method, value semantics and copy-on-write.
  7. [Structs, methods and `mut`](guide/07-structs-and-methods.md): why there are no pointers, and how to model shared state.
  8. [Enums](guide/08-enums.md): enums with data, exhaustive `switch`, trees without pointers.
  9. [Interfaces and function values](guide/09-interfaces-and-functions.md): `implements`, `error` as an interface, `sortBy`, function literals.
  10. [Concurrency](guide/10-concurrency.md): goroutines that share nothing, channels, `select`, groups and Erlang-style failure.
  11. [Packages and visibility](guide/11-packages.md): `pub`, imports, package layout, tests.
  12. [The compiler is your friend](guide/12-the-compiler.md): the pedantic checks, reading error messages, formatting.
  13. [Seed for Go programmers](guide/13-for-go-programmers.md): the differences side by side, and why.

## Reference (the language spec)

- [SPEC.md](../SPEC.md): the language specification: types, null, errors, concurrency, packages, tests, the compiler's checks and the toolchain.
- [grammar.md](grammar.md): the lexical rules (keywords, the newline rule) and the grammar in EBNF.
- [builtins.md](builtins.md): every method of the built-in types, conversions, runtime failures, and the core packages `errors`, `fmt`, `os`, `fs` and `time`.
- [stdlib.md](stdlib.md): the standard library, all 15 packages, with signatures, errors and crashes.
- [formatting.md](formatting.md): the one canonical layout that `seed fmt` produces and the compiler requires.
- [DECISIONS.md](../DECISIONS.md): the decisions made during implementation without the language's author, grouped by topic, for review.

## Implementation

- [compiler/README.md](../compiler/README.md): the compiler written in Seed: phases, data structures, code generation, the formatter, the bootstrap cycle, decisions and known limitations.
- [runtime/README.md](../runtime/README.md): the C runtime: value representations, ownership, goroutines, channels, crashes, the native-function ABI.
- [lib/README.md](../lib/README.md): the standard library's implementation status, conventions and test conventions.
- [seed0.md](seed0.md): the retired bootstrap compiler in C and the language subset it accepted; the compiler is now built from a C snapshot of itself (`bootstrap/seed.c`, see compiler/README.md).
- [tests/README.md](../tests/README.md): the conformance tests: layout, file format, how to run them.
- [examples/README.md](../examples/README.md): the example programs and how they are checked.
- [compiler/BUGS_FIXED.md](../compiler/BUGS_FIXED.md) and [lib/COMPILER_BUGS.md](../lib/COMPILER_BUGS.md): compiler bugs found during QA and while writing the library, with their status.
- [lib/BUGS_FIXED.md](../lib/BUGS_FIXED.md): library bugs found by the QA pass, and how the library was tested.
- [first-impressions.md](first-impressions.md) and [dogfood.md](dogfood.md): friction logs of a Go programmer writing Seed, first small programs, then four realistic ones (a key-value server, a Markdown converter, a web crawler, a Lisp interpreter) with measurements against Go.
- [performance.md](performance.md): benchmarks against Go ([bench/](../bench/)), where the time goes, what the runtime optimises, and proposals for the compiler.

## Tools

- [editors/vscode/](../editors/vscode/README.md): syntax highlighting, editor settings and a client for the language server (`seed lsp`) for VS Code.
