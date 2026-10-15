# Compiler issues found while writing lib/

Add an entry here when library code has to work around the compiler: a
minimal program that shows the problem, what the compiler says, what it
should do, and the workaround used (with the file it is in).

Open:

- Returning a list element of struct type as the function's nullable
  result generates C that does not compile: the element is read as if the
  list held boxes (`S?`), not `S` values.

  ```
  type F struct {
      string name
  }

  fn first([]F fs) F? {
      for (int i = 0; i < fs.len(); i++) {
          return fs[i]
      }
      return null
  }
  ```

  `cc` reports `incompatible type for argument 1 of 'sd_rt_S1F'`
  (`expected 'sd_S_F' but argument is of type 'sd_box *'`); the program
  should compile and return the element. Workaround: copy the element to a
  variable first (`F f = fs[i]` then `return f`), as `flags.Spec.find` and
  `findShort` do (lib/flags/flags.sd).

Fixed since the library was first written (each with tests):

- Two typed discards in one declaration (`string _, int _, error err =
  three()`) were rejected with `_ is declared twice`: `_` is never entered
  as a name now, and a typed `error _` discards the error like `_` (so the
  values beside it must be `T?`; tests/full/discard_typed.sd,
  tests/full/errors/discard_typed_error.sd).
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
