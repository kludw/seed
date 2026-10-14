# Built-in methods and core packages

This is part of the language spec ([SPEC.md](../SPEC.md)). It lists every method that built-in types have, and the core packages every Seed compiler provides.

## Runtime failure behaviour

| Situation | What happens |
|-----------|--------------|
| `main` returns an error | `error: <message>` is printed to stderr, exit code 1. |
| A crash (index out of range, division by zero, failed conversion, sending on a closed channel, `os.crash(message)`, ...) | `crash: <message>` is printed to stderr, exit code 2. Inside a `group`, the crash becomes the group's error instead. Either way the crashing goroutine's deferred calls run first. |
| `os.exit(n)` | Exits immediately with code `n` (its low 8 bits), without running defers. |
| Every goroutine is blocked on a channel (or waits for a group) and no timer is pending | `crash: deadlock: all goroutines are blocked` is printed to stderr, exit code 2. Defers do not run. |

## int

| Method / conversion | Result |
|---|---|
| `i.string()` | Decimal text, e.g. `"-42"`. |
| `float(i)` | The nearest `float`. |
| `byte(i)` | The byte with that value. Crashes unless `0 <= i <= 255`. |

## float

| Method / conversion | Result |
|---|---|
| `f.string()` | The shortest text that reads back as the same value, always containing `.` or `e`: `1.0`, `0.1`, `1e+300`. `nan`, `inf`, `-inf` for special values. |
| `int(f)` | Truncates toward zero. Crashes on `nan` or infinity. |

## bool

| Method | Result |
|---|---|
| `b.string()` | `"true"` or `"false"`. |

## byte

| Method / conversion | Result |
|---|---|
| `b.string()` | Decimal text, e.g. `'a'.string()` is `"97"`. |
| `int(b)` | The value 0–255. |

## string

Strings are immutable UTF-8 bytes. Lengths and indexes count bytes.

| Method / conversion | Result |
|---|---|
| `s.len()` | Number of bytes. |
| `s[i]` | The byte at `i`. Crashes when out of range. |
| `s.slice(a, b)` | Bytes `a` up to (not including) `b`. Crashes unless `0 <= a <= b <= len`. |
| `s.contains(sub)` | `bool`. |
| `s.index(sub)` | First byte index of `sub` as `int?`, or `null`. |
| `s.startsWith(p)`, `s.endsWith(p)` | `bool`. |
| `s.split(sep)` | `[]string`. `"a,b,".split(",")` is `["a", "b", ""]`. Crashes if `sep` is empty. |
| `s.trim()` | Removes leading and trailing spaces, tabs, `\n` and `\r`. |
| `s.replace(old, new)` | Replaces every occurrence. Crashes if `old` is empty. |
| `s.toUpper()`, `s.toLower()` | ASCII letters only. |
| `s.toInt()` | `(int, error)`. Accepts an optional `-` and decimal digits only. The error is `string.toInt: "12x": invalid integer`. |
| `s.toFloat()` | `(float, error)`. The error is `string.toFloat: "abc": invalid float`. |
| `s.bytes()` | `[]byte`. |
| `s.chars()` | `[]string`, one string per UTF-8 character. |
| `string(xs)` | Converts a `[]byte` into a string. |

`+` joins two strings. `==` and `<` compare bytes.

## Lists `[]T`

| Method | Result |
|---|---|
| `xs.len()` | `int`. |
| `xs[i]`, `xs[i] = v` | Read or write an element. Crashes when out of range. |
| `xs.push(x)` | `mut`. Adds to the end. |
| `xs.pop()` | `mut`. Removes and returns the last element as `T?`, or `null` when empty. |
| `xs.insert(i, x)` | `mut`. `0 <= i <= len`. |
| `xs.remove(i)` | `mut`. Removes the element at `i`. |
| `xs.contains(x)` | `bool`. |
| `xs.index(x)` | `int?`. |
| `xs.slice(a, b)` | A new list with elements `a` up to (not including) `b`. |
| `xs.reverse()` | `mut`. |
| `xs.sort()` | `mut`. Only for `[]int`, `[]float`, `[]string`, `[]byte`. |
| `xs.sortBy(f)` | `mut`. `f` is `fn(T, T) bool`, returning true when the first argument goes first. The sort is stable. |
| `xs.join(sep)` | Only for `[]string`. Joins the strings with `sep` between them. |

## Maps `map[K]V`

`K` is `int`, `byte`, `bool` or `string`.

| Operation | Result |
|---|---|
| `m[k]` | `V?`. |
| `m[k] = v` | Inserts or replaces. A new key goes to the end of the order. |
| `m.len()` | `int`. |
| `m.delete(k)` | `mut`. Does nothing if `k` is missing. The order of the other keys is kept. |
| `m.keys()` | `[]K` in insertion order. |

