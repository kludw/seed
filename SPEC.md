# Seed — language spec (draft)

Seed is a small, strongly typed language inspired by Go. It is a proof of concept.

Items marked **(proposed)** were filled in while writing this spec and have not been agreed yet. Everything else was decided in discussion.

## Principles

1. **One way to do things.** No syntax sugar, except `try` for passing errors up.
2. **Nothing implicit.** No hidden control flow, no `init()`, no implicit conversions, no code that runs behind your back.
3. **Everything you need is built in.** A huge standard library and a single toolchain binary, with no third-party dependencies.
4. **The compiler is a strict friend.** It is pedantic, it reports errors only (no warnings), and every error explains itself and suggests a fix. Slower compiles are an acceptable cost.

## Syntax basics

- Comments use `//`.
- Statements end at the end of a line. There are no semicolons **(proposed)**.
- Braces are always required, and the opening brace goes on the same line.
- Conditions are always in parentheses: `if (...)`, `for (...)`, `switch (...)`, `select`.
- Formatting is part of the language. Code that isn't formatted fails to compile, and the error gives the command that fixes it.
- Naming is enforced by the compiler. Types and enum cases use `PascalCase`, and everything else uses `camelCase`, including constants.

## Literals

| Literal            | Type     | Notes                                                      |
|--------------------|----------|------------------------------------------------------------|
| `42`, `0xff`       | `int`    | Decimal or hexadecimal.                                    |
| `3.14`, `1.0e9`    | `float`  | Must contain a `.`; `1e9` alone is not a float.            |
| `'a'`, `'\n'`      | `byte`   | A single byte. Escapes are the same as in strings.         |
| `"hi\n"`           | `string` | Escapes: `\n \t \r \0 \\ \" \' \xHH`. No raw or multi-line strings. |
| `true`, `false`    | `bool`   |                                                            |
| `null`             | any `T?` |                                                            |
| `[]int{1, 2}`      | `[]int`  | The type is always written. `[]int{}` is an empty list.    |
| `map[string]int{"a": 1}` | `map[string]int` | Keys may be `int`, `byte`, `bool` or `string`. |

## Types

