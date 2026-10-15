# First impressions: a Go programmer tries Seed

This is the friction log of one Go programmer learning Seed from the user-facing documentation alone (the README, [the guide](guide/README.md), [stdlib.md](stdlib.md), [builtins.md](builtins.md) and the compiler's messages), without reading the compiler, the runtime, the library sources or the tests. The toolchain was built with `make`, installed with `make install PREFIX=$TMPDIR/seedtry`, and used as `seed` from a directory outside the repository.

The four programs written for the exercise are in [examples/newcomer/](../examples/newcomer/):

| Program | What it exercises |
|---|---|
| `todo` | A CLI (`add`, `list`, `done`, `remove`) over a `store` package that keeps items in a JSON file; tests with `fs.makeTemp`. |
| `linkcheck` | A worker pool of goroutines checking URLs from a file with `http.get`, a per-request timeout with `select` and `time.after`, and a sorted report. `--demo` (and the tests) serve a local `http` server on a free port and check it. |
| `logstat` | Parses lines like `2026-10-14T12:00:00Z GET /path 200 12ms` with `time.parse` and `strings.fields`, counts per status and per path with maps, p50/p95 latency, `sortBy`. |
| `vending` | Two state machines (a vending machine whose state is an enum with data, and a turnstile) behind one interface with a `mut` method, driven by a script on stdin. |

Each item below says what I tried, what happened, what I expected, and how it was resolved, and whether it has been **fixed** (in the docs, the compiler or the tooling, after the programs were finished) or is **not fixed**.

## Overall

The experience was much smoother than expected. The guide is thorough, and once I wrote code in its idiom, three of the four programs compiled on the first attempt, and their tests passed on the first or second run. The compiler's messages are, with the exceptions below, the best I have seen in a young language: they name the thing, point at it and say what to write instead (`spawn is only allowed inside a group block`, `a select case cannot assign to an existing variable` (since then, a case can receive into an existing variable), `++ cannot change a map entry: the key may be missing`). A failed `assert` that prints both sides of the comparison made one bug in my own code obvious at once. I found no compiler bugs and no library bugs: every library edge case I probed (`"".split(",")`, `json` numbers and `asInt`, `time.parse` with offsets and a space for `T`, `time.format` with fractions, `fmt.fixed` rounding, `strings.padLeft` on UTF-8, `http.get` on a bad scheme or a refused port, stopping a server by closing its listener) behaved exactly as stdlib.md says.

Most of the friction came from Go habits, from a few idioms the guide leaves for the reader to discover, and from the deliberate verbosity of "one way to do things".

## Doc errors

1. **The operator table said `==` works on "every type except channels and functions".** I wrote `assert(a == 3)` for an `int? a` from a JSON lookup and got `a is nullable (int?), so it can only be compared with null`. The rule is in SPEC.md, but the guide's table in chapter 2 contradicted it and chapter 5 didn't mention it. Resolved with `a != null && a == 3`. **Fixed:** chapter 2's table points to the rule, and chapter 5's rules state it with the workaround.
2. **The guide showed a misleading compiler hint.** Chapter 7 quoted `hint: copy it into a local variable first, or make the method mut` for a `mut` method called on a function parameter, where making a method `mut` cannot help (see bad error messages, 6). **Fixed** together with the message.

## Doc gaps

1. **Timing out an HTTP request.** stdlib.md said "run the request in a goroutine and `select` on its result channel and `time.after`". My first reading put the `select` in the `group` block next to `spawn fetch(...)`. That prints "timeout" on time, but the group then waits for the request anyway: 2 s against a slow local handler, and up to 30 s against a hung server. The correct shape (a second spawned goroutine that fails on timeout, so that the group stops the request) is in the guide's chapter 10 example, but nothing says why it has to be that way. **Fixed:** stdlib.md's client section has a complete, tested `getWithin(url, d)` and says why the `select` must be in a goroutine; chapter 10 has a paragraph after the timeout example.
2. **Discarding the value and keeping the error.** The guide shows `string? text, _ = fs.read(path)` and `_ = f()`, but not `_, error err = f()`, which is what a test that expects a failure needs. I guessed it and it worked. **Fixed:** one sentence in chapter 4.
3. **Declaring a variable whose value comes from one of two branches.** With no zero values, `string text` followed by an `if`/`else` that assigns it doesn't compile. I first wrote `string text = ""` with a dummy value. The better idiom, `string? text = null` assigned on every branch and narrowed to `string` after the `if`, follows from chapter 5's rules but isn't pointed out where the "no zero values" rule is introduced. **Fixed:** chapter 2's rule mentions it.
4. **A null `error` is not checked by the compiler.** `err.message()` on a `null` error compiles and crashes (`crash: message() called on a null error`), unlike every `T?`. Nothing in the guide said so. **Fixed:** a note in chapter 4; since then the compiler checks it (language design friction, 3).
5. **`seed fmt` takes files only.** Coming from `gofmt -w .`, I ran `seed fmt .` and got `error: fs.read: .: is a directory`. **Fixed:** `seed fmt` now takes directories (language design friction, 9), and chapter 1 shows `seed fmt .`.
6. **`seed test` on a `tests/` directory.** `seed test store/tests` printed `0 passed, 0 failed` and exited with 0, which looks like success. **Fixed:** chapter 11 says to name the package directory, and the command now reports the mistake (bad error messages, 8).
7. **There is no way to run one test or list the tests run.** `seed test -v` is taken as a directory. The guide doesn't say there are no flags. **Fixed:** `seed test` has `-v` (prints each package and each test, `PASS` or `FAIL`, as it runs) and `--run text` (only the tests whose description contains the text; matching none is an error), documented in chapter 11 and the usage text; other options, such as Go's `-run`, are usage errors that name `--run`. Tests in `tests/test_cmd.sh`.

## Bad error messages

All the fixed ones have regression tests in `tests/full/errors/` (`go_short_decl`, `go_var_decl`, `go_builtin_len`, `go_builtin_append`, `go_nil`, `conv_int_to_string_hint`, `conv_byte_to_string_hint`, `param_change_element`, `fmt_printf_hint`, `tuple_value_try_hint`) or in `tests/test_cmd.sh`.

1. **`x := 5`** gave `expected the end of the statement, found ':'` with the hint `put one statement per line`, which is the wrong advice. **Fixed:** `there is no :=; a declaration writes the type first`, hint `declare it with its type (int count = 0), or assign an existing variable with =`. Also for `a, b := f()`.
2. **`var n int = 5`** gave `expected '=' in the declaration or assignment, found 'int'` (the parser took `var` for a type). **Fixed:** `there is no var: a declaration starts with its type`.
3. **Go built-ins** (`len(xs)`, `append(xs, x)`, `make`, `new`, `delete`, `close`, `copy`, `cap`, `panic`, `recover`, `print`) gave `undefined function len` with no hint. **Fixed:** each gets a hint with the Seed way (`Seed has no len(): use the method, xs.len()`, `... add to a list in place with xs.push(x)`, `... return an error, or call os.crash(message) for a bug`, ...).
4. **`nil`** gave `undefined: nil` with no hint, typically in `if (err != nil)`. **Fixed:** `hint: Seed has no nil: use null, which only nullable types (T?) and error can hold`.
5. **`fmt.println(n)` with an `int`** gave `cannot use the value (type int) as string ...; there are no implicit conversions` without a hint, although `"n=" + n` gets one. **Fixed:** `hint: turn it into text with .string()`, and for a `byte` (`string c = s[i]`) a hint that a byte is a number, with the three ways to get text from it.
6. **Assigning to a plain function's parameter** (`args = args.slice(1, args.len())` in `main`) hinted `copy it into a local variable first, or make the method mut`; `main` isn't a method, and a `mut` method only lets its receiver change. **Fixed:** `copy it into a local variable and change that; return the new value if the caller needs it` (receivers keep their own message). Since then, assigning a parameter is allowed (language design friction, 6), and the hint is for changing one in place (`xs.push(x)`, `xs[0] = x`).
7. **`seed fmt .`** said `error: fs.read: .: is a directory`. **Fixed:** `error: seed fmt: . is a directory; name the .sd files, such as seed fmt ./*.sd`; since then, `seed fmt .` formats the directory (language design friction, 9).
8. **`seed test store/tests`** said `0 passed, 0 failed` (exit 0). **Fixed:** `error: seed test: store/tests holds the tests of a package; name the package's directory instead: seed test store` (exit 1).
9. **`crash: deadlock: all goroutines are blocked`** says nothing about where the goroutines are blocked. In a bigger program this would be hard to track down. **Fixed:** the first line is unchanged, and the lines after it give the number of blocked goroutines and, for each, its spawn (`worker()`, `main()`, or `test "..."` under `seed test`) and what it waits for (receiving from or sending on a channel, a select, its group's goroutines), indented under the goroutine that owns its group. Tests in `tests/test_cmd.sh` and `runtime/tests/test_exit.sh`.
10. **`fmt.printf(...)`** gives `package fmt has no printf; did you mean print?`; a hint that there are no format strings (`use + and .string(), or fmt.fixed`) would help more. **Fixed:** `hint: Seed has no format strings: join text with + and .string() ("n=" + n.string()), or use fmt.fixed(f, places) for a float`.
11. **`int q = div(1, 2)` for a `(int, error)` function** hints `assign them with a declaration such as T a, error err = ...`; `try` is usually what a newcomer wants and could be mentioned. **Fixed:** `hint: use try to pass the error up (try div(...)), or handle it yourself: T a, error err = div(...)`.

## Compiler bugs

None found.

## Library bugs

None found.

## Language design friction

These are deliberate choices; the notes record what they cost a newcomer in practice, for the language owner to weigh.

1. **Counting with a map is five or six lines.** `m[k]++` is rejected, so each count is `int? n = m[k]`, `if (n == null) { m[k] = 1 } else { m[k] = n + 1 }`. `logstat` needed it twice; I wrote a helper, which (because parameters are read-only) has to take the map and return a new one: `statuses = bump(statuses, key)`. Counting is common enough that a lookup with a default (`m.get(k, 0)` or similar) would remove most of the noise without adding a second way to do anything else. **Fixed:** `m.getOr(k, d)`, so a count is `m[k] = m.getOr(k, 0) + 1` (chapter 6; `logstat` uses it and lost its helper).
2. **A timeout on one blocking call costs about 25 lines.** `getWithin` needs two goroutines, two channels, a nested `group` and an error used as a cancellation signal; to tell a timeout from a crash, `linkcheck` sends the outcome on a channel before failing. Go would use `http.Client{Timeout: ...}` or a context. `net.dial` and `net.Conn.setTimeout` already take a timeout; `http.send` has a fixed 30 seconds. A timeout field on `http.Request` (or a `http.sendWithin`) would cover the most common case.
3. **`error` is the one nullable type the compiler doesn't check.** "No nil" holds for every `T?`, but `err.message()` on a null error is a run-time crash. Narrowing `error` like a `T?` would close the gap, at the cost of a check in the rare places that read a message they know is set. **Fixed:** `error` is narrowed like a `T?`; `message()` on an error that may be null is a compile error (`err may be null (an error is null when there is no error)`), chapter 5.
4. **A `T?` can't be compared with a value.** In tests this turns `assert(counts["a"] == 3)` into two lines (`int? a = counts["a"]`, `assert(a != null && a == 3)`). Comparing `T?` with `T` (false when null) would be unambiguous. **Fixed:** `assert(counts["a"] == 3)` compiles, false when the key is missing (two `T?` still don't compare).
5. **No zero values and no conditional expression** make "the value depends on a branch" awkward: either a dummy initial value or the `T? x = null` dance (doc gaps, 3).
6. **Parameters can't even be reassigned.** Shifting `args` after parsing a flag (`args = args.slice(2, args.len())`) is rejected, so `todo` names the parameter `argv` and copies it into `[]string args`. Making a reassignment of a parameter local to the function would keep the promise that a call can't change the caller's variables. **Fixed:** a parameter can be assigned a new value (only the function's copy changes); changing one in place is still an error.
7. **Helpers that never return don't count as leaving.** A `usage()` that ends with `os.exit(2)` doesn't narrow anything after `if (name == null) { usage() }`, so the call must be followed by a `return` or replaced by returning an error from `main`.
8. **Enums have no text form of their own.** Printing an enum needs a hand-written `string()` with a full switch (`check.Outcome`, `machine.VendState`). Fine for cases with data, tedious for plain enums like `Direction`. **Fixed:** every enum has a built-in `name()`, the name of its case (`Direction.North.name()` is `"North"`).
9. **Formatting is checked before types.** An unformatted file reports only `not formatted`; the real error (in my case `len(xs)`) appears after running `seed fmt`. Together with `seed fmt` taking files only, a project's formatting command is `seed fmt *.sd */*.sd */tests/*.sd`. Accepting directories in `seed fmt` (as `seed test` does) would be in keeping with "one binary does everything". **Fixed** for the first part (tooling, not design): an unformatted file is still an error, but the program is checked anyway, and the other errors are reported first, followed by the formatting error (test in `tests/test_cmd.sh`). **Fixed** for the second part too: `seed fmt DIR` (and `--check DIR`) formats every `.sd` file under `DIR`.
10. **Building JSON is verbose.** Each field is `"id": json.Value.Number(float(it.id))` inside `json.Value.Object(map[string]json.Value{...})`. Without reflection or generics this is expected; small helpers in the program keep it manageable.
11. **`seed test` has no filter and no verbose mode.** With tests running in goroutines and a summary line only, there is no `-run` equivalent for iterating on one failing test. **Fixed:** `seed test --run text` and `-v` (doc gaps, 7).
