# 2. Basics

## Types and literals

| Type | Literals | Notes |
|---|---|---|
| `int` | `42`, `-7`, `0xff` | The only integer type. It grows as needed and never overflows. |
| `float` | `3.14`, `1.0e9` | 64-bit. A float literal always contains a `.`, so `1` is an `int` and `1.0` a `float`. |
| `bool` | `true`, `false` | |
| `byte` | `'a'`, `'\n'`, `'\x7f'` | One 8-bit value. Single quotes are always a byte. |
| `string` | `"hi\n"` | Immutable bytes, normally UTF-8. Escapes: `\n \t \r \0 \\ \" \' \xHH`. No raw or multi-line strings. |
| `[]T` | `[]int{1, 2}`, `[]int{}` | A list. The type is always written in the literal. |
| `map[K]V` | `map[string]int{"a": 1}` | Keys are `int`, `byte`, `bool` or `string`. Keeps insertion order. |
| `T?` | `null` | A `T` that may be missing. No other type can be `null`. |
| `error` | `null`, `errors.new("...")` | The built-in error interface. `null` means "no error". |

You can also define `struct`, `enum` and `interface` types, use function types such as `fn(int) bool`, and, for concurrency, channels `chan T`. Later chapters cover each.

```seed
package main

import "fmt"

fn main([]string args) error {
    int count = 42
    int mask = 0xff
    float ratio = 2.5
    float huge = 1.0e9
    bool done = false
    byte letter = 'a'
    string greeting = "hi\tthere"
    []int primes = []int{2, 3, 5, 7}
    map[string]int ages = map[string]int{"ann": 31, "bob": 27}
    string? nickname = null

    fmt.println(count.string() + " " + mask.string())
    fmt.println(ratio.string() + " " + huge.string())
    fmt.println(done.string())
    fmt.println(letter.string())
    fmt.println(greeting)
    fmt.println(primes.len().string() + " primes, " + ages.len().string() + " ages")
    fmt.println((nickname == null).string())
    return null
}
```

```
42 255
2.5 1000000000.0
false
97
hi	there
4 primes, 2 ages
true
```

