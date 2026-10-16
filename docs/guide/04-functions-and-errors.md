# 4. Functions and errors

## Functions

```seed
fn add(int a, int b) int {
    return a + b
}
```

Parameters write the type first, like every declaration, and the result type follows the parameter list. A function with several results lists them in parentheses: `fn minMax([]int xs) (int, int)`. There are no named results, no naked `return`, and no variadic functions: a function that takes "any number of" values takes a list.

```seed
package main

import "errors"
import "fmt"

fn add(int a, int b) int {
    return a + b
}

// Several results are listed in parentheses.
fn minMax([]int xs) (int, int) {
    int lo = xs[0]
    int hi = xs[0]
    for (int i = 1; i < xs.len(); i++) {
        if (xs[i] < lo) {
            lo = xs[i]
        }
        if (xs[i] > hi) {
            hi = xs[i]
        }
    }
    return lo, hi
}

// No variadic functions: take a list instead.
fn sum([]int xs) int {
    int total = 0
    for (int i = 0; i < xs.len(); i++) {
        total = total + xs[i]
    }
    return total
}

fn divide(int a, int b) (int, error) {
    if (b == 0) {
        return null, errors.new("divide by zero")
    }
    return a / b, null
}

fn main([]string args) error {
    fmt.println(add(2, 3).string())

    int lo, int hi = minMax([]int{4, 8, 1, 9})
    fmt.println(lo.string() + ".." + hi.string())
    fmt.println(sum([]int{1, 2, 3}).string())

    int q, error err = divide(10, 2)
    if (err != null) {
        return err
    }
    fmt.println("10 / 2 = " + q.string())

    // A name without a type in front is an existing variable.
    q, err = divide(1, 0)
    if (err != null) {
        fmt.println("failed: " + err.message())
    } else {
        fmt.println(q.string())
    }
    return null
}
```

```
5
1..9
6
10 / 2 = 5
failed: divide by zero
```

A function cannot change its caller's variables through its parameters. A parameter is the function's own copy: it can be assigned a new value (`n = n - 1`, `args = args.slice(1, args.len())`) without the caller seeing it, but not changed in place, with a `mut` method or through a field or element ([chapter 7](07-structs-and-methods.md) says why). To give something back, a function returns new values. A function's result must be used; to throw a result away on purpose, assign it to `_`: `_ = cache.add(key)`.

## Errors are values

A function that can fail returns an `error` as its last result. `error` is a built-in interface with one method, `message() string`, and it is the one type that can be `null` without a `?`: a `null` error means success. `errors.new(text)` makes a simple error.

