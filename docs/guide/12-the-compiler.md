# 12. The compiler is your friend

The Seed compiler is strict on purpose. It reports **errors only, never warnings**: anything worth mentioning is worth fixing, and a warning that everyone learns to ignore helps nobody. Every error shows the code it is about, explains what is wrong, and suggests a fix. Seed accepts slower compiles as the price for this.

## Reading an error

```seed
package main

import "fmt"

type User struct {
    string name
    int age
}

fn main([]string args) error {
    User u = User{name: "Ann", age: 30}
    fmt.println(u.nmae)
    return null
}
```

```
12_typo.sd:12:19: error: struct User has no field nmae
   12 |     fmt.println(u.nmae)
      |                   ^
    hint: did you mean name?
```

Every message has the same parts:

1. **Where**: `file:line:column`, which most editors can jump to.
2. **What**: one sentence that names the thing involved.
3. **The code**, with a `^` under the exact spot.
4. **A hint** with the fix. Misspelt names get a "did you mean" suggestion.

The compiler reports up to 20 errors at once, sorted by position, and avoids reporting knock-on errors caused by an earlier one, so you can usually fix them all in one pass. Syntax errors come first: when a file has one, the compiler reports one for each broken statement, struct field or other member (including those inside the blocks of an `if` or a function whose first line is broken), but checks types only once the syntax is right.

## The pedantic checks

Beyond the type system, null safety and error handling (chapters [4](04-functions-and-errors.md) and [5](05-null-safety.md)), the compiler rejects code that is probably a mistake, or that makes programs harder to read. Every program below fails to compile, with the message shown.

### Unused variables and imports

```seed
package main

import "fmt"
import "os"

fn main([]string args) error {
    int count = 3
    fmt.println("hello")
    return null
}
```

```
12_unused.sd:4:1: error: "os" is imported but not used
    4 | import "os"
      | ^
    hint: remove the import
12_unused.sd:7:9: error: count is declared but never used
    7 |     int count = 3
      |         ^
    hint: remove it, or use _ where a value must be discarded
```

The same goes for results: calling a function and ignoring what it returns is an error. Assign the result to `_` when you really don't need it.

### Shadowing

```seed
package main

import "fmt"

fn main([]string args) error {
    int total = 0
    for (int i = 0; i < 3; i++) {
        int total = i * 2
        fmt.println(total.string())
    }
    fmt.println(total.string())
    return null
}
```

```
12_shadowing.sd:8:13: error: total is already declared at line 6; shadowing is not allowed
    8 |         int total = i * 2
      |             ^
    hint: assign with = instead, or choose another name
```

In Go, `:=` in an inner block that was meant to be `=` is a classic bug: the outer variable is never updated. In Seed a name always means one thing within a function.

### Unreachable code and missing returns

```seed
package main

import "fmt"

fn sign(int n) string {
    if (n < 0) {
        return "negative"
    } else {
        return "not negative"
    }
    fmt.println("done")
}

fn main([]string args) error {
    fmt.println(sign(3))
    return null
}
```

```
12_unreachable.sd:11:5: error: unreachable code
   11 |     fmt.println("done")
      |     ^
    hint: the statement before it always returns, breaks, continues or exits
```

Likewise, a function with results that can reach its closing brace is an error ("missing return at the end of ..."). The compiler knows that a `switch` covering every case of an enum, with each case returning, always returns.

### Exhaustive switches

```seed
package main

import "fmt"

type Light enum {
    Red
    Yellow
    Green
}

fn next(Light l) Light {
    switch (l) {
    case Light.Red:
        return Light.Green
    case Light.Green:
        return Light.Yellow
    }
}

fn main([]string args) error {
    fmt.println((next(Light.Red) == Light.Green).string())
    return null
}
```

```
12_missing_case.sd:12:5: error: switch over Light does not handle case Light.Yellow
   12 |     switch (l) {
      |     ^
    hint: add a case for it; a switch over an enum must list every case
```

A switch over an `int`, `byte`, `string` or `bool` must end with `default` instead.

### Naming

```seed
package main

import "fmt"

const int MAX_SIZE = 10

fn main([]string args) error {
    fmt.println(MAX_SIZE.string())
    return null
}
```

```
12_naming.sd:5:11: error: the constant MAX_SIZE must be written in camelCase
    5 | const int MAX_SIZE = 10
      |           ^
    hint: rename it to maxSize
```

Types and enum cases are `PascalCase`; functions, methods, variables, parameters, fields and constants are `camelCase`. Since every Seed program uses the same names in the same style, nobody has to argue about it, and a name tells you whether it is a type.

### No implicit conversions

```seed
package main

import "fmt"

fn main([]string args) error {
    int items = 3
    float price = 2.5
    float total = items * price
    fmt.println(total.string())
    return null
}
```

