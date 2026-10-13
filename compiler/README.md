# The Seed compiler

The real Seed compiler, written in Seed. It is compiled by the bootstrap
compiler `seed0` first, and then compiles itself:

```
seed0 compiler/*.sd          -> build/stage1.c -> build/seed1
seed1 c --strict compiler/*.sd -> build/stage2.c -> build/seed2
seed2 c --strict compiler/*.sd -> build/stage3.c      (identical to stage2.c)
```

```
make verify     # builds all stages and checks stage2.c == stage3.c
make test       # tests/lang and tests/seed0 with build/seed2
make test-strict  # tests/lang with build/seed2 c --strict
make test-fmt   # seed fmt: self-checks on every .sd file, tests/fmt goldens
make test0      # the same suites with seed0
```

Because stage 1 is built by seed0, the compiler source may only use the
seed0 subset (docs/seed0.md) until a snapshot replaces seed0 (see "Adding a
language feature").

## Command line

```
seed c [--strict] <file.sd>...       C program on stdout (what tests/run.sh uses)
seed build -o <out> <file.sd>...     compile to an executable
seed run <file.sd>... [-- args...]   compile into $TMPDIR and run; passes the exit code
seed ast <file.sd>...                print the syntax tree (debugging aid)
seed fmt [--check] <file.sd>...      format in place, or report unformatted files
```

All files given together form one package. Compile errors go to stderr and
exit with code 1; usage errors exit with 2.

The generated C contains the runtime `compiler/runtime0.c` (a copy of
`seed0/runtime.c`). The compiler finds it through `$SEED_ROOT`, or else
relative to its own path: the binary is `<root>/build/seedN`, so the root is
the parent of the binary's directory. The C compiler and its flags for
`build`/`run` are the constants `cCompiler`, `cFlags` and `cLibs` at the top
of `main.sd` (the Makefile has the same flags in `SEED_CFLAGS`).

## Files

| File | Phase | Contents |
|---|---|---|
| `main.sd` | driver | CLI, reading files, runtime location, invoking `cc` |
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
| `check.sd` | check | `Checker`, program passes, type resolution, scopes, narrowing |
| `check_expr.sd` | check | expressions, places (lvalues), T/T? conversions |
| `check_call.sd` | check | calls: functions, methods, built-ins, core packages, conversions |
| `check_stmt.sd` | check | statements, switch, assignment, "always exits" analysis |
| `check_style.sd` | check | `--strict` checks: unused names/results, unreachable code, naming |
| `gen.sd` | gen | `Gen`, program layout, C types, per-type helpers, type definitions |
| `gen_expr.sd` | gen | expressions, places, built-in calls |
| `gen_stmt.sd` | gen | statements |
| `runtime0.c` | — | runtime pasted into every generated program |

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
`imports`, `tops`, `files`, `comments`. A node refers to another by its `int`
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

`ExprInfo` holds:

- `ty`: the type the parent sees.
- `res` (an enum) plus `ref`/`sub`/`builtin`/`pkgFn`: what the expression
  resolved to — a local (`ref` = variable), constant, function, method,
  built-in method (`builtin`), core package function (`pkgFn`), conversion,
  enum constructor or value (`ref` = decl, `sub` = case), struct field
  (`sub` = field), list/string/map index.
- `wrap`/`inner`: the expression produces `inner` (a `T`) and is converted
  to `ty` (a `T?`). seed0 inserted a wrap node instead; with arenas a flag is
  simpler.
- `narrowed`/`orig`: the expression is stored as `orig` (a `T?`) and read as
  `ty` (a `T`).

Variables are an arena `vars []Var {name, ty, kind, pos, used}`. The scope is
a stack of variable indices with marks for nested blocks.

Package-level names are in `globals` (name → index into `globalList`, which
records kind and declaration index). Methods are in `methods`, keyed by
`"Type.name"`.

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

1. Collect package-level names; check that all files share one package;
   check imports (per file: a package must be imported in the file that
   uses it).
2. Create a type for every struct and enum, then resolve fields and enum
   case fields, and report structs that contain themselves by value.
3. Resolve every function signature and register methods.
4. Check `main`'s signature, then constants (lazily, in dependency order),
   then every function body.

Expression checking mirrors seed0: there are no implicit conversions except
`T → T?` (`coerce` sets `wrap`), `null → T?/error`, and reading a narrowed
`T?` as `T`. Narrowing keeps a stack of narrowed *paths* (`"v12.email"` =
variable 12, field `email`), added by conditions (`!=`/`==` null through
`&&`, `||`, `!`) and by an `if` whose branch always exits, killed by
assignment to the path or a prefix, and by a mut method call on a prefix.
Before a loop, every path the loop assigns or mutates is un-narrowed.
Whether a statement always exits is a pure function over the checked tree
(`stmtExits`), used for "missing return", unreachable code and narrowing
after early exits.

`--strict` adds the pedantic checks of the full language that seed0 lacks
(check_style.sd): unused variables, imports and call results, unreachable
code, locals that shadow package-level names, and the naming rules
(camelCase/PascalCase, with a suggested spelling). They are off by default
because some seed0-era test programs fail them; the compiler itself is always
built with `--strict`.

Features that parse but are not implemented are rejected in checking with
"X is not supported by seed0, and this compiler does not implement it yet".

**C generation** produces exactly the representation seed0 uses (see
seed0/README.md), so generated programs behave identically:

| Seed | C |
|---|---|
| `int`, `float`, `bool`, `byte` | `int64_t` (checked `sd_add`/... crash on overflow), `double`, `bool`, `uint8_t` |
| `string`, `error` | `sd_str` by value, `sd_err*` |
| `[]T`, `map[K]V` | `sd_list*`, `sd_map*` (counted, copy-on-write) |
| struct, enum | `sd_S_Name` by value, `sd_E_Name*` (immutable heap object) |
| `int?` etc., `Struct?`, other `T?` | `sd_O_i`..., `sd_B_Name*` box, the same pointer/`sd_str` with NULL |
| several results | `sd_T_<mangle>` with fields `r0, r1, ...` |
| mut receiver | pointer to the caller's place |

Names are prefixed (`l_x`, `f_x`, `sd_f_name`, `sd_m_Type__name`,
`sd_c_name()`, `_tN`). Expressions become C expressions, with GNU statement
expressions for temporaries; `genList` evaluates operands left to right
through temporaries when more than one has side effects. `gowned` retains a
value copied out of a place that still holds it; `gplace` builds a pointer to
a place, making every container on the way unique; `gstore` releases the old
value. Per-type helpers (`sd_eq_`, `sd_eqp_`, `sd_rt_`, `sd_rtp_`, `sd_rl_`)
are requested on demand through a queue (`Gen.helper`) and generated at the
end. The output is: runtime, type definitions, tuple typedefs, prototypes,
helpers, constants (lazily initialised functions), functions, `main`.

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
`compiler/` and `tests/` that parses, and checks that formatting the
formatted text changes nothing, that the tokens are unchanged (ignoring
newlines and commas before closing brackets), and that the comments and the
syntax tree are unchanged. `tests/fmt/run.sh` runs the golden tests
(`NAME.sd` must format to `NAME.expected`, which must be a fixed point;
`--update` rewrites the expected files). Finally, `seed fmt --check` checks
that the compiler and the test programs are formatted. The few test files
with deliberate syntax errors, and `tests/full/errors/unformatted_code.sd`,
are not formatted.

## Writing the compiler in Seed

Value semantics shape the code:

- All compiler state lives in a few structs (`Parser`, `Checker`, `Gen`) whose
  methods are `mut`, so the state is passed by pointer and lists are changed
  in place.
- Never copy an arena list into a local (`[]Expr xs = c.ast.exprs`): it
  raises the list's count, and the next change to the arena copies all of it.
  Read elements through the owner (`c.ast.exprs[e]`).
- Node structs contain no lists, so copying a node out of an arena is free.
- Build long strings as `[]string` and `join("")`; repeated `+` on a growing
  string is quadratic.
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
teaching may need a `--strict`-style switch until all test programs pass it.

## Decisions

- **Same runtime and representation as seed0.** The generator is a careful
  port of seed0's, so semantics (copy-on-write, mut paths, narrowing, crash
  messages and exit codes) match by construction. `runtime0.c` is a copy of
  `seed0/runtime.c`; a later phase switches to `runtime/`.
