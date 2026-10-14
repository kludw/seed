# The Seed compiler

The real Seed compiler, written in Seed. It is compiled by the bootstrap
compiler `seed0` first, and then compiles itself:

```
seed0 compiler/*.sd   -> build/stage1.c -> build/seed1
seed1 c compiler/*.sd -> build/stage2.c -> build/seed2
seed2 c compiler/*.sd -> build/stage3.c      (identical to stage2.c)
```

```
make verify     # builds all stages and checks stage2.c == stage3.c
make test       # tests/lang and tests/seed0 with build/seed2
make test-full  # tests/full (programs and packages) and tests/test_cmd.sh (seed test, build, run)
make test-lib   # the standard library's tests: build/seed2 test lib
make test-leaks # every test program under macOS leaks --atExit
make test-fmt   # seed fmt: self-checks on every .sd file, tests/fmt goldens
make test0      # tests/lang, tests/seed0 and tests/seed0/only with seed0
```

`tests/seed0/only` holds what only seed0 does (64-bit ints that crash on
overflow, rejecting interfaces); the real compiler's ints grow
(`tests/full/int_*`).

Because stage 1 is built by seed0, the compiler source may only use the
seed0 subset (docs/seed0.md) until a snapshot replaces seed0 (see "Adding a
language feature").

## Command line

```
seed build [-o out] <dir | file.sd>   compile a program (default output: the directory's or file's name)
seed run <dir | file.sd> [args...]    compile into $TMPDIR and run; passes the exit code
seed test [dir...]                    run the tests of every package under the dirs (default .)
seed fmt [--check] <file.sd>...       format in place, or report unformatted files
seed c <dir | file.sd...>             C program on stdout (what tests/run.sh uses)
seed ast <file.sd>...                 print the syntax tree (debugging aid)
```

A program is the `package main` of a directory (all its `.sd` files) or a
single `.sd` file; `seed c` also takes several files of one directory (the
bootstrap passes `compiler/*.sd`). The packages it imports are found as
described in "Packages" below. Compile errors go to stderr and exit with
code 1; usage errors exit with 2. The pedantic checks and the formatting
check are always on (there is no `--strict` any more).

The generated C is self-contained: it starts with the runtime of `runtime/`
(`seed.h`, `seed_internal.h` and the seven `seed_*.c` files pasted in that
order, without their `#include "seed..."` lines; `readRuntime` in
`gen.sd`), followed by the program, into which the C files of the standard
library packages it uses are pasted (`readNatives`). The compiler finds its
root (the directory with `runtime/` and `lib/`) through `$SEED_ROOT`, or
else from its own path: `os.executable()` gives the binary's real path
(symbolic links resolved, so `seed` works through `$PATH` or a link), and
the binary is `<root>/build/seedN`. The C compiler and its flags for
`build`/`run`/`test` are the constants `cCompiler`, `cFlags` and `cLibs` at
the top of `main.sd` (the Makefile has the same flags in `SEED_CFLAGS` and
`SEED_LIBS`).

### seed test

`seed test [dir...]` walks each directory (a trailing `/...` is accepted
and means the same) and finds every package directory that has a `tests/`
directory with `.sd` files. For each it compiles a test program: the
package, its `tests/*.sd` files (which belong to the package and see its
private names) and its imports, with the directory given on the command
line as the project root. The checker runs in *test mode* (test blocks are
checked, `main` is not needed) and the generator emits a table of the tests
and a `main` that calls `sd_test_main` (runtime/README.md, "Native
functions", for the runner). The program is built in
`$TMPDIR/seed-test-<path>` and run with the name of a results file; it
prints one line per failed test and writes its counts there, and `seed
test` prints the total, `N passed, M failed`, and exits with 1 if anything
failed. A package that does not compile counts as one failure.

A test block is parsed into a `TestDecl` and also into a `FnDecl` with
`isTest` set (no parameters, result `error`, the description as its name),
so it is checked and generated like a function: `try` returns the error,
and the generator ends the body with `return SD_ERR_NULL`. `return` and
(outside tests) `assert` are errors. `assert(cond)` (`genAssert`) evaluates
the operands of a comparison once, and when the condition is false builds
`<file>:<line>: assert failed: <source> (<a> <op> <b>)` (the source text
was recorded by the parser, `Stmt.name`; values only for int, float, bool,
byte, string, error and their `T?`) and returns `sd_assert_error(msg)` like
`try` returns an error. After an `assert`, the checker narrows on its
condition (as after `if (!cond) { return }`).

## Packages

`load.sd` (the `Loader`) parses the root package and everything it
imports into one `Ast`. Each `SourceFile` has the index of its package in
`Ast.pkgs` (`Package {path, name, dir, cprefix, isLib}`), and each
`Import` the index of the package it names (`target`).

- `import "x/y"` is resolved, in order, to `$SEED_ROOT/lib/x/y` (the
  standard library; such packages have `isLib`) and to `<project
  root>/x/y`, where the project root is the root package's directory (or
  the directory given to `seed test`). The code calls it by its last
  element, `y`. Import paths are lowercase names separated by `/`.
- Every `.sd` file directly in a package's directory belongs to it; all
  must have the same `package` line, which must be the last element of the
  directory's path (only `package main` may differ). Package `main` cannot
  be imported.
- Every file must be formatted: after parsing, `formatSource` must give
  the same text, otherwise the error points at the first differing line
  with the hint `run: seed fmt <file>`.
- Cycles are found by a depth-first search over the import graph and
  reported at the import that closes them: `import cycle: a -> b -> a`.

The checker keys package-level names by package (`globals["<pkg>:<name>"]`,
`Global.pkg`) and finds the package of any node from its position
(`pkgOf(pos)`, through `SourceFile.pkgIndex`). Imports are per file
(`imported["<file>:<name>"]`). A qualified name is checked where the
unqualified one is: `pkg.f(...)` in `checkSelectorCall`, `pkg.name` in
`checkField`, `pkg.Type` in `resolveQualified`, `pkg.Type{...}` in
`checkStructLit`, `pkg.Type.Case` through `typeNamed`, and
`case pkg.Type.Case(...)` in `labelNamesDecl`; all of them go through
`pkgMember`, which reports names that do not exist or are not `pub`.
Fields (`fieldVisible`) and methods are checked for `pub` when used outside
their package, and a struct literal of another package's type needs every
field to be `pub`. Methods are keyed by declaration (`methodKey`).

Types of other packages are named `cart.Item` in messages and `cart_Item`
in C (`declDisplayName`, `declCName`; the mangled type name uses the C
name). Every C name of a declaration has its package's `cprefix`:
`sd_f_cart_newCart`, `sd_m_cart_Cart__add`, `sd_S_cart_Item`,
`sd_c_cart_maxItems`; package `main` has no prefix, so its names are as
they always were.

**Native functions.** A function without a body (`FnDecl.body == -1`) is
only allowed in a package with `isLib`. The generator (`genNative`) emits a
prototype of its C implementation `sd_lib_<path>_<name>` (several results:
the last returned, the others through out-pointers) and a wrapper with the
normal calling convention under the usual name, so calls, function values,
`defer` and `spawn` need nothing special. The ABI is in runtime/README.md,
"Native functions". The checker knows only that `os.exit` and `os.crash`
never return (`isNoReturn`).

## Files

| File | Phase | Contents |
|---|---|---|
| `main.sd` | driver | CLI, runtime location, building and running programs, `seed test` |
| `load.sd` | driver | loading packages: import resolution, package names, formatting check, cycles |
| `errors.sd` | all | `Pos`, `Diags` (collect, sort and print errors), "did you mean" |
| `lexer.sd` | lex | `Tok`, `Token`, `Comment`, newline insertion |
| `ast.sd` | parse | the AST node structs and the `Ast` arenas |
| `parser.sd` | parse | parser state, statements and expressions, error recovery |
| `parse_decl.sd` | parse | files, top-level declarations, types |
| `dump.sd` | debug | `seed ast` S-expression printer |
| `fmt.sd` | fmt | `seed fmt` command, safety check, `--verify` |
| `fmt_layout.sd` | fmt | `Formatter` state, bracket matching and roles |
| `fmt_print.sd` | fmt | line breaks, indentation, spaces, comments |
| `types.sd` | check | `Kind`, `Type`, the interned `Types` arena |
| `check.sd` | check | `Checker`, program passes, packages and visibility, type resolution, scopes, narrowing |
| `check_expr.sd` | check | expressions, places (lvalues), T/T? conversions |
| `check_call.sd` | check | calls: functions (also of other packages), methods, function values, interface methods, built-ins, conversions |
| `check_iface.sd` | check | interfaces, `implements`, which types have `==` |
| `check_stmt.sd` | check | statements, switch and enum patterns, assignment, defer, "always exits" analysis, error guards, select, group, spawn, assert |
| `check_style.sd` | check | pedantic checks: unused names/imports/results, unreachable code, naming |
| `gen.sd` | gen | `Gen`, program layout, C types, retain/release, descriptors, type definitions, vtables, constants, natives, the test table, the runtime |
| `gen_expr.sd` | gen | temporaries and slots, evaluation order, expressions, operators, `try`, `group` |
| `gen_call.sd` | gen | calls, places, built-in methods, conversions |
| `gen_stmt.sd` | gen | statements, scopes, return, `defer`, `spawn`, `select`, `assert` |

## Data structures

### Positions and diagnostics

`Pos {file, line, col}`: `file` indexes `Diags.paths`/`Diags.sources` (and
`Ast.files`); `line` and `col` start at 1 and `col` counts bytes, as in seed0.

`Diags` collects `Diag {pos, message, hint}`. It keeps at most 20 errors,
drops a second error at a position that already has one (almost always a
cascade), and prints them sorted by position:

```
x.sd:17:19: error: struct User has no field nme
   17 |     fmt.println(u.nme)
      |                   ^
    hint: did you mean name?
```

`Suggest` finds the closest name (case-insensitive edit distance, at most
len/3+1 and at most 3 edits) for "did you mean" hints.

### Tokens and comments

The lexer produces `[]Token {kind, pos, text, raw}`: `text` is the identifier,
the decoded bytes of a string or byte literal, or the number as written;
`raw` is the exact source text. Newline tokens are inserted with Go's rule.
Comments are not tokens: they are collected separately as
`Comment {pos, text, ownLine}` and stored in `Ast.comments`, for a future
`seed fmt`.

### The AST: arenas

Seed has no pointers, so the tree is a set of arenas inside `Ast`:
`exprs`, `stmts`, `blocks`, `targets`, `cases`, `labels`, `typeExprs`,
`params`, `fns`, `typeDecls`, `enumCases`, `methodSigs`, `consts`, `tests`,
`imports`, `pkgs`, `tops`, `files`, `comments`. A node refers to another by its `int`
index (-1 for none). A variable-length list of children (call arguments,
block statements, case labels, ...) is a run of indices in the shared
`Ast.lists`, stored in the parent as `(start, count)`, and read with
`ast.at(start, i)`.

Nodes hold only ints, strings, bools and enums, so reading one
(`Expr x = ast.exprs[e]`) is a cheap copy with no reference counting. Each
node struct has a `kind` enum and a few generic fields whose meaning per kind
is documented above the struct in `ast.sd` (for example `Expr.a`/`Expr.b`
are the operands of a binary expression, the callee of a call, the base of a
field access).

Some notable shapes:

- A declaration and an assignment are both `StmtKind.Assign` with a list of
  `Target`s: `Decl` (`Type name`), `Place` (an lvalue expression) or
  `Discard` (`_`).
- `else if` is kept as `Stmt.elseIf` (a statement), `else { }` as `Stmt.els`
  (a block), so a formatter can reproduce the source.
- Map literal entries are `Entry` expressions, struct literal fields are
  `FieldInit` expressions, so every child list is a list of expressions.
- A case label that looks like `Type.Case` is marked `isPattern` and is *also*
  parsed as an expression, so the same label works in a value switch; one
  with parentheses carries its bindings as `Param`s.
- Top-level declarations are recorded in source order in `Ast.tops` (with
  per-file ranges in `SourceFile`), besides the per-kind arenas.

The checker never modifies the AST.

### Types

`Types` is an interned arena: a type is an `int`, and two types are equal iff
their ids are equal. Every constructor (`listOf`, `mapOf`, `optOf`,
`tupleOf`, `funcOf`, `chanOf`, `declared`) looks the type up by its mangled
name (`Li`, `MsLi`, `Oi`, `T2ie`, `S4User`, ...) in `byMangle` first. The
basic types have fixed ids (`tyVoid` ... `tyInvalid`, in `types.sd`).
Tuple parts and function parameters live in the shared `Types.parts` list.

`tyInvalid` is the type of anything that failed to check. Every check that
sees it stays quiet, so one mistake produces one error.

`Type.rt` and `Type.helpers` are scratch fields for the C generator.

### Check results

The checker stores its results in lists parallel to the arenas, so that later
analyses can add their own lists without touching the tree:

| List | Parallel to | Meaning |
|---|---|---|
| `info []ExprInfo` | `exprs` | type, resolution, wrapping and narrowing |
| `teType []int` | `typeExprs` | resolved type |
| `declType []int` | `typeDecls` | the struct/enum type |
| `fnRet`, `fnRecv`, `fnCName`, `recvVar` | `fns` | result type, receiver decl, C name, receiver variable |
| `constType`, `constState` | `consts` | type, checking state |
| `targetVar`, `targetType` | `targets` | declared variable, target type |
| `stmtVar`, `stmtDiscard` | `stmts` | local const variable; `T? x, _ = f()` |
| `labelCase` | `labels` | enum case index |
| `paramVar` | `params` | variable of a parameter or pattern binding |
| `methodSigType` | `methodSigs` | the `fn(...)` type of an interface method |

`impls` lists the `(type declaration, interface)` pairs whose `implements`
was checked; the generator emits one vtable per pair.

`ExprInfo` holds:

- `ty`: the type the parent sees.
- `res` (an enum) plus `ref`/`sub`/`builtin`: what the expression
  resolved to — a local (`ref` = variable), constant, function (a call, or
  a function used as a value), method, call of a function value
  (`ValueCall`), interface method (`ref` = interface decl, `sub` = method),
  built-in method (`builtin`), conversion, enum constructor or value
  (`ref` = decl, `sub` = case), struct field (`sub` = field),
  list/string/map index. A name of another package (`fmt.println`,
  `time.second`) resolves like a name of the current one.
- `ifaceFrom`: the expression produces a value of this struct or enum type,
  which is converted to the interface (`ty`, or `inner` when `wrap` is set
  too).
- `wrap`/`inner`: the expression produces `inner` (a `T`) and is converted
  to `ty` (a `T?`). seed0 inserted a wrap node instead; with arenas a flag is
  simpler.
- `narrowed`/`orig`: the expression is stored as `orig` (a `T?`) and read as
  `ty` (a `T`).

Variables are an arena `vars []Var {name, ty, kind, pos, used}`. The scope is
a stack of variable indices with marks for nested blocks.

Package-level names are in `globals` (`"<package>:<name>"` → index into
`globalList`, which records kind, declaration index and package). Methods
are in `methods`, keyed by `"<type declaration>.name"` (`methodKey`).

## How each phase works

**Lexing** (`lex`) is a single pass over the bytes. After an identifier,
literal, `return`, `break`, `continue`, `)`, `]`, `}`, `?`, `++` or `--`, a
newline becomes a `Newline` token. Errors (bad escapes, unterminated
literals, stray characters) are collected and the lexer carries on.

**Parsing** is recursive descent, one file at a time into one shared `Ast`.
It covers all of docs/grammar.md, including interfaces, `implements`,
function types, `chan`, `select`, `group`, `spawn`, `defer`, `test`,
`assert`, qualified names (`pkg.Type`, `pkg.Type{...}`, `pkg.Type.Case(...)`)
and imports of any path.

- A statement is a declaration when a type followed by a name starts it.
  `scanType` decides this without building nodes (no backtracking).
- In a `Type name` position, `fn(int) f` has no result type while
  `fn(int) bool f` has one: after the `)`, an identifier is a result type
  only if it is followed by another identifier, `.` or `?`
  (`fnHasResult`). Elsewhere an identifier after `)` is always the result.
- `Ident.Ident{` is a qualified struct literal. As in seed0, `Name{` is
  always a struct literal (conditions are always parenthesised).
- `-` directly before an int literal is folded into it, so that
  `-9223372036854775808` is valid; the literal keeps its raw text, and the
  checker reports literals that do not fit in 64 bits (the full language
  will have integers that grow, so this is not a syntax error).
- Comparisons do not chain; `x += 1` gets a specific message.

Error recovery: the first error in a statement sets `failed`, and parse
functions then return quickly without reporting more. The block loop skips
to the end of the statement (`syncStatement`) and continues; the top level
skips to the next line that starts a declaration (`syncTop`), always moving
past the offending token. When a file has syntax errors, checking is skipped.

**Checking** (`checkProgram`):

1. Collect package-level names (per package); check the names' style;
   check imports (per file: a package must be imported in the file that
   uses it, and its name must not clash with a package-level name).
2. Create a type for every struct, enum and interface, then resolve fields,
   enum case fields and interface method signatures (`methodSigType`), and
   report structs that contain themselves by value.
3. Resolve every function signature and register methods (not on
   interfaces).
4. Check every `implements` declaration (`checkImplements`): the named type
   must be an interface (or `error`), and the type must have each of its
   methods with the same parameter and result types and the same `mut`.
   Each checked pair goes into `impls`.
5. Check `main`'s signature (not in test mode), then constants (lazily,
   in dependency order), then every function body (natives have none; test
   bodies only in test files), then that every import is used.

