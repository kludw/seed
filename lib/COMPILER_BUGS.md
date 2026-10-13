# Compiler issues found while writing lib/

Found with `build/seed2` at `seed-v2` (7a63a1d). None of them blocked the
library; the workaround is given for each.

**Status:** both are fixed (with packages, natives and tests): a `T?` next
to an error is not guarded any more (tests/full/error_guard_nullable.sd),
and `pub` struct fields parse and are enforced across packages
(tests/full/pkg_basic, tests/full/errors/pkg_private_field). Of the limits
at the end, `os.crash` now reports a caller's mistake, a `case` with
several labels may have patterns whose bindings are all `_`, and packages,
natives and `test` blocks exist (lib/run_tests.sh is gone).

## 1. Error guard on a variable that is already nullable (`--strict`)

A value declared as `T?` next to an error is still guarded until the error
is checked, so even comparing it with `null` is rejected. The guard exists to
stop a `T` from being read while it may be null; a `T?` can always be read
safely.

```
package main

import "fmt"

fn f() (string, error) {
    return "x", null
}

fn main([]string args) error {
    string? s, error err = f()
    if (s == null) {              // error: s is read before err is checked
        fmt.println("null")
    }
    if (err != null) {
        return err
    }
    return null
}
```

`build/seed2 c --strict` reports `s is read before err is checked; when err
is not null, s is null`; without `--strict` it compiles. Expected: no error,
because `s` is declared `string?`.

Workaround: discard the value with `_, error err = f()` when only the error
matters (lib/encoding/tests, lib/json/tests).

## 2. `pub` on struct fields is not parsed

SPEC.md ("Structs, enums, interfaces") and DECISIONS.md 20 say struct fields
are private unless marked `pub` (`pub string name`), and docs/stdlib.md
relies on it (`http.Request`, `http.Response` have public fields;
`rand.Source`, handles have private ones).

```
package main

type Req struct {
    pub string method             // error: expected a type, found 'pub'
}

fn main([]string args) error {
    return null
}
```

Workaround: lib/http/http.sd declares the fields without `pub`, with a
comment saying every field of `Request` and `Response` is meant to be `pub`.
Until field privacy exists, nothing would stop another package from reading
or setting `rand.Source.state`; that cannot happen yet, because user
packages are not implemented either.

## Not bugs, but limits the library had to work around

These are documented in lib/README.md:

- There is no way for library code to crash with its own message
  (docs/stdlib.md: `crash: rand.range: lo (5) must be less than hi (5)`).
  Each package has a private `<pkg>Crash(message)` that prints
  `crash: <message>` to stderr and calls `os.exit(2)`; inside a `group` this
  ends the whole process instead of failing the group.
- A `case` label with bindings must be the only label of its case
  (`case Value.Null, Value.Bool(bool _):` is rejected), and enum switches
  cannot have `default`, so json's accessors list every case.
- User packages, non-core imports, native (bodiless) functions and `test`
  blocks are not implemented yet (lib/run_tests.sh works around them).
