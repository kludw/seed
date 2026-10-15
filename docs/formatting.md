# Formatting

This is part of the language spec ([SPEC.md](../SPEC.md)). Seed code has one canonical layout, produced by `seed fmt`. Formatting is part of the language: code that isn't formatted fails to compile, with an error at the first line that differs and the hint `run: seed fmt <file>`.

The style is close to Go's `gofmt`, with 4-space indentation and C-style conditions in parentheses.

## The command

```
seed fmt <file.sd | dir>...            rewrite the files in canonical form
seed fmt --check <file.sd | dir>...    change nothing; report unformatted files
```

A directory stands for every `.sd` file under it, in its subdirectories too, in sorted order; directories whose names start with `.` are skipped, and files that don't end in `.sd` are left alone. `seed fmt .` formats a whole project.

`--check` prints, for every file that is not formatted, the first line that differs, and a hint, to stderr:

```
x.sd:9: not formatted
    hint: run: seed fmt x.sd
```

The exit code is 1 if any file is not formatted (with `--check`) or has syntax errors, and 0 otherwise. A file with syntax errors is never rewritten; its errors are printed as the compiler prints them. (`seed fmt --verify` exists for testing the formatter itself; see compiler/README.md.)

## What the formatter changes

The formatter only changes whitespace and line breaks, plus the trailing comma of a list (see [Lists](#lists)). It never adds, removes or reorders anything else: parentheses, names, literals and comments are kept exactly as written. In particular it never changes what a program means. There is no maximum line length: long lines are not wrapped, and lines are only broken where the source breaks them or where the rules below require it.

The output uses `\n` line endings, ends with exactly one newline, has no trailing whitespace and no tabs outside string literals and comments.

## Indentation

Each level is 4 spaces. A block's contents are one level deeper than the line where the statement that owns the block starts.

```
fn main([]string args) error {
    for (int i = 0; i < 3; i++) {
        fmt.println(i.string())
    }
    return null
}
```

## Statements and blocks

- One statement per line.
- `{` goes at the end of the line that opens the block, after one space. The block's statements start on the next line, and `}` goes on a line of its own, at the indentation of the statement that opened the block.
- An empty block is written `{}`.
- `else` goes on the line of the `}` before it: `} else {`, `} else if (x) {`.

```
if (x > 0) { return 1 } else { return 2 }
```

becomes

```
if (x > 0) {
    return 1
} else {
    return 2
}
```

## switch and select

`case` and `default` are at the same indentation as the `switch` or `select`; the statements of a case are one level deeper, starting on the line after the `:`.

```
switch (shape) {
case Shape.Circle(float r):
    return 3.14 * r * r
case Shape.Rect(float w, float h):
    return w * h
}
```

## Declarations

- `package` comes first, then the imports, one per line, in the order written, as one group with no blank lines between them.
- `struct` fields, `enum` cases and `interface` methods are one per line. An empty body is written `{}`.

```
type Point struct { int x
    int y }
```

becomes

```
type Point struct {
    int x
    int y
}
```

## Blank lines

- At the top level of a file there is exactly one blank line after the `package` line, after the imports, and between declarations.
- Exception: a one-line `const` declaration followed by another `const` declaration keeps the source's choice of no blank line or one blank line between them, so related constants can stay in a group.
- A comment directly above a declaration (no blank line between them) stays attached to it, and the blank line goes before the comment. A comment separated from the next declaration by blank lines is followed by exactly one blank line.
- Inside blocks, type bodies and lists written one element per line, a run of blank lines between two statements (or elements, or comments) becomes one blank line. There are no blank lines right after `{`, after `case ...:` or after the opening bracket of a list, and none before a closing `}`, `)` or `]`.
- There are no blank lines inside a statement that continues over several lines.

```
const int tyVoid = 0
const int tyInt = 1

// Comment about f.
fn f() {

    a()


    b()

}
fn g() {}
```

becomes

```
const int tyVoid = 0
const int tyInt = 1

// Comment about f.
fn f() {
    a()

    b()
}

fn g() {}
```

## Spaces

- One space on each side of binary operators (`+ - * / % & | ^ << >> == != < <= > >= && ||`) and of `=`: `x = a + b * c`.
- No space after the unary operators `-` and `!`: `-x`, `!done`, `a - -b`. One space after `try`: `try f()`.
- No space before `,`, `:` and `;`; one space after `,` and after the `:` of a map entry or struct field: `f(a, b)`, `map[string]int{"a": 1}`, `Point{x: 1, y: 2}`.
- No space inside parentheses and brackets, or inside the braces of a literal: `f(x)`, `xs[i]`, `(a + b) * c`, `[]int{1, 2}`.
- No space before the `(` of a call or a parameter list, around `.`, or before `?`, `++` and `--`: `xs.len()`, `int? n`, `i++`.
- One space after a keyword: `if (x)`, `for (`, `switch (v)`, `return x`, `case 1:`. `assert(x)` is written like a call, and so is a function type: `fn(int) bool`.
- A method's receiver is separated from `fn` by one space: `fn (Point p) mut move(int dx)`.
- One space before the `{` of a block, none before the `{` of a literal: `if (ok) {`, `Point{x: 1}`.
- Types have no inner spaces except after `chan`, between a function type's parameters and its result, and after commas: `[]int`, `map[string][]int`, `[](string?)`, `chan int`, `fn(int, int) (int, bool)`. A result type follows the parameter list after one space: `fn f() (int, error)`, `fn f() []int`.
- In a `for` header, `;` is followed by one space, except in `for (;;)`: `for (int i = 0; i < n; i++)`, `for (; i < n; )`, `for (int i = 1; ; i = i * 2)`.

## Continuation lines

A statement may continue on the next line after an operator, `=`, a comma, `(` and so on (docs/grammar.md). The formatter keeps those line breaks where they are, and indents every continuation line one level deeper than the line where the statement starts. A block opened at the end of a continued line is still indented relative to the start of the statement.

```
return k == Tok.Ident || k == Tok.Int ||
        k == Tok.Float ||
  k == Tok.String
if (first &&
second) {
run()
}
```

becomes

```
return k == Tok.Ident || k == Tok.Int ||
    k == Tok.Float ||
    k == Tok.String
if (first &&
    second) {
    run()
}
```

## Lists

Call arguments, parameter lists (of functions, methods, enum cases and case patterns) and list, map and struct literals are lists. A list is either kept on one line or written one element per line:

- If the source breaks a line right after the opening bracket, before or after a comma between elements, or before the closing bracket, the list is written one element per line. Every element is followed by a comma, including the last one, and the closing bracket goes on a line of its own at the indentation of the line with the opening bracket.
- Otherwise the list stays on one line, and a trailing comma is removed.

Line breaks inside an element do not count. That is why a call whose last argument is a multi-line literal keeps its closing `)` next to the literal's `}`.

```
[]int a = []int{1, 2, 3,}
[]int b = []int{1, 2,
    3}
call(a, Point{
    x: 1,
    y: 2,
})
Diag d = Diag{pos: pos,
    message: m, hint: ""}
```

becomes

```
[]int a = []int{1, 2, 3}
[]int b = []int{
    1,
    2,
    3,
}
call(a, Point{
    x: 1,
    y: 2,
})
Diag d = Diag{
    pos: pos,
    message: m,
    hint: "",
}
```

Parentheses that do not hold a list are never split into elements and never get a trailing comma: conditions of `if`, `for` and `switch`, grouping parentheses, result lists such as `(int, error)`, a method receiver, `assert(...)` and `chan T(n)`. The same holds for `[...]`. A line break inside them is a continuation line.

## Comments

Comments are kept exactly as written, except that trailing spaces and tabs are removed.

- A comment that follows code on its line stays at the end of that line, separated from the code by one space (comments are not aligned with each other).
- A comment on a line of its own stays on a line of its own, indented like the code that follows it.
- A comment just before a closing `}` (or the closing bracket of a list written one element per line) is indented like the contents of the block or list.
- A comment just before `case` or `default` is indented like the `case` if in the source it starts at or to the left of the `case` keyword; if it starts further right, it is indented like the statements of the case before it.

```
switch (x) {
case 1:
    foo()
        // more about case 1
// about case 2
case 2:
    bar()
}
```

becomes

```
switch (x) {
case 1:
    foo()
    // more about case 1
// about case 2
case 2:
    bar()
}
```

A comment can force a line break where the formatter would otherwise join two lines (for example `} else // why` followed by `{` on the next line); the break is then kept.