## error

`error` is an interface with one method, `message() string`.

## Channels `chan T`

`chan T(n)` makes a channel with a buffer of `n` values (`0`: unbuffered, every send waits for a receiver). Crashes if `n` is negative. A channel value is a handle: copies of it (in variables, structs, lists, other channels) all refer to the same channel. `T` cannot be nullable. Channels have no `==`.

| Method | Result |
|---|---|
| `ch.send(v)` | Sends a copy of `v`, waiting while the buffer is full. Crashes if the channel is closed (`send on closed channel`). |
| `ch.receive()` | The next value as `T?`, waiting until there is one; `null` once the channel is closed and empty. For `chan error` the result is `error` (null when closed). |
| `ch.close()` | Closes the channel: buffered values can still be received. Crashes if it is already closed (`close of closed channel`). |

`send`, `receive` and `select` are stop points, where a goroutine whose group has failed stops (so are `time.sleep` and the standard library's blocking calls, listed in [SPEC.md](../SPEC.md#concurrency)); `close` is not.

## Core packages

These are the packages almost every program uses. They are ordinary packages of the standard library (`lib/fmt`, `lib/os`, ...), written with native functions, and are imported like any other (`import "fmt"`). This section lists the part of them that the bootstrap compiler `seed0` also builds in (it lacks `time`, `errors.wrap` and `os.crash`); the complete packages, with more functions (`os.output`, `fs.open`, `time.now`, ...), are in [stdlib.md](stdlib.md).

Errors follow [stdlib.md](stdlib.md#error-messages): `<package>.<function>: <subject>: <reason>`, for example `fs.read: notes.txt: no such file or directory` or `os.run: frobnicate: no such file or directory`.

### errors

| Function | Result |
|---|---|
| `errors.new(string msg) error` | An error whose `message()` is `msg`. |
| `errors.wrap(error err, string context) error` | An error whose `message()` is `context + ": " + err.message()`. Crashes if `err` is null. (Not in seed0.) |

### fmt

| Function | Result |
|---|---|
| `fmt.print(string s)` | Writes `s` to stdout. |
| `fmt.println(string s)` | Writes `s` and a newline to stdout. |
| `fmt.eprint(string s)` | Writes `s` to stderr. |
| `fmt.eprintln(string s)` | Writes `s` and a newline to stderr. |

### os

| Function | Result |
|---|---|
| `os.exit(int code)` | Exits immediately with the low 8 bits of `code` (`os.exit(256)` exits with 0). Output is flushed first; defers do not run. |
| `os.env(string name) string?` | An environment variable, or `null`. |
| `os.run([]string argv) (int, error)` | Runs a program (looked up on `PATH`) with the same stdin, stdout and stderr, waits for it and returns its exit code. The error is set only if the program could not be started. Crashes if `argv` is empty. |
| `os.crash(string message)` | Crashes the current goroutine exactly like a runtime fault: `crash: <message>`, its deferred calls run, and inside a `group` the group fails with the error `crash: <message>`. Library code uses it when a caller broke a rule: `os.crash("rand.range: lo (5) must be less than hi (5)")`. Never returns. (Not in seed0.) |
| `os.executable() string` | The absolute path of the running program, with symbolic links resolved, or `""` if it cannot be found. |
| `os.cwd() (string, error)` | The absolute path of the current directory. |

### fs

| Function | Result |
|---|---|
| `fs.read(string path) (string, error)` | Whole file contents. |
| `fs.write(string path, string data) error` | Creates or replaces the file. |
| `fs.exists(string path) bool` | |
| `fs.isDir(string path) bool` | |
| `fs.list(string dir) ([]string, error)` | Entry names (not paths), sorted, without `.` and `..`. |
| `fs.mkdir(string path) error` | Creates the directory and any missing parents. Succeeds if it exists. |
| `fs.remove(string path) error` | Removes a file or an empty directory. |

### time

Durations are `int` nanoseconds. The full `time` package is in [stdlib.md](stdlib.md#time); the real compiler provides this part as a core package (seed0 does not).

| Function / constant | Result |
|---|---|
| `time.nanosecond`, `time.microsecond`, `time.millisecond`, `time.second`, `time.minute`, `time.hour` | `int` constants: 1, 1000, 1000000, 1000000000, 60 seconds, 60 minutes. |
| `time.after(int d) chan int` | A channel that receives the current time (Unix nanoseconds) once, `d` nanoseconds from now, and is then closed. Meant for timeouts in `select`. |
| `time.sleep(int d)` | Pauses the goroutine for at least `d` nanoseconds (a stop point, like a receive). |
