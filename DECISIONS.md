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
19. **The pedantic checks are always on** (unused variables, imports and results, unreachable code, shadowing, the naming rules, the error guards), and so is the formatting check. (They used to be opt-in with `seed c --strict`; the seed0-era test programs were fixed instead.)

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

## From the compiler's concurrency (compiler/README.md)

38. **`group { ... }` is an expression of type `error`** and is only used whole: `error err = group {...}`, `err = group {...}`, `return group {...}`, `_ = group {...}` or `try group {...}`. A bare `group { ... }` statement is an unchecked error.
39. **A group block cannot be left early.** `return` and `try` are not allowed inside it, nor `break`/`continue` of a loop outside it, because the group must wait for its goroutines first. (The alternative, waiting on every exit path, hides the wait in a `return`.)
40. **`spawn` takes the same calls as `defer`** (function, non-`mut` method, function value, interface method, package function) whose result is `error` or nothing. The receiver and the arguments are deep-copied for the goroutine; spawning a `mut` method is an error, because the goroutine would only change its copy.
41. **Channel elements cannot be nullable** (`chan (int?)`), because `receive()` already uses null for "closed"; `chan int?` is a nullable channel. A `chan error` receives `error` (null when closed).
42. **Select cases** are `ch.send(v)`, `ch.receive()`, `_ = ch.receive()` and `T? v = ch.receive()`, which declares a new variable; a case cannot assign to an existing one. All channels and values are evaluated, in order, before select waits.
43. **`defer ch.close()` is allowed** (the only built-in method `defer` accepts), so a producer can close its channel however it ends.
44. **Package-level constants cannot hold channels:** every goroutine computes the constants it uses for itself, so each would get its own channel.
45. **`time` is a core package** in the real compiler: `time.after(int d) chan int`, `time.sleep(int d)` and the duration constants `nanosecond` ... `hour`. `time.after` sends the time once and closes the channel, as docs/stdlib.md says.
46. **Deadlock is a crash:** when every goroutine is blocked and no timer is pending, the program prints `crash: deadlock: all goroutines are blocked` and exits with code 2, without running defers.
47. **The code of a group block itself is not stopped** when one of the group's goroutines fails: only the spawned goroutines are. The block should spawn work and leave the waiting on channels to goroutines (or to the code after the group); if it waits for a channel that only stopped goroutines would have served, the program ends with the deadlock crash.

## From packages, natives and tests (compiler/README.md)

48. **Import paths are resolved in the standard library first, then relative to the project root**, the directory of the program's `package main` (or the directory given to `seed test`). There is no project file (`seed.mod`). `import "shop/cart"` is `<root>/shop/cart`, and code calls it `cart`. A project package with the name of a standard library package can't be imported (the standard library wins).
49. **A package's name is its directory's name** (only `package main` may differ), and package `main` can't be imported.
50. **Struct fields are private unless `pub`**, and a struct literal of another package's type needs every field to be `pub`. Enum cases and interface methods are as visible as their type. Methods need `pub` to be called from other packages.
51. **A local name can't shadow an imported package's name**, and an import can't have the same name as a package-level declaration of the file's package.
52. **Enum case patterns match the case exactly.** A case with data binds every field with its type (`_` to ignore one), a case without data has no parentheses, and a `case` with several labels binds only `_`. Unused pattern bindings are errors like any unused variable.
53. **A `T?` declared beside an error is not guarded**: `string? s, error err = f()` may read `s` (it has to be checked against null anyway). Only a plain `T` waits for the error check.
54. **Tests**: a test body is like a function returning `error` (`try` works; `return` is not allowed); `assert` is only allowed in tests (and not in a `group` block); a failed `assert` ends the test like a returned error, with the message `<file>:<line>: assert failed: <condition> (<left> <op> <right>)`, the values shown for numbers, bools, bytes, strings (quoted), errors (their quoted message) and nulls. After `assert(cond)` the compiler narrows on `cond`.
55. **`seed test` runs each test in a goroutine of its own group**, one after the other, and prints one line per failed test (`FAIL "<name>": <where>: <reason>`) and `N passed, M failed`; it exits with 1 when a test failed or a package didn't compile. A test of a crash runs the code in a group: `error err = group { spawn f() }`.
56. **Native functions** are declared without a body in standard library packages and written in C in the package's directory (`lib/<pkg>/<pkg>.c`), as `sd_lib_<pkg>_<name>`; several results come back as the last result plus out-pointers. See runtime/README.md, "Native functions". (docs/stdlib.md had proposed `runtime/lib/` and returning the multiple-results struct; out-pointers are what the runtime already used, and need no compiler-internal type names.)
57. **The core packages are ordinary packages in `lib/`**: `fmt`, `os`, `fs` and `time` are natives over the runtime; `errors` is plain Seed (`new` makes a private struct that implements `error`; `wrap` was added). The compiler only knows that `os.exit` and `os.crash` never return.
58. **`os.crash(message)`** crashes the goroutine exactly like a runtime fault, so library code can report a caller's mistake; `os.executable()` and `os.cwd()` were added (the compiler uses them to find its root through symbolic links).
59. **Error messages of the core packages follow docs/stdlib.md**: `fs.read: notes.txt: no such file or directory`, `os.run: x: no such file or directory`, `string.toInt: "x": invalid integer`, `string.toFloat: "x": invalid float`. `os.run` with an empty `argv` crashes.
60. **Compiling unformatted code is an error** at the first line that differs, with the hint `run: seed fmt <file>`. The check applies to every file of the program, the standard library's included.
61. **Commands**: `seed build [-o out] <dir | file.sd>` (default output: the directory's or file's name), `seed run <dir | file.sd> [args...]` (a `--` before the arguments is skipped), `seed test [dir...]`, `seed fmt`, and `seed c` for the tests. `seed` finds its root from its real path (symbolic links resolved) unless `$SEED_ROOT` is set.
