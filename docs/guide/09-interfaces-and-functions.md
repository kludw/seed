# 9. Interfaces and function values

## Interfaces

An interface lists methods. A type satisfies it only by saying so with `implements`:

```seed
type Shape interface {
    area() float
    name() string
}

type Square struct implements Shape {
    float side
}
```

The compiler then checks that `Square` has every method of `Shape`, with exactly the same parameters, results and `mut`. In Go, a type satisfies an interface by accident of having the right methods, and you find out it doesn't at the place where you use it. In Seed the intent is written on the type, the check happens there, and a reader can see at a glance what a type is for.

A struct or an enum that implements an interface converts to it automatically wherever the interface is expected: in an assignment, an argument, a return, or a list literal.

```seed
package main

import "fmt"

type Shape interface {
    area() float
    name() string
}

type Square struct implements Shape {
    float side
}

fn (Square s) area() float {
    return s.side * s.side
}

fn (Square s) name() string {
    return "square"
}

// Enums can implement interfaces too.
type Blob enum implements Shape {
    Small
    Big(float size)
}

fn (Blob b) area() float {
    switch (b) {
    case Blob.Small:
        return 1.0
    case Blob.Big(float size):
        return size
    }
}

fn (Blob b) name() string {
    return "blob"
}

// An interface method can be mut.
type Counter interface {
    mut add(int n)
    total() int
}

type Tally struct implements Counter {
    int sum
}

fn (Tally t) mut add(int n) {
    t.sum = t.sum + n
}

fn (Tally t) total() int {
    return t.sum
}

fn totalArea([]Shape shapes) float {
    float sum = 0.0
    for (int i = 0; i < shapes.len(); i++) {
        sum = sum + shapes[i].area()
    }
    return sum
}

fn main([]string args) error {
    []Shape shapes = []Shape{Square{side: 2.0}, Blob.Small, Blob.Big(2.5)}
    for (int i = 0; i < shapes.len(); i++) {
        fmt.println(shapes[i].name() + " " + shapes[i].area().string())
    }
    fmt.println("total " + totalArea(shapes).string())

    Counter c = Tally{sum: 0}
    c.add(3)
    c.add(4)
    fmt.println(c.total().string())

    Shape? nothing = null
    if (nothing == null) {
        fmt.println("no shape")
    }
    return null
}
```

```
square 4.0
blob 1.0
blob 2.5
total 7.5
7
no shape
```

More rules:

- A type can implement several interfaces: `type File struct implements Reader, Writer { ... }`.
- An interface value is a value like any other. Copying it copies what it holds, and a `mut` method called through it (`c.add(3)`) changes only that interface variable.
- There is no way to get the original type back out of an interface value: no type switches and no type assertions. If callers need to know which kind of thing they have, use an enum, which is exactly a closed set of kinds, with an exhaustive switch.
- Methods can't be declared on an interface type, and there is no built-in `any`: since every type must declare what it implements, an empty interface would hold only the types that say so. There are no generics in user code either: only the built-in types (`[]T`, `map[K]V`, `chan T`, `T?`) are generic.

## error is an interface

`error` is a built-in interface with one method, `message() string`. Any struct or enum that implements it is an error:

```seed
package main

import "errors"
import "fmt"

// Any type can be an error by implementing error's one method.
type ParseError struct implements error {
    int line
    string reason
}

fn (ParseError e) message() string {
    return "line " + e.line.string() + ": " + e.reason
}

fn parseLine(int line, string text) (int, error) {
    if (text == "") {
        return null, ParseError{line: line, reason: "empty line"}
    }
    int n, error err = text.toInt()
    if (err != null) {
        return null, errors.new("line " + line.string() + ": " + err.message())
    }
    return n, null
}

fn sumLines([]string lines) (int, error) {
    int sum = 0
    for (int i = 0; i < lines.len(); i++) {
        sum = sum + try parseLine(i + 1, lines[i])
    }
    return sum, null
}

fn main([]string args) error {
    int total = try sumLines([]string{"1", "2", "3"})
    fmt.println("total " + total.string())

    int bad, error err = sumLines([]string{"1", "", "3"})
    if (err != null) {
        fmt.println("failed: " + err.message())
    } else {
        fmt.println("total " + bad.string())
    }
    return null
}
```

