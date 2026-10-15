# Decisions made without you

You asked not to be woken up, so these are the language and toolchain decisions that were made during the overnight build without you.

**How to read this.** Each line is one decision, in its final state: where a later decision replaced an earlier one, only the final one is listed (the history is in git). Every decision here is reflected in [SPEC.md](SPEC.md) and the documents it links to, which give the full rules. Nothing here is set in stone: anything can be reversed, and the line tells you what would change. Implementation details that don't affect how you write Seed are in [compiler/README.md](compiler/README.md#decisions) and [runtime/README.md](runtime/README.md#16-decisions-and-limitations).

## Syntax

- **No semicolons.** One statement per line; `;` only separates the parts of a `for` header.
- **Newlines end statements using Go's rule.** A line continues when its last token can't end a statement: an operator, `=`, `.`, a comma or an opening bracket.
- **Byte literals use single quotes:** `'a'`, `'\n'`, with the same escapes as strings. A lexer can't be written comfortably without them.
- **Float literals must contain a `.`** (`1.0`, `2.5e3`), so `1` is always an `int`. Integers are decimal or hex (`0xff`), of any size; no octal, binary or `_` separators.
- **A number never starts with `0` followed by a digit.** `017` is an error with the hint `write 17` (C and Go would read it as octal 15); `0`, `0.5` and `0x0f` are fine.
- **Names are ASCII.** Identifiers are ASCII letters, digits and `_`; any other character outside strings and comments is an error, reported once as the whole UTF-8 character.
- **Built-in type names (`int`, `string`, ...) are predeclared identifiers,** not keywords, and like every name they can't be declared again.
- **No raw or multi-line strings.** One way to write a string.
- **`?` applies to the whole type on its left.** `[]string?` is a nullable list; a list of nullable strings is `[](string?)`. So a nullable function type is `(fn(int) int)?`, and `fn(int) int?` returns `int?`.
- **`fn(int) f` vs `fn(int) bool f`.** Where a type is followed by a name, an identifier after a function type's `)` is its result type only if another name, `.` or `?` follows.
- **Several results: a typed name is declared, an untyped one is assigned.** `string text, err = fs.read(p)` declares `text` and reuses `err`.
- **No labels and no `goto`.** `break`/`continue` inside a `switch` or `select` apply to the enclosing loop.
- **Case bindings write their type** like every declaration: `case Shape.Rect(float w, float h):`.
- **A case without data has no parentheses,** in its declaration (`Eof`, not `Eof()`), as a value (`Token.Eof`) and in a pattern.
- **Comparisons don't chain** (`a < b < c` is an error), and `x += 1` gets an error with the hint `x = x + 1`.

## Types and values

- **`error` is the one type that can be `null` without `?`.** A null error means "no error".
- **Package-level variables are not allowed**, only `const`: goroutines share nothing, so there is no global mutable state.
- **Constants are computed at compile time:** literals, other constants, operators, conversions and value-only built-in methods (`name.len()`); no calls. They are allowed inside functions too, and cannot hold channels.
- **Map keys are limited to `int`, `byte`, `bool` and `string`.** Maps keep insertion order; map `==` ignores the order.
- **Map values can't be nullable** (`map[K](V?)`), because `m[k]` already returns `V?` with null for a missing key; for the same reason `m[k]++` is not allowed.
- **`==` compares contents** for structs, lists, maps and enums; errors are equal when their messages are; interfaces compare dynamic type and value. Channels and function values have no `==`.
- **Strings can hold any bytes.** Files, sockets and hashes use `string` for raw data.
- **`string()` exists on `int`, `float`, `bool`, `byte` and `string` only.** `byte.string()` gives the number (`"97"`); text from bytes is `string([]byte)`. Lists and maps have no text form.
- **Bytes have bitwise operators but no arithmetic;** convert with `int(b)`.
- **Shifts:** the count has the operand's type (`b << byte(1)`) and a negative count crashes; `int` shifts never overflow and `>>` rounds toward negative infinity; `byte` shifts keep the low 8 bits.
- **List operations crash out of range:** `xs[i]`, `xs[i] = v` (which never grows the list), `insert` (`0 <= i <= len`), `remove` and `slice` (`0 <= a <= b <= len`), like their string counterparts.
- **String operations are methods** (`s.split`, `s.trim`, `s.toInt()`, ...), and `xs.join(sep)` is a method on `[]string`. Extra helpers are in the `strings` package.
- **Float division by zero gives `inf`/`nan`;** only integer division by zero crashes.
- **A struct cannot contain itself by value;** use a `T?` field, a list or an enum.

## Null and errors

