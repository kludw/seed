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
