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
| [kv/](kv/) | A key-value server over TCP: a length-prefixed protocol, one goroutine owning the data, a goroutine per client, an append-only log replayed on start (a torn last record dropped) and compacted, a shutdown in order on SIGINT or SIGTERM (`os.signals`), a client CLI with `flags` and a benchmark; tests run concurrent clients against a server on a free port and restart it. | `seed run examples/kv demo`, `seed test examples/kv` |
| [mdhtml/](mdhtml/) | Markdown to HTML (headings, emphasis, code, quotes, nested lists, links), with `regex` for inline markup and golden-file tests in `md/testdata`. | `seed run examples/mdhtml README.md`, `seed test examples/mdhtml` |
| [crawl/](crawl/) | A bounded concurrent web crawler: a coordinator and a worker pool on channels, `regex` link extraction, `url` resolution, per-request time limits, depth, page and host limits; tested against a local site. | `seed run examples/crawl --demo`, `seed test examples/crawl` |
| [lisp/](lisp/) | A small Scheme interpreter: reader with positions, closures over environments in an arena, tail calls in constant space, a depth limit, a REPL; test programs with expected outputs. | `seed run examples/lisp`, `seed test examples/lisp` |
| [csvgrep.sd](csvgrep.sd) | Filtering CSV files by a regular expression: `flags` with a generated `--help`, `regex`, reading and writing with `csv`, and a `--verbose` trace with `log`. | `seed run examples/csvgrep.sd -c city Lisbon examples/testdata/people.csv` |
| [links.sd](links.sd) | The links of HTML pages: `regex` groups and `findAll`, resolving relative references against the page's URL with `url`, query parameters. | `seed run examples/links.sd -b https://example.com/docs/ examples/testdata/page.html` |
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
| [newcomer/](newcomer/) | Four projects written by a Go programmer learning Seed from the docs ([first impressions](../docs/first-impressions.md)): `todo` (a JSON-file store package), `linkcheck` (a worker pool checking URLs with a per-request timeout, against its own local server), `logstat` (log parsing with `time.parse`, maps, percentiles and `sortBy`) and `vending` (two state machines, enums behind one interface). | `seed run examples/newcomer/linkcheck --demo`, `seed test examples/newcomer` |

A directory example is a project: `main.sd` is `package main`, and its packages are subdirectories (`calc/expr`, `bank/bank`, `kvstore/store`, `httpapi/api`, `kv/wire`, `md`, `crawl`, `lisp`), found relative to the project directory. Their tests are in `tests/` inside each package, and `seed test examples/NAME` runs them.

## Checking the examples

`make test-examples` runs two checkers. `examples/run.sh` builds every example into `build/examples/`, runs the non-interactive ones (the REPLs with input from `testdata/`) and compares their stdout, stderr and exit code with `expected/CASE.out`, runs `seed test` on every project, and checks that every file is formatted. `examples/run.sh build/seed2 --update` rewrites the expected files after a deliberate change; review the diff before committing it. `tests/examples.sh` checks wordcount, todo, server and fetch, which need a temporary file or a running server.
