# Compiler issues found while writing lib/

Add an entry here when library code has to work around the compiler: a
minimal program that shows the problem, what the compiler says, what it
should do, and the workaround used (with the file it is in).

Open:

- **Two typed discards in one declaration are rejected** (found by the
  library QA pass):

  ```
  fn three() (string, int, error) { return "a", 1, null }
  ...
  string _, int _, error err = three()
  ```

  says `error: _ is declared twice` (at the second `_`). SPEC.md
  ("Variables and constants") says `_` discards a value, and the untyped
  form `_, _, error err = three()` compiles, so the typed one should too:
  `_` should never be entered as a name. Workaround: the untyped form (the
  QA programs used it; no library file is affected).

Fixed since the library was first written (each with tests):

- A `T?` declared beside an error is not guarded
  (tests/full/error_guard_nullable.sd).
- `pub` struct fields parse and are enforced across packages
  (tests/full/pkg_basic, tests/full/errors/pkg_private_field).
- A `T?` variable or field path is narrowed by assigning a `T` to it, also
  after an `if` whose every falling-through branch leaves it narrowed
  (`if (kind == null) { kind = "none" }`; tests/full/narrow_assign.sd).
- `os.crash(message)` lets library code crash with its own message; no
  package has its own `print` + `os.exit(2)` any more.
- A `case` with several labels may have patterns whose bindings are all `_`
  (`case Value.Null, Value.Bool(bool _):`), which json's accessors use
  (tests/full/enum_patterns.sd).
- Packages, natives and `test` blocks exist (lib/run_tests.sh is gone).
