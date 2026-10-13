# The Seed standard library

This directory holds the standard library ([docs/stdlib.md](../docs/stdlib.md)):
one directory per package, imported by its name (`import "json"`). The
compiler finds it next to its own binary (`<root>/lib`, or
`$SEED_ROOT/lib`) and looks there before the project's own packages.

- The core packages `fmt`, `os`, `fs` and `time` declare their functions
  without a body (native functions) and implement them in C, in
  `<pkg>/<pkg>.c`, on top of the runtime (`runtime/seed_pkg.c`,
  `seed_task.c`). `errors` is plain Seed. They provide what
  [docs/builtins.md](../docs/builtins.md#core-packages) lists, plus
  `errors.wrap`, `os.crash`, `os.executable` and `os.cwd`.
- `strings`, `path`, `math`, `encoding`, `rand`, `crypto`, `json` and `http`
  are written in Seed (no natives yet).
- `io`, `net`, the rest of `os`, `fs`, `time`, `math`, `rand` and `crypto`
  (the natives listed below and in docs/stdlib.md) are still to be written.
  How to write a native function is in
  [runtime/README.md](../runtime/README.md#13-native-functions).

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
| `fmt` | `print`, `println`, `eprint`, `eprintln` (natives) | `fixed` |
| `os` | `exit`, `crash`, `env`, `run`, `executable`, `cwd` (natives) | `output`, `hostname` |
| `fs` | `read`, `write`, `exists`, `isDir`, `list`, `mkdir`, `remove` (natives) | `append`, `stat`, `rename`, `removeAll`, `makeTemp`, `open`, `create`, `File` |
| `time` | the duration constants, `after`, `sleep` (natives) | `now`, `monotonic`, `date`, `fromDate`, `format`, `parse` |
| `strings` | everything: `repeat`, `padLeft`, `padRight`, `lastIndex`, `fields`, `isDigit`, `isLetter`, `isSpace` | — |
| `path` | everything: `join`, `clean`, `base`, `dir`, `ext` | — |
| `math` | `pi`, `e`, `absInt`, `minInt`, `maxInt`, `powInt`, `absFloat`, `minFloat`, `maxFloat`, `isNan`, `isInf`, `inf` | `powFloat`, `sqrt`, `floor`, `ceil`, `round`, `trunc`, `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`, `exp`, `log`, `log2`, `log10` |
| `encoding` | everything: hex, base64 (standard and URL), `urlEncode`, `urlDecode` | — |
| `rand` | `seeded`, `Source` (SplitMix64) with `range`, `fraction`, `permutation` | `nextOs` (private native); then the package functions `range`, `fraction`, `permutation` |
| `crypto` | `sha256`, `hmacSha256` (docs/stdlib.md makes them native; see below) | `randomBytes`, `equal` |
| `json` | everything: `Value`, `parse`, `string`, `pretty`, all accessors | — |
| `http` | `Request`, `Response`, `Handler`, `path`, `query`, `statusText`, the status constants, `textResponse`, `jsonResponse`; the message reader and writer for `serve`/`send` (wire.sd) | `serve`, `get`, `send` need `net` (written in Seed on top of it, not native) |

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

`inf()` is written in Seed (`1.0 / 0.0`, IEEE) although docs/stdlib.md lists
it as native. `minFloat`/`maxFloat` treat `-0.0` as smaller than `0.0` and
return `nan` if either argument is `nan`. `absFloat(-0.0)` is `0.0`.

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

Not done: the package functions `rand.range`, `rand.fraction` and
`rand.permutation`, which draw from a per-goroutine generator seeded by the
OS. They need the native `nextOs() int`. The intended shape: give `Source` a
private `bool os` field (always false from `seeded`), make `next` return
`nextOs()` when it is set, and write each package function as
`Source s = Source{state: 0, os: true}` followed by the method call, so the
algorithms exist once.

### crypto

docs/stdlib.md makes `sha256` and `hmacSha256` native because "64-bit
wrapping arithmetic is slow with growing ints". SHA-256 only needs 32-bit
words, and the Seed version keeps every intermediate below 2^62 (a rotation
masks before shifting left), so nothing allocates a big integer. Measured:
**about 80 ms per MiB (~12 MiB/s)**; the one-million-`a` FIPS vector takes
about 0.08 s and is part of the tests. That is fast enough to keep these in
Seed; if they become native, `tests/crypto_test.sd` holds the vectors they
must pass (FIPS 180-2 examples, padding-boundary lengths 55, 56, 63, 64, 65,
119, 120 and 1000, and RFC 4231 cases 1, 2, 3, 4, 6 and 7).

`randomBytes` (OS randomness) and `equal` (constant time, kept native so
that no optimiser can make it exit early) are left native, as the doc says.

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
