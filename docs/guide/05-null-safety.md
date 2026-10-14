# 5. Null safety

Seed has no `nil` that can turn up anywhere. A value of type `T` is always a real `T`. A value that might be missing has the type `T?`, and the compiler won't let you use it as a `T` until you have checked it.

```seed
string name = "ann"      // never null
string? email = null     // may be null
```

Any type can be made nullable: `int?`, `[]string?`, `User?`, `Shape?`. Some details:

- A `T` converts to `T?` automatically: `string? email = "ann@example.com"`.
- `?` applies to the whole type on its left. `[]string?` is a list that may be missing; a list of strings that may each be missing is `[](string?)`.
- A nullable function type needs parentheses: `(fn(int) int)?`. Without them, `fn(int) int?` is a function that returns an `int?`.
- `error` is the exception: it can always be `null`, and a `null` error means "no error".

`T?` is how Seed says "there might be nothing here", and the standard library uses it wherever that is a normal outcome rather than a failure: a map lookup returns `V?`, `xs.pop()` returns `T?` (`null` on an empty list), `s.index(sub)` returns `int?`, and `os.env(name)` returns `string?`.

## Narrowing

After a check, the compiler knows the value isn't `null`, and treats it as a plain `T`. This is called narrowing.

```seed
package main

import "fmt"

type User struct {
    string name
    string? email
}

// Inside the if, email is a plain string.
fn contact(User u) string {
    if (u.email != null) {
        return u.name + " <" + u.email + ">"
    }
    return u.name + " (no email)"
}

// After an early return, the rest of the function knows it isn't null.
fn domain(string? email) string {
    if (email == null) {
        return "unknown"
    }
    int? at = email.index("@")
    if (at == null) {
        return "invalid"
    }
    return email.slice(at + 1, email.len())
}

// Narrowing works through && and ||.
fn isLong(string? s) bool {
    return s != null && s.len() > 5
}

fn orDefault(string? s, string fallback) string {
    if (s == null || s == "") {
        return fallback
    }
    return s
}

// Assigning a value that is never null narrows too: after the if, who is
// a plain string on both paths.
fn greeting(string? name) string {
    string? who = name
    if (who == null) {
        who = "stranger"
    }
    return "hello, " + who
}

fn main([]string args) error {
    User ann = User{name: "Ann", email: "ann@example.com"}
    User bob = User{name: "Bob", email: null}
    fmt.println(contact(ann))
    fmt.println(contact(bob))
    fmt.println(domain(ann.email) + " " + domain(bob.email) + " " + domain("nope"))
    fmt.println(isLong("seedling").string() + " " + isLong(null).string())
    fmt.println(orDefault(null, "anonymous") + " " + orDefault("kam", "anonymous"))
    fmt.println(greeting(null) + ", " + greeting("Ann"))

    // A map lookup gives a V?. Store it in a variable to check it.
    map[string]int stock = map[string]int{"apples": 3}
    int? pears = stock["pears"]
    if (pears == null) {
        fmt.println("no pears")
    }
    int? apples = stock["apples"]
    if (apples != null) {
        fmt.println((apples * 2).string() + " half-apples")
    }

    // A T converts to T? automatically; going back needs a check.
    int? maybe = 5
    maybe = null
    fmt.println((maybe == null).string())
    return null
}
```

```
Ann <ann@example.com>
Bob (no email)
example.com unknown invalid
true false
anonymous kam
hello, stranger, hello, Ann
no pears
6 half-apples
true
```

## The rules

The compiler narrows a variable, or a field of a variable such as `u.email` or `cfg.server.host`:

- inside `if (x != null) { ... }`, and in the `else` of `if (x == null)`;
- after `if (x == null) { return ... }`, or any `if` whose body always leaves (with `return`, `break`, `continue`, `os.exit` or `os.crash`);
- to the right of `x != null &&`, and to the right of `x == null ||`;
- through `!`: `if (!(x == null))` narrows like `if (x != null)`;
- after an assignment of a value that is never null (a `T`, not a `T?`): after `if (x == null) { x = "none" }`, `x` is a `string` on every path, as `greeting` above shows.

Narrowing ends:

- when the variable (or a part of the path, such as `u` for `u.email`) is assigned a value that may be null;
- when a `mut` method is called on it or on something that contains it, because the method might have set it back to `null`;
- at the end of the block where the check was made;
- inside a loop, for anything the loop assigns or calls a `mut` method on, since the check may not hold on the next iteration.

For example, assigning inside the narrowed block ends the narrowing:

```seed
package main

import "fmt"

fn main([]string args) error {
    string? name = "ann"
    if (name != null) {
        fmt.println(name.toUpper())
        name = null
        fmt.println(name.toUpper())
    }
    return null
}
```

```
05_narrowing_ends.sd:10:21: error: name is nullable (string?)
   10 |         fmt.println(name.toUpper())
      |                     ^
    hint: check name != null before calling toUpper()
```

Only variables and their fields are narrowed, not other expressions. `stock["apples"]` or `xs[i]` gives a new answer every time you evaluate it, so a check on one occurrence says nothing about the next. Store the value in a variable first, as the example above does with `int? apples = stock["apples"]`. Using the lookup directly is an error:

```
05_map_lookup.sd:7:23: error: the value is nullable (int?) and cannot be used as int to initialize apples
    7 |     int apples = stock["apples"]
      |                       ^
    hint: check the value != null first
```

## Why no nil?

In Go, any pointer, map, slice, interface, channel or function can be `nil`, and nothing in the type says which ones might be. Using one by mistake panics at run time. In Seed the type says it: a `User` is always there, and a `User?` must be checked. You only check where the type tells you to, and when you forget, the compiler tells you:

```
05_unchecked_null.sd:6:12: error: name is nullable (string?)
    6 |     return name.toUpper()
      |            ^
    hint: check name != null before calling toUpper()
```

Next: [Lists and maps](06-lists-and-maps.md).