```
total 6
failed: line 2: empty line
```

Since there are no type switches, a caller can't ask whether an `error` is a `ParseError`. An error type is about the message it builds. Adding context as an error is passed up is done by making a new error around the old message, as `parseLine` does.

## Function values

A named function can be used as a value. Its type is written `fn(parameter types) result`:

```seed
fn(int) int f = square
fn(Person, Person) bool order = byAge
```

Function values can be passed to functions, returned, stored in variables, lists, maps and struct fields. A field of function type is called directly: `cmd.run(7, 2)`. (A struct can't also have a method called `run`, so `cmd.run` always means one thing.)

```seed
package main

import "fmt"

type Person struct {
    string name
    int age
}

fn byAge(Person a, Person b) bool {
    return a.age < b.age
}

fn byName(Person a, Person b) bool {
    return a.name < b.name
}

fn names([]Person people) string {
    []string out = []string{}
    for (int i = 0; i < people.len(); i++) {
        out.push(people[i].name)
    }
    return out.join(" ")
}

// A function value is passed like any other value.
fn applyAll([]int xs, fn(int) int f) []int {
    []int out = []int{}
    for (int i = 0; i < xs.len(); i++) {
        out.push(f(xs[i]))
    }
    return out
}

fn square(int x) int {
    return x * x
}

fn negate(int x) int {
    return -x
}

// A struct field can hold a function, and is called directly.
type Command struct {
    string name
    fn(int, int) int run
}

fn add(int a, int b) int {
    return a + b
}

fn sub(int a, int b) int {
    return a - b
}

fn main([]string args) error {
    []Person people = []Person{
        Person{name: "Cy", age: 25},
        Person{name: "Ann", age: 29},
        Person{name: "Bo", age: 35},
    }
    people.sortBy(byAge)
    fmt.println(names(people))
    people.sortBy(byName)
    fmt.println(names(people))

    []int squares = applyAll([]int{1, 2, 3}, square)
    []int negated = applyAll(squares, negate)
    fmt.println(squares[2].string() + " " + negated[2].string())

    []Command commands = []Command{Command{name: "add", run: add}, Command{name: "sub", run: sub}}
    for (int i = 0; i < commands.len(); i++) {
        fmt.println(commands[i].name + " 7 2 = " + commands[i].run(7, 2).string())
    }

    // A nullable function type needs parentheses.
    (fn(int) int)? hook = null
    if (args.len() > 5) {
        hook = square
    }
    if (hook != null) {
        fmt.println(hook(3).string())
    } else {
        fmt.println("no hook")
    }
    return null
}
```

```
Cy Ann Bo
Ann Bo Cy
9 -9
add 7 2 = 9
sub 7 2 = 5
no hook
```

### sortBy

`xs.sortBy(f)` sorts a list with any order you like. `f` is a `fn(T, T) bool` that returns `true` when its first argument should come first. The sort is stable, so sorting by a second key keeps the order of the first among equal elements. (`xs.sort()` sorts lists of `int`, `float`, `string` and `byte` in their natural order.)

To sort by several keys, compare them in turn:

```seed
fn byCountThenWord(WordCount a, WordCount b) bool {
    if (a.count != b.count) {
        return a.count > b.count
    }
    return a.word < b.word
}
```

### No closures

There are no anonymous functions and no closures. A function value is always a named, top-level function, so it never carries hidden state: what it does depends only on its arguments. When a function needs extra data, pass it in explicitly, or use an interface whose implementing struct carries the data in its fields.

Function values can't be compared with `==`.

Next: [Concurrency](10-concurrency.md).
