# seed0

The bootstrap compiler for Seed: it compiles the subset described in
[docs/seed0.md](../docs/seed0.md) to a single self-contained C file.

```
make build/seed0
build/seed0 a.sd b.sd > out.c
cc -std=c11 -O2 -o prog out.c
make test0        # tests/lang and tests/seed0
```

## Files

- `seed0.c` is the compiler: lexer, parser, checker and C generator, in that order.
- `runtime.c` is the runtime. The Makefile turns it into a C string
  (`build/runtime.inc`), and seed0 writes it at the top of every output file.
  It can also be compiled on its own, which is handy when changing it.

## Pipeline

1. **Lexer** (`lex`). Turns each file into tokens. It inserts a newline token
   using Go's rule (docs/grammar.md). Integer literals are kept as `uint64` so
   that `-9223372036854775808` can be folded by the parser.
2. **Parser** (`parse_file`). Recursive descent that builds the AST (`Expr`,
   `Stmt`, `FnDecl`, `TypeDecl`, `ConstDecl`). A statement that starts with
   `Type name` is a declaration. The parser checks this speculatively with
   `try_typed_name` and backtracks if it fails. Enum patterns in `case` labels
   (`Shape.Circle(float r)`) are recognised the same way. Top-level names go
   into one table shared by all input files, so declaration order doesn't
   matter.
3. **Checker** (`check_program`). Resolves the types of struct fields, enum
   payloads and function signatures. Then it checks constants and every
   function body. It annotates the AST: each `Expr` gets its type, what a
   call/field/index resolves to (`sub`, `bid`, `fn`, `var`, ...), and
   `narrowed` when a `T?` is read as `T`. Implicit `T -> T?` conversions
   become `E_WRAP` nodes.
   - Narrowing keeps a stack of narrowed *paths* (`v12.email` = variable 12,
     then field `email`). Conditions add paths (`!=`/`==` null, through `&&`,
     `||` and `!`). An `if` whose branch always exits narrows the rest of the
     block. Assigning to a path ends narrowing of that path and everything
     under it. Calling a mut method ends narrowing of everything under the
     receiver. Before a loop, the body is scanned, and any path the loop
     assigns or mutates is un-narrowed at the loop start.
   - "Always exits" (`return`, `break`, `continue`, `os.exit`, an if/else or
     switch where every branch exits, `for (;;)` without `break`) is also used
     for the "missing return" error.
4. **C generation** (`gen_program`). Expressions become C expressions. GNU
   statement expressions `({ ... })` are used for temporaries, `try`, list,
   map and struct literals, and to force left-to-right evaluation when more
   than one operand has side effects. Clang and GCC accept them in `-std=c11`.
   Per-type helpers (equality, retain, release) are generated on demand
   through a work queue. The output is laid out as: runtime, type
   definitions, prototypes, helpers, constants, functions, then `main`.

Every generated name has a prefix, so Seed names can't clash with C or the
runtime: locals `l_x`, fields `f_x`, functions `sd_f_name`, methods
`sd_m_Type__name`, constants `sd_c_name()`, types `sd_S_`/`sd_E_`/`sd_B_`/`sd_T_`,
and temporaries `_tN`.

## C representation

| Seed | C |
|---|---|
| `int` | `int64_t`. `+ - *`, unary `-`, `/` and `%` are checked and crash on overflow or division by zero. |
| `float`, `bool`, `byte` | `double`, `bool`, `uint8_t` |
| `string` | `sd_str {const char *p; int64_t len}` by value. It is immutable, so it is shared without counting. `slice` doesn't copy. |
| `error` | `sd_err*` (a message). `NULL` is "no error". |
| `[]T` | `sd_list*` `{rc, len, cap, data}`. Elements are stored unboxed; the code casts `data` to `T*`. |
| `map[K]V` | `sd_map*`: an insertion-ordered hash map (entries array + open-addressing index), with keys stored as `int64` or `sd_str`. Also has `rc`. |
| struct | A C struct passed by value (`sd_S_Name`). |
| enum | Pointer to an immutable heap object `{tag, union of payload structs}`. Cases without data are static singletons. |
| `int?` `float?` `bool?` `byte?` | `{bool has; T v}` (`sd_O_i`, ...). |
| `string?` | `sd_str` with `p == NULL` meaning null. |
| `[]T?` `map?` `Enum?` | The same pointer, `NULL` meaning null. |
| `Struct?` | Pointer to a ref-counted box `{rc; Struct v}` (`sd_B_Name*`). This allows recursive types such as `Node? next`. |
| several results | A generated struct `sd_T_...` with fields `r0, r1, ...`. |
| a mut method's receiver | `T*` pointing at the caller's variable, field or element. |