Expression checking mirrors seed0: there are no implicit conversions except
`T → T?` (`coerce` sets `wrap`), `null → T?/error`, reading a narrowed `T?`
as `T`, and a struct or enum that declares `implements I` → `I` (and `I?`,
and `error`; `coerce` sets `ifaceFrom`). Narrowing keeps a stack of
narrowed *paths* (`"v12.email"` = variable 12, field `email`), added by
conditions (`!=`/`==` null through `&&`, `||`, `!`) and by an `if` whose
branch always exits, killed by assignment to the path or a prefix, and by a
mut method call on a prefix. Before a loop, every path the loop assigns or
mutates is un-narrowed. Whether a statement always exits is a pure function
over the checked tree (`stmtExits`), used for "missing return", unreachable
code and narrowing after early exits.

The same conditions also record paths known to be *null* (keys starting
with `!`). They drive the **error guards**: after `int n, error err = f()`
(or `n, err = f()`), `n` carries a `Guard` until `err` is known to be null
(`if (err != null) { return ... }`, inside `if (err == null)`, after
`err == null &&`, after `assert(err == null)` in a test), `n` is assigned
again, or the scope ends. Reading a guarded variable is an error. A target
declared `T?` gets no guard (and is null whenever the error is not); a
field, element or mut receiver cannot be a plain `T` target beside an
error. `Checker.guards` is the set of guards in effect on the current
path: `checkIf`, `checkSwitch` and `checkSelect` check each branch from a
copy and join the ends that fall through; `break` and `continue` carry
the guards of variables declared outside the loop (`breakGuards`,
`contGuards`) to the loop's exit and post statement; a guard that comes
around the loop is an error when the next iteration reads the variable
before assigning it (`checkLoopGuards`). Assigning the error variable
again marks the guards waiting for it `lost`.

