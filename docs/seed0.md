# seed0: the bootstrap compiler

`seed0/seed0.c` is a compiler for a subset of Seed, written in C. Its only job is to compile the real Seed compiler (`compiler/*.sd`) once. After the real compiler compiles itself, a snapshot of its generated C is committed to `bootstrap/` and `seed0.c` is no longer needed.

Because it is scaffolding, seed0 is allowed to be simple: it trusts its input more than the real compiler does and skips most of the pedantic checks. It must still generate **correct** code for everything in the subset.

## Usage

```
seed0 file1.sd file2.sd ... > out.c
cc -std=c11 -O2 -w -o prog out.c
```

- All input files belong to one package (normally `package main`). Their declarations share one namespace, and order doesn't matter: a function may use a type or function declared later or in another file.
- The C output is self-contained: the runtime is emitted into it, and it needs only the C standard library and POSIX.
- On error, seed0 prints `file:line:col: error: message` to stderr and exits with code 1. Reporting the first error and stopping is fine.

## The subset

seed0 supports everything in [SPEC.md](../SPEC.md), [grammar.md](grammar.md) and [builtins.md](builtins.md) **except**:

| Left out | Notes |
|---|---|
| `interface`, `implements` | `error` is the only interface type. Values of type `error` come from `errors.new` or from functions that return `error`. |
| Function values and `fn(...)` types | So `sortBy` is also left out. |
| `chan`, `select`, `group`, `spawn` | |
| `defer` | |
| `test`, `assert` | |
| User packages | `import` may only name the core packages: `fmt`, `os`, `fs`, `errors`. |
| `s.chars()` | |

Everything else is in, including: structs, enums with data, methods and `mut` methods, multiple return values, `try`, `_`, nullable types with narrowing, `const`, lists, maps, `switch` (enum and value forms), `break`, `continue`, every operator, and every built-in method and core package function listed in [builtins.md](builtins.md) apart from the exceptions above.

`pub` is accepted and ignored.

## Semantics that must be right

These are the parts of the language that are easy to get subtly wrong. Generated programs must behave exactly as described.

### Value semantics

Every value behaves like an independent copy. After `[]int b = a` followed by `b.push(1)`, `a` is unchanged. The same holds when a list, map or struct is passed to a function, stored in a struct field, stored in a list or map, or returned.

Suggested implementation (copy-on-write with conservative counts):

- Lists and maps are heap objects with a reference count. Strings are immutable, so they can be shared without counting.
- Whenever a value is copied from a place that still holds it (a variable, field, element or parameter) into another place, increment the count of every list and map inside it. A struct copy must do this for each of its fields, recursively. A freshly created value (a literal, or the result of a call) doesn't need an increment.
- Before mutating a list or map (`push`, `pop`, `insert`, `remove`, `reverse`, `sort`, `xs[i] = v`, `m[k] = v`, `delete`), check its count. If it is greater than 1, copy it first (with count 1) and update the place that holds it to point at the copy.
- seed0 never frees memory, so counts never go down. This only causes extra copies, never wrong behaviour.

Any other approach is fine as long as the behaviour is identical.

### mut methods

A `mut` method (built-in or user-defined) changes the variable it was called on, including through field and index paths: `u.tags.push(t)`, `users[2].tag(x)`, `g.out.push(s)`. In C, pass a pointer to the place. Calling a `mut` method on a function parameter is a compile error.

### Integers

seed0 implements `int` as a 64-bit signed integer. Overflow in `+ - *` and in unary `-` crashes the program (use `__builtin_*_overflow`). The full compiler will switch to integers that grow. Division and `%` truncate toward zero (as in C); division by zero crashes.

### Nullable values

- `T?` holds either a `T` or `null`. A `T?` cannot be used as a `T` until it has been narrowed.
- Narrowing applies to a local variable or parameter, or a field path starting at one (`u.email`):
  - In `if (x != null) { A } else { B }`, `x` is a `T` inside `A`.
  - In `if (x == null) { A } else { B }`, `x` is a `T` inside `B`.
  - After `if (x == null) { ... }` whose block always ends in `return`, `break`, `continue` or `os.exit(...)`, `x` is a `T` for the rest of the enclosing block.
  - In `x != null && E`, `x` is a `T` inside `E`. In `x == null || E`, `x` is a `T` inside `E`.
  - Narrowing ends when the variable (or the root of the path) is assigned.
- A `T?` can only be compared with `null`.
- A `T` can be used where a `T?` is expected.

### Errors and multiple returns

- `error` can always be `null` (meaning "no error") and is compared with `null` without narrowing. Calling `message()` on a null error crashes.
- `T a, error err = f()` declares both. Reading `a` when `err` is not null is the program's mistake; seed0 doesn't have to detect it.
- `return null, err` is allowed in a function that returns `(..., error)`, even though the first slot is not nullable.
- `T x = try f()`: if `f`'s error is not null, return it from the current function (with every other result slot empty). Also works when `f` returns only `error` (`try fs.write(p, s)` as a statement) and with several values (`A a, B b = try f()`).
- `T? x, _ = f()`: if the error is not null, `x` is `null`. Declaring `x` as a plain `T` here is a compile error.
- `_ = f()` discards an error-only result.
- A call that returns an error and is used as a plain statement (not assigned, not `try`) is a compile error. This is the one pedantic check seed0 must do, because forgetting it hides real bugs in the compiler.

### Program entry

`fn main([]string args) error` is required. `args[0]` is the program path. Returning `null` exits with code 0. Returning an error prints `error: <message>` to stderr and exits with code 1. A crash prints `crash: <message>` to stderr and exits with code 2. Stdout must be flushed before exit in every case.

### Equality

`==` and `!=` work on every type in the subset. Lists, maps, structs and enums (including their data) compare by contents.

## Quality bar

- seed0 must compile 20,000 lines of Seed in well under a second, and the generated programs should be reasonably fast. The real compiler will be compiled by it and must not be painfully slow.
- Generated C must compile with no errors under `cc -std=c11 -O2`. Warnings don't matter.
- Prefix every generated C identifier (`sd_`, for example) so Seed names can never clash with C keywords or runtime names.
