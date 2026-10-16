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

// Enums can implement interfaces too. Blob's name() is the built-in one
// that every enum has (chapter 8): the name of its case.
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
Small 1.0
Big 2.5
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

## Function literals

A *function literal* is a function written where a value is needed: `fn`, the parameters, the results and a body, like a declaration without a name. Its type comes from its parameters and results: `fn(Person a, Person b) bool { ... }` is a `fn(Person, Person) bool`.

```seed
people.sortBy(fn(Person a, Person b) bool {
    return a.age > b.age
})
```

The body goes on lines of its own, like every block, and a literal that is the last argument of a call closes with `})`.

A literal can use the variables around it. It **captures them by value**: when the literal is evaluated, it takes a copy of the value each variable has at that moment (cheap, because values are copy-on-write), and from then on the literal and the variable are independent. This is what makes literals useful for the callbacks that need some data: a comparator that depends on a parameter, a filter, the function given to `regex.replaceWith`.

```seed
package main

import "fmt"
import "regex"

type Person struct {
    string name
    int age
}

// Returns a function that remembers min: a copy of it, taken now.
fn olderThan(int min) fn(Person) bool {
    return fn(Person p) bool {
        return p.age > min
    }
}

fn filter([]Person people, fn(Person) bool keep) []Person {
    []Person out = []Person{}
    for (int i = 0; i < people.len(); i++) {
        if (keep(people[i])) {
            out.push(people[i])
        }
    }
    return out
}

fn names([]Person people) string {
    []string out = []string{}
    for (int i = 0; i < people.len(); i++) {
        out.push(people[i].name)
    }
    return out.join(" ")
}

fn main([]string args) error {
    []Person people = []Person{
        Person{name: "Cy", age: 25},
        Person{name: "Ann", age: 29},
        Person{name: "Bo", age: 35},
    }

    // A comparator that needs a value from around it.
    string first = "Bo"
    people.sortBy(fn(Person a, Person b) bool {
        if (a.name == first || b.name == first) {
            return a.name == first
        }
        return a.name < b.name
    })
    fmt.println(names(people))

    fmt.println(names(filter(people, olderThan(26))))

    // Each evaluation captures the values of that moment.
    []fn() string greeters = []fn() string{}
    for (int i = 0; i < people.len(); i++) {
        string name = people[i].name
        greeters.push(fn() string {
            return "hi " + name + " (" + i.string() + ")"
        })
    }
    for (int j = 0; j < greeters.len(); j++) {
        fmt.println(greeters[j]())
    }

    // State for regex.replaceWith: a map captured by the literal.
    map[string]string vars = map[string]string{"name": "Ann", "city": "Oslo"}
    regex.Regex re = regex.mustCompile("\\{([a-z]+)\\}")
    string text = re.replaceWith("{name} lives in {city}, {country}", fn(regex.Match m) string {
        string? key = m.groupText(1)
        if (key == null) {
            return ""
        }
        return vars.getOr(key, "?")
    })
    fmt.println(text)
    return null
}
```

```
Bo Ann Cy
Bo Ann
hi Bo (0)
hi Ann (1)
hi Cy (2)
Ann lives in Oslo, ?
```

`olderThan` returns a literal that keeps its own copy of `min`. The loop makes three literals, and each one has the `name` and `i` of its own iteration, so there is no Go-style surprise where every function sees the last value of the loop variable.

Captured variables are read-only inside the literal. Changing one would change only the literal's copy, which is almost never what was meant, so the compiler rejects it:

```seed
package main

import "fmt"

fn main([]string args) error {
    int count = 0
    fn() next = fn() {
        count = count + 1
    }
    next()
    fmt.println(count.string())
    return null
}
```

```
09_closure_counter.sd:8:9: error: cannot assign to count: count is captured by the function literal, which has a read-only copy of its value
    8 |         count = count + 1
      |         ^
    hint: declare a local copy inside the literal (int countCopy = count) and change that, or return the new value
```

A function value therefore never shares mutable state with anything: calling it twice with the same arguments does the same thing, and goroutines stay share-nothing. For a counter, keep the count in a variable and let the function return the new value (`count = next(count)`), or keep it in a struct with a `mut` method.

The rest follows from a literal being an ordinary function:

- Its parameters and locals follow the usual rules: no shadowing of the variables around it, no unused locals.
- `return` and `try` belong to the literal (`try` needs the literal's own last result to be `error`); `break` and `continue` need a loop inside the literal, and `spawn` a `group` inside it. A `defer` in a literal runs when the literal returns.
- What the compiler knew about a captured variable where the literal was evaluated still holds: a `T?` checked against `null` before the literal is a `T` inside it.
- A literal can't call itself through the variable it is assigned to (that variable is declared after its value); recursion goes through a named function.
- A function value can be sent on a channel, stored anywhere, and passed to `spawn`, which copies it with its captures for the new goroutine: `spawn fn(int id) error { ... }(i)` starts a literal in a goroutine.
- In a crash's trace, a literal is named after the function it is in and its line: `at sortByRank.<fn at line 16> (rank.sd:18:16)`.

Function values, literals included, can't be compared with `==`.

Next: [Concurrency](10-concurrency.md).