Function values: a function name used as a value has `Res.Func` and type
`fn(params) result`; a call whose callee is not a function or method name
(a variable, field, element or call result of function type) is a
`ValueCall`. Function types and types that contain them have no `==`
(`comparable`). `xs.sortBy(f)` takes `fn(T, T) bool`.

`defer f(args)` must be a call of a function, method, function value,
interface method or package function, or `ch.close()`, not a mut method
(the deferred call runs after the function's variables are released); an
error result must be discarded with `defer _ = f(args)` (`Stmt.discard`).

Concurrency (SPEC.md, "Concurrency"):

- `chan T` is `Kind.Chan` (`chanOf`); nullable elements are rejected in
  `resolveUncached`. `ch.send(v)`, `ch.receive()` and `ch.close()` are
  built-in methods (`Builtin.Send/Receive/Close`), not mut, so they work on
  parameters. `receive` returns `T?` (or `error` for `chan error`).
- `group {...}` (`checkGroup`) has type `error`. It is only accepted where
  the caller set `Checker.allowGroup` to that expression: the value of a
  one-target assignment or declaration, of a one-value `return`, or the
  operand of `try`; a bare group statement gets the unhandled-error message.
  Inside the block `groupDepth > 0`: `return` and `try` are errors, and
  `break`/`continue` are errors when `loopDepth == groupLoop` (the loop
  depth at the innermost group), i.e. when they would leave the block.
- `spawn` (`checkSpawn`) needs `groupDepth > 0` and a call of a function,
  non-mut method, function value, non-mut interface method or package
  function whose result is `error` or void.
- `select` (`checkSelect`): each case's comm statement must be an
  expression statement `ch.send(v)`/`ch.receive()`, or a one-target
  assignment of `ch.receive()` to a new variable of exactly the receive
  type, or to `_`. Each case has its own scope. A select whose cases all
  exit, exits (`stmtExits`).
- Package constants cannot hold channels (`hasChan`), because constants are
  cached per goroutine.
- `time` is a standard library package (`lib/time`): `after` and `sleep`
  are natives, the durations are constants.

The pedantic checks of the full language that seed0 lacks are always on
(check_style.sd and `declare`): unused variables, imports and call results,
unreachable code, locals that shadow package-level names or imports, the
naming rules (camelCase/PascalCase, with a suggested spelling), the error
guards (a plain `T` declared beside an error; a `T?` is never guarded) and
formatting. The seed0-era test programs were fixed to pass them.

Enum case patterns must match the case exactly (`checkPattern`): a case
with data binds every field with its type, `_` ignoring one; a case without
data has no parentheses. A case with several labels may only bind `_`.

**C generation** targets the runtime in `runtime/` (runtime/README.md is
the reference for every representation and ownership rule):

| Seed | C |
|---|---|
| `int`, `int?` | `sd_int`, a tagged word that grows into a big integer (`sd_int_add`, ...); null is `SD_INT_NULL` |
| `float`, `bool`, `byte` | `double`, `bool`, `uint8_t` |
| `float?`, `bool?`, `byte?` | `sd_opt_float` ... (`{has, v}`) |
| `string`, `[]T`, `map[K]V` (and `T?` of them) | `sd_str`, `sd_list *`, `sd_map *` (counted, copy-on-write); null is `NULL` |
| struct `S` | `sd_S_S` by value, with a descriptor `sd_D_S` |
| `S?` | `sd_box *` holding an `sd_S_S` (copy-on-write) |
| enum `E`, `E?` | `sd_box *` holding `sd_E_E {int32_t tag; union {...} u}`; cases without data are static immortal boxes `sd_k_E__Case` |
| interface `I`, `I?`, `error` | `sd_iface` / `sd_err` `{vt, obj}`: a vtable `sd_V_I` and a box with the value |
| `fn(A) R` | a C function pointer `sd_fn_<mangle>` |
| `chan T`, `(chan T)?` | `sd_chan *` (atomic count, shared between goroutines); null is `NULL` |
| several results | `sd_T_<mangle>` with fields `r0, r1, ...` |
| mut receiver | pointer to the caller's place |

Names are prefixed (`l_x`, `f_x`, `sd_f_name`, `sd_m_Type__name`,
`sd_c_name()`, `_tN`, `sd_dN` for defer thunks, `sd_vt_Type__Iface` for
vtables, `sd_test_N` for tests); the names of a package other than `main`
also carry its prefix (`sd_f_cart_newCart`, `sd_S_cart_Item`).

*Memory management.* Ownership follows runtime/README.md section 3:
parameters are borrowed, results are owned, stores consume. The generator
tracks the counted values the current function owns in a stack of *slots*
(`Gen.slots`): local variables and the temporaries of the statement being
generated.

- `gexpr(e)` returns a C expression for a *borrowed* value, valid until the
  end of the statement. Every owned intermediate result (a call result, an
  arithmetic result, a new list, a box) goes into a temporary `_tN` that is
  registered as a slot. Calls are always evaluated into temporaries, so
  side effects happen in source order.
- `ownVal(v)` turns a value into an owned one for a store: a live
  temporary is *moved* (its slot stops being live, nothing is released),
  anything else is retained (`sd_str_retain`, `sd_rt_<struct>`, ...).
  Literals are immortal and need neither.
- Each statement takes a mark on the slot stack and releases the slots above
  it at its end (`releaseTo`). A block's locals are released at its end
  unless the block always exits. `return` computes its result, releases
  every slot (except a returned local, which is moved), runs the defers and
  returns; `break`/`continue` release the slots of the loop body; `try`
  releases everything on its error path.
- `operands` evaluates the operands of an operation left to right. A
  borrowed operand is read when the operation runs, so when a later
  operand has side effects (a call, a mut built-in, `try`), the earlier one
  is first copied into a temporary.
- `a && b`, `a || b`: `b`'s temporaries live in an `if` block of their own.
- `gplace` builds a pointer to a place, making every container on the way
  unique (`sd_list_at_mut`, `sd_box_mut`); `gstore` evaluates the value,
  then the place, releases the old value and stores. A mut call evaluates
  (and keeps) its arguments before the receiver's place (`bag.addAll(bag)`).
- `x = x + a + b` on a local string appends in place (`sd_str_append`);
  other string `+` chains are one `sd_str_concat_n`.
- Constants are functions returning a borrowed value cached in a
  `_Thread_local` static `sd_cv_<name>` (literals are returned directly).
  A counted value registers `sd_cf_<name>` with `sd_goroutine_atexit`, so a
  goroutine releases the constants it computed when it ends.
- `defer f(args)` evaluates the callee, receiver and arguments into an
  environment (owned), pushed with `sd_defer_push`; the thunk `sd_dN` calls
  `f` and releases the environment and the result. A function with defers
  takes `sd_defer_mark()` on entry and calls `sd_defer_run` before every
  return; on a crash the runtime runs them.
- `switch` becomes an `if`/`else` chain (so `break`/`continue` in a case
  reach the loop); `for` becomes `for (;;) { if (!cond) break; { body }
  _cN:; post }` so that the condition and post statement can have
  temporaries and `continue` reaches the post statement.

*Concurrency* (runtime/README.md sections 10 and 11):

- `chan T(n)` is `sd_chan_new(<descriptor of T>, n)`; `ch.send(v)` is
  `sd_chan_send(ch, &v)` (borrowed; the receiver gets a deep copy);
  `T? x = ch.receive()` is `T o; bool ok = sd_chan_receive(ch, &o);` and
  then `ok ? o : null` (wrapped into a box for a struct, `{true, o}` for
  `float?`/`bool?`/`byte?`; `okOpt`, shared with `pop`); `ch.close()` is
  `sd_chan_close(ch)`. They count as side effects for evaluation order.
- `group {...}` (`ggroup`) is
  `sd_err _tR = SD_ERR_NULL; { sd_group *_tG = sd_group_begin(); <block>;
  _tR = sd_group_wait(_tG); }`, and `_tR` is an owned temporary of the
  statement, used like any call's error (moved into `err`, released by
  `_ =`, tested by `try`). `Gen.groups` is the stack of the C names of the
  enclosing groups; `spawn` uses the innermost. Nothing jumps out of the
  block (the checker forbids it); a crash unwinds through the runtime,
  which stops and waits for the group.