- **Narrowing works on variables and their fields** (`u.email`), through `&&`, `||`, `!`, after an `if` that always leaves, and after `assert` in tests. List elements and map lookups must be stored in a variable first.
- **Narrowing on assignment:** after `x = <non-null value>`, `x` is a `T` until it may become null again (`if (x == null) { x = "default" }` leaves `x` non-null).
- **Narrowing ends** when the path is assigned a value that may be null, when a part above it is assigned or has a `mut` method called on it (`u.reset()` for `u.email`), and before a loop that changes it. A `mut` method on the narrowed value itself (`xs.push(x)`) keeps it narrowed: the receiver is a plain `T` inside the method.
- **A `T?` compares only with `null`** (`int? n` then `n == 2` is an error): narrow it first. Inside lists, structs and enums, nullable parts compare as part of the container.
- **`error?` is not a type,** because `error` is already nullable (like `T??`).
- **Error guards:** a plain `T` returned beside an error can be read only once the error is known to be null (`if (err == null)`, after `if (err != null) { return ... }`, right of `err == null &&`), or after it is assigned again. A `T?` beside an error is not guarded.
- **Only a local variable can receive a plain `T` beside an error;** a field or element can't (`b.s, err = f()`), because it would hold null until the check. A `T?` field or element can.
- **Every result must be used,** not only errors: discard with `_ = f()`. A value beside a discarded error must be declared `T?`. A typed `_` (`string _, int _, error err = f()`) discards like `_`, any number of times, and `error _` discards the error like `_` does.
- **`return null, null` for a plain `T` result is an error;** `return null, err` with an `err` that turns out to be null crashes (`f returned null as result 1 without an error`).
- **`try` works anywhere in an expression** in a function whose last result is `error`, including `try group {...}`.
- **Crash vs error exit codes:** an error returned from `main` exits with 1 (`error: ...`), a crash with 2 (`crash: ...`).
- **`os.crash(message)`** crashes the goroutine like a runtime fault, so library code can report a caller's mistake.
- **No type switches on interfaces.** Errors are told apart by message or by returning richer results.

## Functions, methods and interfaces

- **Interface satisfaction is explicit** (`implements`) and needs the exact signature, including `mut`. Structs and enums can implement interfaces; methods cannot be declared on interface types.
- **A type that implements an interface converts to it automatically,** and to the nullable interface; a type that implements `error` converts to `error`.
- **Interface methods can be `mut`;** calling one needs a variable (or a part of one) and changes that interface value only.
- **A struct field of function type is called directly:** `op.apply(1, 2)`. A struct can't have a field and a method with the same name (a compile error), so `op.apply` always means one thing.
- **Unused parameters and unused package-level declarations are allowed;** unused locals, pattern bindings and imports are errors.

## Control flow

- **`switch` over an enum cannot have `default`;** every case must be listed, so adding a case points at every `switch` to update.
- **`switch` works on enums, `int`, `byte`, `string` and `bool` only.**
- **`switch` over `int`, `byte`, `string` or `bool` must end with `default`:** `default` is the last case (a `case` after it is an error), and one `case` can list several values.
- **Enum case patterns match the case exactly:** every field bound with its type (`_` to ignore one), no parentheses for a case without data, and only `_` bindings in a `case` with several labels.
- **Defers run last-first,** like Go, and also when the goroutine crashes or is stopped; `os.exit` and a deadlock skip them.
- **`defer` takes a call of a function, method, function value, interface method or package function, or `ch.close()`**, but not a `mut` method (its variable is gone when the call runs). A deferred error must be discarded: `defer _ = f.close()`.

## Concurrency

