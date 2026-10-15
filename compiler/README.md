# The Seed compiler

The Seed compiler, written in Seed. It is built from a snapshot of its own
generated C, `bootstrap/seed.c`, and then compiles itself:

```
cc bootstrap/seed.c        -> build/seed-boot
seed-boot c compiler/*.sd  -> build/stage1.c -> build/seed1
seed1 c compiler/*.sd      -> build/stage2.c -> build/seed2
seed2 c compiler/*.sd      -> build/stage3.c      (identical to stage2.c)
```

`build/seed2` is the compiler everything else uses. The bootstrap compiler
seed0 (C, for a subset of Seed) built the first stages until the snapshot
existed; it is retired (docs/seed0.md).

```
make            # build/seed2 (a fresh clone needs only a C compiler and make)
make verify     # stage2.c == stage3.c, and stage3.c == bootstrap/seed.c while the inputs are unchanged
make release    # verify, then write stage3.c to bootstrap/seed.c (and bootstrap/inputs.sha256)
make test       # tests/lang and tests/core with build/seed2
make test-full  # tests/full (programs and packages) and tests/test_cmd.sh (seed test, build, run, install)
make test-lib   # the standard library's tests: build/seed2 test lib
make test-leaks # every test program under a leak checker (leaks or LeakSanitizer)
make test-fmt   # seed fmt: self-checks on every .sd file, tests/fmt goldens
make test-examples  # tests/examples.sh and examples/run.sh: every example built, run and checked
make test-lsp   # tests/test_lsp.sh: seed lsp driven over stdin/stdout by scripted sessions
make install    # links $(PREFIX)/bin/seed (PREFIX=/usr/local) to build/seed2; make uninstall removes it
```

**The snapshot.** `bootstrap/seed.c` is a verified `stage3.c`:
self-contained C (runtime and natives pasted in) that needs only `cc
-std=c11 -O2 -lm -lpthread`. `bootstrap/inputs.sha256` lists the SHA-256
of every file it was made from (`compiler/*.sd`, `runtime/`, the `lib/`
packages). `make verify` checks the fixed point and, while those files are
unchanged, that `stage3.c` is the snapshot byte for byte; once they change
it only says that the snapshot is out of date. The snapshot does not have
to be current to build: any snapshot that can compile the current source
will do (see "Adding a language feature").

## Command line

```
seed build [-g] [-o out] <dir | file.sd>  compile a program (default output: the directory's or file's name)
seed run [-g] <dir | file.sd> [args...]   compile into $TMPDIR and run; passes the exit code
seed test [-v] [--run text] [dir...]  run the tests of every package under the dirs (default .)
seed fmt [--check] <file.sd | dir>... format in place, or report unformatted files
seed c [-g] <dir | file.sd...>        C program on stdout (what tests/run.sh uses)
seed ast <file.sd>...                 print the syntax tree (debugging aid)
seed lsp                              language server on stdin/stdout (see "The language server")
```

A program is the `package main` of a directory (all its `.sd` files) or a
single `.sd` file; `seed c` also takes several files of one directory (the
bootstrap passes `compiler/*.sd`). The packages it imports are found as
described in "Packages" below. Compile errors go to stderr and exit with
code 1; usage errors exit with 2. The pedantic checks and the formatting
check are always on (there is no `--strict` any more).