- `spawn f(args)` (`genSpawn`) evaluates the same operands as `defer`
  (`callOperands`: function value or receiver, then arguments) into a
  struct `sd_gN_args` with a field descriptor `sd_gN_type`, and calls
  `sd_spawn(group, sd_gN, &sd_gN_type, &args)`; the values are borrowed,
  because `sd_spawn` deep-copies them for the goroutine. The trampoline
  `static sd_err sd_gN(void *p)` calls `f` with the goroutine's copy and
  returns its error (or null for a void function).
- `select` (`genSelect`) evaluates every case's channel and value to send,
  in order (`operands`), declares an uninitialised receive buffer per
  receive case, and builds `sd_select_case cs[] = {{ch, SD_RECV, &buf,
  false}, {ch2, SD_SEND, (void *)&v, false}}`. The chosen index from
  `sd_select` is dispatched with an `if`/`else if` chain (a C `switch`
  would catch `break`). A receive case declares its variable from
  `cs[i].ok` and the buffer, or releases the buffer when it has none.
- `time.after(d)` calls the native `sd_lib_time_after`, which is
  `sd_time_after(d)` (a `chan int`); `time.sleep(d)` is `sd_time_sleep(d)`;
  `time.second` is the constant `sd_c_time_second()`.
- `defer ch.close()` uses the defer machinery with the channel as the one
  operand.