- **`group { ... }` is an expression of type `error`,** used only whole: `error err = group {...}`, `err = ...`, `return ...`, `_ = ...` or `try group {...}`.
- **A group block cannot be left early** (`return`, `try`, `break`/`continue` out of it), because the group must wait for its goroutines first.
- **`spawn` can appear anywhere inside a `group` block,** including loops. It takes the same calls as `defer` (no `mut` method), returning `error` or nothing; the receiver and arguments are deep-copied.
- **When a group fails, only its goroutines are stopped,** not the code of the block itself; the block should spawn and leave waiting to goroutines or to the code after the group.
- **Stop points** are channel operations, `select`, `time.sleep` and the blocking standard-library calls (stdin, reads of an `fs.File`, network, `os.run`, `os.output`, waiting for another goroutine's operation on the same handle), so a stopped server doesn't hang in `accept`. Other file operations (`fs.read`, `fs.write`, `f.write`, ...) don't block for long on regular files and are not stop points, nor is `ch.close()`.
- **Channels are the one reference type.** Copying a channel gives another handle to the same channel (reply channels inside messages).
- **Channel elements cannot be nullable** (`chan (int?)`), because `receive()` uses null for "closed"; `chan int?` is a nullable channel. A `chan error` receives `error`.
- **Select cases** are `ch.send(v)`, `ch.receive()`, `_ = ch.receive()` and `T? v = ch.receive()` (a new variable only; `error e = ch.receive()` for a `chan error`). On a `chan error`, `case ch.receive():` is an unhandled error: declare it or write `case _ = ch.receive():`. All channels and values are evaluated, in order, before select waits. `select` with `default` is an error with a hint to use a timeout channel.
- **`time.after(d)` sends the time once and closes the channel.**
- **Deadlock is a crash:** when every goroutine is blocked and no timer is pending, the program prints `crash: deadlock: all goroutines are blocked` and exits with 2, without running defers.

## Packages, visibility and tests

- **Imports are per file,** and resolved in the standard library first, then relative to the project root (the directory of `package main`, or the one given to `seed test`). There is no project file; importing a project package that has a standard library package's name is an error at the import (rename the package).
- **A package's name is its directory's name** (only `package main` differs), and `main` can't be imported.
- **Struct fields are private unless `pub`,** so no program can forge a file or socket handle; a struct literal of another package's type needs every field to be `pub`. Enum cases and interface methods are as visible as their type; methods need `pub`.
- **A local or package-level name can't take an imported package's name.**
- **The core packages (`fmt`, `os`, `fs`, `errors`, `time`) are ordinary standard library packages** and must be imported.
- **Tests:** `test "name" { ... }` blocks in the package's `tests/` directory; the body works like a function returning `error` (`try` works, `return` doesn't); `assert` only in tests (not in a group), showing both sides of a failed comparison as `.string()` gives them (a byte as its number).
- **`seed test` runs each test in a goroutine of its own group,** one after the other, prints `FAIL "<name>": <where>: <reason>` per failure and `N passed, M failed`, and exits with 1 on any failure.

## Standard library

- **Natives:** standard library functions can be declared without a body and written in C (`lib/<pkg>/<pkg>.c`, `sd_lib_<pkg>_<name>`); only the standard library may do this.
- **Error messages follow one form:** `<package>.<function>: <subject>: <reason>`, e.g. `fs.read: notes.txt: no such file or directory`, `string.toInt: "x": invalid integer`.
- **The package set** is `errors fmt io os fs path strings math rand time encoding json crypto net http`, as designed in docs/stdlib.md; all of it is implemented.
- **An HTTP request's time limit is a function, `http.sendWithin(req, timeout)` (and `http.getWithin(url, timeout)`), not a `timeout` field of `http.Request`.** A struct literal must set every field, so a new field would break every `http.Request{...}` already written (programs, tests, the docs) and would mean nothing on the requests a server receives; a second pair of functions beside `get`/`send` breaks nothing. The limit covers the whole exchange, from connecting to the last byte of the response, in nanoseconds like the rest of `time`, and must be positive (a crash otherwise, like `net.dial`'s negative timeout); the error is `http.sendWithin: <url>: timeout after 2 s`. `get` and `send` keep their 30 seconds without progress per step; giving up for other reasons than time is still done with a group. A field would be the better shape if struct fields ever get default values.
- **`io.read(max)`** reads between 1 and `max` bytes of stdin (null at the end), sharing the buffer with `readLine` and `readAll`, like `fs.File.read`: input that is not made of lines (the language server's messages: a header, then a body of a given length) needs it.

## Toolchain and compiler

- **The pedantic checks and the formatting check are always on;** there are no flags or warnings. Unformatted code is an error at the first differing line, with the hint `run: seed fmt <file>`.
- **Nesting limit:** blocks and expressions nest at most 256 levels deep; deeper code is a compile error (it would overflow the compiler's or the C compiler's stack).
- **Commands:** `seed build [-o out]`, `seed run` (a `--` before the program's arguments is skipped), `seed test`, `seed fmt [--check]`, `seed lsp`, plus `seed c` and `seed ast` for debugging. `seed` finds its root from its real path (symbolic links resolved) unless `$SEED_ROOT` is set.
- **`args[0]` is the program itself.**
- **The language server is part of the `seed` binary** (`seed lsp`, written in Seed with the compiler's own loader, checker and formatter), so an editor's diagnostics are exactly the compiler's: it checks each open document's package as `seed build` or `seed test` would, with the editor's unsaved text, reports the formatting check as a warning rather than an error, and answers definition, hover, completion, document symbols and formatting. Each request runs in a goroutine of its own, so a compiler crash on half-written code loses that answer, not the server.
- **The formatter keeps the source's line structure:** no maximum line length, `{}` for empty blocks, trailing comments not aligned, one-line `const`s may stay grouped (docs/formatting.md).
- **Seed compiles to C,** with the runtime (`runtime/`, written in C) pasted into every program, and uses the system C compiler.
- **Bootstrap:** the C bootstrap compiler `seed0` (64-bit ints, a subset of the language) compiles the compiler once; it has been retired: the compiler is now built from a C snapshot of itself (`bootstrap/seed.c`), refreshed with `make release`.