| Type          | Meaning                                                            |
|---------------|--------------------------------------------------------------------|
| `int`         | The only integer type. It grows as needed and never overflows.    |
| `float`       | A 64-bit floating-point number.                                   |
| `bool`        | `true` or `false`.                                                |
| `byte`        | An 8-bit value, used for raw I/O.                                 |
| `string`      | Immutable bytes, normally UTF-8 text (any bytes are allowed). `s[i]` returns a `byte`, and `s.chars()` returns the characters. |
| `[]T`         | A list that grows in place.                                       |
| `map[K]V`     | A map.                                                            |
| `chan T`      | A channel.                                                        |
| `T?`          | A nullable `T`.                                                   |
| `fn(T, ...) R`| A function value (see [Function values](#function-values)).       |
| `struct`, `enum`, `interface` | User-defined types.                               |
| `error`       | The built-in error interface.                                      |

- There are no implicit conversions. Converting is always explicit, for example `float(n)` or `int(f)`.
- The built-in types (`[]T`, `map[K]V`, `chan T`, `T?`) are generic. User code cannot define generic types or functions.

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
| `sortBy(f)`     | Sorts in place using `f`, a `fn(T, T) bool` that returns true when the first argument goes first. `mut`. |

- `push` is the only way to add to the end of a list. There is no separate `append`.
- Indexing with `xs[i]` out of bounds crashes the goroutine.

### Maps

| Operation       | Result                                                        |
|-----------------|---------------------------------------------------------------|
| `m[k]`          | The value as `V?`, or `null` if the key is missing.           |
| `m[k] = v`      | Sets a value.                                                 |
| `len()`         | The number of entries.                                        |
| `delete(k)`     | Removes a key. `mut`.                                         |
| `keys()`        | The keys as `[]K`, in insertion order.                        |

- Maps keep insertion order, so iterating over them is deterministic.

### Converting to text

- Every built-in type has a `string()` method: `count.string()`, `ratio.string()`, `done.string()`.
- User types that need a text form write their own `string()` method.

## Printing

```
fmt.println("count: " + count.string())
fmt.print("name? ")
```

- `fmt.print(string)` and `fmt.println(string)` take exactly one `string`. `println` adds a newline.
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
- Shadowing is a compile error. Declaring a name that already exists in an outer scope fails, and the error suggests assigning with `=` instead.
- With several return values, a name with a type in front of it is declared, and a name without one is an existing variable being assigned **(proposed)**:

```
[]string names, error err = loadNames(path)
string text, err = fs.read(other)     // text is new, err is reused
```

- A `const` uses the same form as a variable. Its value must be computable at compile time. Constants are allowed both at package level and inside functions.
- Package-level variables are not allowed, only `const` **(proposed)**. Goroutines are isolated, so there can be no global mutable state.

## Operators

| Kind       | Operators                     | Works on                         |
|------------|-------------------------------|----------------------------------|
| Arithmetic | `+ - * / %`                   | `int`, `float` (`%` on `int` only) |
| Joining    | `+`                           | `string`                         |
| Bitwise    | `& \| ^ << >>`                | `int`, `byte`                    |
| Comparison | `== !=`                       | Every value type except `chan` and function values. Structs and lists compare by contents. |
| Ordering   | `< <= > >=`                   | `int`, `float`, `string`, `byte` |
| Logic      | `&& \|\| !`                   | `bool`                           |

- Integer division and `%` by zero crash. Float division by zero follows IEEE and gives `inf`, `-inf` or `nan`.
- Both sides of an operator must have the same type, so mixing `int` and `float` is a compile error: `float(n) * 1.5`.
- There is no compound assignment (`+=` and the like). Write `x = x + 1`.
- There is no ternary `?:` operator.
- Precedence follows Go.

## Null

- No type can be null unless its type says so. A `T?` can be null, while a plain `T` never is.
- A `T?` cannot be used as a `T` until it has been checked:

```
if (u.email != null) {
    send(u.email)        // u.email is a string inside this block
}
```

## Values, not pointers

- Seed has no pointers. Every value behaves like an independent copy.
- Copies are cheap. Data is shared until someone modifies it, and only then is it actually copied (copy-on-write).
- Function arguments are read-only. To change a value, a function returns a new one, or the value is changed through a `mut` method.

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

### Methods

```
fn (User u) name() string {
    return u.name
}

fn (User u) mut tag(string t) {
    u.tags.push(t)
}
```

- There is only one kind of receiver, because there are no pointers.
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
- A struct field of function type is called directly: `op.apply(1, 2)`.
- Function values cannot be compared with `==`.

## Structs, enums, interfaces

```
pub type User struct implements Named {
    string name
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

- Struct fields are private to their package unless marked `pub` (`pub string name`). A struct literal of another package's type is only allowed when every field is `pub`.
- A struct literal must set every field: `User{name: "k", email: null, tags: []string{}}`.
- An enum case is always written with its type, both when creating a value and in a `switch`: `Shape.Circle(2.0)`, `case Shape.Circle(float r):`. A case without data has no parentheses: `Token.Eof`.
- A name from another package is always written with the package name: `users.User`, `users.Shape.Circle(2.0)`.
- A type satisfies an interface only by declaring it with `implements`. The compiler then checks that every required method exists, with exactly the same signature (including `mut`). Methods cannot be declared on an interface type.
- A value of such a type converts to the interface (or the nullable interface) automatically wherever the interface is expected. There is no way to get the original type back out of an interface value (no type switches or assertions).
- An interface method can be `mut`. Like any `mut` method, it can only be called on a variable or a part of one, and it changes only that interface value.
- An enum case can carry data, and a `switch` over an enum must cover every case.

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

- `for` has exactly one form, C-style. An infinite loop is `for (;;)`.
- `i++` and `i--` are statements, not expressions, so `x = i++` does not compile.
- `switch` cases never fall through.
- A `switch` over an enum must list every case and cannot have `default`, so adding a case later makes the compiler point at every `switch` that needs updating.
- A `switch` over an `int`, `byte`, `string` or `bool` must end with `default`. One case can list several values.
- Variables bound in a `case` also write their type, like every other declaration **(proposed)**.
- `break` and `continue` work in loops. There are no labels and no `goto` **(proposed)**.
- `defer` runs when the enclosing function returns (or its goroutine crashes). Several defers run in reverse order (last first). The call's arguments are evaluated when the `defer` statement runs.
- `defer` takes a call of a function, method, function value, interface method or package function, or `ch.close()`, but not of a `mut` method: the variable it would change is gone when the call runs.
- A deferred call that returns an error must discard it explicitly: `defer _ = f.close()`.
- `break` and `continue` inside a `switch` apply to the enclosing loop.

## Errors

- `error` is an interface with a `message() string` method.
- `error` is the one type that can be `null` without a `?`: a `null` error means "no error".
- A function that can fail returns `(..., error)`. When the error is not null, the other return values are null, and the compiler stops you from reading them until the error has been checked: inside `if (err == null)`, after `if (err != null) { return ... }`, or right of `err == null &&`.
- Every error must be handled. Ignoring a returned error is a compile error.

```
[]string names, error err = loadNames(path)
if (err != null) {
    return null, err
}
```

- Writing `string text = try fs.read(path)` returns `null, ..., err` from the current function when `fs.read` fails. It is the only syntax sugar in the language.
- `_` discards an error explicitly. If a value comes back alongside the discarded error, that value must be declared as `T?`, and the compiler rejects a plain `T`:

```
string? data, _ = fs.read(path)    // must be checked before use
_ = fs.remove(path)                // fine: nothing else was returned
```

- Runtime faults, such as an out-of-bounds index or division by zero, crash only the goroutine they happen in. See the next section.

## Concurrency

```
chan int results = chan int(10)
err = group {
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
  - A goroutine can only be stopped at a channel send or receive (or `select`), and its `defer`s still run.
  - An error that nothing handles ends up in `main`, and the program exits with a clear message.
- Goroutines share nothing. Sending a value on a channel copies it, so data races are impossible.
- A `group` block is ordinary code: `spawn` may appear anywhere inside it, including in loops and `if`s.
- Besides channel operations, blocking standard-library calls (`time.sleep`, I/O on files and sockets, `io.readLine`, `os.run`, `net.dial`) are also points where a cancelled goroutine stops.
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

- `chan T(n)` creates a channel with a buffer of `n`, and `n = 0` means unbuffered.
- `ch.send(v)` sends a value. Sending on a closed channel crashes the goroutine.
- `ch.receive()` returns `T?`, and returns `null` once the channel is closed and empty. For a `chan error` it returns `error`, which is also null then.
- `ch.close()` closes the channel. Closing it twice crashes. `close` is not a point where a stopped goroutine stops, so `defer ch.close()` works (it is the one built-in method call that `defer` accepts).
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

- A package is a directory, and each file starts with `package name`.
- Imports are package-level, one per line: `import "fmt"`. Import cycles are compile errors.
- Everything is private. Mark a name `pub` to export it.
- Nothing runs implicitly when a program starts.

### Program entry

```
fn main([]string args) error {
    ...
    return null
}
```

- `main` always has exactly this signature.
- Returning `null` exits with code 0. Returning an error prints its message and exits with code 1.
- `os.exit(int)` exits immediately with a specific code.

## Tests

```
// users/tests/load_test.sd
package users

test "load skips empty lines" {
    []string names = try loadNames("testdata/users.txt")
    assert(names.len() == 3)
}
```

- Tests live in a `tests/` directory inside the package they test, so they stay out of the package's code.
- Files in `tests/` belong to the package they test, so tests can reach its private names.
- A test is a `test "description" { ... }` block, so nothing depends on function naming.
- `assert(bool)` is built in and only allowed in tests. When it fails, the message shows the value of each side, for example `names.len() == 3 → 2 == 3`.
- `seed test` runs every test.

## Memory

- Memory is managed by reference counting.
- Cycles are impossible, because values have no references to one another, so nothing can leak.
- Counts never need atomic operations, because goroutines share nothing.
- When only one owner holds a value, it is modified in place.
- Memory is freed as soon as the last owner is gone, with no garbage-collector pauses.

## Compiler

Every error message shows the code it's about, explains what's wrong and suggests a fix, including "did you mean" suggestions for typos.

The compiler rejects:

- a `switch` that doesn't cover every case
- an unchecked error
- a nullable value used without a null check
- an unused variable, import or return value
- shadowing
- unreachable code
- a missing `return`
- a type whose `implements` declaration doesn't match its methods
- a name that breaks the naming rules
- unformatted code

## Toolchain

There is one binary, `seed`, with these commands **(proposed)**:

- `seed run file.sd` compiles a file and runs it.
- `seed build` compiles a package into a single static binary.
- `seed test` runs the tests.
- `seed fmt` formats the code (the rules are in [docs/formatting.md](docs/formatting.md)).

## Standard library (planned)

`fmt`, `fs`, `io`, `strings`, `math`, `time`, `os`, `net`, `http`, `json`, `crypto`, `errors`, `path`, `rand`, `encoding`. The API is in [docs/stdlib.md](docs/stdlib.md).

Standard-library functions that need the operating system are declared without a body and implemented in C in the runtime. Only the standard library may do this.

## Implementation plan

The same bootstrap approach as the first Seed:

1. Write `seed0.c`, a small C compiler for a subset of Seed that outputs C.
2. Write the real compiler in that subset of Seed and compile it with `seed0`.
3. Compile the compiler with itself and reach a fixed point, where its output reproduces itself exactly.
4. Commit a snapshot of the generated C to `bootstrap/` and delete `seed0.c`.
5. Add features using the "teach, snapshot, use" cycle.

## Open questions

None right now. Decisions made during implementation are listed in [DECISIONS.md](DECISIONS.md) for review.