Like a `T?`, an `error` must be checked before use: `err.message()` compiles only where the compiler can see that `err` is not `null`, such as inside `if (err != null)` or after `if (err == null) { return ... }` ([chapter 5](05-null-safety.md#error-is-narrowed-too) has the rules). Every example here checks it anyway, before reading the other results.

When the error is not `null`, the other results are `null` too. That's why `divide` above returns `null, errors.new(...)`, even though its first result is a plain `int`. The compiler makes sure you never see those nulls: it won't let you read `q` until `err` has been checked.

```seed
package main

import "fmt"

fn main([]string args) error {
    int n, error err = "42x".toInt()
    fmt.println(n.string())
    if (err != null) {
        return err
    }
    return null
}
```

```
04_unchecked_value.sd:7:17: error: n is read before err is checked; when err is not null, n is null
    7 |     fmt.println(n.string())
      |                 ^
    hint: check err != null first (and return or handle it), or use try
```

The value can be read once the error is known to be null: inside `if (err == null) { ... }`, after `if (err != null) { return ... }`, or to the right of `err == null &&`. Assigning the variable a new value also makes it readable. Passing both on unchanged, `return n, err`, is fine too: when `err` is not null, the caller gets a null `n` beside it, and its own check applies. A value declared as a nullable `T?` (`string? s, error err = f()`) is not guarded at all, because it has to be checked against `null` before use anyway.

## Every error must be handled

Ignoring an error is a compile error:

```seed
package main

import "fmt"
import "fs"

fn main([]string args) error {
    fs.write("notes.txt", "hello")
    fmt.println("saved")
    return null
}
```

```
04_ignored_error.sd:7:8: error: the error returned by fs.write() is not handled
    7 |     fs.write("notes.txt", "hello")
      |        ^
    hint: use try, assign it (error err = ...), or discard it with _ = ...
```

In Go, `f.Write(...)` on its own line silently drops the error, and so does `x, _ := ...` without anyone noticing. In Seed an error is either checked, passed up, or visibly discarded. You have three ways to handle one.

### 1. Check it

```seed
string text, error err = fs.read(path)
if (err != null) {
    fmt.eprintln("cannot read settings: " + err.message())
    return defaults()
}
```

### 2. Pass it up with try

`try f()` means: if `f` fails, return its error from the current function right away, with `null` for the other results. Otherwise it gives `f`'s value. It is the only piece of syntax sugar in Seed, because passing an error up is by far the most common thing to do with it.

```seed
package main

import "errors"
import "fmt"

fn parsePort(string text) (int, error) {
    int n = try text.toInt()
    if (n < 1 || n > 65535) {
        return null, errors.new("port out of range: " + text)
    }
    return n, null
}

// The same function written without try.
fn parsePortLong(string text) (int, error) {
    int n, error err = text.toInt()
    if (err != null) {
        return null, err
    }
    if (n < 1 || n > 65535) {
        return null, errors.new("port out of range: " + text)
    }
    return n, null
}

fn parseAll([]string items) ([]int, error) {
    []int ports = []int{}
    for (int i = 0; i < items.len(); i++) {
        ports.push(try parsePort(items[i]))
    }
    return ports, null
}

fn report([]string items) {
    []int ports, error err = parseAll(items)
    if (err != null) {
        fmt.println("error: " + err.message())
        return
    }
    fmt.println("ok: " + ports.len().string() + " ports")
}

fn main([]string args) error {
    report([]string{"80", "443"})
    report([]string{"80", "99999"})
    report([]string{"80", "http"})

    int port = try parsePortLong("8080")
    fmt.println(port.string())
    return null
}
```

```
ok: 2 ports
error: port out of range: 99999
error: string.toInt: "http": invalid integer
8080
```

`try` works on any call whose last result is an `error`, including one that returns only an error (`try fs.write(path, text)`), and it can appear inside a larger expression (`ports.push(try parsePort(...))`). It is only allowed in a function whose own last result is `error`:

```
04_try_needs_error.sd:6:13: error: try can only be used in a function whose last result is error
    6 |     int n = try s.toInt()
      |             ^
    hint: handle the error here instead: T v, error err = ...
```

### 3. Discard it with _

Sometimes an error really doesn't matter. Write `_` in its place to say so. If a value comes back beside the discarded error, that value may be `null`, so it must be declared as a nullable `T?` and checked before use ([chapter 5](05-null-safety.md)):

```seed
package main

import "fmt"
import "fs"

fn main([]string args) error {
    string dir = "/tmp/seed-guide-discard"
    try fs.mkdir(dir)
    string path = dir + "/note.txt"
    try fs.write(path, "remember the milk\n")

    // Discarding the error: the value comes back as string?.
    string? text, _ = fs.read(path)
    if (text != null) {
        fmt.print(text)
    }

    string? missing, _ = fs.read(dir + "/nope.txt")
    if (missing == null) {
        fmt.println("no such file, and that is fine here")
    }

    // Nothing else is returned, so there is nothing to check.
    _ = fs.remove(path)
    _ = fs.remove(dir)
    fmt.println(fs.exists(dir).string())
    return null
}
```

```
remember the milk
no such file, and that is fine here
false
```

The other way round, `_` in place of a value keeps only the error: `_, error err = fs.read(path)`.

Declaring the value as a plain `string` beside a discarded error is rejected:

```
04_discard_plain.sd:7:12: error: the error is discarded with _, so text may be null and must be declared as string?
    7 |     string text, _ = fs.read("notes.txt")
      |            ^
    hint: declare it as string? and check it against null before using it
```

### Your own errors

`errors.new` is enough for most errors. Because `error` is an interface, any type can be an error by implementing `message()`; [chapter 9](09-interfaces-and-functions.md) shows how. There is no way to ask an `error` which type it holds, so when callers need to react to a specific condition, return something they can check directly: a nullable result, a `bool`, or an enum.

## Errors and crashes

Seed separates two kinds of failure:

- **An error** is something that can go wrong outside the program's control: a missing file, bad input, a refused connection. It is a value that the program handles.
- **A crash** is a bug: an index out of range, integer division by zero, a failed conversion such as `byte(300)`. A correct program never crashes, so crashes are not values you can handle. A crash prints `crash:` and a message to stderr and ends the program with exit code 2. (With concurrency, a crash ends only its goroutine and becomes its group's error: see [chapter 10](10-concurrency.md).)

```seed
package main

import "fmt"

fn main([]string args) error {
    []int scores = []int{90, 85, 70}
    defer fmt.println("deferred calls still run")
    fmt.println("third: " + scores[2].string())
    fmt.println("fourth: " + scores[3].string())
    return null
}
```

```
$ seed run 04_crash.sd
third: 70
deferred calls still run
crash: index out of range: 3 with length 3
    at main (04_crash.sd:9:30)
$ echo $?
2
```

Below the message, a crash shows where it happened: the function, the file, the line and the column, then each call that led there (`called from ...`), as Go's panics do. [Chapter 14](14-crashes-and-debugging.md) explains these traces and how to run a program in a debugger.

There is no `panic` and no `recover`. To stop a program on purpose, return an error from `main`, or call `os.exit`.

## Exit codes

| How the program ends | What is printed | Exit code |
|---|---|---|
| `main` returns `null` | nothing | 0 |
| `main` returns an error | `error: <message>` on stderr | 1 |
| A crash | `crash: <message>` and where it happened on stderr | 2 |
| `os.exit(n)` | nothing (stdout is flushed) | `n` (its low 8 bits, so use 0–255) |

Returning an error from `main` is the normal way to fail, and `try` makes it easy:

```seed
package main

import "errors"
import "fmt"

fn checkAge(int age) error {
    if (age < 0) {
        return errors.new("age cannot be negative: " + age.string())
    }
    return null
}

fn main([]string args) error {
    fmt.println("checking")
    try checkAge(30)
    try checkAge(-4)
    fmt.println("all good")
    return null
}
```

```
$ seed run 04_main_error.sd
checking
error: age cannot be negative: -4
$ echo $?
1
```

`os.exit(code)` ends the program at once with a code of your choice, which is useful for command-line tools. Deferred calls do not run.

```seed
package main

import "fmt"
import "os"

fn main([]string args) error {
    if (args.len() < 2) {
        fmt.eprintln("usage: 04_exit <name>")
        os.exit(64)
    }
    fmt.println("hello " + args[1])
    return null
}
```

## defer

`defer f(x)` runs the call when the surrounding function returns, however it returns, and also when it crashes. Several deferred calls run in reverse order. The arguments are evaluated when the `defer` statement runs, not when the call happens. So is the receiver of a method: `defer s.close()` closes the `s` of that moment, a copy. If `s` were given another file or connection later (a log that is replaced when it is compacted), the deferred call would close the old one and leave the new one open, so the compiler rejects that: assigning `s` (or a variable passed to the deferred method) after `defer s.close()` is an error, `s is assigned after the defer of s.close() on line 12, ...`. Close each value where it is replaced and the last one at the end instead, or, in a loop, defer the close right after each assignment (each value then has a deferred call of its own).

```seed
package main

import "fmt"

fn step(string name) {
    fmt.println("  undo " + name)
}

fn work(bool fail) int {
    fmt.println("start")
    defer step("first")
    defer step("second")
    if (fail) {
        fmt.println("failing early")
        return 1
    }
    fmt.println("finished")
    return 0
}

fn main([]string args) error {
    int a = work(false)
    int b = work(true)
    fmt.println(a.string() + " " + b.string())
    return null
}
```

```
start
finished
  undo second
  undo first
start
failing early
  undo second
  undo first
0 1
```

Two rules keep `defer` honest:

- A deferred call that returns an error must discard it in plain sight: `defer _ = fs.remove(tmp)`.
- A `mut` method can't be deferred (`defer list.push(x)`), because the variable it would change is gone by the time the call runs.

Next: [Null safety](05-null-safety.md).
