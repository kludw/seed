# Standard library

This is part of the language spec ([SPEC.md](../SPEC.md)). It defines every package in the Seed standard library: the types, the function signatures, and the exact behaviour, including errors and crashes. Built-in methods on `int`, `string`, lists, maps and so on are in [builtins.md](builtins.md) and are not repeated here.

Everything in this document is **(proposed)**. It was designed in one pass and has not been agreed yet. Changes it would need in the language itself are listed in [Spec changes this design relies on](#spec-changes-this-design-relies-on).

| Package | Purpose |
|---|---|
| [`errors`](#errors) | Making and wrapping errors. |
| [`fmt`](#fmt) | Writing text to stdout and stderr. |
| [`io`](#io) | Reading stdin. |
| [`os`](#os) | The process and its environment: exit, environment variables, child processes. |
| [`fs`](#fs) | Files and directories. |
| [`path`](#path) | Slash-separated paths as text. |
| [`strings`](#strings) | Text helpers that are not built-in methods. |
| [`math`](#math) | Numeric functions and constants. |
| [`rand`](#rand) | Pseudo-random numbers, seeded or not. |
| [`time`](#time) | Clocks, durations, sleeping, timeouts, RFC 3339 dates. |
| [`encoding`](#encoding) | Hex, base64 and URL encoding. |
| [`json`](#json) | Parsing and producing JSON through a dynamic `Value`. |
| [`crypto`](#crypto) | SHA-256, HMAC, secure random bytes, constant-time comparison. |
| [`net`](#net) | TCP connections. |
| [`http`](#http) | An HTTP/1.1 server and client. |

There is no testing package: tests use the built-in `test` block and `assert` (see [Tests](#tests)).

## Conventions

These rules hold across the whole library. A new function should follow them, and a reader who knows them can guess most signatures.

### Naming

- Functions, methods and constants are `camelCase`; types and enum cases are `PascalCase`, as the compiler requires.
- A name never repeats its package: `fs.read`, not `fs.readFile`; `json.parse`, not `json.parseJson`.
- **Int and float variants.** When a function exists for both `int` and `float`, *both* names carry the type as a suffix: `math.minInt` and `math.minFloat`, `math.powInt` and `math.powFloat`. There is no unsuffixed version. A function that only makes sense for one type has no suffix: `math.sqrt`.
- **Encode/decode pairs** are named `xEncode` and `xDecode`: `encoding.hexEncode`, `encoding.hexDecode`.
- **Text form.** A type with a text form has a `string()` method (`json.Value.string()`), following the language rule for built-in types. Turning text into a value is a package function called `parse` that returns `(T, error)`: `json.parse`, `time.parse`.
- **Handles** (open files, connections, listeners) are struct types named after what they are: `fs.File`, `net.Conn`, `net.Listener`. Their `close()` method returns nothing, so `defer f.close()` works.

### Types used for common things

| Thing | Type | Notes |
|---|---|---|
| Text and raw data | `string` | A string is a sequence of bytes. Only `s.chars()` assumes UTF-8. Raw data from files, sockets and hashes is a `string` too, so nothing converts at package boundaries. `[]byte` is used only for building or changing data one byte at a time. |
| A duration | `int` | Nanoseconds. Write `5 * time.second`. |
| A point in time | `int` | Nanoseconds since 1970-01-01T00:00:00Z (Unix time), always UTC. |
| A range of numbers | `int lo, int hi` | Half-open, `lo` up to but not including `hi`, the same as `slice(a, b)`. |
| "Nothing there" | `T?` | End of input, a missing key, a missing variable. These are normal outcomes, not errors. |
| A failure | `error` | Something went wrong outside the program: I/O failed, the input was malformed, the network was down. |

### Errors and crashes

The library draws one line between errors and crashes:

- **An error** is returned when the cause is outside the program's control: a missing file, a refused connection, malformed JSON, a bad number typed by a user.
- **A crash** happens when the caller broke a rule it could have checked: a negative count, `lo >= hi`, an invalid argument that is a programming mistake. The crash message starts with the function name: `crash: rand.range: lo (5) must be less than hi (5)`.
- **A normal "nothing there" outcome** is neither. It is a `null` in a nullable result: `io.readLine()` returns `null` at end of input, `os.env` returns `null` for an unset variable.

### Error messages

Every error from the standard library has a message in this form:

```
<package>.<function>: <subject>: <reason>
```

- `<package>.<function>` is the function that failed, or `<package>.<Type>.<method>` for a method: `fs.read`, `net.Conn.read`.
- `<subject>` is what the call was about, usually a path, an address or a position. It is left out when there is nothing useful to say.
- `<reason>` is lowercase, with no final full stop. For OS failures it is the system's description (`strerror` on POSIX).

```
fs.read: notes.txt: no such file or directory
fs.mkdir: /etc/seed: permission denied
net.dial: example.com:81: connection refused
json.parse: line 3, column 9: expected ',' or '}'
http.get: https://example.com/: https is not supported
time.parse: 2026-13-01T00:00:00Z: month out of range
```

Programs add their own context with `errors.wrap`, which produces the same `context: inner` shape:

```
loading config: fs.read: seed.json: no such file or directory
```

The standard library returns plain errors and defines no error types. A program that needs to react to a specific condition checks for it directly (`fs.exists`, a nullable result, `resp.status`) instead of inspecting an error.

## errors

Making errors. `error` itself is a built-in interface with one method, `message() string`.

| Function | Result |
|---|---|
| `errors.new(string message) error` | An error whose `message()` is `message`. |
| `errors.wrap(error err, string context) error` | An error whose `message()` is `context + ": " + err.message()`. Crashes if `err` is `null`. |

`errors.wrap` is how context is added when an error is passed up. It keeps only the message. To report a condition the caller should branch on, return a nullable value or a status instead of a special error.

```
fn loadConfig(string file) (json.Value, error) {
    string text, error err = fs.read(file)
    if (err != null) {
        return null, errors.wrap(err, "loading config")
    }
    return json.parse(text)
}
```

## fmt

Writing text to stdout and stderr. There is no format-string language: values become text through `.string()` and are joined with `+`.

| Function | Result |
|---|---|
| `fmt.print(string s)` | Writes `s` to stdout. |
| `fmt.println(string s)` | Writes `s` and `"\n"` to stdout. |
| `fmt.eprint(string s)` | Writes `s` to stderr. |
| `fmt.eprintln(string s)` | Writes `s` and `"\n"` to stderr. |
| `fmt.fixed(float f, int places) string` | `f` with exactly `places` digits after the point, correctly rounded from the exact binary value: `fmt.fixed(3.14159, 2)` is `"3.14"`, `fmt.fixed(2.5, 0)` is `"2"`, `fmt.fixed(-0.001, 2)` is `"-0.00"`. `nan`, `inf` and `-inf` come out as those words. Crashes if `places` is negative or more than 100. |

- Each call is a single write. Output from two goroutines can interleave between calls, but never inside one.
- Bytes are written as they are, without checking UTF-8.
- Stdout is buffered. The buffer is flushed when it fills, after every call while stdout is a terminal, before `io` reads stdin, before `os.run` and `os.output` start a child, and when the program exits for any reason, including `os.exit` and crashes. Stderr is not buffered.
- Write failures (a closed pipe, a full disk) are ignored. A program that must know whether its output arrived should write to a file with `fs`.

`fmt.fixed` is here, rather than in `strings` or `math`, because it is the one place where a number needs a format other than `.string()`: prices, percentages and timings.

```
float ratio = float(hits) / float(total)
fmt.println("hit rate: " + fmt.fixed(ratio * 100.0, 1) + "%")
```

## io

Reading the program's standard input. Stdout and stderr are written with `fmt`.

| Function | Result |
|---|---|
| `io.readLine() (string?, error)` | The next line of stdin without its line ending, or `null` at end of input. |
| `io.readAll() (string, error)` | Everything left on stdin, up to end of input. |

- A line ends at `"\n"`. A `"\r"` directly before it is removed too. The last line is returned even if it has no `"\n"`. An empty input gives `null` on the first call.
- `io.readAll()` returns everything not yet returned by `io.readLine()`, so the two can be mixed.
- Stdin is shared by the whole process. Calls from different goroutines are serialised, and each call gets whole lines.
- The error is set only when reading fails (for example, stdin was closed by the OS). Reaching the end is not an error.

```
for (;;) {
    string? line = try io.readLine()
    if (line == null) {
        break
    }
    fmt.println(line.toUpper())
}
```

## os

The running process and its environment. A program's arguments are not here: `main` receives them as `[]string args`.

| Function | Result |
|---|---|
| `os.exit(int code)` | Exits immediately with `code`, after flushing stdout. `defer`s do not run. Crashes unless `0 <= code <= 255`. |
| `os.env(string name) string?` | The environment variable `name`, or `null` if it is not set. A variable set to `""` returns `""`. |
| `os.run([]string argv) (int, error)` | Runs a program and waits for it. See below. |
| `os.output([]string argv) (string, int, error)` | Like `os.run`, but captures the child's stdout and returns it with the exit code. |
| `os.cwd() (string, error)` | The absolute path of the current directory. |
| `os.hostname() (string, error)` | The machine's host name. |
| `os.executable() string` | The absolute path of the running program, with symbolic links resolved, or `""` if it cannot be found. |
| `os.crash(string message)` | Crashes the current goroutine exactly like a runtime fault, with `crash: <message>`. This is how library code reports a caller's mistake (see [Errors and crashes](#errors-and-crashes)). |

`os.run` and `os.output`:

- `argv[0]` is looked up on `PATH` unless it contains a `/`. Crashes if `argv` is empty.
- The child inherits the environment, the current directory, stdin and stderr. With `os.run` it also inherits stdout.
- The exit code is the child's exit code. A child killed by a signal reports `128 + signal`, as shells do.
- The error is set only when the child could not be started: `os.run: frobnicate: no such file or directory`. A child that runs and fails is not an error; check the exit code.
- They are stop points: if the calling goroutine's group is stopped while a child runs, the child is killed (`SIGKILL`) and waited for.

There is deliberately no `os.setEnv` and no `os.chdir`. The environment and the current directory are shared by every goroutine, so changing them would be shared mutable state. Pass values as arguments instead.

```
string rev, int code, error err = os.output([]string{"git", "rev-parse", "HEAD"})
if (err != null) {
    return err
}
if (code != 0) {
    return errors.new("git rev-parse failed with exit code " + code.string())
}
fmt.println("building " + rev.trim())
```

## fs

Files and directories. Most programs only need the whole-file functions; `fs.File` is for files too large to read at once and for writing a file piece by piece.

### Types

```
pub type Info struct {
    int size        // in bytes; for a directory, whatever the OS reports
    bool isDir
    int modified    // last modification, Unix nanoseconds
}

pub type File struct {
    // opaque: a handle, see "Handles" under net
}
```

### Functions

| Function | Result |
|---|---|
| `fs.read(string path) (string, error)` | The whole file. Fails on a directory with `is a directory`. |
| `fs.write(string path, string data) error` | Creates the file or replaces its contents. New files get mode `0644`. |
| `fs.append(string path, string data) error` | Adds `data` to the end of the file, creating it if needed. On a local file system, one `fs.append` call is never interleaved with another. |
| `fs.exists(string path) bool` | Whether anything exists at `path`. Follows symbolic links, so a broken link does not exist. |
| `fs.isDir(string path) bool` | Whether `path` is a directory, following symbolic links. `false` if nothing is there. |
| `fs.stat(string path) (Info, error)` | Size, kind and modification time, following symbolic links. |
| `fs.list(string dir) ([]string, error)` | Entry names (not paths), sorted by bytes, without `.` and `..`. |
| `fs.mkdir(string path) error` | Creates the directory and any missing parents, with mode `0755`. Succeeds if it already exists as a directory. |
| `fs.rename(string from, string to) error` | Moves or renames, replacing `to` if it is a file. Both must be on the same file system. |
| `fs.remove(string path) error` | Removes a file or an empty directory. Fails if nothing is there. |
| `fs.removeAll(string path) error` | Removes `path` and everything inside it. Does not follow symbolic links: a link is removed, not what it points to. Succeeds if nothing is there. |
| `fs.makeTemp(string prefix) (string, error)` | Creates a new, empty directory in `$TMPDIR` (or `/tmp` when it is unset), named `prefix` followed by random characters, and returns its path. The caller removes it with `fs.removeAll`. |
| `fs.open(string path) (File, error)` | Opens an existing file for reading. |
| `fs.create(string path) (File, error)` | Creates a file, or empties an existing one, and opens it for writing. New files get mode `0644`. |

### File methods

| Method | Result |
|---|---|
| `f.readLine() (string?, error)` | The next line without its line ending, or `null` at the end. Same rules as `io.readLine`. |
| `f.read(int max) (string?, error)` | Between 1 and `max` bytes, or `null` at the end. Crashes if `max < 1`. |
| `f.write(string data) error` | Writes all of `data`. Writes are not buffered, so once `write` returns, the data is with the OS. |
| `f.close()` | Closes the file. Closing again does nothing. |

- Reading a file opened with `fs.create`, or writing one opened with `fs.open`, returns an error: `fs.File.write: notes.txt: file is open for reading only`.
- `readLine` and `read` share a buffer, so they can be mixed.
- Every operation after `close()` returns an error ending in `file is closed`.
- Paths are passed to the OS unchanged. Relative paths are relative to the current directory.

```
fs.File f = try fs.open("access.log")
defer f.close()
int errorsSeen = 0
for (;;) {
    string? line = try f.readLine()
    if (line == null) {
        break
    }
    if (line.contains(" 500 ")) {
        errorsSeen++
    }
}
```

## path

Slash-separated paths as text. These functions never touch the file system. Only `/` is a separator; Seed v1 targets POSIX systems.

| Function | Result |
|---|---|
| `path.join([]string parts) string` | The parts joined with `/` and then cleaned. Empty parts are skipped. `path.join([]string{"a", "b/", "../c"})` is `"a/c"`. An empty list, or only empty parts, gives `""`. |
| `path.clean(string p) string` | The shortest equivalent path: repeated `/` become one, `.` elements are removed, `x/..` pairs are removed, `..` at the start of an absolute path is removed, and a trailing `/` is removed. `""` becomes `"."`. Same rules as Go's `path.Clean`. |
| `path.base(string p) string` | The last element, after removing trailing slashes. `path.base("a/b.txt")` is `"b.txt"`, `path.base("/")` is `"/"`, `path.base("")` is `"."`. |
| `path.dir(string p) string` | Everything but the last element, cleaned. `path.dir("a/b.txt")` is `"a"`, `path.dir("b.txt")` is `"."`, `path.dir("/b")` is `"/"`. |
| `path.ext(string p) string` | The extension of `path.base(p)` including the dot: `".txt"`. `""` when there is no dot, or when the only dot is the first character (`".bashrc"` has no extension). For `"a.tar.gz"` it is `".gz"`. |

```
string name = path.base(src)
string stem = name.slice(0, name.len() - path.ext(name).len())
string out = path.join([]string{outDir, stem + ".html"})
```

## strings

Text helpers that are not built-in methods. The built-in `string` methods (`split`, `trim`, `replace`, `index`, `toUpper`, `chars`, ...) and `[]string.join` are in [builtins.md](builtins.md), and are not repeated here.

| Function | Result |
|---|---|
| `strings.repeat(string s, int count) string` | `s` repeated `count` times. Crashes if `count` is negative. |
| `strings.padLeft(string s, int width, byte fill) string` | `s` with `fill` added in front until it is `width` characters long, counted as UTF-8 characters. Unchanged if it is already that long. `strings.padLeft("7", 3, '0')` is `"007"`. |
| `strings.padRight(string s, int width, byte fill) string` | The same, adding at the end. |
| `strings.lastIndex(string s, string sub) int?` | The byte index of the last occurrence of `sub`, or `null`. An empty `sub` gives `s.len()`. |
| `strings.fields(string s) []string` | `s` split around runs of spaces, tabs, `\n`, `\r`, `\v` and `\f`, with no empty strings. `strings.fields("  a b\n")` is `["a", "b"]`. |
| `strings.isDigit(byte b) bool` | `'0'` to `'9'`. |
| `strings.isLetter(byte b) bool` | ASCII `'a'` to `'z'` and `'A'` to `'Z'`. |
| `strings.isSpace(byte b) bool` | Space, `\t`, `\n`, `\r`, `\v` or `\f`. |

There is no string builder. Building text is done one way: push the pieces onto a `[]string` and call `join("")` once at the end. That is linear in the total size. Repeated `s = s + piece` also works and is fine for short strings.

There is no `strings.fromChars` either: `chars.join("")` does it, and `string([]byte{b})` turns one byte into a string.

```
[]string out = []string{}
for (int i = 0; i < rows.len(); i++) {
    out.push(strings.padRight(rows[i].name, 20, ' ') + rows[i].score.string())
}
fmt.println(out.join("\n"))
```

## math

Numeric functions and constants. Float functions follow IEEE 754 and C99: they return `nan` or an infinity instead of crashing, for example `math.sqrt(-1.0)` is `nan` and `math.log(0.0)` is `-inf`.

### Constants

| Constant | Value |
|---|---|
| `const float pi` | 3.141592653589793 |
| `const float e` | 2.718281828459045 |

### Integer functions

| Function | Result |
|---|---|
| `math.absInt(int x) int` | The absolute value. |
| `math.minInt(int a, int b) int` | The smaller of the two. |
| `math.maxInt(int a, int b) int` | The larger of the two. |
| `math.powInt(int base, int exp) int` | `base` to the power `exp`, exactly (integers grow). `math.powInt(0, 0)` is 1. Crashes if `exp` is negative. |

### Float functions

| Function | Result |
|---|---|
| `math.absFloat(float x) float` | The absolute value. |
| `math.minFloat(float a, float b) float` | The smaller; `nan` if either is `nan`. `-0.0` is smaller than `0.0`. |
| `math.maxFloat(float a, float b) float` | The larger; `nan` if either is `nan`. |
| `math.powFloat(float base, float exp) float` | C `pow`. |
| `math.sqrt(float x) float` | Square root. |
| `math.floor(float x) float` | Rounds down. |
| `math.ceil(float x) float` | Rounds up. |
| `math.round(float x) float` | Rounds to the nearest whole number, halves away from zero: `2.5` gives `3.0`, `-2.5` gives `-3.0`. |
| `math.trunc(float x) float` | Rounds toward zero. |
| `math.sin(float x) float`, `math.cos`, `math.tan` | Trigonometry, in radians. |
| `math.asin(float x) float`, `math.acos`, `math.atan` | Inverses, in radians. |
| `math.atan2(float y, float x) float` | The angle of the point `(x, y)`, in `-pi..pi`. |
| `math.exp(float x) float` | e to the power `x`. |
| `math.log(float x) float` | Natural logarithm. |
| `math.log2(float x) float`, `math.log10(float x) float` | Base-2 and base-10 logarithms. |
| `math.isNan(float x) bool` | Whether `x` is `nan`. |
| `math.isInf(float x) bool` | Whether `x` is `inf` or `-inf`. |
| `math.inf() float` | Positive infinity. Negative infinity is `-math.inf()`. |

- The rounding functions return `float`, because the result may be too large or not finite. Convert explicitly: `int(math.round(x))`, which crashes on `nan` and infinities as `int(f)` always does.
- There is no `math.nan()`. A program that needs to mark "no value" uses `float?`.

```
float dist = math.sqrt(dx * dx + dy * dy)
int pages = int(math.ceil(float(items) / float(perPage)))
int biggest = math.maxInt(a, b)
```

## rand

Pseudo-random numbers for simulations, games, sampling and tests. They are **not** suitable for secrets; use `crypto.randomBytes` for keys, tokens and passwords.

There are two ways to get numbers, for two different needs:

- **The package functions** draw from a generator that each goroutine gets automatically, seeded from the OS the first time it is used. Every run is different.
- **A `Source`** is a generator held in a variable and created from a seed. The same seed always gives the same sequence, on every platform and in every Seed version, which makes runs reproducible.

### Functions

| Function | Result |
|---|---|
| `rand.range(int lo, int hi) int` | A uniformly distributed int with `lo <= n < hi`. Works for any size of range. Crashes if `lo >= hi`. |
| `rand.fraction() float` | A uniformly distributed float with `0.0 <= f < 1.0`, a multiple of 2^-53. |
| `rand.permutation(int n) []int` | The numbers `0` to `n - 1` in a uniformly random order. Crashes if `n` is negative. |
| `rand.seeded(int seed) Source` | A new generator. Any `int` is a valid seed; it is reduced modulo 2^64. |

### Source

```
pub type Source struct {
    // opaque: the generator state, plain data
}
```

| Method | Result |
|---|---|
| `s.range(int lo, int hi) int` | `mut`. Same as `rand.range`. |
| `s.fraction() float` | `mut`. Same as `rand.fraction`. |
| `s.permutation(int n) []int` | `mut`. Same as `rand.permutation`. |

- A `Source` is a plain value. Copying it copies the state, so the copy produces the same numbers as the original from that point on. That is useful for replaying a sequence, and it means two goroutines given the same `Source` get the same numbers.
- The algorithm is fixed by this spec so that sequences never change: SplitMix64 for the raw 64-bit output; `range` uses rejection sampling on as many 64-bit outputs as the range needs; `fraction` uses the top 53 bits of one output; `permutation` is a Fisher–Yates shuffle drawing `range(0, i + 1)` for `i` from `n - 1` down to `1`.
- Without generics there is no `shuffle([]T)`. `rand.permutation` gives a random order for a list of any type:

```
[]int order = rand.permutation(cards.len())
[]Card shuffled = []Card{}
for (int i = 0; i < order.len(); i++) {
    shuffled.push(cards[order[i]])
}

rand.Source dice = rand.seeded(42)
int roll = dice.range(1, 7)
```

## time

Clocks, durations, sleeping, timeouts and RFC 3339 dates. Durations are `int` nanoseconds and points in time are `int` Unix nanoseconds, always UTC. There are no time zones in v1 other than reading the offset in `time.parse`.

### Constants

| Constant | Value |
|---|---|
| `const int nanosecond` | 1 |
| `const int microsecond` | 1000 |
| `const int millisecond` | 1000000 |
| `const int second` | 1000000000 |
| `const int minute` | 60 * `second` |
| `const int hour` | 60 * `minute` |

There is no `day`: a day is not always 24 hours once time zones exist, so the library doesn't pretend it is.

### Types

```
pub type Date struct {
    int year         // e.g. 2026
    int month        // 1–12
    int day          // 1–31
    int hour         // 0–23
    int minute       // 0–59
    int second       // 0–59 (leap seconds are not represented)
    int nanosecond   // 0–999999999
}
```

### Functions

| Function | Result |
|---|---|
| `time.now() int` | The current wall-clock time in Unix nanoseconds. It can jump when the system clock is changed. |
| `time.monotonic() int` | Nanoseconds since an arbitrary fixed point, from a clock that never goes backwards. Only differences between two readings mean anything. Use it to measure how long something took. |
| `time.sleep(int d)` | Pauses the goroutine for at least `d` nanoseconds. Returns at once if `d <= 0`. |
| `time.after(int d) chan int` | A channel that receives `time.now()` once, `d` nanoseconds from now, and is then closed. Meant for timeouts in `select`. |
| `time.date(int t) Date` | The UTC calendar date and time of `t`. Works for any `t`, including before 1970 (proleptic Gregorian calendar). |
| `time.fromDate(Date d) (int, error)` | The Unix time of a UTC date. Fails if any field is out of range, including days that don't exist (February 30). |
| `time.format(int t) string` | `t` in RFC 3339, in UTC: `"2026-10-13T08:30:00Z"`. Fractional seconds appear only when non-zero, without trailing zeros: `"2026-10-13T08:30:00.25Z"`. Crashes if the year is outside 0–9999. |
| `time.parse(string s) (int, error)` | Reads an RFC 3339 date-time, `YYYY-MM-DDTHH:MM:SS[.fraction](Z|±HH:MM)`, and returns its Unix time. `T` may also be `t` or a space, `Z` may be `z`. Up to 9 fraction digits. Offsets are applied, so `"2026-10-13T10:30:00+02:00"` and `"2026-10-13T08:30:00Z"` give the same `int`. |

- `time.sleep` and a receive on a `time.after` channel are stop points: a goroutine whose group is stopped wakes up and stops.
- `time.after` has a buffer of one and is closed after its value, so a timer nobody reads never blocks anything.
- There is no `time.since(t)`; write `time.monotonic() - start`.

```
int start = time.monotonic()
try work()
int ms = (time.monotonic() - start) / time.millisecond
fmt.println("took " + ms.string() + " ms at " + time.format(time.now()))

select {
case string? reply = answers.receive():
    ...
case time.after(2 * time.second).receive():
    return errors.new("no answer within 2 seconds")
}
```

## encoding

Turning data into text and back. Every decoder returns an error on malformed input, naming the byte offset: `encoding.hexDecode: offset 3: invalid hex digit 'g'`.

| Function | Result |
|---|---|
| `encoding.hexEncode(string data) string` | Two lowercase hex digits per byte. |
| `encoding.hexDecode(string text) (string, error)` | The bytes back. Accepts upper and lower case. Fails on an odd length or a non-hex character. |
| `encoding.base64Encode(string data) string` | Standard base64 (RFC 4648 §4, `+` and `/`) with `=` padding. |
| `encoding.base64Decode(string text) (string, error)` | Accepts the standard alphabet, with or without padding. Fails on any other character, including whitespace. |
| `encoding.base64UrlEncode(string data) string` | URL-safe base64 (RFC 4648 §5, `-` and `_`) without padding, as used in JWTs and URLs. |
| `encoding.base64UrlDecode(string text) (string, error)` | Accepts the URL-safe alphabet, with or without padding. |
| `encoding.urlEncode(string text) string` | Percent-encoding for one URL path segment or query value: `A–Z a–z 0–9 - _ . ~` stay, every other byte becomes `%XX` with uppercase hex. A space becomes `%20`. |
| `encoding.urlDecode(string text) (string, error)` | Decodes `%XX`, and `+` as a space (as in query strings). Fails on a `%` not followed by two hex digits. |

```
string token = encoding.base64UrlEncode(crypto.randomBytes(32))
string url = "http://localhost:8080/search?q=" + encoding.urlEncode(query)
```

## json

JSON without reflection or generics. A JSON document is a tree of `json.Value`, an enum. Parsing gives a `Value`; a program reads it with accessor methods and builds one with the enum cases.

### Types

```
pub type Value enum {
    Null
    Bool(bool b)
    Number(float n)
    String(string s)
    Array([]Value items)
    Object(map[string]Value fields)
}
```

- Object keys keep their order, because maps keep insertion order: parsing and printing a document keeps its keys in place.
- Numbers are `float`, as in JavaScript. Whole numbers up to 2^53 are exact; larger ones lose precision.
- Two `Value`s compare with `==` by contents, like every enum. Object key order counts in the comparison, as it does for maps.

### Functions

| Function | Result |
|---|---|
| `json.parse(string text) (Value, error)` | Parses one JSON document (RFC 8259). Whitespace around it is allowed; anything else after it is an error. |

- Errors give the line and column (both from 1, column counted in bytes): `json.parse: line 1, column 12: expected ':'`.
- Strings must be valid UTF-8. `\uXXXX` escapes are decoded, including surrogate pairs; a lone surrogate becomes U+FFFD.
- A repeated key keeps its first position and its last value.
- Nesting deeper than 512 arrays and objects is an error, so hostile input cannot exhaust the stack.
- A byte-order mark, comments, trailing commas, `NaN` and single quotes are errors.

### Methods on Value

Accessors never crash. A wrong kind gives `null` (or `Value.Null`), so a program can walk a document and check once at the end.

| Method | Result |
|---|---|
| `v.get(string key) Value` | The field `key` of an object. `Value.Null` if `v` is not an object or has no such key. |
| `v.at(int i) Value` | Element `i` of an array. `Value.Null` if `v` is not an array or `i` is out of range. |
| `v.asString() string?` | The string, or `null` if `v` is not a `String`. |
| `v.asFloat() float?` | The number, or `null` if `v` is not a `Number`. |
| `v.asInt() int?` | The number as an `int` if it is a whole number with absolute value at most 2^53; otherwise `null`. |
| `v.asBool() bool?` | The bool, or `null` if `v` is not a `Bool`. |
| `v.asArray() []Value?` | The elements, or `null` if `v` is not an `Array`. |
| `v.asObject() map[string]Value?` | The fields, or `null` if `v` is not an `Object`. |
| `v.isNull() bool` | Whether `v` is `Value.Null`. |
| `v.string() string` | Compact JSON with no spaces: `{"a":[1,2.5,"x"]}`. |
| `v.pretty() string` | JSON indented by two spaces per level, `"key": value` with one space, no trailing newline. Empty arrays and objects print as `[]` and `{}`. |

`get` and `at` return `Value`, not `Value?`, so lookups chain without a null check at each step: `doc.get("user").get("address").get("city").asString()` is a `string?` that is `null` if any step is missing. A program that must tell a missing key from a JSON `null` uses `asObject()` and the map lookup, which returns `Value?`.

Printing rules for `string()` and `pretty()`:

- A whole number with absolute value below 1e21 prints without a fraction: `3`, `-12`, `0` (and `-0` for negative zero). Other numbers print as `f.string()` does, which is always valid JSON: `0.1`, `1e+300`.
- `nan` and infinities cannot be written in JSON: printing a `Value` that contains one crashes, naming the problem. Check with `math.isNan` and `math.isInf` before building such a value.
- Strings escape `"`, `\` and bytes below 0x20 (as `\n`, `\t`, `\r`, `\b`, `\f`, or `\u00XX`). Other UTF-8 is written as it is; invalid UTF-8 bytes are written as `�`.

```
json.Value doc = try json.parse(text)
string? name = doc.get("name").asString()
if (name == null) {
    return errors.new("config: \"name\" must be a string")
}

json.Value out = json.Value.Object(map[string]json.Value{
    "name": json.Value.String(name),
    "tags": json.Value.Array([]json.Value{json.Value.String("a"), json.Value.String("b")}),
    "count": json.Value.Number(3.0),
})
try fs.write("out.json", out.pretty() + "\n")
```

## crypto

The few cryptographic building blocks most programs need, all modern. Digests and keys are raw bytes in a `string`; turn them into text with `encoding.hexEncode` or `encoding.base64UrlEncode`.

| Function | Result |
|---|---|
| `crypto.sha256(string data) string` | The 32-byte SHA-256 digest. |
| `crypto.hmacSha256(string key, string data) string` | The 32-byte HMAC-SHA-256 (RFC 2104) of `data` with `key`. |
| `crypto.randomBytes(int n) string` | `n` bytes from the operating system's secure random source (`getentropy`, or `/dev/urandom` as a fallback). Crashes if `n` is negative, or if the OS cannot provide randomness, because no program should carry on without it. |
| `crypto.equal(string a, string b) bool` | Whether `a` and `b` are equal, taking time that depends only on their lengths, not their contents. Use it to compare MACs and tokens. |

Nothing else is included in v1: no MD5 or SHA-1 (broken), no ciphers, no password hashing. Those need careful choices and will be added deliberately, not by default.

```
string sig = crypto.hmacSha256(secret, payload)
if (!crypto.equal(sig, try encoding.hexDecode(givenSig))) {
    return errors.new("bad signature")
}
fmt.println(encoding.hexEncode(crypto.sha256(try fs.read("release.tar"))))
```

## net

TCP connections, IPv4 and IPv6. UDP and TLS are not part of v1.

### Handles

Seed has only values, and an open connection is not a value: it lives in the operating system. Every handle type in the library (`net.Conn`, `net.Listener`, `fs.File`) works the same way:

- A handle is a small struct holding the id of an entry in the runtime's handle table. Its fields are private, so a program cannot make one up.
- **Copying a handle copies the id, not the connection.** Both copies refer to the same connection, the same way two copies of a `chan` refer to the same channel. `==` on handles compares ids.
- Handle methods are not `mut`, because they don't change the handle value; they act on the connection behind it. So they can be called on parameters, and a handle can be stored in a struct and used from a read-only method.
- `close()` closes the connection for every copy. After that, every operation on any copy returns an error ending in `connection is closed`. Ids are never reused, so a stale copy can never reach a different connection opened later.
- Handles are not closed automatically when the last copy disappears; close them, usually with `defer`. When a group stops a goroutine, its `defer`s run, so `defer conn.close()` also covers that case. Every handle is closed when the process exits.
- **Goroutines.** A handle can be sent over a channel to hand the connection to another goroutine. This is how a server passes each accepted connection to a worker. The sending goroutine should then stop using its copy. If two goroutines do use one connection, nothing breaks in memory: each operation on a handle is atomic, but the order in which their reads and writes happen is up to the scheduler.

### Types

```
pub type Listener struct {
    // opaque handle
}

pub type Conn struct {
    // opaque handle
}
```

### Functions

| Function | Result |
|---|---|
| `net.listen(string addr) (Listener, error)` | Listens for TCP connections on `addr`, `"host:port"`. An empty host (`":8080"`) listens on every interface. Port `0` picks a free port; read it back with `addr()`. |
| `net.dial(string addr, int timeout) (Conn, error)` | Connects to `"host:port"`. The host can be a name, an IPv4 address or a bracketed IPv6 address (`"[::1]:80"`). A name is resolved with the system resolver, trying each address in order. `timeout` limits the whole call, in nanoseconds; `0` means no limit. |

### Listener methods

| Method | Result |
|---|---|
| `l.accept() (Conn, error)` | Waits for the next connection. |
| `l.addr() string` | The address being listened on, with the real port: `"0.0.0.0:54321"`. |
| `l.close()` | Stops listening. A goroutine waiting in `accept` gets an error. |

### Conn methods

| Method | Result |
|---|---|
| `c.read(int max) (string?, error)` | Waits for data and returns between 1 and `max` bytes as soon as some are available, or `null` when the other side has closed its end. Crashes if `max < 1`. |
| `c.write(string data) error` | Writes all of `data`. |
| `c.setTimeout(int d)` | From now on, each `read` and `write` fails with `timeout` if it makes no progress for `d` nanoseconds. `0` (the default) means no limit. Crashes if `d` is negative. Applies to the connection, so to every copy. |
| `c.remoteAddr() string` | The other side's address, `"203.0.113.7:51234"`. |
| `c.close()` | Closes the connection. |

- `accept`, `read`, `write` and `dial` are stop points: if the goroutine's group is stopped while one of them waits, it returns and the goroutine stops, running its `defer`s.
- Writing to a connection the other side has closed returns an error (`broken pipe`); it never kills the process.
- A timeout error reads `net.Conn.read: 203.0.113.7:51234: timeout`. After a timeout the connection is still open and can be used again.

```
fn echo(net.Conn c) error {
    defer c.close()
    c.setTimeout(30 * time.second)
    for (;;) {
        string? data = try c.read(4096)
        if (data == null) {
            return null
        }
        try c.write(data)
    }
}
```

## http

An HTTP/1.1 server and client, written in Seed on top of `net`. **There is no TLS in v1**: the client only speaks `http://`, and a server meant for the internet should sit behind a proxy that terminates TLS. HTTP/2 is not supported.

### Types

```
pub type Request struct {
    string method                   // "GET", "POST", ... (uppercase)
    string url                      // server: the request target, "/todos?done=1"
                                    // client: the full URL, "http://example.com/todos?done=1"
    map[string]string headers       // names are lowercase
    string body
}

pub type Response struct {
    int status                      // 200, 404, ...
    map[string]string headers       // names are lowercase
    string body
}

pub type Handler interface {
    handle(Request req) (Response, error)
}
```

- **Header names are always lowercase** in both directions: the library lowercases names it receives and names a program sets, so `req.headers["content-type"]` always works. A header that appears more than once is joined into one value with `", "`, in the order received. (`set-cookie` cannot be joined correctly this way; that is a known v1 limitation.)
- Bodies are whole strings. There is no streaming in v1.

### Request methods

| Method | Result |
|---|---|
| `r.path() string` | The path part of `url`, percent-decoded: `"/todos"`. If decoding fails, the raw path. For a client request, the path after the host (`"/"` if there is none). |
| `r.query() map[string]string` | The query parameters, decoded with `encoding.urlDecode` rules (`+` is a space). Keys appear in the order of their first occurrence; a repeated key keeps its last value. A key without `=` has the value `""`. Parameters that fail to decode are skipped. |

### Status codes

`http.statusText(int code) string` gives the reason phrase (`"Not Found"`), or `""` for an unknown code. The common codes are constants:

| Constant | Value | Constant | Value |
|---|---|---|---|
| `statusOk` | 200 | `statusUnauthorized` | 401 |
| `statusCreated` | 201 | `statusForbidden` | 403 |
| `statusNoContent` | 204 | `statusNotFound` | 404 |
| `statusMovedPermanently` | 301 | `statusMethodNotAllowed` | 405 |
| `statusFound` | 302 | `statusConflict` | 409 |
| `statusNotModified` | 304 | `statusInternalServerError` | 500 |
| `statusBadRequest` | 400 | `statusServiceUnavailable` | 503 |

### Building responses

| Function | Result |
|---|---|
| `http.textResponse(int status, string body) Response` | A response with `content-type: text/plain; charset=utf-8`. |
| `http.jsonResponse(int status, json.Value body) Response` | A response with `content-type: application/json` and `body.string()` as the body. |

Any other response is a struct literal: `http.Response{status: 302, headers: map[string]string{"location": "/"}, body: ""}`.

### Server

| Function | Result |
|---|---|
| `http.serve(string addr, Handler handler) error` | Listens on `addr` (as `net.listen`) and serves requests with `handler` until the goroutine running it is stopped. Returns an error only if it cannot listen. |

- **Each connection runs in its own goroutine**, inside a group that `serve` owns. Connections are kept alive between requests (HTTP/1.1 keep-alive) and closed after 60 seconds idle.
- **The handler is an interface, not a function**, because a function value cannot carry anything: Seed has no closures and no global variables. A struct that `implements http.Handler` can hold configuration and channels. Each request sees a copy of it, so the handler can't change shared state directly; state that requests change lives in a goroutine that the handler talks to over channels (see `examples/server.sd`).
- **Errors and crashes are contained per request.** If `handle` returns an error, the client gets `500` with the body `internal server error`, and `http: GET /path: <message>` is written to stderr. If `handle` crashes, the same happens with the crash message. In both cases the server carries on; one bad request never stops the server.
- The server sets `content-length` itself and ignores one set by the handler. It sends no `content-type` unless the handler set one. For `HEAD` requests it sends the headers but no body.
- Limits: request line and headers 64 KiB (else `431`), body 8 MiB (else `413`), and 30 seconds to receive a whole request (else the connection is closed). Chunked request bodies are decoded. Malformed requests get `400` without calling the handler.
- `serve` is normally spawned in the program's top-level group. If the group is stopped, `serve` stops accepting, stops the connection goroutines at their next stop point, closes the listener and returns.

### Client

| Function | Result |
|---|---|
| `http.get(string url) (Response, error)` | Sends a `GET` with no extra headers. Exactly `http.send(Request{method: "GET", url: url, headers: map[string]string{}, body: ""})`. |
| `http.send(Request req) (Response, error)` | Sends any request and returns the response. |

- The URL must be `http://host[:port][/path][?query]`. Anything else is an error, including `https://`: `http.get: https://example.com/: https is not supported`.
- The library adds `host`, `content-length` (when the body is not empty, or the method is `POST`, `PUT` or `PATCH`) and `connection: close`. Each request uses a new connection.
- **Any response is a success**, including `404` and `500`: check `resp.status`. The error is set only when no response arrived (connection refused, timeout, malformed response).
- **Redirects are not followed**; a `3xx` response is returned as it is, with its `location` header.
- The client gives up with a `timeout` error when connecting, or any single read or write, makes no progress for 30 seconds. To wait less, run the request in a goroutine and `select` on its result channel and `time.after`.
- Responses with `transfer-encoding: chunked` are decoded. Bodies larger than 64 MiB are an error. Compressed responses are never requested.

```
type App struct implements http.Handler {
    string greeting
}

fn (App app) handle(http.Request req) (http.Response, error) {
    if (req.path() == "/") {
        return http.textResponse(http.statusOk, app.greeting + "\n"), null
    }
    return http.textResponse(http.statusNotFound, "not found\n"), null
}

fn main([]string args) error {
    return group {
        spawn http.serve(":8080", App{greeting: "hello"})
    }
}
```

```
http.Response resp = try http.send(http.Request{
    method: "POST",
    url: "http://localhost:8080/todos",
    headers: map[string]string{"content-type": "application/json"},
    body: todo.string(),
})
if (resp.status != http.statusCreated) {
    return errors.new("creating todo: HTTP " + resp.status.string() + ": " + resp.body)
}
```

## Tests

There is no testing package. Tests are `test "..." { ... }` blocks in a package's `tests/` directory and use the built-in `assert(bool)`, as described in [SPEC.md](../SPEC.md#tests). A test that needs files uses `fs.makeTemp` and removes the directory with `defer`:

```
test "save then load keeps every todo" {
    string dir = try fs.makeTemp("todo-test-")
    defer fs.removeAll(dir)
    ...
}
```

(`defer fs.removeAll(dir)` discards an error. See the spec questions below.)

## Implementation notes

### Layout

- Each package is a directory `lib/<pkg>/` of ordinary Seed files (`package <pkg>`), with its tests in `lib/<pkg>/tests/` (`seed test lib` runs them). The compiler finds `lib/` next to its own binary, and `seed build` compiles the packages a program imports into the same static binary.
- Code that needs the operating system is written in C, in `.c` files in the package's directory (`lib/<pkg>/<pkg>.c`), which may call the runtime. Everything else is Seed. Packages may import each other, without cycles:

```
errors, strings, math, path, encoding    (no imports, apart from errors)
fmt, io, os, fs, time, rand, crypto      (mostly native)
json      → strings, math, errors
net       → time, errors
http      → net, json, encoding, strings, time, fmt, errors
```

### Native functions

A function or method declared **without a body** is native:

```
pub fn sqrt(float x) float
pub fn (Conn c) read(int max) (string?, error)
```

- Native declarations are only allowed in packages under `lib/`. Anywhere else, a missing body is a compile error ("fn f needs a body").
- The compiler emits a call to a C function with a fixed name: `sd_lib_<pkg>_<name>` for functions and `sd_lib_<pkg>_<Type>_<method>` for methods (`sd_lib_math_sqrt`, `sd_lib_net_Conn_read`). Arguments and results use the runtime's normal C representation of each Seed type; with several results, the last one (normally the `error`) is returned and the others are written through out-pointers after the arguments. A `mut` native method receives a pointer to the receiver. The full contract is in [runtime/README.md](../runtime/README.md#13-native-functions).
- Errors are made with the runtime helper that `errors.new` uses, so their messages follow the format above. The native code builds the full message (`fs.read: notes.txt: no such file or directory`) from `sd_strerror(errno)` (`strerror` with a lowercase first letter).
- **Blocking.** A native call that can wait (reading, accepting, connecting, sleeping, waiting for a child process) must call the runtime's `sd_block_begin()` and `sd_block_end()` around the wait, so that other goroutines keep running and so that a group can stop the goroutine there. `sd_block_end()` reports whether the goroutine was asked to stop; the native then cleans up (kills a child, abandons a read) and returns to the runtime's stop path, which runs `defer`s. How this maps to the scheduler (OS threads, or an event loop with non-blocking descriptors) is the runtime's choice; the observable behaviour is the same.
- **Handles.** The runtime keeps one process-wide table (shared by `fs` and `net`, so it belongs in `runtime/`, not in one package's C file). Each entry holds the kind (file, conn, listener), the descriptor, the read buffer used by `readLine` and `read`, the timeout, the closed flag, and a mutex that makes each operation atomic. The Seed struct holds only the entry's `int` id; ids count up and are never reused.
- **The core packages in seed0.** `seed0` implements `errors.new`, `fmt`, `os.exit`, `os.env`, `os.run`, `os.executable`, `os.cwd` and the `fs` functions in [builtins.md](builtins.md) directly in its runtime and does not read `lib/`. The real compiler uses `lib/`, and both must behave identically for that subset (including the error messages).
- **Blocking natives are not stop points yet.** The runtime has no `sd_block_begin`/`sd_block_end` hooks so far; a native that blocks only blocks its own goroutine (each goroutine is a thread), but a failed group cannot stop it there.

### Native code per package

| Package | Native | Written in Seed |
|---|---|---|
| `errors` | — | `new` (a private struct implementing `error`), `wrap` |
| `fmt` | `print`, `eprint`, `fixed` (correct rounding needs the C library) | `println`, `eprintln` (one `print` of `s + "\n"`, so they stay atomic) |
| `io` | `readLine`, `readAll` (they share the stdin buffer) | — |
| `os` | `exit`, `env`, `run`, `output`, `cwd`, `hostname` | — |
| `fs` | `read`, `write`, `append`, `exists`, `isDir`, `stat`, `list`, `mkdir`, `rename`, `remove`, `removeAll` (needs `lstat` to avoid following links), `makeTemp`, `open`, `create`, and the four `File` methods | — |
| `path` | — | all |
| `strings` | — | all |
| `math` | `powFloat`, `sqrt`, `floor`, `ceil`, `round`, `trunc`, the six trigonometric functions, `atan2`, `exp`, `log`, `log2`, `log10`, `inf` | `pi`, `e`, the `Int` functions, `absFloat`, `minFloat`, `maxFloat`, `isNan` (`x != x`), `isInf` |
| `rand` | `nextOs() int` (private: the next 64-bit output of the calling goroutine's generator, seeded from the OS) | `seeded`, SplitMix64 for `Source`, and `range`, `fraction` and `permutation` on top of one 64-bit step |
| `time` | `now`, `monotonic`, `sleep`, `after` (a runtime timer that sends on and closes a channel; not a Seed goroutine) | constants, `date`, `fromDate`, `format`, `parse` (days-from-civil arithmetic) |
| `encoding` | — | all |
| `json` | — | all (a recursive-descent parser and printer); float printing uses the built-in `f.string()` |
| `crypto` | `sha256`, `hmacSha256` (64-bit wrapping arithmetic is slow with growing `int`s), `randomBytes`, `equal` (so that no compiler optimisation can make it exit early) | — |
| `net` | everything | — |
| `http` | — | all, on top of `net` |

The SplitMix64 step in `rand` needs arithmetic modulo 2^64. With growing integers that is `(x * c) & 0xffffffffffffffff`, which is correct but allocates. If it proves too slow, `Source`'s step can become a native method without any change to its behaviour.

### How `http.serve` contains failures

A sketch, to show that the server needs no special runtime support beyond dynamic `spawn` (see below):

```
pub fn serve(string addr, Handler h) error {
    net.Listener ln = try net.listen(addr)
    defer ln.close()
    return group {
        for (;;) {
            spawn serveConn(h, acceptNext(ln))   // acceptNext retries transient accept errors
        }
    }
}

// Never returns an error, so one connection can't stop the server's group.
fn serveConn(Handler h, net.Conn c) error {
    defer c.close()
    for (;;) {
        Request? req = readRequest(c)       // null when the client is done or sent garbage
        if (req == null) {
            return null
        }
        Response resp = callHandler(h, req)
        error err = writeResponse(c, req, resp)
        if (err != null) {
            return null
        }
    }
}

// Runs the handler in its own group, so a crash becomes an error here.
fn callHandler(Handler h, Request req) Response {
    chan Response out = chan Response(1)
    error err = group {
        spawn runHandler(h, req, out)
    }
    if (err != null) {
        fmt.eprintln("http: " + req.method + " " + req.path() + ": " + err.message())
        return textResponse(statusInternalServerError, "internal server error\n")
    }
    Response? resp = out.receive()
    ...
}
```

## Spec changes this design relies on

These are questions or changes for [SPEC.md](../SPEC.md). The design above assumes the answer given here.

1. **Native functions.** `FnDecl` needs an optional body, allowed only in the standard library.
2. **Private struct fields.** Handle types (`net.Conn`, `fs.File`) and `rand.Source` need fields that other packages can't read or set, or a program could forge a handle. Proposal: fields are private unless marked `pub`, and a struct literal of another package's type is only allowed if every field is `pub`.
3. **Stop points.** The spec says a goroutine can only be stopped at a channel operation. A server's goroutines spend their lives in `accept` and `read`. Proposal: blocking standard-library calls (`time.sleep`, I/O on handles, `io.readLine`, `os.run`, `net.dial`) are stop points too.
4. **`spawn` inside loops in a `group`.** Servers and worker pools spawn a number of goroutines that is only known at run time. Proposal: a `group` block is ordinary code, `spawn` may appear anywhere inside it (in loops and `if`s), and finished goroutines are freed while the group is still running. It also needs saying what `try` and `return` mean inside a group block.
5. **Strings hold any bytes.** The design stores raw data in strings. The spec should say a string is a sequence of bytes that is usually, but not necessarily, UTF-8.
6. **Float division by zero.** The spec says division by zero crashes. For `float`, IEEE results (`inf`, `nan`) fit `math` and `f.string()` better; proposal: only `int` division by zero crashes.
7. **`defer` and errors.** `defer fs.removeAll(dir)` returns an error. Either `defer` may discard an error (proposed: only as `defer _ = f()`), or such calls can't be deferred.
8. **Distinguishing errors.** With no type switch or assertion on interfaces, a program can't tell error kinds apart. The library is designed around that (nullable results, statuses), but the spec should decide whether interface type switches will exist.
9. **Names that match type names.** `rand.int` or `rand.float` would clash with the built-in types inside the `rand` package (shadowing). The library avoids them (`rand.range`, `rand.fraction`); the spec should say whether such names are allowed at all.
10. **Values carrying channels.** `examples/server.sd` sends an enum whose cases hold a `chan int` (a reply channel). The spec should confirm that channels can be stored in structs and enums and sent over channels, with copying giving another reference to the same channel.
11. **The planned package list** in SPEC.md should add `path`, `rand` and `encoding`.
