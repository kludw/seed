# Seed — language spec

Seed is a small, strongly typed language inspired by Go. It is a proof of concept.

This document is the reference for the language. The details live in four companion documents, which are part of the spec: [docs/grammar.md](docs/grammar.md) (the syntax), [docs/builtins.md](docs/builtins.md) (the methods of the built-in types and the core packages), [docs/stdlib.md](docs/stdlib.md) (the standard library) and [docs/formatting.md](docs/formatting.md) (the one canonical layout). Decisions that were made during implementation, without the author, are logged in [DECISIONS.md](DECISIONS.md) for review.

## Principles

1. **One way to do things.** No syntax sugar, except `try` for passing errors up.
2. **Nothing implicit.** No hidden control flow, no `init()`, no implicit conversions, no code that runs behind your back.
3. **Everything you need is built in.** A large standard library and a single toolchain binary, with no third-party dependencies.
4. **The compiler is a strict friend.** It is pedantic, it reports errors only (no warnings), and every error explains itself and suggests a fix. Slower compiles are an acceptable cost.

## Syntax basics

- Comments use `//`. There are no block comments.
- Statements end at the end of a line. There are no semicolons: one statement per line. (`;` only separates the three parts of a `for` header.)
- A line that continues must end with an operator, a comma or an opening bracket (Go's newline rule, see [grammar.md](docs/grammar.md#newlines)).
- Braces are always required, and the opening brace goes on the same line.
- Conditions are always in parentheses: `if (...)`, `for (...)`, `switch (...)`.
- Formatting is part of the language. Code that isn't formatted fails to compile, and the error gives the command that fixes it.
- Naming is enforced by the compiler. Types and enum cases use `PascalCase`, and everything else uses `camelCase`, including constants.

## Literals

| Literal            | Type     | Notes                                                      |
|--------------------|----------|------------------------------------------------------------|
| `42`, `0xff`       | `int`    | Decimal or hexadecimal, of any size. No octal, binary or `_` separators. A leading `-` is the unary operator. |
| `3.14`, `1.0e9`    | `float`  | Must contain a `.`; `1e9` alone is not a float.            |
| `'a'`, `'\n'`      | `byte`   | A single byte. Escapes are the same as in strings.         |
| `"hi\n"`           | `string` | Escapes: `\n \t \r \0 \\ \" \' \xHH`. Any other escape is an error. No raw or multi-line strings. |
| `true`, `false`    | `bool`   |                                                            |
| `null`             | any `T?` | Also the "no error" value of `error`.                      |
| `[]int{1, 2}`      | `[]int`  | The type is always written. `[]int{}` is an empty list.    |
| `map[string]int{"a": 1}` | `map[string]int` | Keys may be `int`, `byte`, `bool` or `string`. |
| `Point{x: 1, y: 2}` | `Point` | A struct literal sets every field by name.                 |
| `chan int(10)`     | `chan int` | A new channel with a buffer of 10 (`0`: unbuffered).     |

## Types

| Type          | Meaning                                                            |
|---------------|--------------------------------------------------------------------|
| `int`         | The only integer type. It grows as needed and never overflows.    |
| `float`       | A 64-bit floating-point number.                                   |
| `bool`        | `true` or `false`.                                                |
| `byte`        | An 8-bit value, used for raw I/O.                                 |
| `string`      | Immutable bytes, normally UTF-8 text (any bytes are allowed). `s[i]` returns a `byte`, and `s.chars()` returns the characters. |
| `[]T`         | A list that grows in place.                                       |
| `map[K]V`     | A map that keeps insertion order.                                 |
| `chan T`      | A channel.                                                        |
| `T?`          | A nullable `T`.                                                   |
| `fn(T, ...) R`| A function value (see [Function values](#function-values)).       |
| `struct`, `enum`, `interface` | User-defined types.                               |
| `error`       | The built-in error interface.                                      |

- There are no implicit conversions. Converting is always explicit: `float(i)`, `int(f)` (truncates), `byte(i)` (crashes outside 0–255), `int(b)`, `string(bytes)` for a `[]byte`. Text is made with `.string()` and parsed with `s.toInt()` and `s.toFloat()`.
- `?` applies to the whole type on its left: `[]string?` is a nullable list, and a list of nullable strings is `[](string?)`. `T??` is not a type.
- The built-in types (`[]T`, `map[K]V`, `chan T`, `T?`) are generic. User code cannot define generic types or functions.
- A struct cannot contain itself by value (directly or through other structs); break the cycle with a nullable field (`Node? next`), a list or an enum.

The methods of every built-in type are listed in [builtins.md](docs/builtins.md). The most common ones:

### Lists

| Method          | Result                                                        |
|-----------------|---------------------------------------------------------------|
| `len()`         | The number of elements.                                       |
| `push(x)`       | Adds `x` to the end. `mut`.                                   |
| `pop()`         | Removes the last element and returns it as `T?`, or `null` if the list is empty. `mut`. |
| `insert(i, x)`  | Inserts `x` at index `i`. `mut`.                              |
| `remove(i)`     | Removes the element at index `i`. `mut`.                      |
| `contains(x)`   | `bool`.                                                       |
| `index(x)`      | The first index of `x` as `int?`, or `null` if it is absent.  |
| `slice(a, b)`   | A copy of elements `a` up to (not including) `b`.             |
| `reverse()`     | Reverses the list in place. `mut`.                            |
| `sort()`        | Sorts in place. Only for lists of `int`, `float`, `string` or `byte`. `mut`. |
| `sortBy(f)`     | Sorts in place (stable) using `f`, a `fn(T, T) bool` that returns true when the first argument goes first. `mut`. |
| `join(sep)`     | Only for `[]string`: the strings joined with `sep` between them. |

- `push` is the only way to add to the end of a list. There is no separate `append`.
- Indexing with `xs[i]` out of bounds crashes the goroutine.

### Maps

| Operation       | Result                                                        |
|-----------------|---------------------------------------------------------------|
| `m[k]`          | The value as `V?`, or `null` if the key is missing.           |
| `m[k] = v`      | Sets a value. A new key goes to the end of the order.         |
| `len()`         | The number of entries.                                        |
| `delete(k)`     | Removes a key. `mut`.                                         |
| `keys()`        | The keys as `[]K`, in insertion order.                        |

- Maps keep insertion order, so iterating over them is deterministic. Two maps are equal when they have the same entries, in any order.

### Converting to text

- `int`, `float`, `bool`, `byte` and `string` have a `string()` method: `count.string()`, `ratio.string()`, `done.string()`. `byte.string()` gives the number (`'a'.string()` is `"97"`); `string([]byte)` builds text from bytes.
- Lists, maps and user types have no automatic text form. A user type that needs one writes its own `string()` method.

## Printing

```
fmt.println("count: " + count.string())
fmt.print("name? ")
```

- `fmt.print(string)` and `fmt.println(string)` take exactly one `string`. `println` adds a newline. `fmt.eprint` and `fmt.eprintln` write to stderr.
- Values are turned into text explicitly with `.string()` and joined with `+`. There is no format-string mini-language and no hidden conversion.

## Variables and constants

```
int count = 0
[]string names = []string{}
string? email = null
count = count + 1

const int maxUsers = 100
```

- A declaration always writes the type first, then the name, then `= value`. There is no `var`, no `:=` and no type inference.
- Every declaration must have a value, including nullable ones (`string? email = null`). There are no zero values.
- Shadowing is a compile error. Declaring a name that already exists in an enclosing scope (a local, a parameter, a package-level name or an imported package) fails, and the error suggests assigning with `=` instead.
- With several return values, a name with a type in front of it is declared, and a name without one is an existing variable (or a field or element) being assigned. `_` discards a value:

```
[]string names, error err = loadNames(path)
string text, err = fs.read(other)     // text is new, err is reused
```

- A `const` uses the same form as a variable. Its value must be computable when the program is compiled: literals, other constants, operators, conversions, and built-in methods that compute a value (such as `.len()`), but no calls of functions or methods, no variables, `try`, `group` or channels. Constants are allowed both at package level and inside functions.
- Package-level variables are not allowed, only `const`. Goroutines are isolated, so there can be no global mutable state.

## Operators

| Kind       | Operators                     | Works on                         |
|------------|-------------------------------|----------------------------------|
| Arithmetic | `+ - * / %`                   | `int`, `float` (`%` on `int` only) |
| Joining    | `+`                           | `string`                         |
| Bitwise    | `& \| ^ << >>`                | `int`, `byte`                    |
| Comparison | `== !=`                       | Every type except channels and function values (and types that contain them). |
| Ordering   | `< <= > >=`                   | `int`, `float`, `string`, `byte` |
| Logic      | `&& \|\| !`                   | `bool`                           |
| Unary      | `-` (numbers), `!` (`bool`)   |                                  |

- `==` compares contents: structs field by field, lists element by element, maps by their entries (ignoring order), enums by case and data, interface values by their dynamic type and value, and errors by their messages.
- Integer division truncates toward zero, and `%` has the sign of the left operand. Integer division and `%` by zero crash. Float division by zero follows IEEE and gives `inf`, `-inf` or `nan`.
- Both sides of an operator must have the same type, so mixing `int` and `float` is a compile error: `float(n) * 1.5`. Bytes have no arithmetic; convert with `int(b)`.
- There is no compound assignment (`+=` and the like). Write `x = x + 1`. `x++` and `x--` are statements.
- There is no ternary `?:` operator and no unary `^`.
- Precedence follows Go ([grammar.md](docs/grammar.md#expressions)). Comparisons do not chain: `a < b < c` is an error.

## Null

- No type can be null unless its type says so. A `T?` can be null, while a plain `T` never is (the one exception is `error`, see [Errors](#errors)).
- A `T` converts to `T?` automatically. A `T?` cannot be used as a `T` until the compiler can see that it is not null. This is called *narrowing*:

```
if (u.email != null) {
    send(u.email)        // u.email is a string inside this block
}
```

- Narrowing applies to variables and their fields (`x`, `u.email`, `u.address.city`), not to list elements, map lookups or call results: store those in a variable first (`int? n = m[k]`).
- A condition narrows where it is known to be true: `x != null` inside the `if` block, `x == null` in its `else`, and through `&&`, `||` and `!` (`if (x != null && x.len() > 0)`, `if (x == null || x.len() == 0)`).
- After an `if` whose block always leaves (`return`, `break`, `continue`, a call of `os.exit` or `os.crash`), the rest of the block knows the opposite of its condition:

```
if (x == null) {
    return null, errors.new("missing")
}
use(x)                   // x is a T from here on
```

- In a test, `assert(cond)` narrows like such an `if`.
- After an assignment of a non-null value, `x = <non-null value>`, `x` is treated as `T` until it may become null again, so `if (x == null) { x = "default" }` leaves `x` non-null after the `if`.
- Narrowing ends when the variable (or a part of the path, such as `u` for `u.email`) is assigned a value that may be null, or when a `mut` method is called on it or on a part of the path. Before a loop, every path the loop assigns or mutates loses its narrowing, because the loop may come around.

## Values, not pointers

- Seed has no pointers. Every value behaves like an independent copy.
- Copies are cheap. Data is shared until someone modifies it, and only then is it actually copied (copy-on-write).
- Function arguments are read-only. To change a value, a function returns a new one, or the value is changed through a `mut` method.
- Channels are the one exception: a channel value is a handle to a shared channel (see [Channels](#channels)).

## Functions and methods

```
fn add(int a, int b) int {
    return a + b
}

fn divide(int a, int b) (int, error) {
    if (b == 0) {
        return null, errors.new("divide by zero")
    }
    return a / b, null
}
```

- Parameters put the type first (`int a`), and the return type comes after the parameter list.
- Functions can return several values. There are no named returns and no naked `return`.
- There are no variadic functions. A function that needs several values takes a list: `sum([]int{1, 2, 3})`.
- There is no overloading and there are no default arguments.

### Methods

```
fn (User u) name() string {
    return u.name
}

fn (User u) mut tag(string t) {
    u.tags.push(t)
}
```

- A method is declared on a struct or enum type of the same package. There is only one kind of receiver, because there are no pointers.
- A plain method only reads its receiver.
- A `mut` method changes the variable it was called on. Built-in methods such as `xs.push(x)` follow the same rule.
- A `mut` method can be called on a local variable or on any part of one: `u.tag("x")`, `u.address.set(...)`, `users[0].tag("x")`.
- A `mut` method cannot be called on a function parameter, because parameters are read-only. Copy the parameter into a local variable first.

### Function values

```
fn olderThan(Person a, Person b) bool {
    return a.age > b.age
}

people.sortBy(olderThan)
```

- A named function can be passed as a value. Its type is written `fn(Person, Person) bool`.
- There are no anonymous functions and no closures, so a function value never carries hidden state.
- A nullable function type needs parentheses: `(fn(int) int)?`, because `fn(int) int?` is a function that returns `int?`.
- A struct field of function type is called directly: `op.apply(1, 2)` (when the struct has no method of that name).
- Function values cannot be compared with `==`.

## Structs, enums, interfaces

```
pub type User struct implements Named {
    pub string name
    string? email
    []string tags
}

type Shape enum {
    Circle(float radius)
    Rect(float w, float h)
}

pub type Named interface {
    name() string
}
```

- A struct literal must set every field: `User{name: "k", email: null, tags: []string{}}`.
- Struct fields are private to their package unless marked `pub` (see [Packages](#packages)).
- An enum case is always written with its type, both when creating a value and in a `switch`: `Shape.Circle(2.0)`, `case Shape.Circle(float r):`. A case without data has no parentheses: `Token.Eof`.
- A case pattern matches its enum case exactly: when the case carries data, the pattern binds every field with its type (`case Shape.Rect(float w, float h):`), using `_` as the name of a field it doesn't need (`case Shape.Rect(float w, float _):`); a case without data is written without parentheses (`case Token.Eof:`). A `case` with several labels can't bind names, so all its bindings are `_`: `case Shape.Circle(float _), Shape.Point:`.
- An enum case can carry data, and a `switch` over an enum must cover every case.
- A struct or enum satisfies an interface only by declaring it with `implements` (several interfaces are separated by commas). The compiler then checks that every required method exists, with exactly the same signature (including `mut`). Methods cannot be declared on an interface type.
- A value of such a type converts to the interface (or the nullable interface) automatically wherever the interface is expected. There is no way to get the original type back out of an interface value (no type switches or assertions).
- An interface method can be `mut`. Like any `mut` method, it can only be called on a variable or a part of one, and it changes only that interface value.

## Control flow

```
if (x > 0) {
    ...
} else if (x < 0) {
    ...
} else {
    ...
}

for (int i = 0; i < n; i++) {
    ...
}
for (;;) {
    ...
}

switch (s) {
case Shape.Circle(float r):
    return 3.14 * r * r
case Shape.Rect(float w, float h):
    return w * h
}

switch (c) {
case 'a', 'e', 'i', 'o', 'u':
    vowels++
default:
    others++
}

defer f.close()
```

- `for` has exactly one form, C-style. Each of its three parts may be empty, and an infinite loop is `for (;;)`. The first part is a declaration or an assignment, the last an assignment or `i++`/`i--`.
- `i++` and `i--` are statements, not expressions, so `x = i++` does not compile.
- `switch` cases never fall through.
- A `switch` over an enum must list every case and cannot have `default`, so adding a case later makes the compiler point at every `switch` that needs updating.
- A `switch` over an `int`, `byte`, `string` or `bool` must end with `default`. One case can list several values.
- Variables bound in a `case` also write their type, like every other declaration.
- `break` and `continue` work in loops. There are no labels and no `goto`. `break` and `continue` inside a `switch` or `select` apply to the enclosing loop.
- `defer` runs when the enclosing function returns (or its goroutine crashes or is stopped). Several defers run in reverse order (last first). The call's arguments are evaluated when the `defer` statement runs. `os.exit` does not run defers.
- `defer` takes a call of a function, method, function value, interface method or package function, or `ch.close()`, but not of a `mut` method: the variable it would change is gone when the call runs.
- A deferred call that returns an error must discard it explicitly: `defer _ = f.close()`.

## Errors

- `error` is an interface with a `message() string` method. A user type can implement it (`type ParseError struct implements error`), and then converts to `error` automatically.
- `error` is the one type that can be `null` without a `?`: a `null` error means "no error".
- A function that can fail returns `(..., error)`, with the error last. When the error is not null, the other return values are null.
- Every error must be handled. Ignoring a returned error is a compile error, and so is ignoring any other result: discard it with `_ = f()`.

```
[]string names, error err = loadNames(path)
if (err != null) {
    return null, err
}
```

- **Error guards.** A plain `T` declared or assigned beside an error cannot be read until the error is known to be null: inside `if (err == null)`, after `if (err != null) { return ... }` (or any block that always leaves), right of `err == null &&`, or after `assert(err == null)` in a test. Assigning the variable again also lifts the guard. A `T?` declared beside an error is not guarded, because it must be checked against null anyway.
- Writing `string text = try fs.read(path)` returns `null, ..., err` from the current function when `fs.read` fails. `try` can be used on any call whose last result is `error` (and on a `group`), anywhere in an expression, in a function whose last result is `error`. It is the only syntax sugar in the language.
- `return null, err` gives a plain `T` result as `null` and is only meant together with an error. Returning a literal `null` for a plain `T` beside a null error is a compile error, and returning `null` beside an error that turns out to be null crashes (`f returned null as result 1 without an error`).
- `_` discards an error explicitly. If a value comes back alongside the discarded error, that value must be declared as `T?`, and the compiler rejects a plain `T`:

```
string? data, _ = fs.read(path)    // must be checked before use
_ = fs.remove(path)                // fine: nothing else was returned
```

- Runtime faults, such as an out-of-bounds index or division by zero, crash only the goroutine they happen in. Library code reports a caller's mistake the same way, with `os.crash(message)`. See [Concurrency](#concurrency).

## Concurrency

```
chan int results = chan int(10)
error err = group {
    spawn count(names, results)
    spawn count(names, results)
}
if (err != null) {
    ...
}
```

- `spawn` is only allowed inside a `group` block, and the group waits for every goroutine it started. Goroutines cannot leak.
- Failures work like Erlang:
  - When a goroutine returns an error or crashes, the other goroutines in its group are stopped, and the group returns the first error. A crash becomes the error `crash: <message>`.
  - A goroutine can only be stopped at a *stop point*, and its `defer`s still run. The stop points are channel sends and receives, `select`, `time.sleep`, and the standard library's blocking calls: reading stdin (`io.readLine`, `io.readAll`), network I/O (`net`, and so `http`), and waiting for a child process (`os.run`). File I/O (`fs`) and `os.output` are not stop points.
  - An error that nothing handles ends up in `main`, and the program exits with a clear message.
- Goroutines share nothing. Sending a value on a channel copies it, so data races are impossible.
- A `group` block is ordinary code: `spawn` may appear anywhere inside it, including in loops and `if`s.
- Channels are the one reference type. Copying a channel value (into a struct, an enum, a list, another channel) gives another handle to the same channel, which is how reply channels work.
- When every goroutine is blocked on a channel or waiting for a group, and no timer is pending, nothing can happen any more: the program prints `crash: deadlock: all goroutines are blocked` and exits with code 2, without running `defer`s.

### Groups

- `group { ... }` is an expression of type `error`: null when every goroutine returned null, otherwise the first error. Its result must be handled like any other error, so a group can only be used whole: `error err = group {...}`, `err = group {...}`, `return group {...}`, `_ = group {...}` or `try group {...}`. A `group` on its own as a statement is an unchecked error.
- The block runs in the goroutine that reaches it, like any other code, and then the group waits for the goroutines the block started. The block cannot leave early: `return` and `try` are not allowed inside it, nor are `break` and `continue` of a loop outside it. Loops inside the block work as usual. A `defer` inside the block belongs to the function, as everywhere else.
- `spawn` belongs to the innermost group block around it. Groups nest: a spawned function can run groups of its own, and stopping it stops them too.
- When the group fails, only the goroutines it started are stopped, not the code of the block itself. So a block should only start goroutines; waiting for their results belongs in a goroutine or after the group. (A block that waits for a channel that only stopped goroutines would have served ends in the deadlock crash.)

### Spawn

- `spawn` takes a call of a function, a method, a function value, an interface method or a package function, which must return `error` or nothing. Results come back on channels.
- The function value, the receiver and the arguments are evaluated when `spawn` runs and deep-copied for the new goroutine, so later changes on either side are not seen by the other (channels inside them are shared, as always).
- A `mut` method cannot be spawned: the goroutine would only change its own copy of the receiver.

### Channels

- `chan T(n)` creates a channel with a buffer of `n`, and `n = 0` means unbuffered. A negative `n` crashes.
- `ch.send(v)` sends a copy of the value, waiting while the buffer is full. Sending on a closed channel crashes the goroutine.
- `ch.receive()` returns `T?`, and returns `null` once the channel is closed and empty. For a `chan error` it returns `error`, which is also null then.
- `ch.close()` closes the channel; buffered values can still be received. Closing it twice crashes. `close` is not a stop point, so `defer ch.close()` works (it is the one built-in method call that `defer` accepts).
- `send`, `receive` and `close` are not `mut`: a channel parameter can be used directly.
- Channel elements cannot be nullable: `receive()` already uses null for "closed". `chan int?` is a nullable channel; a channel of nullable values would be written `chan (int?)` and is rejected.
- Channels have no `==`, and a package-level constant cannot hold a channel (each goroutine computes the constants it uses for itself).

### Select

```
select {
case int? n = results.receive():
    ...
case jobs.send(j):
    ...
case time.after(5 * time.second).receive():
    ...
}
```

- `select` waits until one of its channel operations can proceed, then runs that case. When several can, one is chosen at random.
- A case is `ch.send(v)`, `ch.receive()`, `_ = ch.receive()` or `T? v = ch.receive()`, which declares a new variable for that case. It cannot assign to an existing variable.
- The channels and the values to send of every case are evaluated first, in order, before `select` waits.
- There is no `default` case, so `select` always waits.
- `break` and `continue` inside a case apply to the enclosing loop.
- Timeouts are ordinary channels, such as `time.after(...)`, which receives the time once and is then closed.

## Packages

```
shop/
    main.sd                 package main      (the program)
    cart/
        cart.sd             package cart      import "cart"
        tests/cart_test.sd  package cart      (tests, compiled by seed test only)
    shop/
        money/money.sd      package money     import "shop/money"
```

- A package is a directory. Every `.sd` file directly in it belongs to the package and starts with the same `package name` line; the name is the directory's name, except for `package main`. Package `main` (the program) can't be imported.
- Imports come after the `package` line, one per line: `import "fmt"`. They are per file: a file imports the packages it uses itself. An unused import is a compile error, and so is an import cycle (the error shows it: `import cycle: a -> b -> a`).
- `import "path"` looks for the package in the standard library first (`lib/path` next to the `seed` toolchain), then in the directory `path` relative to the **project root**, the directory of the program's `package main` (or the directory given to `seed test`). There is no project file. Import paths are lowercase names separated by `/`. Code calls the package by the last element of the path: after `import "shop/money"`, it writes `money.Amount`. A project package with the name of a standard library package can't be imported (the standard library wins).
- A name from another package is always written with the package name: `cart.newCart()`, `cart.Item`, `cart.maxItems`, `shapes.Shape.Circle(2.0)`, `case shapes.Shape.Circle(float r):`, `cart.Item{name: "pen"}`. A local or package-level name can't be the same as an imported package's name.
- Everything is private. Mark a top-level name (function, method, type, constant) `pub` to make it usable from other packages. Struct fields are private too, unless marked `pub` (`pub string name`); another package can only read and write `pub` fields, and can only write a struct literal of the type if every field is `pub`. Enum cases and interface methods are as visible as their type.
- Nothing runs implicitly when a program starts or a package is imported.
- The core packages `fmt`, `os`, `fs`, `errors` and `time` ([builtins.md](docs/builtins.md#core-packages)) are ordinary standard library packages and must be imported like any other.

### Native functions

Standard-library functions that need the operating system are declared without a body (`pub fn read(string path) (string, error)`) and implemented in C, in a `.c` file in the package's directory. Only the standard library may do this; anywhere else a missing body is a compile error. The C side is described in [runtime/README.md](runtime/README.md#13-native-functions).

### Program entry

```
fn main([]string args) error {
    ...
    return null
}
```

- A program is the `package main` of a directory (all its files) or a single `.sd` file. `main` always has exactly this signature.
- `args[0]` is the program itself; the arguments follow it.
- Returning `null` exits with code 0. Returning an error prints `error: <message>` to stderr and exits with code 1. A crash prints `crash: <message>` and exits with code 2.
- `os.exit(int)` exits immediately with a specific code, without running defers.

## Tests

```
// users/tests/load_test.sd
package users

test "load skips empty lines" {
    []string names = try loadNames("testdata/users.txt")
    assert(names.len() == 3)
}
```

- Tests live in a `tests/` directory inside the package they test, so they stay out of the package's code. They are compiled only by `seed test`; a `test` block anywhere else is a compile error.
- Files in `tests/` belong to the package they test, so tests can reach its private names. They may also declare helper functions and types, and import packages.
- A test is a `test "description" { ... }` block, so nothing depends on function naming. Its body is like the body of a function that returns `error`: `try` works, and an error that leaves the test fails it. `return` is not allowed in a test.
- `assert(bool)` is built in and only allowed in tests (not inside a `group` block). A failed `assert` ends the test. The message shows where it is, the condition as written, and for a comparison of numbers, bools, bytes, strings or errors, the value of each side: `users/tests/load_test.sd:7: assert failed: names.len() == 3 (2 == 3)`. After `assert(cond)`, the compiler narrows on `cond` as after an `if`.
- A test fails when an `assert` fails, when an error leaves it, or when it crashes. Each test runs in a goroutine of its own group, one after the other, so a crash fails only that test.
- `seed test [dir...]` runs the tests of every package under the directories (by default the current one), prints one line per failed test (`FAIL "<name>": <where>: <reason>`) and a summary `N passed, M failed`, and exits with 1 when a test failed (or a package didn't compile).
- A test of a crash runs the code in a group and checks the error: `error err = group { spawn f() }` then `assert(err.message() == "crash: ...")`.

## Memory

- Memory is managed by reference counting.
- Cycles are impossible, because values have no references to one another, so nothing can leak.
- Counts of ordinary values never need atomic operations, because goroutines share nothing.
- When only one owner holds a value, it is modified in place.
- Memory is freed as soon as the last owner is gone, with no garbage-collector pauses.

## Compiler

Every error message shows the code it's about, explains what's wrong and suggests a fix, including "did you mean" suggestions for typos. There are no warnings, and the checks cannot be turned off.

The compiler rejects:

- a `switch` that doesn't cover every case, or an enum `switch` with `default`
- an unchecked error, or an unused call result
- a nullable value used without a null check, or a value read before the error beside it was checked
- an unused local variable (including a pattern binding) or import
- shadowing
- unreachable code
- a missing `return`
- a type whose `implements` declaration doesn't match its methods
- a name that breaks the naming rules
- an import cycle, or a name, field or method of another package that isn't `pub`
- a case pattern that doesn't match its enum case
- `assert` outside a test, `spawn` outside a group, leaving a group block early
- unformatted code

Unused parameters and unused package-level declarations are allowed.

## Toolchain

There is one binary, `seed`, with these commands:

- `seed run <dir | file.sd> [args...]` compiles a program and runs it with the arguments (a `--` before them is skipped), exiting with its exit code.
- `seed build [-o out] <dir | file.sd>` compiles a program (the `package main` of a directory and the packages it imports, or a single file) into one executable, named after the directory or file unless `-o` says otherwise.
- `seed test [dir...]` runs the tests (see [Tests](#tests)).
- `seed fmt [--check] <file.sd>...` formats the code (the rules are in [docs/formatting.md](docs/formatting.md)). Compiling a file that is not formatted is an error pointing at the first line that differs, with the hint `run: seed fmt <file>`.
- `seed c <dir | file.sd...>` prints the generated C program, and `seed ast <file.sd>...` prints the syntax tree. They are for testing and debugging the compiler.

`seed` compiles to C and uses the system C compiler (`cc`). It finds the standard library and the runtime next to itself (through `$PATH` and symbolic links), or in `$SEED_ROOT`.

## Standard library

`errors`, `fmt`, `io`, `os`, `fs`, `path`, `strings`, `math`, `rand`, `time`, `encoding`, `json`, `crypto`, `net` and `http`. The API is in [docs/stdlib.md](docs/stdlib.md), and the implementation in [lib/](lib/README.md).

## Implementation plan

The compiler bootstraps itself:

1. `seed0` is a small compiler for a subset of Seed, written in C, that outputs C ([docs/seed0.md](docs/seed0.md)).
2. The real compiler is written in that subset of Seed (`compiler/`) and compiled with `seed0`.
3. The compiler compiles itself until it reaches a fixed point, where its output reproduces itself exactly (`make verify`).
4. A snapshot of the generated C is committed to `bootstrap/seed.c`, and `seed0` is deleted. From then on, the compiler is built from the snapshot.
5. New features are added with the "teach, snapshot, use" cycle: teach the compiler the feature without using it, take a new snapshot, then use the feature in the compiler itself ([compiler/README.md](compiler/README.md#adding-a-language-feature-teach-snapshot-use)).

Steps 1–3 are done; step 4 is in progress.

## Open questions

None right now. Decisions made during implementation are listed in [DECISIONS.md](DECISIONS.md) for review.
