# 6. Lists and maps

Seed has two built-in collections: the list `[]T` and the map `map[K]V`. Both are values, like everything in Seed except channels: assigning one or passing it to a function behaves like copying it.

## Lists

A list grows in place. There are no fixed-size arrays and no slices that share memory with another list.

```seed
[]int xs = []int{5, 3, 8}
[]string names = []string{}
[][]int grid = [][]int{[]int{1, 2}, []int{3, 4}}
```

| Method | Result |
|---|---|
| `xs.len()` | The number of elements. |
| `xs[i]`, `xs[i] = v` | Read or write an element. Crashes when `i` is out of range. |
| `xs.push(x)` | Adds `x` to the end. `mut` |
| `xs.pop()` | Removes the last element and returns it as a `T?`, or `null` if the list is empty. `mut` |
| `xs.insert(i, x)` | Inserts `x` at index `i` (`0 <= i <= len`). `mut` |
| `xs.remove(i)` | Removes the element at `i`. `mut` |
| `xs.contains(x)` | `bool`. |
| `xs.index(x)` | The first index of `x` as an `int?`, or `null`. |
| `xs.slice(a, b)` | A new list with the elements `a` up to (not including) `b`. |
| `xs.reverse()` | Reverses the list in place. `mut` |
| `xs.sort()` | Sorts in place. Only for `[]int`, `[]float`, `[]string` and `[]byte`. `mut` |
| `xs.sortBy(f)` | Sorts in place with `f`, a `fn(T, T) bool` that returns true when its first argument goes first. Stable. `mut` (see [chapter 9](09-interfaces-and-functions.md)) |
| `xs.join(sep)` | Only for `[]string`: the strings joined with `sep` between them. |

The methods marked `mut` change the list they are called on, so they can only be called on a variable (or a part of one, like `team[0].items`), never on a function parameter. `push` is the only way to append; there is no `append` function.

```seed
package main

import "fmt"

fn show(string label, []int xs) {
    []string parts = []string{}
    for (int i = 0; i < xs.len(); i++) {
        parts.push(xs[i].string())
    }
    fmt.println(label + ": [" + parts.join(", ") + "]")
}

fn main([]string args) error {
    []int xs = []int{5, 3, 8}
    xs.push(1)
    show("push", xs)

    xs.insert(0, 9)
    show("insert", xs)

    xs.remove(1)
    show("remove", xs)

    xs[0] = 7
    show("set", xs)

    int? last = xs.pop()
    if (last != null) {
        fmt.println("popped " + last.string())
    }
    show("pop", xs)

    fmt.println("len " + xs.len().string() + ", contains 8: " + xs.contains(8).string())

    int? where = xs.index(8)
    if (where != null) {
        fmt.println("8 is at " + where.string())
    }
    fmt.println("index of 42 is null: " + (xs.index(42) == null).string())

    show("slice(1, 3)", xs.slice(1, 3))

    xs.reverse()
    show("reverse", xs)

    xs.sort()
    show("sort", xs)

    []string words = []string{"pear", "fig", "apple"}
    words.sort()
    fmt.println(words.join(" "))

    []int empty = []int{}
    fmt.println("pop on empty is null: " + (empty.pop() == null).string())
    return null
}
```

```
push: [5, 3, 8, 1]
insert: [9, 5, 3, 8, 1]
remove: [9, 3, 8, 1]
set: [7, 3, 8, 1]
popped 1
pop: [7, 3, 8]
len 3, contains 8: true
8 is at 2
index of 42 is null: true
slice(1, 3): [3, 8]
reverse: [8, 3, 7]
sort: [3, 7, 8]
apple fig pear
pop on empty is null: true
```

## Maps

```seed
map[string]int ages = map[string]int{"ann": 31, "bob": 27}
map[int][]string byLength = map[int][]string{}
```

Keys are `int`, `byte`, `bool` or `string`. Values can be anything.

| Operation | Result |
|---|---|
| `m[k]` | The value as a `V?`, or `null` if the key is missing. |
| `m[k] = v` | Inserts or replaces. A new key goes to the end of the order; an existing key keeps its place. |
| `m.getOr(k, d)` | The value for `k`, or `d` if the key is missing (a plain `V`). Doesn't change the map. |
| `m.len()` | The number of entries. |
| `m.delete(k)` | Removes a key, if it is there. `mut` |
| `m.keys()` | The keys as a `[]K`, in insertion order. |

**Maps keep insertion order**, so looping over `m.keys()` gives the same order on every run. Go randomises map order to stop programs from depending on it; Seed makes the order something you can depend on.

