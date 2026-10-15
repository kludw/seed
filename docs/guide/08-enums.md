# 8. Enums

A Seed enum is a type whose value is one of a fixed set of cases, and each case can carry its own data. (Other languages call these tagged unions, sum types, or algebraic data types.)

```seed
type Shape enum {
    Circle(float radius)
    Rect(float w, float h)
    Dot
}

Shape s = Shape.Circle(2.0)
Shape d = Shape.Dot
```

A case is always written with its type: `Shape.Circle(2.0)`, never just `Circle(2.0)`. A case without data has no parentheses. There is no `iota` and no integer behind a case: an enum without data, such as `Direction` in [chapter 3](03-control-flow.md#switch), is simply an enum whose cases carry nothing.

## switch over an enum

The only way to look inside an enum is `switch`. Each case can bind the case's data to variables, which, like every declaration, write their type:

```seed
package main

import "fmt"

type Shape enum {
    Circle(float radius)
    Rect(float w, float h)
    Dot
}

fn area(Shape s) float {
    switch (s) {
    case Shape.Circle(float r):
        return 3.0 * r * r
    case Shape.Rect(float w, float h):
        return w * h
    case Shape.Dot:
        return 0.0
    }
}

fn (Shape s) describe() string {
    switch (s) {
    case Shape.Circle(float r):
        return "circle of radius " + r.string()
    case Shape.Rect(float w, float h):
        return w.string() + " by " + h.string() + " rectangle"
    case Shape.Dot:
        return "dot"
    }
}

// An enum is a good way to return one of several outcomes.
type Lookup enum {
    Found(int value)
    Missing
    Ambiguous([]string candidates)
}

fn find(string key) Lookup {
    switch (key) {
    case "one":
        return Lookup.Found(1)
    case "t":
        return Lookup.Ambiguous([]string{"two", "three"})
    default:
        return Lookup.Missing
    }
}

fn main([]string args) error {
    []Shape shapes = []Shape{Shape.Circle(1.0), Shape.Rect(2.0, 3.5), Shape.Dot}
    for (int i = 0; i < shapes.len(); i++) {
        fmt.println(shapes[i].describe() + ": " + area(shapes[i]).string())
    }
    // Every enum has the built-in name(): the name of the case.
    fmt.println(shapes[0].name() + " " + Shape.Dot.name())

    []string keys = []string{"one", "t", "zzz"}
    for (int i = 0; i < keys.len(); i++) {
        switch (find(keys[i])) {
        case Lookup.Found(int v):
            fmt.println(keys[i] + " = " + v.string())
        case Lookup.Missing:
            fmt.println(keys[i] + " is missing")
        case Lookup.Ambiguous([]string c):
            fmt.println(keys[i] + " could be " + c.join(" or "))
        }
    }

    // Enum values compare by case and data.
    fmt.println((Shape.Dot == Shape.Dot).string() + " " + (Shape.Circle(1.0) == Shape.Circle(2.0)).string())
    return null
}
```

```
circle of radius 1.0: 3.0
2.0 by 3.5 rectangle: 7.0
dot: 0.0
Circle Dot
one = 1
t could be two or three
zzz is missing
true false
```

The bound names are new variables, so they can be called anything (`float r` for the field `radius`), and they follow the no-shadowing rule. Enums can have methods, like `describe()` above.

### The name of a case

Every enum has a built-in method `name()`, which returns the name of the value's case as a `string`, without the type's name and without the data: `Shape.Circle(1.0).name()` is `"Circle"`, and `Direction.North.name()` is `"North"`. For an enum without data, that is its text form, with no `switch` to write; for one with data, write a method that shows the data too, like `describe()` above.

Because `name()` is built in, an enum can't declare a method of its own called `name` (the compiler says so and suggests another name). It does count for `implements`: an enum satisfies an interface that asks for `name() string`, as `Blob` does in [chapter 9](09-interfaces-and-functions.md#interfaces).

### Exhaustive, with no default

A switch over an enum must list every case, and it may not have `default`. This is deliberate: when you add `Shape.Triangle` later, every switch that doesn't handle it stops compiling, and the compiler lists them for you:

```
12_missing_case.sd:12:5: error: switch over Light does not handle case Light.Yellow
   12 |     switch (l) {
      |     ^
    hint: add a case for it; a switch over an enum must list every case
```

A `default` would silently swallow the new case, which is exactly the bug the rule prevents.

A pattern always matches its case exactly: a case with data binds every field, with its type, and a case without data has no parentheses. When you don't need a field, bind it to `_`: `case Shape.Rect(float _, float h):`. Several cases can share one `case` when they bind only `_`: `case Shape.Dot, Shape.Circle(float _):`. Writing `case Shape.Circle:` for a case with data is an error, so the pattern always shows what the case carries.

Because the switch is exhaustive, a function that returns from every case needs no `return` after the switch.

## Trees without pointers

An enum case can hold values of its own enum type, directly or in a list, so a tree is just a value. Here is an expression tree:

```seed
// An expression tree as an enum whose cases hold other expressions.
package main

import "fmt"

type Expr enum {
    Num(int value)
    Neg(Expr inner)
    Add([]Expr terms)
    Mul(Expr left, Expr right)
}

fn eval(Expr e) int {
    switch (e) {
    case Expr.Num(int value):
        return value
    case Expr.Neg(Expr inner):
        return -eval(inner)
    case Expr.Add([]Expr terms):
        int sum = 0
        for (int i = 0; i < terms.len(); i++) {
            sum = sum + eval(terms[i])
        }
        return sum
    case Expr.Mul(Expr left, Expr right):
        return eval(left) * eval(right)
    }
}

fn show(Expr e) string {
    switch (e) {
    case Expr.Num(int value):
        return value.string()
    case Expr.Neg(Expr inner):
        return "-" + show(inner)
    case Expr.Add([]Expr terms):
        []string parts = []string{}
        for (int i = 0; i < terms.len(); i++) {
            parts.push(show(terms[i]))
        }
        return "(" + parts.join(" + ") + ")"
    case Expr.Mul(Expr left, Expr right):
        return show(left) + " * " + show(right)
    }
}

// Rewrites a tree into a new one: values never change in place.
fn double(Expr e) Expr {
    return Expr.Mul(Expr.Num(2), e)
}

fn main([]string args) error {
    Expr e = Expr.Add([]Expr{Expr.Num(1), Expr.Neg(Expr.Num(5)), Expr.Mul(Expr.Num(3), Expr.Num(4))})
    fmt.println(show(e) + " = " + eval(e).string())
    Expr d = double(e)
    fmt.println(show(d) + " = " + eval(d).string())
    return null
}
```

```
(1 + -5 + 3 * 4) = 8
2 * (1 + -5 + 3 * 4) = 16
```

`double(e)` puts `e` inside a new tree without copying it: the two trees share the subtree, which is safe because values never change behind your back. A struct can hold a list of its own type in the same way (a `Node` with a `[]Node children` field), or a nullable one (a `Link` with a `Link? next` field). Only a plain `Node` field inside `Node` is rejected, since that value would be infinitely large.

This style is ideal for data that is built once and then read or transformed: syntax trees, JSON documents, configuration. It has one limit: a node can't point back up to its parent, or across to a sibling, because a value can't contain a reference to another value.

### Arenas

When nodes must refer to each other freely (parents, siblings, graphs), or the tree is edited in place, store the nodes in an **arena**: one list that owns every node, with nodes referring to each other by index. This is the shared-state pattern of [chapter 7](07-structs-and-methods.md#modelling-shared-state) applied to a tree. (The Seed compiler stores its syntax trees this way.)

```seed
// A tree stored in an arena: every node lives in one list, and nodes refer
// to each other by index. A node can know its parent, and the tree can be
// changed in place.
package main

import "fmt"

type Node struct {
    string name
    int parent // -1 for the root
    []int children
}

type Tree struct {
    []Node nodes
}

fn newTree(string rootName) Tree {
    return Tree{nodes: []Node{Node{name: rootName, parent: -1, children: []int{}}}}
}

fn (Tree t) mut add(int parent, string name) int {
    t.nodes.push(Node{name: name, parent: parent, children: []int{}})
    int id = t.nodes.len() - 1
    t.nodes[parent].children.push(id)
    return id
}

fn (Tree t) path(int id) string {
    string p = t.nodes[id].name
    for (int at = t.nodes[id].parent; at != -1; at = t.nodes[at].parent) {
        p = t.nodes[at].name + "/" + p
    }
    return p
}

fn (Tree t) print(int id, string indent) {
    fmt.println(indent + t.nodes[id].name)
    []int kids = t.nodes[id].children
    for (int i = 0; i < kids.len(); i++) {
        t.print(kids[i], indent + "  ")
    }
}

fn main([]string args) error {
    Tree t = newTree("home")
    int docs = t.add(0, "docs")
    int pics = t.add(0, "pictures")
    int notes = t.add(docs, "notes.txt")
    _ = t.add(pics, "cat.png")
    t.nodes[notes].name = "todo.txt"
    t.print(0, "")
    fmt.println(t.path(notes))
    return null
}
```

```
home
  docs
    todo.txt
  pictures
    cat.png
home/docs/todo.txt
```

`_ = t.add(...)` discards a result we don't need: an unused result is an error otherwise.

Which to choose:

| | Enum (or struct) holding its children | Arena with indexes |
|---|---|---|
| Building and reading | Natural and type-safe | Needs the arena at hand |
| Links to parents, siblings, cycles | Not possible | Easy |
| Editing in place | Rebuild the path to the change | Change `t.nodes[i]` directly |
| A node of the wrong kind | Impossible: the enum says what each case holds | Up to you |

Next: [Interfaces and function values](09-interfaces-and-functions.md).