The C of `seed c` is self-contained: it starts with the runtime of
`runtime/` (`seed.h`, `seed_internal.h` and the seven `seed_*.c` files
pasted in that order, without their `#include "seed..."` lines;
`readRuntime` in `gen.sd`), followed by the program, into which the C files
of the standard library packages it uses are pasted (`readNatives`).
`build`, `run` and `test` instead compile one C file per package below
`seed.h` (`gen_unit.sd`), take the objects of the imported packages from
a cache, and link them with a cached object of the runtime (see
"Decisions"; `buildProgram` and `runtimeObject` in `main.sd`), in a
temporary directory of their own that they remove when they finish. The compiler finds its
root (the directory with `runtime/` and `lib/`) through `$SEED_ROOT`, or
else from its own path: `os.executable()` gives the binary's real path
(symbolic links resolved, so `seed` works through `$PATH` or a link), and
the binary is `<root>/build/seedN`. The C compiler and its flags for
`build`/`run`/`test` are the constants `cCompiler`, `cFlags`, `buildOpt`,
`quickOpt` and `cLibs` at the top of `main.sd`: `seed build` compiles with
`-O2` (`buildOpt`), `seed run` and `seed test` with `-O1` (`quickOpt`,
about half the C compiler's time), and the runtime's cached object is
always `-O2`. The Makefile has the `-O2` flags in `SEED_CFLAGS` and
`SEED_LIBS`. With `-g` (`build`, `run` and `c`), the generated C has
`#line` directives that point at the Seed source (`gen_site.sd`), and
`build` and `run` compile it with `-g` and `-O0` (`debugOpt`) instead, for
gdb and lldb (docs/guide, chapter 14); the cache keeps those objects apart
(their text and flags differ). The runtime's object stays the cached `-O2`
one.

### seed test

`seed test [dir...]` walks each directory (a trailing `/...` is accepted
and means the same) and finds every package directory that has a `tests/`
directory with `.sd` files. For each it compiles a test program: the
package, its `tests/*.sd` files (which belong to the package and see its
private names) and its imports, with the directory given on the command
line as the project root. The checker runs in *test mode* (test blocks are
checked, `main` is not needed) and the generator emits a table of the tests
and a `main` that calls `sd_test_main` (runtime/README.md, "Native
functions", for the runner). The program is built as `test` in a
subdirectory of a new `$TMPDIR/seed-test-XXXXXX` (removed at the end) and
run with the name of a results file; it
prints one line per failed test and writes its counts there, and `seed
test` prints the total, `N passed, M failed`, and exits with 1 if anything
failed. A package that does not compile counts as one failure. The options
`-v` and `--run text` are passed on to every test program after the
results file (the runner applies them, runtime/README.md); with `-v`, `seed
test` also prints `testing <dir>` before each package. When `--run` matched
no test in any package, `seed test` says so on stderr and exits with 1. Any
other argument starting with `-` is a usage error (exit 2).

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
- A project package that the standard library would shadow (`<project
  root>/strings` with `package strings` in it, imported as `"strings"` by
  project code) is an error at the import (`projectPackage`), not silently
  replaced by the standard library's.
- Every `.sd` file directly in a package's directory belongs to it; all
  must have the same `package` line, which must be the last element of the
  directory's path (only `package main` may differ). Package `main` cannot
  be imported.
- Every file must be formatted: after parsing, `formatSource` must give
  the same text, otherwise the error points at the first differing line
  with the hint `run: seed fmt <file>`. These errors are kept apart
  (`Loader.unformatted`) and do not stop the checker: `compileProgram`
  prints the other errors first and the formatting errors after them
  (`printUnformatted`), so that formatting never hides a real error.
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
| `main.sd` | driver | CLI, runtime location, building and running programs (temporary directories, the runtime cache), `seed test` |
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
| `gen_int.sd` | gen | ints known to be small: the interval analysis of int locals, unboxed locals, plain C arithmetic |
| `gen_move.sd` | gen | moving a local's value on its last use; borrowed locals and the mod sets of mut methods |
| `gen_unit.sd` | gen | the program as one C unit per package (for the object cache of build, run and test) |
| `gen_site.sd` | gen | crash traces: the table of sites of a unit, the marks after the lines that may crash; `#line` directives for `-g` |
| `gen_fast.sd` | gen | small-int versions of pure int functions, and the functions that choose between them and the general ones |
| `gen_preempt.sd` | gen | where the generated code checks for preemption: the direct call graph's strongly connected components |
| `gen_acc.sd` | gen | loop accumulators: a fast copy of a loop with raw `int64_t` accumulators that spills into the general copy; each loop iteration's preemption check |
| `lsp.sd` | lsp | `seed lsp`: JSON-RPC framing, the open documents, dispatch, one goroutine per request |
| `lsp_check.sd` | lsp | loading and checking a document's package with the editor's text, diagnostics, formatting, LSP positions (UTF-16) |
| `lsp_query.sd` | lsp | definition and hover (what a name refers to), completion, document symbols |

## Data structures

### Positions and diagnostics

`Pos {file, line, col}`: `file` indexes `Diags.paths`/`Diags.sources` (and
`Ast.files`); `line` and `col` start at 1 and `col` counts bytes.

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
`Comment {pos, text, ownLine}` and stored in `Ast.comments`, where
`seed fmt` finds them (see "The formatter").

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
  `-9223372036854775808` is valid; the literal keeps its raw text. Integer
  literals may have any size, because integers grow (see "Decisions").
- Comparisons do not chain; `x += 1` gets a specific message.

Error recovery: the first error in a statement sets `failed`, and parse
functions then return quickly without reporting more. The block loop skips
to the end of the statement (`syncStatementFrom`: a newline outside
brackets, where the brackets the statement opened before the error, such as
the `(` of `if (a < b < c) {`, count as open) and continues; the top level
skips to the next line that starts a declaration (`syncTop`), always moving
past the offending token. So that one compile reports the errors of every
independent statement and declaration:

- In an `if`, `for` or `switch` whose header has the error, the skip still
  parses the statement's blocks (`opensBody`: a `{` that ends its line, not
  inside braces, after `)` or `else`, or after anything but a name or `]`
  when the header left a bracket open; for a switch, its cases), and a
  block whose `{` is found right after a broken header is parsed normally
  (`parseBlockBody` clears `failed` after its `{`).
- After an error in a function's header, its body is parsed if a `{` that
  ends its line comes before the next top-level declaration
  (`findFnBody`).
- A struct field, enum case or interface method with an error is skipped
  to the end of its line, and the type's next members are parsed.

The errors go to the same `Diags` as the checker's, so at most 20
(`maxErrors`) are shown, followed by `too many errors; only the first 20
are shown`. When a file has syntax errors, checking is skipped.

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

There are no implicit conversions except
`T → T?` (`coerce` sets `wrap`), `null → T?/error`, reading a narrowed `T?`
as `T`, and a struct or enum that declares `implements I` → `I` (and `I?`,
and `error`; `coerce` sets `ifaceFrom`). Narrowing keeps a stack of
narrowed *paths* (`"v12.email"` = variable 12, field `email`), added by
conditions (`!=`/`==` null through `&&`, `||`, `!`) and by assigning a
value that is never null (`isNonNullValue`: a `T`, not a `T?`) to a `T?`
path with one target (declarations included), killed by assignment to the
path or a prefix, and by a mut method call on a prefix. Paths of type
`error` are narrowed the same way (`narrowable`): `err.message()` needs
`errorKnownNonNull` (a narrowed path), and an error that is known not to
be null, or a struct or enum converted to `error`, is a value that is never
null for `isNonNullValue`. `checkIf` checks
each branch from the narrowings before the `if` and joins them
(`joinNarrows`): an entry killed in either branch stays killed, and the
paths narrowed at the end of every branch that falls through
(`checkBlockLive`) are narrowed after the `if` (so `if (x == null) {
return }` and `if (x == null) { x = "none" }` both leave `x` narrowed).
Before a loop, every path the loop assigns or mutates is un-narrowed. Whether a statement always exits is a pure function
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
  type, to an existing variable (an identifier) of exactly that type, or
  to `_`. Each case has its own scope. A select whose cases all exit,
  exits (`stmtExits`).
- Package constants cannot hold channels (`hasChan`), because constants are
  cached per goroutine.
- `time` is a standard library package (`lib/time`): `after` and `sleep`
  are natives, the durations are constants.

The pedantic checks are always on
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
parameters are borrowed, results are owned, stores consume. Parameters
cannot be changed in place in Seed, so a callee never retains or releases
one (unless it stores it), and a caller passes its arguments without
retaining them: a call result passed on is released after the statement,
anything else is passed as it is. A parameter that the function assigns
(`findOwnedParams`: a mutation of the borrow analysis whose root is the
parameter; the checker allows only whole assignments) is *owned* instead:
on entry `ownParams` declares a local `o_<name>` (a frame field when its
type lives in frames) holding a retained copy of the C parameter `l_<name>`,
in a scope around the body, and from then on it is an ordinary local
(`varC`), released by assignments, returns and the end of the body. The
fast version of a pure int function (gen_fast.sd) assigns its C parameter
directly, since words need no ownership. The generator
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
  its arguments before the receiver's place, and keeps a copy of those the
  call could change (`bag.addAll(bag)`): `mutArgs` passes an argument
  borrowed when it reads another variable than the one the receiver's place
  starts at (or a field path of it), and no later argument nor the
  receiver's place has mut effects, because a mut call can only change
  what is reachable from its receiver's variable. Literals are never copied
  (`ownedTemp` makes no retained temporary for an immortal value).
- *Move on last use* (`gen_move.sd`): a store of a local `x` (into a
  struct, list, map, box, interface, defer environment, another variable,
  or a mut call's kept argument) moves `x`'s value instead of retaining it
  when the statement is a simple one (expression, assignment, declaration,
  return, local const, defer) directly in the block that declared `x`,
  mentions `x` exactly once (not on the right of `&&`/`||`, not in a group
  block), and no later statement of the block mentions `x`. `analyzeMoves`
  finds these in one walk per function; `ownVal` does the move with
  `moveOut`, like for a temporary: `x`'s slot stops being live, and its frame
  field is set to null after the line that took the value. The rule is
  syntactic, so every path through the statement moves `x` (or leaves it
  early, before the move, with `x` still owned) and nothing touches `x`
  afterwards; a local declared outside a loop is never moved inside it.
  Besides a retain/release pair, a move keeps a list's count at 1, so
  changing the copy later does not copy the list.
- *Borrowed locals* (`gen_move.sd`, `analyzeBorrows`): `T x = place` (a
  variable, a field path of one, an element of a list reached through one,
  as in `Expr x = g.c.ast.exprs[e]`) declares `x` as a plain C local with
  the place's bits, owning nothing (no retain, no release, no slot, no
  frame field), when nothing in the function changes `x`, and nothing in
  `x`'s scope changes the place's variable where `x` could see it. One
  walk per function records the mutations in order (assignments and
  `++`/`--` of places, mut built-ins, mut interface methods, mut methods
  called on places) with the variable and field path of their place (cut
  at the first index); a mutation of `root.p` conflicts with a read of
  `root.q` when one path is a prefix of the other. A mut method called on
  `root.p` only changes `root.p.f` for the fields `f` of its *mod set*
  (`computeModSets`: the first-level fields of the receiver its body
  changes, closed over the mut methods it calls on its own receiver; a
  native changes everything), so `Expr x = g.c.ast.exprs[e]` stays
  borrowed across `g.emit(...)`. A store or a return of `x` retains it,
  as for a parameter; `x` and the roots of borrowed locals are never moved
  (a moved container would get a new owner that may change it in place).
  Other variables cannot alias the place: locals are values, parameters are
  borrowed from the caller and never changed in place (an assigned one owns
  its value, and its assignments are mutations like any local's), and a mut
  call keeps a copy of every argument read from its receiver's own variable
  (`mutArgs`).
- `x = x + a + b` on a local string, or on a string field of a local
  struct or of a mut method's receiver through struct fields only
  (`w.out = w.out + s + ","`, `d.body.text = d.body.text + s`), appends in
  place (`genAppend`): `sd_str_append` for one part, `sd_str_append_parts`
  for several (the parts are written straight into the string, ints as
  their digits, with no concatenation of their own). The place is computed
  after the parts, which must not mention the variable (`appendRoot`,
  `samePlace`); a field of a list element, a map entry or a nullable box
  takes the general path. The borrow analysis already treats the
  assignment as a change of the variable, so no borrowed local can see the
  string change. Other string `+` chains are one `sd_str_concat_n`. When a part of the
  chain is `n.string()` on an int, the chain is one `sd_str_concat_parts`
  instead, which writes the digits straight into the result (no string is
  made for the number).
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

*Cleanup frames* (runtime/README.md, section 9): a crash or a stop unwinds
with `siglongjmp`, so the generated releases do not run; what the locals
and temporaries own is released through the function's frame instead
(`genFn`):

- A slot whose type lives in frames (`frameCounted`: counted types except
  `int`, and except enums whose cases have no data, whose values are all
  immortal) is a field of the function's struct `L` (`frameField`):
  `L.l_name`, `L._t3`. Locals of one name and type in sibling blocks share a
  field.
- The fields hold null or an owned value at every point where the goroutine
  may crash: `L` starts zeroed, `releaseSlot` sets a field to null after
  releasing it, and a temporary moved out (`ownVal`, `consume`) is set to
  null by `emit` right after the line that consumed it. A multiple
  assignment nulls each part of the tuple as it moves it. Operands that may
  crash (an index, a conversion) of a statement that takes several values
  over at once (a struct literal, a map entry, a tuple returned, a defer's
  environment) are evaluated into temporaries first (`ownedOperands`), so
  that they cannot crash after another value was retained in the same
  statement.
- A return computes its result, leaves the frame (`SD_LF_LEAVE`), and then
  releases without nulling.
- Only the fields that need it are kept: while the body is generated, frame
  slots have placeholder names, `emit` counts the lines that may crash
  (`lineMayCrash`: any call other than the retains, releases, constructors
  and other runtime calls of `safeCallSet` that cannot crash), and
  `slotEnd` marks the field of a slot that owned its value while such a
  line was emitted. The other placeholders become plain C locals
  (`expandFrame`), and a function without needed fields has no frame (most
  temporaries live from one call to the next statement).

*Crash sites* (gen_site.sd; runtime/README.md, "Crash traces"): every
unit has a table of sites (`sd_sites_root`, `sd_sites_<cprefix>`, or
`sd_sites_prog` for `seed c`), `{function, file, line, col}`, declared at
its top and defined at its end (`siteDecl`, `siteTable`). While a Seed
function is generated (`siteStart`/`siteEnd`, in `genFn` and `genFast`),
`Gen.pos` is the position of the code being generated: the statement
(`genStmt`, `fastBlock`), or the call inside it (`gexpr`, `fastExpr`), and
`emit` puts `SD_SA(table, k)` after each line that calls something that
may crash (`markable`: like `lineMayCrash`, without C's keywords and the
checks below; a `return` with a call keeps its value in a variable
first). The fast versions (gen_fast.sd) have no marks (`noMarks`): even
one `asm` statement changes how GCC lays out a small recursion like fib's,
which cost fib 10 to 20 %; the chooser's call of the fast version is
marked with `SD_SAV`, a non-volatile mark that keeps the function pure,
at the function's own position, so that a trace through a fast version
names the function (and counts its calls). The checks whose failure path the C
compiler moves out of line take the address of their site instead
(`siteAddr`): `SD_LIST_GET_AT` and `sd_str_at_s` in `gindex` (the
position of the indexed value), `sd_int_div_at` and `mod`, `shl`, `shr`
in `binaryOp` (the operator's), `sd_crash_at` for `return null, err`; a
call of `os.crash` is preceded by `sd_crash_site_` (`plainCall`). A spawn
trampoline has no marks (the trace ends with the spawn's own site). The
function names of the table are `fnSiteLabel`'s (`Cart.add`,
`cart.newCart`, `test "name"`), and the files `sitePath`'s (as given, the
standard library's from `lib/` on). With `-g`, `lineMark` puts a `#line`
before every line of a Seed function (a directive numbers the lines after
it), with the file's absolute path, and `lineReset` a placeholder after
it that `finishLines` (main.sd) turns into a `#line` back to the C file;
the cleanup frame's setup goes on the line of the function's declaration.

*Small ints* (gen_int.sd): `analyzeInts` computes, before a function is
generated, an interval for every `int` local declared in it (by a
one-target declaration or a local `const`) from all its assignments, by
iterating from "no value" until nothing changes (intervals that keep
growing are widened to "any int"). Literals, constants, `len()`,
`int(byte)`, and `+ - * / % &` on known intervals (`/` and `%` only when
the divisor cannot be zero) have known intervals; anything else is any
int. The post statement `i++` of a `for` whose condition has a conjunct
`i < B` (or `<=`, or `i--` with `>`/`>=`) and whose body and condition do
not assign `i` ends at most at B's upper bound. A local whose interval is
inside the small range (`|x| < 2^62`) is *unboxed*: an `int64_t` C local
(`l_i`), read as `SD_INT_S(l_i)`, stored with `SD_SMALL(v)`, incremented
with `l_i++`, never retained or released. A comparison of two small values,
and `+ - * / % &` whose result is small too, is plain C on `int64_t` with
no temporary (`smallBinary`). In an int `+` chain, the simple small operands
(literals, constants, unboxed locals and arithmetic on them) are added in C
first and then added to the others with one general addition (`gsumSmall`;
ints never overflow, so `+` is associative). A small `/` or `%` whose
dividend's interval is `>= 0` (`divNonNeg`): by a simple constant `c > 0`,
a power of two is a shift or a mask, and anything else a multiplication by
a magic number, `sd_divc_(x, m, s, c)` = the high word of `x * m` shifted
right by `s` (`magicDiv` takes the smallest `s` with `m = ceil(2^(64+s) /
c) < 2^64` and `(m * c - 2^(64+s)) * max < 2^(64+s)` for the dividend's
upper bound `max`, which makes it exact on `0..max`), and `sd_modc_` = `x -
sd_divc_(x) * c`; by a divisor that is `> 0` but not constant, an unsigned
division. C's signed division by a constant needs a correction for
negative dividends (`sar $63`, `sub`), which GCC keeps even when it could
know the operand is not negative (it turns an unsigned division of a value
it knows to be non-negative back into the signed one). The helpers are
emitted once per C unit (`divcHelpers`, behind `#ifndef SD_DIVC_`) and use
`unsigned __int128`, or C's unsigned division where it does not exist.

*Int fast paths* (gen_int.sd, gen_fast.sd, gen_expr.sd):

- `x = x + a + b`, `x = x - e`, `x++` and `x--` on an int that is not
  unboxed (a sum that grows) update `x` in place (`genIntUpdate`,
  `intUpdate`): when both words are small and the add does not overflow,
  `x.w` is replaced; only the slow path calls `sd_int_add_slow` (or
  `_sub_slow`) and releases the old value. Before, every update made a
  temporary and released the old value, two branches GCC could not remove.
- *Fast versions* (`gen_fast.sd`): a pure int function (parameters `int` or
  `bool`, result `int` or `bool`, a body of int and bool locals, literals
  and literal constants, arithmetic, comparisons, `if`, `for`, `return` and
  calls of other such functions; not a method) gets `sd_fs_f`, which works
  on the words of small ints (`2n`) with `__builtin_*_overflow` checks, and
  returns the odd word 1 to give up (overflow out of the small range, a
  division by zero). `sd_f_f` calls it when every int argument is small and
  falls back to the general version `sd_fg_f` (the normal body) when it
  gives up. The function being pure, computing it again is invisible (a
  division by zero then crashes in the general version, with the usual
  message). General versions call general versions, so a fallback costs one
  fast attempt per call the program makes, not one per recursion level.
  `findFastFns` keeps only the candidates all of whose callees have fast
  versions.
- `v == E.Case` and `!=` (either way round), where `E.Case` has no data and
  `v` is an `E`, compare the tag (`genumIs`) instead of calling
  `sd_box_equal`, which compares two different boxes through their
  descriptor.

Intervals for parameters and fields were not added: a parameter or a field
can hold any int when the function is called or the struct built, so an
interval for it needs every caller and every store (a whole-program
analysis, and none for exported functions or function values); the fast
versions are the checked-at-run-time form of "the parameters are small".

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
  `cs[i].ok` and the buffer, stores it into an existing variable (`gstore`,
  which releases the old value), or releases the buffer when it has none.
- `time.after(d)` calls the native `sd_lib_time_after`, which is
  `sd_time_after(d)` (a `chan int`); `time.sleep(d)` is `sd_time_sleep(d)`;
  `time.second` is the constant `sd_c_time_second()`.
- `defer ch.close()` uses the defer machinery with the channel as the one
  operand.

*Preemption points* (gen_preempt.sd; runtime/README.md, "The
scheduler"): `SD_PREEMPT()` is a relaxed load of a process-wide word and a
branch not taken; the runtime sets the word when a goroutine has computed
for 10 ms while others wait, and the slow path yields if it was this one.
A goroutine runs for long only in a loop or a recursion, so the points are:

- at the top of every loop iteration, before the condition (`genFor` and
  `loopCheck`, and `fastStmt` in fast versions). A *counted* loop (its post
  statement is `i++` or `i--` of an unboxed local that nothing else in the
  loop assigns) whose body is pure small-int code (arithmetic on unboxed
  locals and raw accumulators, comparisons, `if`, `break`, `continue`; in a
  fast version, any body) checks every 1024th iteration instead:
  `if (SD_UNLIKELY((l_i & 1023) == 0)) SD_PREEMPT();` (`& 2046` on a fast
  version's word), a test of a register instead of a load, since each
  iteration takes a short, bounded time;
- before every call that may close a cycle of calls (`preemptCall`): a
  call of a function or method by name (or a deferred one, in its thunk)
  whose callee is in the caller's own strongly connected component of the
  direct call graph (`findPreemptFns`: Tarjan's algorithm over the calls by
  name, including those in `defer` and `spawn`), and every indirect call
  (function value, interface method, `sortBy`'s function, `message()` of
  an error), which may lead anywhere;
- on entry to the methods in vtables (`preemptsOnEntry`), which the runtime
  calls without a call in the generated code (comparing errors, say).

Every cycle of calls is either made of direct calls within one component,
or has an explicit indirect call, or a call by the runtime into a vtable
method. Checking before the call rather than on the callee's entry keeps
the callee shrink-wrappable: GCC compiles fib's leaves (`n < 2`) without
saving registers, which an entry check (a call on its cold path) prevents,
doubling fib's time. A function checks at most once on each path through
its top-level statements (`preemptLine`): a check made while a top-level
statement that runs straight through (an expression, assignment,
declaration, `++`/`--` or `return`, outside the right side of `&&`/`||`)
is generated covers the calls after it (`pcSeen`), so `fib(n - 1) +
fib(n - 2)` checks once (loops do not count: their first check may be
masked). Direct calls only go
from a package to itself or to those it imports, so a package's checks do
not depend on the packages that import it (the object cache relies on a
unit's text depending only on its package and its imports). A preemption
point is not a stop point and crashes nothing, so `SD_PREEMPT` is in
`safeCallSet`: a value held in a C local across one is not lost (nothing
unwinds there), and it needs no frame field. The compiler's own C has
551 loop checks (2 of them masked) and 626 call and entry checks.

*Loop accumulators* (gen_acc.sd): an int local that a loop only adds to
(`sum = sum + e1 + e2`, `sum = sum - e`, `sum++`, `sum--`), such as
`intloop`'s `sum`, grows without bound, so `analyzeInts` cannot unbox it.
`accVars` finds such locals of a `for` loop: declared before the loop,
every mention of them in the loop (condition, post statement and body) is
in an update in *tail position* (`accTail`: the last statement of the body,
or of a branch of an `if` or a case of a `switch` in tail position), and
the body is small (at most 64 statements) without `defer`, `spawn`,
`select` or `group`. Then `genFor` emits a *fast copy* of the loop
(`genAccLoop`), entered when all the accumulators are small, in which each
is a raw `int64_t` (`r_<name>_<var>`) and each update is one
overflow-checked add in place (`genAccUpdate`); the loop's end and `break`
make them general ints again (`sd_int_from_i64`). An update that
overflows `int64_t` (or adds a big int) *spills*: it computes the exact
result as a general int (the old raw value is the wrapped one minus the
increment, modulo 2^64), releases what `continue` would, and jumps to the
continue label of the *general copy*, the loop as before, which runs the
post statement and the following iterations. That is exact because an
update in tail position is the last thing its iteration does, and no other
code in the loop reads the accumulator (a `return` or a crash leaves the
variable holding its small entry value, which owns nothing). In the
general copy, an empty `asm` that may change the counter (`spillBarrier`)
follows the continue label: otherwise GCC hoists the post statement `i++`,
which both copies' paths run next, above the overflow jump, and keeps the
overflow flag across it with `seto`/`test`/`jne`. tests/full/
int_accumulators.sd crosses 2^63 and -2^63, the small range both ways,
starts big, and uses `++`, `--`, `continue`, `break`, `return`, nested
loops and `switch`.

*Bounded accumulators* (`accBounded`): in a counted loop (`countedVar`: the
post statement is `i++` or `i--` and nothing else in the loop assigns `i`)
whose counter is unboxed with interval `lo..hi`, one run of the loop has at
most `hi - lo + 1` iterations (the counter takes a different value in each,
all in its interval, which holds everywhere in the function), and each
iteration runs at most one update of a given accumulator (tail position).
When every update of an accumulator adds or subtracts a raw small value of
magnitude at most `d` (`accStep`) and `(hi - lo + 1) * d <= 2^62`, the raw
value, which enters the fast copy in `-2^62..2^62-1`, stays inside
`int64_t` for the whole run, so its updates are a plain `+=`/`-=` with no
overflow check and no spill (`LoopCtx.bounded`). The end of the loop makes
it a general int as before, big when it left the small range.
tests/full/int_bounded_loops.sd has loops right at the edge (two trips of
2^61 from the top and bottom of the small range), one past it, counts over a
length, bounds and counters assigned in the loop, nested runs, and the
divisions above against general ints.

Built-in methods are `Res.Builtin` with a `Builtin` (check_call.sd,
`checkBuiltin`; gen_call.sd, `gbuiltin`). `m.getOr(k, d)` evaluates the
map, the key and the default (`operands`) and is the borrowed `(p ? *p :
d)` of `sd_map_get`. An enum's `e.name()` (`Builtin.EnumName`, chosen in
`checkSelectorCall` when the enum has no method `name`, which
`resolveSignature` forbids) reads the tag once and is a chain of `?:` over
immortal string literals (`enumNameChain`); for `implements`, the built-in
satisfies `name() string` (`checkImplementsOne`), and the vtable gets a
thunk that does the same on the payload (`genVtables`).

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

A directory on the command line stands for the `.sd` files under it
(`sdFilesUnder`: sorted, subdirectories included, hidden ones skipped).

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

## The language server

`seed lsp` (`lsp.sd`, `lsp_check.sd`, `lsp_query.sd`) speaks the Language
Server Protocol over stdin and stdout: JSON-RPC 2.0 messages with a
`Content-Length` header, read with `io.readLine` (headers) and `io.read`
(the body), parsed and written with `lib/json`. It supports `initialize`,
`shutdown`, `exit`, `didOpen`, `didChange` (full text; a ranged change is
applied too), `didClose`, `didSave`, `publishDiagnostics`, `formatting`,
`definition`, `hover`, `completion` (trigger character `.`) and
`documentSymbol`; other requests get the error MethodNotFound.

**State and checking.** The server keeps the text of every open document
(`LspServer.docs`, by URI) and nothing else: each request loads and checks
the document's program again (`lspAnalyze`), as `seed build` and `seed
test` would, with the open documents' text in place of the files
(`Loader.overlay`, consulted by `addFiles`). Which program that is:

- the `.sd` files of the document's directory, as a package; for a file in
  a package's `tests/` directory, the package and its tests in test mode;
- a package other than `main` in test mode (as `seed test` checks it,
  without `main`); when several files of a directory declare `fn main` (a
  directory of single-file programs, like `examples/`), the document alone;
- imports resolved from the project root: the nearest of the package's
  directory and the directories above it from which every import that is
  not in the standard library resolves (`lspProjectRoot`), so a package
  deep in a project finds its siblings as `seed build` of the program
  would; a package of the standard library gets its import path.

Checking the compiler itself (about 20,000 lines) takes about 0.2 s, a
small program a few milliseconds, so there is no cache: after every
`didOpen`, `didChange`, `didClose` and `didSave` the server checks every
open document and publishes its diagnostics.

**Diagnostics** are the compiler's: the loader's and parser's errors when
there are any (as for `seed build`, the checker's errors are not shown
then, since they would mostly be cascades), otherwise the checker's, plus
a warning (severity 2) at the first line `seed fmt` would change. The hint
follows the message on a line of its own. Errors in another package are
shown at the document's import of that package, with their file and line.

**Positions.** `Pos` counts bytes from 1; LSP counts lines from 0 and
characters in UTF-16 code units (`lspOffset`, `lspPosition`): a character
of 2 or 3 bytes of UTF-8 is one unit, one of 4 bytes is two. A diagnostic's
range covers the identifier or number at its position, or one character.

**Crashes.** Every request runs in a goroutine of its own group
(`runJob`, `lspTryJob`), which gets a copy of the request and sends its
JSON result back on a channel. If the compiler crashes on what the editor
sent (an index out of range on a half-written declaration, say), the
group returns the crash as an error: the request's result is `null` (an
empty list for formatting and symbols), the crash is logged to stderr, and
the server carries on. A completion whose check crashed is tried again
without the checker, from the tree alone. To make crashes rare, a tree with
syntax errors is repaired before it is checked (`lspRepair`): the parser
leaves `-1` where a type is missing (a field `[]` still being typed), and
each becomes a type name that does not exist, which the checker reports
and treats as invalid. The request `seed/crash` crashes on purpose, for the
tests.

**Queries.** Definition and hover lex the document to find the identifier
under the cursor (or right before it) and look, in order, for:

1. a declaration with that name at that position: function, method,
   receiver, type, field, enum case and its fields, interface method,
   constant, parameter, pattern binding, variable, local constant;
2. an expression at that position that the checker resolved (`ExprInfo`):
   going backwards through the arena, so that a call (which starts where
   its callee does and knows the function or method) comes before its
   callee; locals (`Var.pos`), constants, functions, methods, interface
   methods, enum cases, struct fields (through the base's type), struct
   literals and their field names, built-in methods and conversions;
3. `pkg.name`, `Type.Case` and `pkg.Type.Case` from the tokens before it
   (types in type positions, case patterns);
4. a type name (`Checker.teType`), then any name of the package or an
   imported package.

The result is the declaration's position and a signature rebuilt from the
tree (`fn (Cart c) mut add(Item item) error`, a type with its fields or
cases, `const int maxItems = 100`, `shop.Cart cart` for a local), with the
`//` comment lines right above the declaration as its documentation.

Completion after `.` with nothing typed after it inserts a placeholder
name at the cursor first (`lspPlaceholder`), so that `cart.` parses as a
field access whose base the checker types. Then: after the name of an
imported package, its `pub` functions, types and constants; after a type
name, the enum's cases; after any other expression, the fields (visible
from the document's package) and methods of its type, the methods of an
interface, or the built-in methods of a string, list, map, channel,
number or error (docs/builtins.md). Elsewhere, the parameters and locals
in scope (`lspLocals`: the blocks that contain the cursor, walked in the
tree, with the declarations before it), the package's names, the file's
imports, the keywords and the built-in types.

Document symbols come from the document alone (parsed, not checked):
types with their fields, cases or interface methods as children,
functions, methods (`Type.name`), constants and tests; a declaration's
range ends at the newline that ends it outside brackets.

Formatting returns one edit that replaces the whole text with
`formatSource`'s, after the same safety check as `seed fmt` (the result
parses to the same tree and comments); nothing when the text is formatted
already or has syntax errors.

**Tests** (`make test-lsp`, tests/test_lsp.sh): tests/lsp/driver (a Seed
program) turns session scripts (`tests/lsp/*.lsp`: open, change, hover,
definition, completion, symbols, format, any request) into framed
messages, and checks the server's answers (framing, one response per
request) against the transcripts `tests/lsp/*.out`. The sessions work on
tests/lsp/proj, a program with a package of two files and its tests, with
non-ASCII text before the names they ask about, and change it with the
versions in tests/lsp/edits (a type error, a syntax error, three syntax
errors, an unformatted line, a `.` with nothing after it, an unsaved edit
of the other file of the package). The fuzz sessions type each file of the project in, three
bytes at a time, and ask for everything at each step; every request must
be answered, and nothing may crash.

Not supported: incremental document sync is accepted but not asked for,
and there is no rename, find references, signature help, workspace
symbols, code actions or semantic tokens. Diagnostics are published for
open documents only, and an edit to a closed file on disk is seen at the
next check.

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
  retains every counted field, unless the local is borrowed (it is never
  changed and its place is not changed while it is in scope; see "Memory
  management"): `Expr x = g.c.ast.exprs[e]` followed by `g.emit(...)` is
  free, because `emit` does not change `g.c`.
- Build long strings as `[]string` and `join("")`, or append to a local
  (`s = s + x` appends in place when `s` is not shared); `+` on a growing
  string stored elsewhere is quadratic.
- Calling a mut method on a struct kills narrowing of everything under it, so
  copy a narrowed field into a local before calling mut methods.

## Adding a language feature: teach, snapshot, use

The compiler's source may use the whole language, but only what the
compiler in `bootstrap/seed.c` already understands, because that compiler
builds stage 1. A new feature therefore takes three steps:

1. **Teach.** Implement the feature in the compiler (checker and generator,
   plus tests in tests/full) *without using it* in the compiler's own
   source. `make` builds it with the old snapshot (stage 1), then with
   itself; `make verify` must reach the fixed point, and `make test`,
   `make test-full`, `make test-lib` must pass.
2. **Release.** `make release` writes the new `stage3.c` to
   `bootstrap/seed.c` (and its inputs to `bootstrap/inputs.sha256`). Commit
   the snapshot together with the change that taught the feature.
3. **Use.** Now the compiler's source may use the feature. `make verify`
   rebuilds from the new snapshot; `make release` again when the compiler
   should be built from the newest version.

A change that is not used by the compiler's own source (a fix, a new check,
a library function) needs only step 1; `make release` then just refreshes
the snapshot. For a new check, fix the test programs that fail it in the
same change. If a compiler bug is in the snapshot itself, fix it in the
source, build (the fixed compiler compiles itself in stage 2), verify and
release. Never edit `bootstrap/seed.c` by hand; if it cannot build the
current source, check out an older source that it can build, and walk
forward one release at a time.

## Decisions

- **The runtime of `runtime/`, pasted into the output.** `seed c` output is
  one self-contained C file (the tests and the bootstrap snapshot rely on
  it). Crash messages and exit codes are the same as seed0's were.
- **`seed build`/`run`/`test` link a cached runtime.** Compiling the
  runtime (about 4,000 lines) took most of the time of building a small
  program, so these commands compile it once into
  `$SEED_ROOT/build/cache/runtime-<key>.o` (or `$TMPDIR/seed-cache-$USER/`
  when the root cannot be written), where the key is a hash of the
  runtime's pasted text, `cCompiler` and `cFlags`, and paste only `seed.h`
  above the program (`compileProgramC`, `runtimeObject`). The object is
  written under a random name and renamed, so concurrent compilers are
  safe. Old cache objects are not removed (`make clean` removes
  `build/`).
- **Packages are compiled into cached objects** (`gen_unit.sd`,
  `buildProgram`). The standard library is most of the C of most programs
  (`examples/server.sd`: 300 of 8,000 lines are its own), so `build`,
  `run` and `test` generate one C unit per package: its own type
  definitions, prototypes and declarations (only those its text refers to,
  found by scanning its `sd_` identifiers), natives, helpers, thunks,
  constants and functions. The object of every package but the root is
  kept in the cache as `pkg-<path>-<key>.o`, the key a hash of the unit's
  whole text (with `seed.h`), the C compiler and its flags, so a change
  anywhere it depends on (the package, a package it imports, the compiler,
  the runtime's header) gives a new key. A unit's text depends only on its
  package and its imports: declarations are sorted by name (by rank where
  one needs another first), helper and thunk numbering restarts per unit.
  Functions, constants, descriptors and vtables have external linkage in
  units (the runtime compares descriptors by address, so each is defined
  once, by its package); `seed c` still writes one self-contained file
  with everything static. Missing objects and the root unit are compiled
  in parallel (`group`/`spawn`), then linked. Without a writable cache the
  program is one file, as before.
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
- **Arenas plus parallel result lists**, with `ExprInfo.wrap`,
  `ExprInfo.narrowed` and `ExprInfo.ifaceFrom` instead of rewriting the tree.
- **Diagnostics**: up to 20, sorted, deduplicated by position, one error per
  failing expression (`tyInvalid` suppresses cascades). A declaration named
  like a built-in type (`fn float(...)`) is reported once; its uses are not
  reported again (`Checker.builtinNamed`), so `float(x)` does not also
  fail as a conversion. "did you mean x?"
  hints are written without backticks because the tests (from seed0's time) look for that
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
- **Imports are per file** (seed0 treated them as package-wide).
- **The pedantic checks and the formatting check are always on**; the
  `--strict` flag is gone. The seed0-era test programs were changed to pass
  them.
- **Float literals are emitted as written** (they are valid C and C rounds
  them correctly); seed0 printed them with `%a`.
- **`select` with `default`** is a syntax error with a hint (select always
  waits).
- **`seed run`/`seed build`/`seed test`** work in a new directory
  `$TMPDIR/seed-run-XXXXXX` (`seed-build-`, `seed-test-`; `fs.makeTemp`)
  per invocation, removed when they finish, so concurrent runs of programs
  with the same name do not collide. `seed test` builds each package in a
  numbered subdirectory, as `test`.
- **`seed ast`** exists only for debugging the parser.
- **Concurrency** (the language decisions are in DECISIONS.md, "Concurrency"):
  `group` is an expression of type `error`, used only whole (assigned,
  returned, discarded or with `try`); its block cannot `return`, `try`, or
  `break`/`continue` out of it; `spawn` takes the calls `defer` takes,
  returning `error` or nothing, never a mut method; select cases are send,
  receive, `_ =` receive or a receive into a new or existing `T?` variable; channel
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
- **`tests/full/NAME.leaks`** marks a test that is expected to leak, with
  the reason; there are none since the cleanup frames (`tests/leaks.sh`
  checks the crashing programs like the others).
- **Cleanup frames hold only what lives across a crash point** (see
  "Memory management"). Putting every counted slot into the frame cost the
  compiler compiling itself 13%; keeping only the fields that need it, and
  leaving out enums without data, brought that to 7%. The bench programs
  did not change measurably either way.
- **Small ints are found by a flow-insensitive interval analysis** over the
  whole function: simple, and enough for loop counters, lengths and
  constants. Lengths are bounded only by the small range (the runtime's own
  limit), so `i + 1` with `i < xs.len()` is not small, while `i < 1000` is.
- **Fast versions only for pure functions** (gen_fast.sd): giving up and
  computing again is only invisible when the function has no effects, and
  the fast version stays simple (words of small ints, no temporaries, no
  frames). Functions with lists, strings or calls of other functions keep
  one version.
- **Ownership analyses are syntactic** (gen_move.sd): move on last use
  looks at the statements of the declaring block, borrowed locals at the
  mutations in the local's scope; neither follows the control flow, so
  neither can be wrong about a path, and both are one walk per function.
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

- A crash or a stopped goroutine still leaks a big integer held by an `int`
  local (ints are not in cleanup frames), and what a native function holds
  in its C locals when it crashes; everything else the unwound frames held
  is released.
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
- A mut call evaluates its arguments before the receiver's place, so in
  `xs[f()].push(g())` `g` runs before `f`.
- A deadlock in one test ends the whole test program (deadlock detection is
  global); `seed test` then reports the package as failed with the exit
  code.
- Package identity is by directory path as written: the same directory
  reached through two different spellings would be loaded twice (cannot
  happen with the resolution rules unless the project root is itself
  inside `lib/`).
- A crash's trace has no frame for a Seed function that the C compiler
  inlined into its caller: the crash's own site is shown, the line in the
  caller where the inlined call was is not (`seed run -g` compiles without
  inlining). The column of a line that crashes in a conversion or another
  runtime call whose value is used later in the line is the statement's,
  not the call's.
- The parser reports one syntax error per statement (or member of a type);
  an error in an expression that spans lines may hide a second error on
  the next line. A function written with its `{` on the next line parses
  as a native function followed by a stray block, whose contents are not
  checked for errors.