Per-type helpers (`sd_rt_`/`sd_rl_` retain and release for structs and
tuples, `sd_eq_` for struct `==`, `sd_less_` sortBy adapters) are requested
on demand through a queue (`Gen.helper`) and generated at the end. Each
`(type, interface)` pair gets a vtable whose thunks call the type's
methods on the payload of the interface's box.

The output is: the runtime, type definitions (forward typedefs, function
pointer typedefs, structs, enum payloads, tuples, descriptors, vtable
types, static enum boxes), prototypes (including those of the natives), the
C files of the standard library packages (natives), helpers, vtables, defer
thunks and spawn trampolines, constants, functions, `main` (which calls
`sd_main`, or `sd_test_main` with the table of tests).

## The formatter (`seed fmt`)

The rules are in docs/formatting.md. The formatter is a **token printer**,
not a syntax-tree printer: it lexes the file again and prints the tokens in
their original order, deciding only where lines break, how they are indented
and where single spaces go. Comments (`Comment {pos, text, ownLine}` from
the lexer) are merged back in by position. A tree printer would have to
rebuild every construct and re-attach comments to nodes; printing the tokens
keeps the program the same by construction, keeps redundant parentheses, and
needs very little knowledge of the grammar.

1. **Validate.** The file is parsed with the normal parser. A file with
   syntax errors is reported and left alone.
