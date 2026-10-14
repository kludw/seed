# Examples

Complete Seed programs, each with a header comment saying what it shows. Run them from the repository root after `make` (which builds `build/seed2`; `seed` below means that binary).

| Example | What it shows | Run it |
|---|---|---|
| [calc/](calc/) | A small expression language: tokenizer (`enum Token`), recursive-descent parser building a tree of enums, evaluator with recursion and errors that point at a column, a REPL on stdin, tests. | `seed run examples/calc`, `seed test examples/calc` |
| [grep.sd](grep.sd) | Searching files for a substring with line numbers; flags parsed from `args`; per-file errors and grep's exit codes. | `seed run examples/grep.sd -i sea examples/testdata/harbour.txt` |
| [primes.sd](primes.sd) | The concurrent prime sieve: a pipeline of filter goroutines joined by channels, each stage starting the next in a nested `group`. | `seed run examples/primes.sd 1000` |
| [workers.sd](workers.sd) | A worker pool with a results channel; one failing job stops the whole batch (Erlang-style); a deadline with `select` and `time.after`. | `seed run examples/workers.sd` |
| [bank/](bank/) | A package with private struct fields and `mut` methods, an interface with two implementations, transactions as an enum, atomic transfers by value, tests. | `seed run examples/bank`, `seed test examples/bank` |
| [kvstore/](kvstore/) | A key-value store owned by one goroutine, with requests carrying reply channels ("share memory by communicating"), concurrent clients, and a JSON snapshot on disk. | `seed run examples/kvstore kv.json < commands`, `seed test examples/kvstore` |
| [jsonfmt.sd](jsonfmt.sd) | Pretty-printing, compacting or validating JSON from stdin, with errors that show the line and column. | `seed run examples/jsonfmt.sd < examples/testdata/order.json` |
| [httpapi/](httpapi/) | A JSON HTTP API for a to-do list owned by a goroutine; tests start the server on a free port and call it with the http client. | `seed run examples/httpapi`, `seed test examples/httpapi` |
| [life.sd](life.sd) | Conway's Game of Life on a wrapping board, printed for N generations. | `seed run examples/life.sd 10` |
| [bigmath.sd](bigmath.sd) | Growable `int`: factorials, Fibonacci(500), modular exponentiation, a toy RSA round trip, Lucas-Lehmer. | `seed run examples/bigmath.sd` |
| [wc/](wc/) | A `wc` that walks directories, sums per extension, and can ask `git ls-files` (with `os.output`) which files to count. | `seed run examples/wc -e .sd examples`, `seed test examples/wc` |
| [values.sd](values.sd) | Value semantics next to the Go aliasing surprises they replace: sub-slices, `append`, struct copies, range variables, maps, channels. | `seed run examples/values.sd` |
| [wordcount.sd](wordcount.sd) | The most common words on stdin. | `seed run examples/wordcount.sd 5 < book.txt` |
| [todo.sd](todo.sd) | A to-do list kept in a JSON file. | `seed run examples/todo.sd add buy milk` |
| [server.sd](server.sd) | A small HTTP server with a counter owned by a goroutine. | `seed run examples/server.sd :8080` |
| [fetch.sd](fetch.sd) | Fetching URLs concurrently with a pool of workers. | `seed run examples/fetch.sd http://localhost:8080/` |

A directory example is a project: `main.sd` is `package main`, and its packages are subdirectories (`calc/expr`, `bank/bank`, `kvstore/store`, `httpapi/api`), found relative to the project directory. Their tests are in `tests/` inside each package, and `seed test examples/NAME` runs them.

## Checking the examples

```
make
examples/run.sh
```

`run.sh` builds every example into `build/examples/`, runs the non-interactive ones (the REPLs with input from `testdata/`) and compares their stdout, stderr and exit code with `expected/CASE.out`, runs `seed test` on every project, and checks that every file is formatted. `examples/run.sh build/seed2 --update` rewrites the expected files after a deliberate change; review the diff before committing it.

`make test-examples` (`tests/examples.sh`) also builds every single-file example, and checks wordcount, todo, server and fetch, which need a temporary file or a running server. To make it run these checks too, add a line to its recipe in the Makefile:

```
test-examples: build/seed2
	tests/examples.sh build/seed2
	examples/run.sh build/seed2
```
