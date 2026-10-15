# 7. Structs, methods and mut

## Structs

```seed
type Point struct {
    int x
    int y
}

Point p = Point{x: 1, y: 2}
```

A struct literal names every field. There are no zero values, so leaving a field out is an error, and adding a field to a struct makes the compiler point at every literal that needs it. Fields are read and written with `.`: `p.x = p.x + 1`, `hero.pos.x = 0`.

### Default values

A field can declare a default value, written after its name. A literal may then leave that field out, and gets the default:

```seed
package main

import "fmt"
import "time"

type Retry struct {
    int attempts = 3
    int wait = 500 * time.millisecond
}

type Fetch struct {
    string url
    string method = "GET"
    map[string]string headers = map[string]string{}
    Retry retry = Retry{}
    string? note = null
}

fn show(Fetch f) string {
    return f.method + " " + f.url + " headers=" + f.headers.len().string() + " attempts=" + f.retry.attempts.string() + " wait=" + (f.retry.wait / time.millisecond).string() + "ms"
}

fn main([]string args) error {
    Fetch a = Fetch{url: "/a"}
    Fetch b = Fetch{url: "/b", method: "POST", retry: Retry{attempts: 1}}
    fmt.println(show(a))
    fmt.println(show(b))

    // Each literal gets its own map: changing a's leaves the next one empty.
    a.headers["accept"] = "text/plain"
    Fetch c = Fetch{url: "/c"}
    fmt.println(a.headers.len().string() + " " + c.headers.len().string())
    return null
}
```

```
GET /a headers=0 attempts=3 wait=500ms
POST /b headers=0 attempts=1 wait=500ms
1 0
```

This is still "no zero values": every value is written somewhere, in the literal or in the declaration, and a field without a default (`url` here) must be set in every literal. Adding a field with a default to a struct breaks no literal, which is what makes it the way to grow a struct that other code builds, such as `http.Request` and its `timeout`.