2. **Classify brackets** (`computeRoles`, fmt_layout.sd). Brackets are
   matched, and every `{` gets a role from the keywords before it: a block
   (`fn` declaration, `if`/`for` after their `(...)`, `else`, `group`,
   `test`), a switch/select body, a type body (`struct`/`enum`/`interface`),
   or otherwise a literal. Some `(` are marked too: parameter lists are
   *lists* (they may be written one element per line); conditions, result
   lists, receivers, `assert(...)` and `chan T(n)` are *plain*. Any other
   `(` is a list (a call) when the token before it ends an operand, and
   plain (grouping) otherwise. A `)` that closes a parameter list is marked
   so that `fn f() (int, error)` gets its space.
3. **Print** (fmt_print.sd). A stack of `FmtFrame`s mirrors the open
   brackets. A frame knows its kind, the indentation of its items, of its
   closing line and of the line where the current item started, and whether
   the next token starts an item. For each token, `place` decides between
   "new line with indentation N" and "same line"; `needSpace` decides the
   space from the two tokens (and their roles). New lines come from:
   statement ends (Newline tokens at statement level), block and case-label
   openings, the elements of a broken list, closing brackets of blocks and
   broken lists, and any other line break in the source, which is kept as a
   continuation line (base indentation + 1). Whether a list is broken is
   decided when it opens, by looking at the source line breaks at the list's
   own level (`isBroken`); the closing bracket of a broken list gets a comma
   before it if it has none, and a one-line list loses its trailing comma.
4. **Blank lines** come from a prefix count of blank source lines, so
   "was there a blank line between these two lines" is O(1). The top level
   has its own state (`FmtTop`: after package / import / const /
   declaration / comment).
5. **Safety net.** The output is parsed again, and its `seed ast` dump and
   comments must equal the original's; otherwise the file is left unchanged
   with an internal error. Line breaks are only added after tokens that
   never end a statement (`{`, `(`, `,`, `:`) or before a closing bracket,
   and only removed before `,` or a closing bracket, where the parser skips
   newlines, so the newline rule reads the output the same way.

The formatter only uses the source and the tokens, so its result does not
depend on the input's indentation or spacing at all; that, together with
keeping the source's line structure, is what makes it idempotent.

Testing (`make test-fmt`): `seed fmt --verify` runs on every `.sd` file in
`compiler/`, `tests/` and `lib/` that parses, and checks that formatting the
formatted text changes nothing, that the tokens are unchanged (ignoring
newlines and commas before closing brackets), and that the comments and the
syntax tree are unchanged. `tests/fmt/run.sh` runs the golden tests
(`NAME.sd` must format to `NAME.expected`, which must be a fixed point;
`--update` rewrites the expected files). Finally, `seed fmt --check` checks
that the compiler and the test programs are formatted. The few test files
with deliberate syntax errors, and `tests/full/errors/unformatted_code.sd`,
are not formatted. Since compiling checks formatting too (load.sd), every
program the tests compile is formatted anyway.