## Value semantics (copy-on-write)

This is the scheme suggested in docs/seed0.md:

- Lists, maps and struct boxes carry a reference count. When a value is
  copied from a place that still holds it (a variable, field, element or
  constant) into another place, the counts inside it are incremented
  (`gowned`, the `sd_rt_*` helpers). Fresh values (call results, literals)
  are not.
- Mutation goes through a pointer to the place (`gplace`). Every step of the
  path makes its container unique first (`sd_list_at_mut`, `sd_list_push`,
  `sd_map_slot`, `sd_bm_*` for boxes). If the count is above 1, the container
  is copied and the old count decremented.
- Assigning over a place decrements the old value's top-level counts
  (`sd_rl_*`). This is only an optimisation. Nothing is ever freed.
- Arguments to normal functions are passed without counting, because
  parameters are read-only. Arguments to user **mut** methods *are* counted,
  and they are evaluated before the receiver's place is computed. Without
  that, `bag.addAll(bag)` or `bs[0].absorb(bs)` would see their own
  modifications.
- `return x` for a local `x` moves it out without counting, because the
  frame is gone. So building a list in a function and returning it doesn't
  cause a copy later.

With this scheme, `push` on an unshared list is amortised O(1),
`g.out.push(s)` through a mut method doesn't copy, and `join` is linear.
`tests/seed0/perf_linear.sd` guards these: it runs in about 0.1s, and a
quadratic implementation would take minutes.

## Decisions not covered by the docs

- **float.string()** follows Python's `repr`: the shortest round-trip digits,
  in fixed notation when `1e-4 <= |x| < 1e16`, otherwise `1e+300` style.
- **Equality.** Maps compare equal when they have the same keys and values,
  in any order. Two errors are equal when both are null, or both have the
  same message.
- **Runtime error texts.** `toInt` gives `invalid integer: "x"` and `toFloat`
  gives `invalid float: "x"`. fs errors are `path: strerror`. `os.run`
  returns `128 + signal` for a child killed by a signal. Crashes are
  `crash: index out of range: 5 with length 3`, `crash: integer overflow`,
  `crash: division by zero`, and so on. A stack overflow (or any other memory
  fault) is reported as `crash: stack overflow or invalid memory access` with
  exit code 2.
- **Shifts.** A negative shift count crashes. An `int <<` that loses bits
  crashes as overflow. Byte shifts by 8 or more give 0. Following the
  operator table, `byte` supports bitwise operators and comparisons but not
  arithmetic.
- **`++` and `--`** work on `int` and `float` places.
- **Read-only.** Assigning to a parameter, or to anything inside a non-mut
  receiver, is an error, the same as calling a mut method on it.
- **Map values can't be nullable** (`map[string](int?)`), because `m[k]`
  would have the invalid type `int??`. `pop()` on a list of nullable
  elements is rejected for the same reason.
- **Discarded results.** Only calls that return an error must be handled.
  Other results may be dropped (`stack.pop()` as a statement is fine).
- **Pedantic checks.** seed0 enforces exhaustive enum switches, `default` in
  value switches, shadowing between locals, missing returns, comparison
  chaining, and `import` before using a core package. It does not check
  unused variables or imports, unreachable code, naming, or formatting.
- **Locals.** A local may have the same name as a package-level declaration;
  the local wins.
- **Constants** that aren't literals are computed on first use and cached, so
  list and map constants are built once.
- **Leniency.** Inside list, map and struct literals and argument lists,
  newlines are allowed before the closing bracket. The trailing comma is
  optional.
- **Evaluation order.** Arguments, operands and struct literal fields are
  evaluated left to right.
- **Narrowing extras.** If the *else* branch always exits, the condition's
  narrowing also applies after the `if`. The body of
  `for (; x != null; )` sees `x` as non-null.

## Not supported / known limits

- Everything that docs/seed0.md leaves out: interfaces, function values and
  `sortBy`, channels, `select`/`group`/`spawn`, `defer`, tests and `assert`,
  user packages, `s.chars()`.
- `int` is 64-bit, and memory is never freed (by design).
- Lists and maps have no `.string()`. builtins.md only defines it for
  `int`/`float`/`bool`/`byte`; seed0 also accepts `string.string()`.
