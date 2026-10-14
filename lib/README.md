# The Seed standard library

This directory holds the standard library ([docs/stdlib.md](../docs/stdlib.md)):
one directory per package, imported by its name (`import "json"`). The
compiler finds it next to its own binary (`<root>/lib`, or
`$SEED_ROOT/lib`) and looks there before the project's own packages.

- `fmt`, `os`, `fs`, `time`, `math`, `rand` and `crypto` have native
  functions (declared without a body) written in C, in `<pkg>/<pkg>.c`,
  some on top of the runtime (`runtime/seed_pkg.c`, `seed_task.c`). How to
  write a native function is in
  [runtime/README.md](../runtime/README.md#13-native-functions).
- `errors`, `strings`, `path`, `encoding`, `json` and `http` are plain Seed;
  `time`, `math` and `rand` are partly Seed.
- `io` and `net` (and `http.serve`, `get` and `send`, which need `net`) are
  still to be written.

## Layout

```
lib/
  <pkg>/<pkg>.sd              the package (package <pkg>, pub API as in docs/stdlib.md)
  <pkg>/<pkg>.c               its native functions, if any (sd_lib_<pkg>_<name>)
  <pkg>/tests/<pkg>_test.sd   its tests (package <pkg>: test blocks)
  http/wire.sd                http's second file: message reader and writer
  COMPILER_BUGS.md            compiler issues found, with reproductions
```

## Running the tests

```
make build/seed2
build/seed2 test lib          # every package (or: make test-lib)
build/seed2 test lib/json     # one package
```

Each package's tests are `test "description" { ... }` blocks in
`<pkg>/tests/*.sd`, which belong to the package (they can use private
names). `seed test` runs each test in a goroutine of its own and prints one
line per failure and a summary. Every file must be formatted (the compiler
checks it).

### Test conventions

- A check is `assert(cond)`; a comparison shows both values when it fails
  (`assert(got == want)`). An expected error is
  `assert(err != null)` followed by `assert(err.message() == "...")`;
  after `assert(err == null)` the values returned with `err` can be used.
- A crash test runs the code in a group and checks the crash message:

  ```
  fn crashRepeatNegative() {
      fmt.println(repeat("x", -1))
  }

  test "repeat negative crashes" {
      error err = group {
          spawn crashRepeatNegative()
      }
      assert(err != null)
      assert(err.message() == "crash: strings.repeat: count (-1) must not be negative")
  }
  ```

- Tests that need files work in `$TMPDIR` and remove what they made.
- Helpers in test files still carry the package's name as a prefix
  (`jsonTestError`, `httpTestParse`), from the time when the tests were
  merged into one namespace; that is no longer necessary.

### Conventions in the packages

- **Crashes.** A caller's mistake is reported with
  `os.crash("<pkg>.<fn>: <what is wrong>")` (docs/stdlib.md, "Errors and
  crashes"), which crashes the goroutine like a runtime fault: inside a
  group it fails the group.
- **Private names** are still prefixed in places (`jsonUtf8Len`,
  `httpIsToken`), from the same merged-namespace time.
- **Struct fields** are `pub` where docs/stdlib.md makes them public
  (`http.Request`, `http.Response`); `rand.Source.state` is private.

## What is implemented

| Package | Implemented | Still to do (natives, or blocked) |
|---|---|---|
| `errors` | everything: `new`, `wrap` | — |
| `fmt` | everything: `print`, `println`, `eprint`, `eprintln`, `fixed` (natives) | — |
| `os` | everything: `exit`, `crash`, `env`, `run`, `output`, `executable`, `cwd`, `hostname` (natives) | `run` and `output` are not stop points yet (runtime hooks) |
| `fs` | everything: `read`, `write`, `append`, `exists`, `isDir`, `stat`, `list`, `mkdir`, `rename`, `remove`, `removeAll`, `makeTemp`, `open`, `create`, `File` with `readLine`, `read`, `write`, `close` (natives) | — |
| `time` | everything: the duration constants, `now`, `monotonic`, `after`, `sleep` (natives); `Date`, `date`, `fromDate`, `format`, `parse` (Seed) | — |
| `strings` | everything: `repeat`, `padLeft`, `padRight`, `lastIndex`, `fields`, `isDigit`, `isLetter`, `isSpace` | — |
| `path` | everything: `join`, `clean`, `base`, `dir`, `ext` | — |
| `math` | everything: `pi`, `e`, `absInt`, `minInt`, `maxInt`, `powInt`, `absFloat`, `minFloat`, `maxFloat`, `isNan`, `isInf`, `inf` (Seed); `powFloat`, `sqrt`, `floor`, `ceil`, `round`, `trunc`, `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`, `exp`, `log`, `log2`, `log10` (natives over libm) | — |
| `encoding` | everything: hex, base64 (standard and URL), `urlEncode`, `urlDecode` | — |
| `rand` | everything: `seeded`, `Source` (SplitMix64) with `range`, `fraction`, `permutation`; the package functions `range`, `fraction`, `permutation` on the private native `nextOs` | — |
| `crypto` | everything: `sha256`, `hmacSha256`, `randomBytes`, `equal` (natives) | — |
| `json` | everything: `Value`, `parse`, `string`, `pretty`, all accessors | — |
| `http` | `Request`, `Response`, `Handler`, `path`, `query`, `statusText`, the status constants, `textResponse`, `jsonResponse`; the message reader and writer for `serve`/`send` (wire.sd) | `serve`, `get`, `send` need `net` (written in Seed on top of it, not native) |

### fmt

`fixed(f, places)` is `snprintf("%.*f")`, which macOS and glibc round
correctly from the exact binary value (ties to even: `fixed(2.5, 0)` is
`"2"`, `fixed(0.125, 2)` is `"0.12"`); `nan` (of either sign), `inf` and
`-inf` are spelled out. Crash: `fmt.fixed: places (101) must be between 0
and 100`.

### os

- `output` is `run` with the child's stdout on a pipe (both ends
  close-on-exec, so concurrent children don't inherit them); the parent
  reads until end of file, then waits. Errors: `os.output: <program>:
  <reason>` when it cannot start (or reading or waiting fails); crash
  `os.output: argv is empty`.
- `hostname` is `gethostname`; error `os.hostname: <reason>`.
- `run` and `output` block their goroutine's thread but are not stop
  points yet (that needs the runtime's blocking hooks).

### fs

- `write`, `append`, `create` use mode `0644` and `mkdir` `0755` (before
  the umask), as docs/stdlib.md says; `write` and `mkdir` are therefore in
  fs.c rather than calling the runtime's versions, with the same messages
  as seed0 (`fs.mkdir("")` is `fs.mkdir: : not a directory`).
- `append` holds a process-wide mutex around its `write` loop and opens
  with `O_APPEND`, so appends from goroutines never interleave (tested with
  5000-byte lines from two goroutines).
- `stat`: `size` is `st_size`; `modified` is `st_mtim` (`st_mtimespec` on
  macOS) in nanoseconds.
- `rename`'s error names both paths: `fs.rename: a -> b: <reason>`.
- `removeAll` uses `lstat`, so a symbolic link is unlinked and never
  followed. It reads all names of a directory before removing them. An
  error names the entry that failed: `fs.removeAll: dir/x/f: permission
  denied`. `removeAll("")` does nothing.
- `makeTemp` is `mkdtemp("$TMPDIR/<prefix>XXXXXX")` (trailing slashes of
  `$TMPDIR` removed; `/tmp` when it is unset or empty).
- **File handles.** docs/stdlib.md puts the handle table in the runtime,
  shared with `net`; until the runtime has one, fs.c keeps its own. A
  `File` holds the entry's id (index + 1) and the path (for messages).
  Ids are never reused and entries are never freed (closing frees the
  descriptor and buffer, about 100 bytes stay), so a stale copy finds its
  own closed entry. A table mutex guards lookups and growth; each entry
  has a mutex that makes every operation atomic (a handle sent to two
  goroutines writes whole `write` calls). Descriptors are close-on-exec.
- `open` fails on a directory with `is a directory`. `readLine` and `read`
  share a buffer that is refilled 64 KiB at a time; `read` returns what is
  buffered, or does one `read(2)`. End of file is not remembered, so a file
  that grows can be read further. Messages: `fs.File.<method>: <path>:
  file is open for reading only` / `writing only` / `file is closed`; crash
  `fs.File.read: max (0) must be at least 1`.
- File I/O is not a stop point (regular files don't block for long).

### time

- `now` is `CLOCK_REALTIME`; `monotonic` is the runtime's clock
  (`CLOCK_MONOTONIC`, the one its timers use).
- `Date`'s fields are `pub` (docs/stdlib.md now says so). `date`,
  `fromDate`, `format` and `parse` are Seed, with Howard Hinnant's
  days-from-civil formulas and floor division, so they work for any `int`
  (years before 0 and beyond 9999 included; tested against Python's
  `calendar.timegm` and with 2000 random round trips over ±3000 years).
- `fromDate` checks month, day (leap years included), hour, minute, second
  and nanosecond, in that order: `time.fromDate: 2026-02-29T00:00:00Z: day
  out of range`. The subject shows the fields as given.
- `format` crashes outside years 0–9999: `time.format: year (10000) must be
  between 0 and 9999`.
- `parse` is strict RFC 3339 as docs/stdlib.md describes. Errors: the
  range reasons above, or `expected a digit`, `expected '-'`, `expected
  'T'`, `expected ':'`, `expected 'Z' or an offset like +02:00`, `expected a
  digit after '.'`, `more than 9 fraction digits`, `offset out of range`,
  `unexpected text`, each followed by ` at offset N`. A leap second `:60`
  is `second out of range`; offsets must be at most 23:59.

### strings

- `padLeft`/`padRight` count characters as `s.chars()` does (an invalid UTF-8
  byte is one character); each `fill` byte counts as one character.
- `repeat` doubles the piece, so it does O(log count) concatenations.

### path

Go's `path` semantics; the rules and examples are at the top of
`path/path.sd`. Two edge cases docs/stdlib.md does not spell out:
`dir("a/b/")` is `"a/b"` (as in Go: the last element is the empty one after
the slash), and `ext` gives `""` for the bases `"."` and `".."` and `"."` for
`"a."`. `join([]string{"a", "/b"})` is `"a/b"`.

### math

`inf()` is written in Seed (`1.0 / 0.0`, IEEE); docs/stdlib.md now says so.
`minFloat`/`maxFloat` treat `-0.0` as smaller than `0.0` and return `nan`
if either argument is `nan`. `absFloat(-0.0)` is `0.0`. The other float
functions are one-line natives over the C library (math.c), so they give
exactly what C99 gives: `round` is C `round` (halves away from zero),
`sqrt(-1.0)` is `nan`, `log(0.0)` is `-inf`, `atan2(-0.0, -1.0)` is `-pi`.

The exponent parameter of `powInt` and `powFloat` is `exponent`, not `exp`
as docs/stdlib.md first had it: a parameter may not shadow a package-level
name, and `exp` is now a function. The crash reads `math.powInt: exponent
(-1) must not be negative`.

### encoding

- Errors: `encoding.<fn>: offset N: <reason>`, where a byte is shown as
  `'g'` when it is printable ASCII and as `byte 0xff` otherwise. Reasons:
  `invalid hex digit 'g'`, `odd number of hex digits` (offset of the last,
  unpaired digit), `invalid base64 character '-'`, `incorrect padding`,
  `input ends in the middle of a byte`, `'%' must be followed by two hex
  digits`.
- Base64 decoding with padding requires exactly the padding the last group
  needs (`Zg=` is an error). Unused bits in the last character are ignored,
  as Go's decoders do by default (`Zh==` decodes like `Zg==`).
- `urlDecode` keeps every byte other than `%XX` and `+` as it is.

### rand

The algorithm, fixed by docs/stdlib.md and made precise here (also at the
top of `rand/rand.sd`): SplitMix64 for raw output; `range` draws the
smallest number k >= 1 of 64-bit outputs with 2^(64k) >= n, joins them with
the first one most significant, and rejects values at or above the largest
multiple of n below 2^(64k); `fraction` is the top 53 bits times 2^-53;
`permutation` is Fisher-Yates from the end. The outputs for seeds 0 and
1234567 match the reference `splitmix64.c`.

64-bit wrapping arithmetic is emulated with `& 0xffffffffffffffff`, and the
products are ~128-bit big integers, so each step allocates. Measured on an
Apple-silicon Mac (`-O2`): **about 0.5 µs per 64-bit output (2 million per
second)**, and about 0.6 µs per `range(0, 100)`. A native `next` would be
much faster; it can replace the Seed one without changing any output.

The package functions `rand.range`, `rand.fraction` and `rand.permutation`
draw from a per-goroutine generator: the native `nextOs()` (rand.c) is a
thread-local SplitMix64 state (each goroutine is a thread), seeded with
`getentropy` (or `/dev/urandom`) on the goroutine's first use. `Source` has
a private `bool fromOs` (false from `seeded`); when it is set, `next`
returns `nextOs()`, and each package function is
`Source{state: 0, fromOs: true}` followed by the method call, so the
algorithms exist once. Crash messages name the function that was called:
`rand.range: ...` for the package function, `rand.Source.range: ...` for
the method.

### crypto

Everything is native (crypto.c), as docs/stdlib.md says. `sha256` and
`hmacSha256` were first written in Seed (about 12 MiB/s); the C versions
pass the same vectors in `tests/crypto_test.sd` (FIPS 180-2 examples,
padding-boundary lengths 55, 56, 63, 64, 65, 119, 120 and 1000, the
one-million-`a` vector, and RFC 4231 cases 1, 2, 3, 4, 6 and 7).

- `randomBytes(n)` uses `getentropy` in 256-byte pieces, falling back to
  `/dev/urandom`. Crashes: `crypto.randomBytes: n (-1) must not be
  negative`, and `crypto.randomBytes: the OS provided no randomness:
  <reason>`.
- `equal` returns `false` at once when the lengths differ; otherwise it ORs
  the XOR of every pair of bytes through a `volatile` accumulator, so the
  time depends only on the length.

### json

- Parse errors: `json.parse: line L, column C: <reason>` (docs/stdlib.md's
  format; the column counts bytes). The reasons are listed at the top of
  `json/json.sd` and covered by tests, including hints for comments, single
  quotes and the byte-order mark.
- Strictly RFC 8259: no leading zeros, `+`, `.5`, `1.`, `NaN`, trailing
  commas, comments or BOM; control characters must be escaped; strings must
  be valid UTF-8 (overlong forms, surrogates and code points above
  U+10FFFF are rejected). `\uXXXX` is decoded with surrogate pairs; a lone
  surrogate becomes U+FFFD.
- Nesting deeper than 512 arrays and objects is an error; 512 is accepted.
- A number too large for a float (`1e400`) is an error, `number out of
  range`; one too small (`1e-400`) becomes `0`.
- Printing: whole numbers below 1e21 print as integers (`-0` for negative
  zero), others as `f.string()`; control characters are escaped as `\n`,
  `\t`, `\r`, `\b`, `\f` or `\u00xx` (lowercase hex); each byte of invalid
  UTF-8 becomes one U+FFFD; DEL is written as it is. Printing `nan` or an
  infinity crashes: `json.Value.string: nan cannot be written in JSON`.
- json uses only `errors` (plus `os` for `os.crash`), not `strings` and
  `math` as the dependency table in docs/stdlib.md allows.
- Measured: parsing and printing a 3.8 MB document takes about 0.15 s
  (~25 MB/s for parse plus print).

### http

`http.sd` has the public API. `wire.sd` has what `serve` and `send` will
use, all private and free of I/O:

- `MessageReader`: fed bytes as they arrive (`feed`), it returns
  `nextRequest()` → `NeedMore | Done(Request) | Reject(status, reason)` and
  `nextResponse(method, eof)` → `NeedMore | Done(Response) | Fail(reason)`.
  It is resumable, keeps bytes after a message for the next one
  (keep-alive, pipelining), and copies each byte a bounded number of times,
  so a large body arriving in small reads is linear (a 1 MB body in 4 KiB
  pieces is tested). The reading rules (line endings, header validation,
  framing, limits) are at the top of `wire.sd`.
- `responseBytes(method, resp, close)`, `errorResponseBytes(status)`,
  `parseUrl(fnName, url)` → `HttpTarget{address, host, target}`, and
  `requestBytes(fnName, req, target)`.

Decisions not fixed by docs/stdlib.md:

- `Request.path()` decodes `%XX` but keeps `+` (a `+` is a space only in
  queries). Empty query parameters (`a=1&&b=2`) are skipped.
- An HTTP/1.1 request without `host` is rejected with 400 (RFC 9112). Other
  rejects: 400 malformed, 413 body over 8 MiB, 431 headers over 64 KiB, 501
  a transfer-encoding other than chunked, 505 an HTTP version other than 1.x.
  A message with both transfer-encoding and content-length is rejected.
- Obsolete header folding is rejected. Header values have spaces and tabs
  trimmed and may not contain other control characters.
- `transfer-encoding` stays in the headers after the chunked body is decoded.
  Chunk extensions and trailers are ignored.
- Writing: header names are lowercased and invalid ones skipped; CR and LF
  in values become spaces (no response splitting); the server writes
  `content-length` itself and drops `content-length`, `transfer-encoding`
  and `connection` set by the handler; 1xx, 204 and 304 responses get no
  `content-length` and no body; an unknown status gets an empty reason
  phrase. The client keeps a `host` header the program set.
- Responses: 1xx interim responses are skipped; HEAD, 204 and 304 have no
  body; with neither length nor chunking, the body runs until the
  connection closes.
- No `date` header is written yet (it needs `time`).

## Inconsistencies found in the docs

- docs/stdlib.md says two `json.Value`s compare with key order counting, "as
  it does for maps", but DECISIONS.md 30 (and the runtime) make map `==`
  ignore insertion order. `Value`'s `==` is the built-in enum `==`, so key
  order is ignored; the json tests check that.
- The task brief asked for JSON errors with a byte offset; docs/stdlib.md
  specifies line and column, which is what the parser reports.