## Writing the compiler in Seed

Value semantics shape the code:

- All compiler state lives in a few structs (`Parser`, `Checker`, `Gen`) whose
  methods are `mut`, so the state is passed by pointer and lists are changed
  in place.
- Never copy an arena list into a local (`[]Expr xs = c.ast.exprs`): it
  raises the list's count, and the next change to the arena copies all of it.
  Read elements through the owner (`c.ast.exprs[e]`).
- Node structs contain no lists, so copying a node out of an arena is cheap
  (one retain per string or int field, which is a no-op for small ints and
  enum cases without data).
- Passing a struct (even `Checker`) to a function or a plain method costs a
  memcpy and no counting: parameters are borrowed. Storing it in a local
  retains every counted field.
- Build long strings as `[]string` and `join("")`, or append to a local
  (`s = s + x` appends in place when `s` is not shared); `+` on a growing
  string stored elsewhere is quadratic.
- Calling a mut method on a struct kills narrowing of everything under it, so
  copy a narrowed field into a local before calling mut methods.

## Adding a language feature: teach, snapshot, use

1. **Teach.** Implement the feature in the compiler (checker and generator,
   plus tests in tests/full) *without using it* in the compiler's own source.
   The compiler must still build with the previous compiler and reach the
   fixed point (`make verify`).
2. **Snapshot.** Commit the generated C of the new compiler (today:
   `build/stage2.c`) as the new bootstrap (`bootstrap/`), so that the
   compiler can be rebuilt by a compiler that knows the feature. Once the
   first snapshot exists, it replaces seed0 as stage 0.
3. **Use.** Now the compiler's source may use the feature. Rebuild from the
   snapshot and verify the fixed point again.

Until the first snapshot is taken, stage 0 is seed0, so the compiler source
is limited to the seed0 subset. For a new check (rather than new syntax),
fix the test programs that fail it in the same change. A small new library
function the compiler itself needs can also be taught to seed0 directly
(seed0 got `os.executable()` and `os.cwd()` this way, to find the root
through symbolic links and to compare directories).

## Decisions

- **The runtime of `runtime/`, pasted into the output.** Generated programs
  stay one self-contained C file (so the bootstrap snapshot will be one
  file). seed0 keeps its own runtime (`seed0/runtime.c`); `compiler/runtime0.c`
  is gone. Crash messages and exit codes are the same as seed0's.
- **Every call result goes into a temporary**, even when it is not counted:
  side effects (printing, crashing) then happen in source order, and C's
  unspecified argument order never matters. clang removes the copies.
- **Borrowed reads plus the "later effects" rule** instead of retaining every
  operand: an operand that only reads a place is passed as is, and is copied
  into a temporary only when a later operand of the same operation has side
  effects. Almost all reads in the compiler itself cost nothing.
- **Enums are boxes** (the runtime's recommended layout). Cases without
  data are static immortal boxes, so `Kind k = x.kind` never allocates.
- **Interface values hold the value in a box**; an enum's value (itself a
  box pointer) is boxed again. Vtable thunks take a pointer to the payload.
- **Interface methods may be `mut`.** A call needs a mutable place (not a
  parameter); it makes the interface's box unique (`sd_box_mut`) and passes
  the payload to the mut method.
- **`implements` requires the exact signature**, including `mut`. Methods
  cannot be declared on interface types.
- **A struct or enum converts to `I`, `I?` and (when it implements it)
  `error` automatically**, wherever such a value is expected (arguments,
  assignments, returns, list and map literals, struct fields).
- **`defer` takes a call of a function, method, function value, interface
  method or package function.** Not a mut method (its receiver would be
  gone by the time the call runs), not a built-in method or conversion.
  `defer _ = f()` is `Stmt.discard`.
- **Nullable function types need parentheses**: `(fn(int) int)?`, because
  `fn(int) int?` is a function returning `int?`. The type is printed that
  way in messages.
- **Error guards** follow the paths through the function (see
  "Checking"): a guard made inside an `if` or a loop holds after it on the
  paths that did not check the error. Only a plain `T`
  is guarded: `T? x, error err = f()` can read `x` (and compare it with
  null) at once, because a `T?` is checked before use anyway.
- **A field of function type can be called directly** (`op.apply(1, 2)`),
  when the struct has no method of that name.
- **Integer literals may have any size** (decimal or hex); values outside
  `SD_INT`'s 63-bit range become `SD_INT_BIGLIT` (parsed once, immortal).
- **Constants are computed once per thread** (`_Thread_local` cache) and
  never freed, as runtime/README.md section 12 asks.
- **tests/seed0/only** holds the tests whose expectations only hold for
  seed0 (64-bit overflow crashes, interfaces rejected); `make test0` runs
  them.
- **Arenas plus parallel result lists**, with `ExprInfo.wrap`,
  `ExprInfo.narrowed` and `ExprInfo.ifaceFrom` instead of rewriting the tree.
- **Diagnostics**: up to 20, sorted, deduplicated by position, one error per
  failing expression (`tyInvalid` suppresses cascades). "did you mean x?"
  hints are written without backticks because the seed0 tests look for that
  exact text.
- **"missing return"** is reported at the closing brace of the function.
- **`return null, err`** for a plain result is rejected when `err` is the
  literal `null` (or known to be null), and otherwise checked when the
  function returns: a null `err` there crashes (`f returned null as result
  1 without an error`) instead of handing the caller a null it may read.