```
12_mixed_types.sd:8:25: error: mismatched types int and float for *
    8 |     float total = items * price
      |                         ^
    hint: there are no implicit conversions; use float(x), int(x), x.string(), ...
```

Write `float(items) * price`.

### implements must match

```seed
package main

import "fmt"

type Named interface {
    name() string
}

type Dog struct implements Named {
    string label
}

fn (Dog d) name(bool loud) string {
    if (loud) {
        return d.label.toUpper()
    }
    return d.label
}

fn main([]string args) error {
    Named n = Dog{label: "rex"}
    fmt.println(n.name())
    return null
}
```

```
12_implements.sd:13:12: error: method Dog.name(bool) string does not match Named.name() string
   13 | fn (Dog d) name(bool loud) string {
      |            ^
    hint: Dog declares implements Named, so the signatures must be the same
```

Only the mismatch is reported: `Dog` still counts as a `Named` everywhere else, so `Named n = Dog{...}` is not a second error.

### Syntax that doesn't exist

Some things from other languages are left out on purpose, and the compiler says what to write instead:

```seed
package main

import "fmt"

fn main([]string args) error {
    int n = 1
    n += 2
    fmt.println(n.string())
    return null
}
```

```
12_syntax_compound.sd:7:7: error: there is no compound assignment operator +=
    7 |     n += 2
      |       ^
    hint: write it out: x = x + y
```

### The full list

The compiler rejects:

| Check | Chapter |
|---|---|
| A type mismatch, including any implicit conversion | [2](02-basics.md) |
| A variable declared without a value | [2](02-basics.md) |
| Shadowing | [2](02-basics.md) |
| A name that breaks the naming rules | [2](02-basics.md) |
| A switch over an enum that misses a case or has `default` | [3](03-control-flow.md), [8](08-enums.md) |
| A case pattern that doesn't bind every field of its case | [8](08-enums.md) |
| A switch over a value without `default` | [3](03-control-flow.md) |
| An unhandled error | [4](04-functions-and-errors.md) |
| A value read before the error returned with it is checked | [4](04-functions-and-errors.md) |
| A value kept beside a discarded error as a plain `T` | [4](04-functions-and-errors.md) |
| `try` in a function that doesn't return an error | [4](04-functions-and-errors.md) |
| A missing `return` | [4](04-functions-and-errors.md) |
| A nullable value used without a null check | [5](05-null-safety.md) |
| A `mut` method called on a parameter | [7](07-structs-and-methods.md) |
| A struct literal that misses a field without a default value | [7](07-structs-and-methods.md) |
| A field's default value that calls a function or could fail | [7](07-structs-and-methods.md) |
| A type whose methods don't match its `implements` | [9](09-interfaces-and-functions.md) |
| `spawn` outside a group, a spawned `mut` method, `return` or `try` inside a group block | [10](10-concurrency.md) |
| A name, field or method of another package that isn't `pub` | [11](11-packages.md) |
| An import cycle | [11](11-packages.md) |
| `assert` outside a test | [11](11-packages.md) |
| An unused variable, import or result | this chapter |
| Unreachable code | this chapter |
| Unformatted code | below |

## Formatting

Formatting is part of the language: there is one layout, and `seed fmt` produces it. Given this file:

```seed
package main
import "fmt"
fn main([]string args) error {
  int x=1+2
    if (x>2) { fmt.println("big") } else { fmt.println("small") }
  []int xs = []int{1, 2,
     3}
  fmt.println(xs.len().string())
  return null }
```

Compiling it fails at the first line that differs from the formatted text, and the hint says how to fix it:

```
$ seed run messy.sd
messy.sd:2:1: error: messy.sd is not formatted
    2 | import "fmt"
      | ^
    hint: run: seed fmt messy.sd
```

If the file also has other errors, the compiler still checks it and reports those first, followed by the formatting error, so formatting never hides a real mistake.

`seed fmt --check` reports the same without compiling (useful in a build script):

```
$ seed fmt --check messy.sd
messy.sd:2: not formatted
    hint: run: seed fmt messy.sd
```

`seed fmt messy.sd` rewrites it:

```seed
package main

import "fmt"

fn main([]string args) error {
    int x = 1 + 2
    if (x > 2) {
        fmt.println("big")
    } else {
        fmt.println("small")
    }
    []int xs = []int{
        1,
        2,
        3,
    }
    fmt.println(xs.len().string())
    return null
}
```

The formatter only changes spaces, line breaks and the trailing comma of lists; it never changes what the program means. You still control two things:

- **Where long lines break.** There is no line-length limit. A statement continues on the next line after an operator or a comma, and the formatter keeps your line breaks and indents the continuation.
- **Whether a list is on one line or one element per line.** If you break a list (arguments, a literal, parameters) anywhere, the formatter puts each element on its own line with a trailing comma, like `xs` above. Otherwise it stays on one line.

The complete rules are in [formatting.md](../formatting.md).

Next: [Seed for Go programmers](13-for-go-programmers.md).
