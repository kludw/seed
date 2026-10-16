# Dogfooding: four realistic programs

[First impressions](first-impressions.md) was a Go programmer's first week with Seed: small programs, mostly to learn the idioms. This is the next step: four programs of the size and shape a Go programmer writes for real (a server with persistence, a text converter, a concurrent crawler, an interpreter), written from the user-facing documentation (the README, [the guide](guide/README.md), [stdlib.md](stdlib.md), [builtins.md](builtins.md), `seed doc`), to find the bugs and the friction that micro-tests miss. The compiler, runtime and library sources were read only to diagnose something that went wrong.

| Program | Lines (tests) | What it does |
|---|---|---|
| [examples/kv/](../examples/kv/) | 930 (303) | A key-value store server over TCP with a length-prefixed text protocol (`wire`), one goroutine that owns the data and applies requests in order while each connection has a goroutine of its own (`server`), an append-only log file replayed on start, with a torn last record dropped and the log compacted on open, automatically and on request (`store`), a client package, and a CLI (`flags`) with `serve`, `get`, `set`, `del`, `keys`, `compact`, `stats`, `bench` and a scripted `demo`. Tests start the server on a free port, run 8 clients at once, restart it from the log, and check malformed requests and idle timeouts. |
| [examples/mdhtml/](../examples/mdhtml/) | 696 (63) | Markdown to HTML: ATX and setext headings, paragraphs, emphasis, code spans, fenced and indented code, block quotes with lazy lines, nested tight and loose lists, links, images, autolinks, hard breaks, rules; the output matches CommonMark's reference renderer for these. Block structure line by line, inline markup with `regex` in passes. Golden-file tests in `md/testdata`. |
| [examples/crawl/](../examples/crawl/) | 366 (97) | A bounded concurrent crawler: a coordinator that owns the set of seen pages hands jobs to a pool of workers over channels (with `select` on send and receive), links found with `regex` and resolved with `url` (including `<base href>`), a per-request time limit with `http.Request.timeout`, depth, page and host limits. Tested against a local site (`site`) with a cycle, a 404, a redirect, a JSON page, a page slower than the limit and an external link. |
| [examples/lisp/](../examples/lisp/) | 1199 (100) | A small Scheme: a reader with line and column positions, environments in an arena (no pointers), closures with shared mutable state, `define`, `set!`, `let`/`let*`/`letrec`, `cond`, `and`/`or`, variadic lambdas, `apply`, integers of any size, tail calls in constant space (frames are given back as the loop goes), a depth limit that turns deep recursion into an error, a REPL on stdin that keeps reading an unfinished form, and test programs with expected outputs. |

Each has tests run by `seed test`, cases in `examples/run.sh` (`kv-demo`, `kv-help`, `kv-bad-command`, `mdhtml`, `mdhtml-standalone`, `crawl-demo`, `crawl-bad-url`, `lisp` on a REPL session), and is formatted.

## Overall

Seed held up. All four programs worked on the first or second run once they compiled, and the compiler's messages got me there quickly: most of my errors were Go habits (`key[i] == 127` on a byte, `int n, _ = s.toInt()`, a forgotten import) and the message said what to write. The concurrent code was the pleasant surprise: `group` plus channels made the kv server, the crawler's coordinator and the tests that start a server on a free port and stop it by closing its listener short and obviously correct, and nothing deadlocked or leaked. Every test passed when run ten times in a row and four at a time, and all four programs ran clean under AddressSanitizer and UndefinedBehaviorSanitizer (the kv server under a 64-client benchmark, oversized and truncated requests, and a 500 KB value).

I found no memory-safety or code-generation bug. What I found were bad error messages, one missing library function, a test-runner behaviour that made golden-file tests impossible, a strictness about returning a value beside its error that rejected a correct and common idiom, and a few places where the docs left out something a Go programmer trips on.

## Measurements

Linux x86_64, 4 cores, clang; `seed build` of each project, cold (empty build cache, so it includes compiling the runtime and the standard library packages it uses) and warm (nothing changed); binary size of the warm build.

| Program | Cold build | Warm build | Binary |
|---|---|---|---|
| kv | 3.8 s | 0.70 s | 559 KB |
| mdhtml | 4.3 s | 0.21 s | 583 KB |
| crawl | 5.0 s | 0.36 s | 910 KB (includes `http`, `json`, `url`, `regex`) |
| lisp | 4.5 s | 0.25 s | 411 KB |

**kv throughput** (`kv bench`: each client does SET then GET of its own keys, one request at a time, so it measures round trips; every SET is written to the log before it is acknowledged, one write per SET; 80,000 requests per run). For comparison, a Go server with the same protocol and structure (one owner goroutine fed by a channel, a goroutine per connection, an unbuffered `O_APPEND` log), benchmarked by the same Seed client:

| Clients | Seed server | Go server |
|---|---|---|
| 1 | 9,000 req/s | 11,000 req/s |
| 8 | 40,400 req/s | 56,500 req/s |
| 32 | 63,700 req/s | 89,500 req/s |