The default is evaluated again at every literal that leaves the field out, so each one gets its own list or map. It is computed like a [constant](02-basics.md#constants): literals, constants (of the struct's package too, even private ones), operators, conversions, enum values, and list, map and struct literals, but no calls of functions (built-in methods that only compute a value, such as `len()`, are fine), no other fields and nothing that could fail, so a literal never runs hidden code:

```
07_default_call.sd:7:19: error: the default value of Event.at must be computable without running code, but it uses the call time.now()
    7 |     int at = time.now()
      |                   ^
    hint: a default may use literals, constants, operators, conversions and list, map and struct literals; set the field in each literal instead
```

Only struct fields have defaults; function parameters don't. A struct literal of another package's type still needs every field to be `pub`, defaulted ones included.

## Methods

A method is a function with a receiver in front of its name:

```seed
fn (Point p) moved(int dx, int dy) Point {
    return Point{x: p.x + dx, y: p.y + dy}
}
```

There is only one kind of receiver, because there are no pointers. A plain method only reads its receiver. A **`mut` method** changes the variable it is called on:

```seed
fn (Point p) mut move(int dx, int dy) {
    p.x = p.x + dx
    p.y = p.y + dy
}
```

A `mut` method can be called on a local variable or on any part of one, such as `hero.pos.move(0, 5)` or `team[1].pickUp("rope")`, and it changes exactly that place. Built-in methods follow the same rule: `xs.push(x)` is a `mut` method of lists.

```seed
package main

import "fmt"

type Point struct {
    int x
    int y
}

type Player struct {
    string name
    Point pos
    []string items
}

// A plain method reads its receiver.
fn (Point p) string() string {
    return "(" + p.x.string() + ", " + p.y.string() + ")"
}

// A plain method can return a changed copy.
fn (Point p) moved(int dx, int dy) Point {
    return Point{x: p.x + dx, y: p.y + dy}
}

// A mut method changes the variable it is called on.
fn (Point p) mut move(int dx, int dy) {
    p.x = p.x + dx
    p.y = p.y + dy
}

fn (Player p) mut pickUp(string item) {
    p.items.push(item)
}

fn main([]string args) error {
    Point a = Point{x: 1, y: 2}
    Point b = a.moved(10, 0)
    fmt.println(a.string() + " " + b.string())

    a.move(1, 1)
    fmt.println(a.string())

    Player hero = Player{name: "Ada", pos: a, items: []string{}}
    hero.pickUp("lamp")
    hero.pos.move(0, 5)
    hero.pos.x = 0
    fmt.println(hero.name + " at " + hero.pos.string() + " with " + hero.items.join(", "))

    // Mut methods work on any part of a local variable.
    []Player team = []Player{hero, Player{name: "Bo", pos: b, items: []string{}}}
    team[1].pickUp("rope")
    team[1].pos.move(-1, -1)
    fmt.println(team[1].name + " at " + team[1].pos.string() + " with " + team[1].items.join(", "))

    // Structs compare by contents.
    fmt.println((Point{x: 1, y: 1} == Point{x: 1, y: 1}).string())
    return null
}
```

```
(1, 2) (11, 2)
(2, 3)
Ada at (0, 8) with lamp
Bo at (10, 1) with rope
true
```

`Point` has a method called `string()`, the same name the built-in types use for their text form. That is the convention for any type that has one.

## Values, not pointers

Seed has no pointers. Every value behaves like an independent copy: assigning a struct, putting it in a list, or passing it to a function never creates a second name for the same data.

```seed
package main

import "fmt"

type Account struct {
    string owner
    int balance
}

fn (Account a) mut deposit(int amount) {
    a.balance = a.balance + amount
}

// Parameters are read-only, so this returns the new value instead.
fn withInterest(Account a, int percent) Account {
    Account out = a
    out.deposit(a.balance * percent / 100)
    return out
}

fn main([]string args) error {
    Account mine = Account{owner: "kam", balance: 100}
    Account copy = mine
    copy.deposit(50)
    fmt.println(mine.balance.string() + " " + copy.balance.string())

    mine = withInterest(mine, 10)
    fmt.println(mine.balance.string())

    []Account all = []Account{mine}
    Account first = all[0]
    first.deposit(1000)
    fmt.println(all[0].balance.string() + " " + first.balance.string())

    all[0].deposit(1)
    fmt.println(all[0].balance.string())
    return null
}
```

```
100 150
110
110 1110
111
```

**Function parameters can't be changed in place.** A function can't change its caller's data. A parameter is the function's own copy of the argument, so it can be given a new value (`args = args.slice(2, args.len())` after reading two flags), like any local, and the caller doesn't see it. But `mut` methods (including `push`) can't be called on a parameter, and its fields and elements can't be assigned: in Go, `tags[0] = "x"` in a function changes the caller's slice (and `m[k] = v` the caller's map), and in Seed it wouldn't, so the compiler rejects the change rather than let it silently do less than it seems to:

```
07_mut_on_param.sd:6:5: error: cannot call the mut method push() on tags: a parameter can be assigned a new value, but not changed in place
    6 |     tags.push("new")
      |     ^
    hint: copy it into a local variable and change that; return the new value if the caller needs it
```

To change something, a function returns the new value (`mine = withInterest(mine, 10)`), or the change is a `mut` method called on the caller's own variable (`mine.deposit(10)`). Either way, the line that changes a variable names that variable. The receiver of a method that is not `mut` can't even be assigned: only a `mut` method changes its receiver. Copying a parameter into a local, as `withInterest` does, is cheap: the data is shared until the copy is changed ([copy-on-write](06-lists-and-maps.md#values-and-copy-on-write)).

### Why no pointers?

Pointers make two names for the same data. Most of the bugs they bring come from that: a function that quietly changes its argument, a struct changed through an alias you forgot about, a nil pointer, a data race between goroutines. Without them:

- Reading `f(x)`, you know `x` is the same afterwards. Only `x = ...`, `x++` and `x.method()` with a `mut` method can change `x`.
- There is no `nil` pointer to dereference, and no `*T` versus `T` choice for receivers.
- Goroutines can't share memory, so data races are impossible ([chapter 10](10-concurrency.md)).
- Memory management is simple reference counting with no cycles and no garbage collector.

Copy-on-write makes the cost small: copies are only made when shared data is changed.

## Modelling shared state

What do you do when two things must refer to the same thing, like a book and the member who borrowed it? Without pointers, the answer is:

1. **One owner holds the data.** Put the shared things in lists inside one struct, and change them only through that struct's `mut` methods.
2. **Refer by index (or by key).** Instead of a pointer to a member, a book stores the member's index in the owner's list, or an id that is a key in a map.

```seed
// Several borrowers and books that refer to each other. Instead of pointers,
// one Library owns everything and the parts refer to each other by index.
package main

import "errors"
import "fmt"

type Book struct {
    string title
    int? borrower // an index into Library.members
}

type Member struct {
    string name
    []int books // indexes into Library.books
}

type Library struct {
    []Book books
    []Member members
}

fn newLibrary() Library {
    return Library{books: []Book{}, members: []Member{}}
}

fn (Library lib) mut addBook(string title) int {
    lib.books.push(Book{title: title, borrower: null})
    return lib.books.len() - 1
}

fn (Library lib) mut join(string name) int {
    lib.members.push(Member{name: name, books: []int{}})
    return lib.members.len() - 1
}

fn (Library lib) mut lend(int book, int member) error {
    int? holder = lib.books[book].borrower
    if (holder != null) {
        return errors.new(lib.books[book].title + " is already lent to " + lib.members[holder].name)
    }
    lib.books[book].borrower = member
    lib.members[member].books.push(book)
    return null
}

fn (Library lib) report() {
    for (int i = 0; i < lib.members.len(); i++) {
        Member m = lib.members[i]
        string line = m.name + " has " + m.books.len().string() + " book(s)"
        for (int j = 0; j < m.books.len(); j++) {
            line = line + ", " + lib.books[m.books[j]].title
        }
        fmt.println(line)
    }
}

fn main([]string args) error {
    Library lib = newLibrary()
    int dune = lib.addBook("Dune")
    int emma = lib.addBook("Emma")
    int ann = lib.join("Ann")
    int bob = lib.join("Bob")

    try lib.lend(dune, ann)
    try lib.lend(emma, ann)
    error err = lib.lend(dune, bob)
    if (err != null) {
        fmt.println("cannot lend: " + err.message())
    }
    lib.report()
    return null
}
```

```
cannot lend: Dune is already lent to Ann
Ann has 2 book(s), Dune, Emma
Bob has 0 book(s)
```

This is the same structure a database uses: tables of records, linked by ids. It has real advantages over a web of pointers: the whole state is one value you can copy, compare, print or save, and every change goes through a method of the owner, which can keep the parts consistent (`lend` updates the book and the member together).

Other patterns, covered later:

- **Trees** are enums or structs that contain lists of their children; a tree whose nodes need to know their parents is stored in an arena, the pattern above applied to nodes ([chapter 8](08-enums.md)).
- **State shared between goroutines** belongs to one goroutine, and the others send it messages ([chapter 10](10-concurrency.md#shared-state-reply-channels)).

Next: [Enums](08-enums.md).
