# 11. Packages and visibility

> **Not implemented yet.** The compiler can only import the core packages `fmt`, `os`, `fs` and `errors` today, and doesn't run tests. This chapter follows the spec; its example project is in [examples/pending/11_shop/](examples/pending/11_shop/) and has not been compiled. How import paths map to directories, and the exact arguments of `seed build` and `seed test` for a package, are not settled yet: the chapter assumes paths relative to the project's root directory.

## Packages

A package is a directory of `.sd` files that all start with the same `package` line. A program is the package called `main`, with the `main` function in one of its files.

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

A name from another package is always written with the package name in front: `cart.newCart()`, `cart.Cart`, and for enums `shapes.Shape.Circle(2.0)`. Import cycles are compile errors, and so is an unused import.

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
2 items, total 1350 cents
```

`main` can call `cart.newCart()` and the `pub` methods, and write `cart.Item{...}` because all of `Item`'s fields are `pub`. It can't write `cart.Cart{...}`, read `c.items`, or call `cart.clamp`.

## Nothing runs on its own

There is no `init()` function, and there are no package-level variables, only constants. Importing a package never runs any of its code. Everything a program does starts in `main`, and the state it needs is created there and passed along. This keeps programs easy to follow, and it is also required by concurrency: goroutines share nothing, so there can't be global mutable state.

## Tests

Tests live in a `tests/` directory inside the package they test, so they don't clutter the package's code. Test files belong to the package itself (`package cart`), so tests can use private names too.

A test is a `test "description" { ... }` block. The built-in `assert(condition)` checks a condition, and is only allowed in tests. A test fails when an `assert` fails, or when an error leaves it (through `try`), or when it crashes.

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

`seed test` runs every test. There is no testing package and no naming convention to remember: a test is a block with a description. When an `assert` fails, the message shows the value of each side of the comparison, for example `c.total() == 600 → 450 == 600`, so you rarely need to print anything.

A test that needs files makes a temporary directory and removes it with `defer` (`fs.makeTemp` and `fs.removeAll` are in the planned standard library, [stdlib.md](../stdlib.md)).

Next: [The compiler is your friend](12-the-compiler.md).