Note that `letter.string()` is `"97"`: a byte is a number. To turn bytes into text, use `string(xs)` on a `[]byte` (see [Strings](#strings)).

## Declarations

A declaration always has the same shape: the type, the name, `=`, the value.

```seed
package main

import "fmt"

const int maxUsers = 100
const string appName = "seedling"

fn main([]string args) error {
    int count = 0
    count = count + 1
    count++

    []string names = []string{}
    names.push("ann")

    string? email = null
    email = "ann@example.com"

    const float taxRate = 0.25

    fmt.println(appName + ": " + count.string() + " of " + maxUsers.string())
    fmt.println(names[0] + " " + taxRate.string())
    if (email != null) {
        fmt.println(email)
    }
    return null
}
```

```
seedling: 2 of 100
ann 0.25
ann@example.com
```

The rules:

- **The type comes first and is always written.** There is no `var`, no `:=` and no type inference. Reading a declaration never requires looking at the expression on the right.
- **Every variable is initialised.** There are no zero values: `int n` alone does not compile. A variable that may have no value yet is a `T?` set to `null`. When the value comes from one of several branches, declare `string? text = null` and assign it in every branch: after the `if`, the compiler knows it is a plain `string` ([narrowing](05-null-safety.md#narrowing)).
- **No shadowing.** Declaring a name that already exists in an enclosing scope is an error, so a name means the same thing throughout a function. The compiler suggests assigning with `=` instead.
- **Assignment is `=`, nothing else.** There is no `+=` and the like: write `count = count + 1`. `count++` and `count--` exist, but they are statements, not expressions: `x = i++` doesn't compile.
- **Names are checked.** Types and enum cases are `PascalCase`; everything else, constants included, is `camelCase`.

When a function returns several values, each name with a type in front of it is declared, and each name without one is an existing variable being assigned:

```seed
int n, error err = parse(a)    // declares n and err
n, err = parse(b)              // assigns both
int m, err = parse(c)          // declares m, reuses err
```

### Constants

A constant uses the same form with `const` in front. Its value must be computable at compile time. Constants are allowed at package level and inside functions, and their names are `camelCase` like everything else:

```seed
const int maxUsers = 100
const []string weekend = []string{"sat", "sun"}
```

There are no package-level variables, only constants. Goroutines share nothing (see [chapter 10](10-concurrency.md)), so there is no global mutable state either.

## Operators

| Kind | Operators | Works on |
|---|---|---|
| Arithmetic | `+ - * / %` | `int`, `float` (`%` only on `int`) |
| Joining | `+` | `string` |
| Bitwise | `& \| ^ << >>` | `int`, `byte` |
| Comparison | `== !=` | Every type except channels and functions. Structs, lists and maps compare by contents. A nullable `T?` compares with `null` or with a `T` (false when it is null), not with another `T?` ([chapter 5](05-null-safety.md#the-rules)). |
| Ordering | `< <= > >=` | `int`, `float`, `string`, `byte` |
| Logic | `&& \|\| !` | `bool` |

Precedence follows Go. Comparisons do not chain: `a < b < c` is an error. There is no ternary `?:`.

**Both sides of an operator must have the same type.** There are no implicit conversions at all, not even from `int` to `float`. Convert explicitly by calling the type like a function: `float(n)`, `int(f)` (truncates toward zero), `byte(i)`, `int(b)`.

```seed
package main

import "fmt"

fn main([]string args) error {
    // Integer division truncates toward zero.
    fmt.println((7 / 2).string() + " " + (-7 / 2).string() + " " + (7 % 3).string())

    // int never overflows: it grows as needed.
    int big = 1
    for (int i = 0; i < 100; i++) {
        big = big * 2
    }
    fmt.println(big.string())

    // No implicit conversions: convert explicitly.
    int n = 3
    float half = float(n) / 2.0
    fmt.println(half.string() + " " + int(2.9).string())

    // Bitwise operators work on int and byte.
    fmt.println((0xf0 | 0x0f).string() + " " + (1 << 10).string())

    // Strings compare byte by byte.
    fmt.println(("apple" < "banana").string() + " " + ("a" + "b" == "ab").string())

    bool ok = n > 0 && !(n == 5)
    fmt.println(ok.string())
    return null
}
```

```
3 -3 1
1267650600228229401496703205376
1.5 2
255 1024
true true
true
```

Integer division or `%` by zero crashes the program (see [chapter 4](04-functions-and-errors.md)). Float division by zero follows IEEE and gives `inf`, `-inf` or `nan`.

Why a single growing `int`? A program never has to choose between `int32`, `int64` and `uint`, never converts between them, and never silently wraps around. Small values are stored directly and cost no more than a machine word.

## Strings

A string is an immutable sequence of bytes, normally UTF-8 text. `len()` and indexes count bytes, and `s[i]` is a `byte`. All string operations are methods:

| Method | Result |
|---|---|
| `s.len()` | Number of bytes. |
| `s[i]` | The byte at `i`. Crashes when out of range. |
| `s.slice(a, b)` | Bytes `a` up to (not including) `b`. |
| `s.contains(sub)`, `s.startsWith(p)`, `s.endsWith(p)` | `bool`. |
| `s.index(sub)` | The first byte index of `sub` as an `int?`, or `null`. |
| `s.split(sep)` | `[]string`. |
| `s.trim()` | Without leading and trailing spaces, tabs, `\n` and `\r`. |
| `s.replace(old, new)` | Every occurrence replaced. |
| `s.toUpper()`, `s.toLower()` | ASCII letters only. |
| `s.toInt()`, `s.toFloat()` | `(int, error)`, `(float, error)`. |
| `s.bytes()` | `[]byte`. |
| `s.chars()` | `[]string`, one string per UTF-8 character. |
| `string(xs)` | A `[]byte` turned into a string. |
| `xs.join(sep)` | On a `[]string`: the strings joined with `sep`. |

```seed
package main

import "fmt"

fn main([]string args) error {
    string s = "  Hello, Seed!  "
    string t = s.trim()
    fmt.println("[" + t + "]")
    fmt.println(t.len().string() + " bytes")
    fmt.println(t.toUpper() + " " + t.toLower())
    fmt.println(t.slice(0, 5))
    fmt.println(t.contains("Seed").string() + " " + t.startsWith("Hell").string())
    fmt.println(t.replace("Seed", "world"))

    int? comma = t.index(",")
    if (comma != null) {
        fmt.println("comma at " + comma.string())
    }

    []string parts = "a,b,c".split(",")
    fmt.println(parts.join(" + "))

    // s[i] is a byte; byte.string() gives its number.
    fmt.println(t[0].string())
    fmt.println(string([]byte{t[0], t[1]}))

    // len() counts bytes, chars() splits into UTF-8 characters.
    string word = "naïve"
    fmt.println(word.len().string() + " bytes, " + word.chars().len().string() + " chars")

    int n, error err = "123".toInt()
    if (err != null) {
        return err
    }
    fmt.println((n + 1).string())
    return null
}
```

```
[Hello, Seed!]
12 bytes
HELLO, SEED! hello, seed!
Hello
true true
Hello, world!
comma at 5
a + b + c
72
He
6 bytes, 5 chars
124
```

`"123".toInt()` can fail, so it returns an error as well as the number. [Chapter 4](04-functions-and-errors.md) explains the pattern.

## Printing

| Function | Writes |
|---|---|
| `fmt.print(s)` | `s` to stdout |
| `fmt.println(s)` | `s` and a newline to stdout |
| `fmt.eprint(s)`, `fmt.eprintln(s)` | the same, to stderr |

Each takes exactly one `string`. `int`, `float`, `bool`, `byte` and `string` have a `string()` method, and pieces are joined with `+` (lists and maps have no text form: build it yourself, as `show` does in [chapter 6](06-lists-and-maps.md)):

```seed
fmt.println("count: " + count.string() + ", ratio: " + ratio.string())
```

There is no `Printf`-style format language and no automatic conversion to text, so `"n = " + n` is a type error. The one way to print a value is to ask for its text with `.string()`. Your own types get a text form by defining a `string()` method (see [chapter 7](07-structs-and-methods.md)).

Next: [Control flow](03-control-flow.md).
