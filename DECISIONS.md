# Decisions made during the overnight build

You asked not to be woken up, so these are the language decisions I made without you. Each one is reflected in the spec. Anything here can be reversed.

## Syntax

1. **`switch` over an enum cannot have `default`.** Every case must be listed, so adding an enum case makes the compiler point at every `switch` that needs updating.
2. **`switch` over `int`, `byte`, `string` or `bool` must have `default`,** and one `case` can list several values: `case 'a', 'e':`.
3. **Byte literals use single quotes:** `'a'`, `'\n'`. A lexer can't be written comfortably without them.
4. **Float literals must contain a `.`** (`1.0`, `2.5e3`), so `1` is always an `int`.
5. **No raw or multi-line strings.** One way to write a string.
6. **`?` applies to the whole type on its left.** `[]string?` is a nullable list. A list of nullable strings is written `[](string?)`.
7. **Newlines end statements using Go's rule.** A line that continues must end with an operator or a comma.

## Types and built-ins

8. **`error` is the one type that can be `null` without `?`.** A null error means "no error". This matches every example we wrote (`error err = ...`, `if (err != null)`).
9. **`byte.string()` gives the number** (`'a'.string()` is `"97"`), the same as `int`. To build text from bytes, use `string([]byte)`.
10. **`s.split`, `s.trim`, `s.replace`, `s.toInt()` and friends are methods on `string`,** not functions in a `strings` package. `xs.join(sep)` is a method on `[]string`. The full list is in [docs/builtins.md](docs/builtins.md).
11. **`fmt.eprint` and `fmt.eprintln`** write to stderr.
12. **Crash vs error exit codes:** an error returned from `main` exits with 1 (`error: ...`), a crash exits with 2 (`crash: ...`).
13. **Map keys are limited to `int`, `byte`, `bool` and `string`.**

## Implementation

14. **The bootstrap compiler `seed0` implements `int` as 64-bit** with overflow detection. Only the real compiler has integers that grow.
15. **The runtime is written in C** and lives in `runtime/`. Generated programs are C files compiled by the system C compiler.

## The compiler in Seed (compiler/)

Details are in [compiler/README.md](compiler/README.md#decisions). The ones that touch the language:

16. **Imports are per file.** A core package must be imported in the file that uses it (seed0 treats imports as package-wide).
17. **`fn(int) f` vs `fn(int) bool f`.** Where a type is followed by a name, an identifier right after a function type's `)` is its result type only if another name, `.` or `?` follows it. Elsewhere it is always the result type.
18. **`select` with `default` is a syntax error**, with a hint to use a timeout channel.
19. **The pedantic checks are opt-in for now** (`seed c --strict`): unused variables, imports and results, unreachable code, locals shadowing package-level names, and the naming rules. Some seed0-era test programs fail them; the compiler's own source is always built with `--strict`.

## From the standard library design (docs/stdlib.md)

20. **Struct fields are private unless marked `pub`.** Otherwise any program could forge a file or socket handle.
21. **Standard-library functions can be declared without a body** and implemented in C. Only the standard library may do this.
22. **`spawn` can appear anywhere inside a `group` block,** including loops. Worker pools and servers need it.
23. **Blocking standard-library calls are cancellation points,** as well as channel operations. Otherwise a cancelled server goroutine would hang in `accept` forever.
24. **Strings can hold any bytes.** Files, sockets and hashes use `string` for raw data.
25. **Float division by zero gives `inf`/`nan`;** only integer division by zero crashes.
26. **A deferred call that returns an error must discard it:** `defer _ = f.close()`.
27. **No type switches on interfaces.** Errors are told apart by message or by returning richer results.
28. **Channels are the one reference type.** Copying a channel gives another handle to the same channel (reply channels inside messages).
29. **Defers run last-first,** like Go. **`break`/`continue` inside a `switch` apply to the loop.** **A struct converts to an interface it `implements` automatically.**
30. **Map equality ignores insertion order;** errors are equal when their messages are.

## From the compiler's interfaces, function values and defer (compiler/README.md)

31. **A nullable function type is written with parentheses:** `(fn(int) int)?`. `fn(int) int?` is a function that returns `int?` (rule 6 applied to the result type).
32. **`defer` takes a call of a function, method, function value, interface method or package function.** It cannot call a `mut` method (the variable it would change is gone when the call runs) or a built-in method.
33. **Interface methods can be `mut`.** Calling one needs a variable (or a part of one), as with any `mut` method; it changes the value inside that interface variable only.
34. **`implements` requires the exact signature,** including `mut`. Methods cannot be declared on interface types.
35. **A value returned beside an error can be read once the error is known to be null:** inside `if (err == null)`, after `if (err != null) { return ... }`, or right of `err == null &&`. Assigning the value again also lifts the rule; the check is block-scoped.
36. **A struct field of function type is called directly:** `op.apply(1, 2)`, when the struct has no method of that name.
37. **A type that implements an interface converts to the nullable interface too** (`Shape? s = Square{...}`), and a type that implements `error` converts to `error`.
