# 3. Control flow

Seed has `if`, one `for`, `switch`, `break`, `continue` and `return`. Conditions are always in parentheses, and braces are always required, with the opening brace on the same line.

## if

```seed
if (x > 0) {
    fmt.println("positive")
} else if (x < 0) {
    fmt.println("negative")
} else {
    fmt.println("zero")
}
```

The condition must be a `bool`: `if (n)` or `if (name)` doesn't compile. Unlike Go, `if` has no init statement; declare the variable on the line before.

## for

There is one loop, the C-style `for`, and it covers every case:

```seed
for (int i = 0; i < n; i++) { ... }   // counting
for (; n != 1; ) { ... }               // "while"
for (;;) { ... }                       // forever, until break or return
```

There is no `range` and no `foreach`. Loop over a list by index with `xs.len()`, and over a map with `m.keys()` (see [chapter 6](06-lists-and-maps.md)).

```seed
package main

import "fmt"

fn main([]string args) error {
    // The classic three-part loop.
    for (int i = 1; i <= 15; i++) {
        if (i % 15 == 0) {
            fmt.println("FizzBuzz")
        } else if (i % 3 == 0) {
            fmt.println("Fizz")
        } else if (i % 5 == 0) {
            fmt.println("Buzz")
        } else {
            fmt.println(i.string())
        }
    }

    // A "while" loop: leave out the first and last parts.
    int n = 27
    int steps = 0
    for (; n != 1; ) {
        if (n % 2 == 0) {
            n = n / 2
        } else {
            n = 3 * n + 1
        }
        steps++
    }
    fmt.println("collatz(27) takes " + steps.string() + " steps")

    // An infinite loop, left with break.
    int power = 1
    for (;;) {
        power = power * 2
        if (power > 1000) {
            break
        }
    }
    fmt.println(power.string())

    // continue skips to the next iteration (running i++ first).
    []string evens = []string{}
    for (int i = 0; i < 10; i++) {
        if (i % 2 == 1) {
            continue
        }
        evens.push(i.string())
    }
    fmt.println(evens.join(" "))

    // Looping over a list.
    []string fruits = []string{"apple", "banana", "cherry"}
    for (int i = 0; i < fruits.len(); i++) {
        fmt.println(i.string() + ": " + fruits[i])
    }
    return null
}
```

```
1
2
Fizz
4
Buzz
Fizz
7
8
Fizz
Buzz
11
Fizz
13
14
FizzBuzz
collatz(27) takes 111 steps
1024
0 2 4 6 8
0: apple
1: banana
2: cherry
```

The loop variable `i` belongs to the loop, so two loops in a row can both use `i`. A loop variable may not shadow a name from outside, though.

## switch

`switch` has two forms. Neither falls through, and neither needs `break`.

**A switch over a value** (`int`, `byte`, `string` or `bool`) compares the value with each case in turn. One case can list several values, and the switch **must end with `default`**, so it always says what happens to the values nobody listed (even for a `bool` that lists both `true` and `false`).

**A switch over an enum** lists the enum's cases. It **must list every case and cannot have `default`**. When you add a case to the enum later, the compiler points at every switch that needs updating. Cases can carry data, which the switch binds to typed variables; [chapter 8](08-enums.md) covers that.

```seed
package main

import "fmt"

type Direction enum {
    North
    East
    South
    West
}

fn turnRight(Direction d) Direction {
    switch (d) {
    case Direction.North:
        return Direction.East
    case Direction.East:
        return Direction.South
    case Direction.South:
        return Direction.West
    case Direction.West:
        return Direction.North
    }
}

fn describe(Direction d) string {
    switch (d) {
    case Direction.North, Direction.South:
        return "vertical"
    case Direction.East, Direction.West:
        return "horizontal"
    }
}

fn countLetters(string text) string {
    int vowels = 0
    int spaces = 0
    int others = 0
    for (int i = 0; i < text.len(); i++) {
        switch (text[i]) {
        case 'a', 'e', 'i', 'o', 'u':
            vowels++
        case ' ':
            spaces++
        default:
            others++
        }
    }
    return vowels.string() + " vowels, " + spaces.string() + " spaces, " + others.string() + " others"
}

fn main([]string args) error {
    Direction d = Direction.North
    for (int i = 0; i < 4; i++) {
        fmt.println(describe(d))
        d = turnRight(d)
    }
    fmt.println(countLetters("hello seed world"))

    string command = "stop"
    switch (command) {
    case "go", "start":
        fmt.println("starting")
    case "stop":
        fmt.println("stopping")
    default:
        fmt.println("unknown command " + command)
    }
    return null
}
```

```
vertical
horizontal
vertical
horizontal
5 vowels, 2 spaces, 9 others
stopping
```

Enum cases are always written with their type, `Direction.North`, both when you make a value and in a `case`. `turnRight` needs no `return` after the switch: the compiler knows that a switch which covers every case, with every case returning, always returns.

There is no tagless `switch { case x > 0: ... }`; use `if`/`else if` for conditions.

## break and continue

`break` leaves the innermost loop and `continue` goes on with its next iteration. Inside a `switch` they still refer to the loop, because a `switch` never needs `break` for itself:

```seed
package main

import "fmt"

fn main([]string args) error {
    []string words = []string{"one", "skip", "two", "end", "three"}
    for (int i = 0; i < words.len(); i++) {
        switch (words[i]) {
        case "skip":
            continue
        case "end":
            break
        default:
            fmt.println(words[i])
        }
    }
    fmt.println("done")
    return null
}
```

```
one
two
done
```

There are no labels and no `goto`. To leave several loops at once, move them into a function and `return`.

Next: [Functions and errors](04-functions-and-errors.md).