- **Constants** must be computable when the program is compiled
  (`requireConstant`): no calls of functions, methods or function values,
  no variables, `try`, `group` or channels; conversions and built-in
  methods that change nothing are allowed.
- **Nesting is limited to 256 levels** of blocks and expressions
  (`maxNesting` in parser.sd): every phase recurses over the tree, and
  deeper code overflowed the compiler's stack (and, around 600 levels, the
  C compiler's).
- **Imports are per file.** seed0 treats them as package-wide.
- **The pedantic checks and the formatting check are always on**; the
  `--strict` flag is gone. The seed0-era test programs were changed to pass
  them (they still compile with seed0).
- **Float literals are emitted as written** (they are valid C and C rounds
  them correctly); seed0 printed them with `%a`.
- **`select` with `default`** is a syntax error with a hint (select always
  waits).
- **`seed run`/`seed build`** put the C file and executable in
  `$TMPDIR/seed-run-<name>` / `$TMPDIR/seed-build-<name>`.
- **`seed ast`** exists only for debugging the parser.
- **Concurrency** (the language decisions are DECISIONS.md 38-47):
  `group` is an expression of type `error`, used only whole (assigned,
  returned, discarded or with `try`); its block cannot `return`, `try`, or
  `break`/`continue` out of it; `spawn` takes the calls `defer` takes,
  returning `error` or nothing, never a mut method; select cases are send,
  receive, `_ =` receive or a receive into a new `T?` variable; channel
  elements are not nullable; constants cannot hold channels; `defer
  ch.close()` is allowed; `time` (after, sleep, constants) is a standard
  library package.
- **select dispatches with `if`/`else if`**, not a C `switch`, so that
  `break` and `continue` in a case reach the enclosing loop.
- **spawn arguments are borrowed** (`sd_spawn` deep-copies them), and one
  trampoline is generated per `spawn` statement.
- **time.after sends the wall-clock time and closes its channel**, as
  docs/stdlib.md specifies (the runtime's timer used to send the monotonic
  time and leave the channel open). `sd_chan_retain` became NULL-safe, for
  copies of a null `(chan T)?`.
- **Goroutines release the constants they computed** (`sd_goroutine_atexit`,
  added to the runtime); before, every goroutine that used a list constant
  leaked it.
- **`tests/full/NAME.leaks`** marks a test that is expected to leak because
  a goroutine crashes inside a group and the program goes on (the crash
  leaks the locals of the unwound frames); `tests/leaks.sh` reports it with
  the crashing programs.
- **`seed fmt` prints tokens, not the tree** (see "The formatter"). Style
  decisions not fixed by the task are listed in docs/formatting.md: no line
  length limit, `{}` for empty blocks, trailing comments not aligned,
  one-line `const`s may stay grouped, comment indentation before `case`
  follows the source column. `--check` reports to stderr.

- **Packages** (see "Packages"): one `Ast` for the whole program; names
  keyed by package; import paths resolved in the standard library first,
  then relative to the project root (no `seed.mod`); the core packages
  `fmt`, `os`, `fs`, `errors` and `time` are ordinary packages in `lib/`
  (natives over `runtime/seed_pkg.c` and `seed_task.c`; `errors` in plain
  Seed), so nothing about them is built into the checker except that
  `os.exit`/`os.crash` never return.
- **C names**: package `main` keeps unprefixed names; other packages'
  declarations get the import path as prefix (`shop/cart` -> `shop_cart_`),
  for both package-level and native names. Two packages whose paths differ
  only in `/` vs `_` would clash; import paths cannot contain `_`.
- **A test block is a function** (`FnDecl.isTest`) returning `error`;
  `assert` fails the test by returning an error (no crash, so defers run
  and nothing leaks); `return` is not allowed in a test.
- **Each test runs in a goroutine of its own group** (runtime
  `sd_test_main`), one after the other; `seed test` builds one program per
  package.
- **Unused enum pattern bindings are errors** like any unused variable;
  write `_`.

## Known limitations

- A crash leaks the counted values held by the locals of the unwound C
  frames (runtime/README.md section 16); normal execution leaks nothing.
  This includes a goroutine that crashes inside a group, and a goroutine
  stopped while it holds counted locals (the program goes on, so the
  memory is lost for good).
- When a group fails, only its goroutines are stopped, not the code of the
  group block (it is not a goroutine of the group). A block that waits on a
  channel only the stopped goroutines would have served ends in the
  deadlock crash; waiting belongs in a goroutine or after the group.
- A goroutine that is stopped while it waits for an inner group gets that
  group's result (often null: stopped goroutines report no error) and keeps
  running until its next stop point.
- Deadlock detection is global: it only fires when *every* goroutine is
  blocked and no timer is pending; a goroutine stuck while others run (or
  sleep) waits forever. Defers do not run on a deadlock.
- A select case cannot assign the received value to an existing variable;
  declare a new one in the case.
- A mut call evaluates its arguments before the receiver's place, so in
  `xs[f()].push(g())` `g` runs before `f`.
- Two concurrent `seed run` (or `seed test`) of programs with the same name
  share a temporary directory.
- A deadlock in one test ends the whole test program (deadlock detection is
  global); `seed test` then reports the package as failed with the exit
  code.
- Package identity is by directory path as written: the same directory
  reached through two different spellings would be loaded twice (cannot
  happen with the resolution rules unless the project root is itself
  inside `lib/`).
- The parser reports at most one syntax error per statement; an error in an
  expression that spans lines may hide a second error on the next line.