One client is bound by the round trip in this VM: a Go client against the Go server does 13,000 req/s, and against the Seed server 11,300. With more clients the Seed server reaches 70 to 75% of Go's throughput.

**Others.** mdhtml converts the whole guide plus stdlib.md and performance.md (280 KB, 6,300 lines) in 0.25 s (pandoc takes 1.6 s). The crawler crawls its 12-page test site, including a page cut off by its 300 ms limit, in 0.3 s. The lisp interpreter runs a loop of a million tail calls in 1.8 s (about 1.8 µs per iteration of `(if (= i n) acc (loop (+ i 1) n (+ acc i)))`; a tree-walking interpreter with a map lookup per variable, where the profile is flat: allocation, list building, string comparison of the special-form names).

## Friction log

Each item: what I tried, what happened, what I expected, the resolution, and whether it was **fixed**.

### Bugs and toolchain behaviour

1. **Tests could not find their files.** mdhtml's golden tests read `testdata/*.md`. `seed test examples/mdhtml` runs the test program in the directory where `seed test` was started, so the path depended on where you ran it from (from the repository root, `examples/run.sh` and an editor all differ). Go runs a package's tests in the package's directory, and SPEC.md's own example (`loadNames("testdata/users.txt")`) assumes it. **Fixed:** `seed test` runs each package's tests in the package's directory (the test program gets `--dir` and enters it; the results file path is made absolute first). DECISIONS.md, SPEC.md "Tests", the guide's chapter 11; `tests/testcmd/testdata`. While at it, a regression the change first introduced (a test program that deadlocked left an empty results file, which `seed test` reported as "unexpected results file") was caught by `tests/test_cmd.sh` and fixed: the file is written only at the end.
2. **`return v, err` was rejected.** In the interpreter, `Value v, error err = in.evalLoop(...)`, then frames to release, then `return v, err`: "v is read before err is checked". The rule protects a reader of `v`, but this passes the pair on unchanged, which is what every Go programmer writes, and the workaround (`if (err != null) { return null, err }` then `return v, null`) is four lines of noise. **Fixed:** a variable guarded by an error may be returned beside that same error variable, in a result of its own type (not widened into a `T?`, where the caller could read the null without checking). DECISIONS.md, SPEC.md "Error guards", guide chapter 4; `tests/full/return_guarded_pair.sd` (ints, big ints, strings, lists, structs, enums, two guarded values; also run under ASan), `tests/full/errors/guard_return_other_error.sd`, `tests/full/errors/guard_return_widened.sd`.
3. **No way to open a file for appending.** kv's store keeps its log open and appends to it. `fs` has `open` (read), `create` (truncate) and `append` (a whole write that opens and closes the file each time), so a long-lived log, or a `log.Output.File` that should survive a restart, could not be kept open for appending. I worked around it by rewriting the log compacted when the store opens (`fs.create` of a new file, then `fs.rename` over the old one), which kv keeps because it also drops a torn last record. **Fixed:** `fs.openAppend(path)`, like Go's `os.OpenFile(path, O_APPEND|O_CREATE|O_WRONLY, 0644)`. DECISIONS.md, stdlib.md; test in `lib/fs/tests/file_test.sd`.

### Bad error messages

All fixed, with tests in `tests/full/errors/` and entries in [compiler/BUGS_FIXED.md](../compiler/BUGS_FIXED.md) (44 to 46).

4. **A keyword as a name.** `Value test = ...` in the interpreter said `expected the end of the statement, found 'test'` with the hint `put one statement per line`; `fn f(int group)` said `expected a name for the parameter`. `test`, `group`, `select`, `spawn` and `map` are words a Go programmer uses as names all the time. **Fixed:** `'test' is a keyword, so it cannot be used as a name`, hint `choose another name, such as testValue`, for variables, loop variables, parameters and fields (`keyword_as_name`, `keyword_as_name_param`).
5. **A byte compared with a number.** `key[i] == 127` said `mismatched types byte and int for ==` and nothing more. **Fixed:** the hint says a number is an int and shows `'\x7f'` or `byte(127)` (for other byte/int operands, `byte(n)` or `int(b)`) (`byte_int_compare_hint`).
6. **A second error after a fixed-up one.** `int start, _ = number.toInt()` is rightly an error (`start` may be null, declare it `int?`), but my `if (start == null)` on the next line then got `start has type int, which is never null`, which contradicts the first message. **Fixed:** a variable reported that way gets no further errors (`discard_plain_no_cascade`).

### Documentation

