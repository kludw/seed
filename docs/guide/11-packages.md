# 11. Packages and visibility

The example project of this chapter is [examples/11_shop/](examples/11_shop/).

## Packages

A package is a directory of `.sd` files that all start with the same `package` line, and the package has the name of its directory. A program is the package called `main`, with the `main` function in one of its files; its directory is the **project root**.

```
shop/
    main.sd                 package main
    cart/
        cart.sd             package cart
        tests/
            cart_test.sd    package cart (tests)
```

Imports come after the `package` line, one per line, and each file imports what it uses itself:

```seed
package main

import "cart"
import "fmt"
```

An import path is looked up in the standard library first (`fmt`, `fs`, `json`, ... live in the `lib` directory next to the `seed` toolchain), and then relative to the project root: `import "cart"` is the directory `cart/` next to `main.sd`, and `import "shop/money"` would be `shop/money/`. Code uses the last part of the path as the package's name (`money.Amount`). There is no project file to write.

A name from another package is always written with the package name in front: `cart.newCart()`, `cart.Cart`, `cart.Item{...}`, and for enums `shapes.Shape.Circle(2.0)` and `case shapes.Shape.Circle(float r):`. Import cycles are compile errors (the error shows the cycle, `import cycle: cart -> money -> cart`), and so is an unused import.

## Everything is private until marked pub

A top-level name (function, type, constant) is visible only inside its own package unless it is marked `pub`. Go uses capitalisation for this; Seed's naming rules already fix the case of every name (types are `PascalCase`, functions `camelCase`), so visibility gets its own keyword, and making something public doesn't mean renaming it everywhere.

Struct fields are private too, unless marked `pub`. Another package can read and write only `pub` fields, and it can only write a struct literal of your type if **every** field is `pub`. A type with private fields can therefore only be made by your own functions, which lets a package protect its rules (the standard library relies on this, so that nobody can forge an open file handle).

```seed
// Package cart keeps the items of a shopping cart and prices them.
package cart

import "errors"

pub type Item struct {
    pub string name
    pub int cents
    pub int quantity
}

// Fields without pub can only be used inside package cart, so other
// packages must call newCart() instead of writing a Cart{...} literal.
pub type Cart struct {
    []Item items
    int discountPercent
}

pub fn newCart() Cart {
    return Cart{items: []Item{}, discountPercent: 0}
}

pub fn (Cart c) mut add(Item item) error {
    if (item.quantity < 1) {
        return errors.new("cart.add: " + item.name + ": quantity must be at least 1")
    }
    c.items.push(item)
    return null
}

pub fn (Cart c) mut setDiscount(int percent) {
    c.discountPercent = clamp(percent, 0, 100)
}

pub fn (Cart c) total() int {
    int sum = subtotal(c.items)
    return sum - sum * c.discountPercent / 100
}

pub fn (Cart c) count() int {
    return c.items.len()
}

// Private helpers: not visible outside package cart.
fn subtotal([]Item items) int {
    int sum = 0
    for (int i = 0; i < items.len(); i++) {
        sum = sum + items[i].cents * items[i].quantity
    }
    return sum
}

fn clamp(int x, int lo, int hi) int {
    if (x < lo) {
        return lo
    }
    if (x > hi) {
        return hi
    }
    return x
}
```

```seed
// The program's entry point, in package main at the root of the project.
//
//     seed build -o shop .
//     ./shop
package main

import "cart"
import "fmt"

fn main([]string args) error {
    cart.Cart c = cart.newCart()
    try c.add(cart.Item{name: "notebook", cents: 450, quantity: 2})
    try c.add(cart.Item{name: "pencil", cents: 120, quantity: 5})
    c.setDiscount(10)
    fmt.println(c.count().string() + " items, total " + c.total().string() + " cents")
    return null
}
```

```
$ seed run .
2 items, total 1350 cents
```

`seed run` and `seed build` take the project's directory (or a single `.sd` file): `seed build -o shop .` builds the executable `shop`, and without `-o` the executable is named after the directory.

`main` can call `cart.newCart()` and the `pub` methods, and write `cart.Item{...}` because all of `Item`'s fields are `pub`. It can't write `cart.Cart{...}`, read `c.items`, or call `cart.clamp`; the compiler says why. Adding `fmt.println(c.items.len().string())` to `main` gives:

```
main.sd:15:19: error: the field items of cart.Cart is not pub
   15 |     fmt.println(c.items.len().string())
      |                   ^
    hint: only pub fields can be used outside package cart
```

## Nothing runs on its own

There is no `init()` function, and there are no package-level variables, only constants. Importing a package never runs any of its code. Everything a program does starts in `main`, and the state it needs is created there and passed along. This keeps programs easy to follow, and it is also required by concurrency: goroutines share nothing, so there can't be global mutable state.

## Tests

Tests live in a `tests/` directory inside the package they test, so they don't clutter the package's code. Test files belong to the package itself (`package cart`), so tests can use private names too.

A test is a `test "description" { ... }` block. The built-in `assert(condition)` checks a condition, and is only allowed in tests. A test fails when an `assert` fails, or when an error leaves it (through `try`), or when it crashes. Test files are only compiled by `seed test`, so they never end up in your program.

```seed
// Tests belong to the package they test, so they can use private names.
package cart

test "total multiplies price by quantity" {
    Cart c = newCart()
    try c.add(Item{name: "pen", cents: 150, quantity: 2})
    try c.add(Item{name: "pad", cents: 300, quantity: 1})
    assert(c.total() == 600)
}

test "discount is applied and clamped" {
    Cart c = newCart()
    try c.add(Item{name: "pen", cents: 1000, quantity: 1})
    c.setDiscount(25)
    assert(c.total() == 750)
    c.setDiscount(250)
    assert(c.total() == 0)
}

test "zero quantity is rejected" {
    Cart c = newCart()
    error err = c.add(Item{name: "pen", cents: 150, quantity: 0})
    assert(err != null)
    assert(c.count() == 0)
}

test "clamp keeps values in range" {
    assert(clamp(-5, 0, 10) == 0)
    assert(clamp(5, 0, 10) == 5)
    assert(clamp(50, 0, 10) == 10)
}
```

`seed test` runs the tests of every package under the current directory (or under the directories you name: `seed test cart` for one package, not `seed test cart/tests`), each test in a goroutine of its own, so that one crash fails only that test:

```
$ seed test
4 passed, 0 failed
```

There is no testing package and no naming convention to remember: a test is a block with a description. When an `assert` fails, the message shows where, the condition, and for a comparison the value of each side, so you rarely need to print anything. If `discountPercent` were applied twice, the output would be:

```
FAIL "discount is applied and clamped": cart/tests/cart_test.sd:16: assert failed: c.total() == 750 (563 == 750)
3 passed, 1 failed
```

An error that leaves a test is shown as `cart/tests/cart_test.sd:4: error: ...` and a crash as `... crash: index out of range: 5 with length 2`, with the line of the test. `seed test` exits with code 1 when a test fails.

Two options help while you work on one test. `--run text` runs only the tests whose description contains the text (a plain substring, not a pattern), and `-v` prints each package and each test as it runs, the passing ones too:

```
$ seed test -v --run discount
testing cart
PASS "discount is applied and clamped"
1 passed, 0 failed
```

The options come before the directories. When `--run` matches no test at all, `seed test` says so and exits with code 1, so that a typo does not look like success.

After `assert(x != null)` the compiler knows that `x` is not null, as after an `if` that returns. To test that a function crashes on a caller's mistake, run it in a group and check the error:

```seed
test "repeat rejects a negative count" {
    error err = group {
        spawn repeatNegative()
    }
    assert(err != null)
    assert(err.message() == "crash: strings.repeat: count (-1) must not be negative")
}
```

Library code reports such mistakes with `os.crash(message)`, which crashes the goroutine exactly like an index out of range.

A test that needs files makes a temporary directory with `fs.makeTemp` and removes it with a deferred `fs.removeAll`, discarding its error explicitly ([stdlib.md](../stdlib.md#tests)):

```seed
test "save writes the file" {
    string dir = try fs.makeTemp("cart-test-")
    defer _ = fs.removeAll(dir)
    ...
}
```

Next: [The compiler is your friend](12-the-compiler.md).