- **Arenas plus parallel result lists**, with `ExprInfo.wrap` and
  `ExprInfo.narrowed` flags instead of rewriting the tree.
- **Diagnostics**: up to 20, sorted, deduplicated by position, one error per
  failing expression (`tyInvalid` suppresses cascades). "did you mean x?"
  hints are written without backticks because the seed0 tests look for that
  exact text. Unsupported features say "not supported by seed0" for the same
  reason.
- **"missing return"** is reported at the closing brace of the function.
- **Imports are per file.** seed0 treats them as package-wide.
- **`--strict` is opt-in** (see above); the bootstrap stages use it.
- **Float literals are emitted as written** (they are valid C and C rounds
  them correctly); seed0 printed them with `%a`.
- **`select` with `default`** is a syntax error with a hint (select always
  waits).
- **`seed run`/`seed build`** put the C file and executable in
  `$TMPDIR/seed-run-<name>` / `$TMPDIR/seed-build-<name>`.
- **`seed ast`** exists only for debugging the parser.
- **`seed fmt` prints tokens, not the tree** (see "The formatter"). Style
  decisions not fixed by the task are listed in docs/formatting.md: no line
  length limit, `{}` for empty blocks, trailing comments not aligned,
  one-line `const`s may stay grouped, comment indentation before `case`
  follows the source column. `--check` reports to stderr.

## Known limitations

- Everything outside the seed0 subset parses but is rejected by the checker:
  interfaces and `implements`, function types and function values (and
  `sortBy`), `chan`/`select`/`group`/`spawn`, `defer`, `test`/`assert`, user
  packages and qualified names, `s.chars()`, integers wider than 64 bits.
- `int` is 64-bit and memory is never freed (seed0's runtime).
- Formatting is not checked during compilation yet; `seed fmt --check` does
  the check on its own.
- Two concurrent `seed run` of files with the same name share a temporary
  directory.
- The parser reports at most one syntax error per statement; an error in an
  expression that spans lines may hide a second error on the next line.