7. **A deferred method call takes its receiver when the `defer` runs.** kv's store holds its open log file and replaces it when it compacts. `defer st.close()` at the top of the owner goroutine closed the log the store had when the `defer` ran, a handle already closed by the first compaction, and never the current one. That is consistent with "the arguments are evaluated when the defer statement runs" (and with Go's value receivers), but nothing said the receiver is one of them, and in Go one would have used a pointer receiver and got the current state. **Fixed (docs):** SPEC.md and the guide's chapter 4 say so, and what to do (close the new one where it is replaced, or at the end). **Fixed (checker):** assigning the receiver of a deferred method call, or a variable passed to it, after the `defer` is an error (`log is assigned after the defer of log.close() on line 12, but the deferred call uses the value log had when the defer ran`); field assignments and `mut` method calls stay allowed, to avoid rejecting harmless code (DECISIONS.md, SPEC.md "Control flow"; `tests/full/errors/defer_receiver_reassigned*.sd`, `defer_argument_reassigned.sd`, `tests/full/defer_loop_values.sd`).
8. **Stack size.** The interpreter's first non-tail recursion test, `(deep 20000)`, crashed with `stack overflow` after about 5,100 levels (each level is two Seed calls with large frames: an `eval` with a big `switch`). Go's stacks grow to 1 GB, so a Go programmer doesn't think about this; nothing in the guide gave the size. **Fixed (docs):** chapter 14 says that spawned goroutines (and tests) have 8 MB, that `main` has the shell's `ulimit -s` (up to 1 GB), and that programs whose depth comes from their input should count it, as `json.parse` does. The interpreter limits evaluation to 4,000 nested calls and reports `recursion too deep` with the position. **Proposal, not implemented:** a `spawn` with a stack size, or a documented environment variable, for the rare program that needs a deep stack in a goroutine.

### Language and library design (not fixed; proposals)

9. **Testing for one case of an enum.** To ask "is this value a `Value.Closure`?" (the interpreter's `procedure?`, `number?`, ...) or "is the first block a paragraph?" (mdhtml), a `switch` must list every case, even with only one of interest, because a switch over an enum is exhaustive and has no `default`. In the interpreter I used the built-in `name()` instead (`v.name() == "Closure"`), which works but is a string the compiler doesn't check. **Proposal:** a built-in `v.is(Value.Closure)`-style test, or let `==` compare with a case pattern whose fields are `_` (`v == Value.Closure(_)`); either keeps the exhaustive `switch` for code that handles every case.
10. **No closures for `regex.replaceWith`.** mdhtml's inline passes need state (HTML already produced must be stashed so later passes don't touch it), and `replaceWith(s, fn(Match) string)` takes a plain function, which can't carry it. I wrote the loop by hand with `findAt` (15 lines, once). That is the "no closures" trade-off as designed; a `Regex.split`-like method that returned the pieces between and including the matches would make the hand-written loop shorter. Not fixed.
11. **A package-level name blocks every local of that name.** A helper `fn reason(string message)` in package `crawl` made `case Outcome.Failed(string reason):` an error elsewhere in the package (`reason is already declared at the package level`). Go allows the shadowing. It follows from "no shadowing", and the message is clear; noted because it happens more in big packages, where the package-level names are many. Not fixed.
12. **`select` with an optional case.** The crawler's coordinator sends the next job only when its queue isn't empty. Go would set the channel to `nil` to disable the case; Seed has no nil channel, so the code is an `if` around two forms (a plain receive, or a `select` with the send and the receive). Clear enough; noted. Not fixed.
13. **Values that hold a handle need a real one at construction.** `Store{log: ...}` must be given an open `fs.File`, so the first draft created a throwaway file just to have one. Restructuring (a function that opens the log and returns it with its size, then the literal) was better code anyway. By design (no zero values, no forgeable handles); noted.
14. **No signals.** The kv server is stopped with SIGTERM, which kills it at once. Its log is written before each acknowledgement, so nothing is lost, but a server that buffers, or wants to log "shutting down", can't. **Proposal:** `os.signals([]string names) chan string` (a channel that receives the signal's name), so that a `select` in a goroutine can fail its group, which stops the server through the usual path.
15. **A failed `assert` on two lists shows no values.** Lists have no text form, so `assert(got == want)` on `[]Request` or `[]string` says only `assert failed: got == want`. For the golden tests I compared strings with the file name prefixed, to see the first difference. **Proposal:** for lists, show the lengths and the index of the first element that differs (`(len 4 == len 5, first difference at 2)`).
16. **`url.parse` keeps characters a request line can't carry.** A link `href="&quot;odd&quot;.html"` became the URL `/docs/deep/"odd".html`, and `http.send` put the quote in the request line as it was. Go's `url.Parse` accepts it too but its client escapes the path when it writes the request. Minor; not fixed.

### What worked well

- Exhaustive enums for the protocol (`wire.Request`, `wire.Reply`) and for the interpreter's values: adding a reply kind pointed at every switch to update.
- Copy-on-write values made the kv store, the crawler's page list and the interpreter's frames simple to reason about; the arena pattern of chapter 7 was exactly right for environments that closures share.
- `group` and `spawn` for servers in tests: `try group { spawn serve(ln, ...); spawn client(ln, ...) }` with the client closing the listener when it is done is a complete, leak-free integration test, and a failing client stops the server.
- `http.Request.timeout` cut off the slow page exactly at its limit, and closing the listener stopped the server with that handler still sleeping.
- `flags` (`--help` for free), `log` (`with(...)` fields), `url.resolve` and `regex` needed no workarounds at all.