A lookup returns `V?` because the key may be missing. Store the result in a variable and check it before use ([chapter 5](05-null-safety.md)), or compare it with a value: `counts["cat"] == 2` is false when the key is missing. When a missing key has a natural default, `m.getOr(k, d)` returns a plain `V`, the value or `d`. Counting is the classic case, one line per count:

```seed
counts[w] = counts.getOr(w, 0) + 1
```

(`counts[w]++` is not allowed: `counts[w]` may be missing, and `++` would have to invent a value for it.)

```seed
package main

import "fmt"

fn main([]string args) error {
    map[string]int counts = map[string]int{}
    []string words = "the cat saw the dog and the cat ran".split(" ")
    for (int i = 0; i < words.len(); i++) {
        counts[words[i]] = counts.getOr(words[i], 0) + 1
    }

    // keys() is in insertion order, so this output is always the same.
    []string keys = counts.keys()
    for (int i = 0; i < keys.len(); i++) {
        fmt.println(keys[i] + " " + counts.getOr(keys[i], 0).string())
    }

    // A lookup is an int?: check it, or compare it with an int.
    int? dogs = counts["dog"]
    if (dogs != null) {
        fmt.println("dog: " + dogs.string())
    }
    fmt.println((counts["cat"] == 2).string() + " " + (counts["cow"] == 0).string())

    counts.delete("the")
    counts.delete("missing")
    fmt.println(counts.len().string() + " words left")

    // Setting an existing key keeps its place; a new key goes to the end.
    counts["cat"] = 10
    counts["the"] = 0
    fmt.println(counts.keys().join(" "))

    map[int]bool seenIds = map[int]bool{7: true}
    fmt.println((seenIds[7] != null).string() + " " + (seenIds[8] != null).string())
    return null
}
```

```
the 3
cat 2
saw 1
dog 1
and 1
ran 1
dog: 1
true false
5 words left
cat saw dog and ran the
true false
```

A set is a `map[K]bool`: `seenIds[id] != null` asks whether `id` is in it.

## Values and copy-on-write

Every list and map behaves like an independent copy. After `[]int b = a`, changing `b` never changes `a`, and a function that receives a list can't change the caller's list.

```seed
package main

import "fmt"

// xs is the function's own copy of the caller's list.
fn withZero([]int xs) []int {
    []int out = xs
    out.push(0)
    return out
}

fn main([]string args) error {
    []int a = []int{1, 2, 3}
    []int b = a
    b.push(4)
    b[0] = 100
    fmt.println("a has " + a.len().string() + ", a[0] = " + a[0].string())
    fmt.println("b has " + b.len().string() + ", b[0] = " + b[0].string())

    []int c = withZero(a)
    fmt.println("a has " + a.len().string() + ", c has " + c.len().string())

    // Lists inside lists are values too.
    [][]int grid = [][]int{[]int{0, 0}, []int{0, 0}}
    []int row = grid[0]
    row[1] = 5
    fmt.println(grid[0][1].string() + " " + row[1].string())
    grid[1][1] = 7
    fmt.println(grid[1][1].string())

    // To change a list stored in a map: take it out, change it, put it back.
    map[string][]string groups = map[string][]string{"red": []string{"apple"}}
    []string? reds = groups["red"]
    if (reds != null) {
        reds.push("cherry")
        groups["red"] = reds
    }
    []string? after = groups["red"]
    if (after != null) {
        fmt.println(after.join(", "))
    }

    // Lists and maps compare by contents.
    fmt.println(([]int{1, 2} == []int{1, 2}).string())
    return null
}
```

```
a has 3, a[0] = 1
b has 4, b[0] = 100
a has 3, c has 4
0 5
7
apple, cherry
true
```

Copying everything would be slow, so Seed doesn't. Under the hood, `[]int b = a` makes `a` and `b` share the same memory and counts the owners. The first time one of them is changed while it is shared, that one gets its own copy (**copy-on-write**). A value with a single owner is changed in place, with no copy at all. In practice:

- Passing a list to a function, returning it, or storing it in a struct never copies the elements.
- `xs.push(x)` on a list that only you hold is an ordinary append.
- A change to a shared list copies it once; later changes to the new copy are in place again.

Two habits keep programs fast:

- **Change data where it lives.** `grid[1][1] = 7` and `team[0].items.push(x)` change the element in place. Taking it out first (`[]int row = grid[0]`) creates a second owner, and the next change to either copies the row.
- **Build big strings in a list.** `parts.push(piece)` and one `parts.join("")` at the end is linear. `s = s + piece` on a local string is fine too: it appends in place while nobody else holds `s`.

Memory is managed by reference counting. Values can't refer to each other, so there are no cycles and nothing leaks, and memory is freed the moment its last owner is gone, with no garbage-collector pauses.

Next: [Structs, methods and `mut`](07-structs-and-methods.md).
