# 13. Seed for Go programmers

Seed started from Go and kept most of its spirit: a small language, fast to learn, one toolchain binary, a big standard library, goroutines and channels, errors as values, `gofmt`-style formatting. It then removed the parts that, in the designers' experience, cause the most bugs or the most arguments. The guiding principles:

1. **One way to do things.** No syntax sugar, except `try` for passing errors up.
2. **Nothing implicit.** No hidden control flow, no `init()`, no implicit conversions, no code that runs behind your back.
3. **Everything you need is built in.** A large standard library and a single binary, with no third-party dependencies.
4. **The compiler is a strict friend.** Errors only, no warnings, and every error explains itself and suggests a fix.

## Side by side

| Go | Seed | Why |
|---|---|---|
| `var n int`, `n := 0`, `var n = 0` | `int n = 0` | One form. The type is always visible, and every variable starts with a value. |
| Zero values | No zero values: every declaration and struct literal is complete | A forgotten field is a compile error, not a silent `0` or `""`. |
| `nil` for pointers, maps, slices, interfaces, channels, funcs | `T?` and `null`, only where the type says so (and `error`, which the compiler checks like a `T?`) | "Might be missing" is part of the type, and the compiler makes you check it. |
| `*T`, `&x`, pointer and value receivers | No pointers. Values only; `mut` methods change the caller's variable | No aliasing, no nil dereference, no data races, no `*T` versus `T` decision. |
| Slices share their backing array; `append` may or may not copy | `[]T` is a value; copy-on-write makes copies cheap | A list you passed somewhere never changes under you. |
| `append(xs, x)` | `xs.push(x)` | One way to add to a list. |
| `int`, `int8` ... `uint64`, overflow wraps | One `int` that grows and never overflows | No choosing, converting or overflow bugs. |
| Untyped constants mix freely; `1e9` is a float | No implicit conversions at all; float literals contain a `.` | `float(n) * 1.5` says exactly what happens. |
| `x += 1`, `x++` as statements | `x = x + 1`, `x++` | One way to assign; `++` stays because loops need it. |
| `for`, `for cond`, `for range` | Only `for (init; cond; post)` | One loop. Iterate by index or over `m.keys()`. |
| Map iteration order is random | Maps keep insertion order | Programs are deterministic. |
| `switch` with optional `default`, `fallthrough` | Enum switches are exhaustive with no `default`; value switches need `default`; no fallthrough | Adding an enum case points you at every switch to update. |
| `const ( A = iota ... )` | `enum`, whose cases can carry data | Real sum types, checked by the compiler. |
| Implicit interface satisfaction | `type T struct implements I` | The intent is written down and checked where the type is defined. |
| Type switches, type assertions | None | Use an enum when the set of kinds matters. |
| Generics | Only the built-in generic types (`[]T`, `map[K]V`, `chan T`, `T?`) | Keeps the language small. |
| Closures and func literals | Named functions as values only; no closures | A function value never carries hidden state. |
| `if err != nil { return ..., err }` | Same, or `try f()` | Passing an error up is the common case, so it gets the one piece of sugar. |
| `f()` silently drops a returned error; `v, _ :=` | Every error must be handled; `_` is explicit, and the value becomes `T?` | Errors can't be lost by accident. |
| `panic`, `recover` | Crashes (exit code 2, or the group's error); no recover | Bugs crash; expected failures are errors. |
| `fmt.Printf("%d items", n)` | `fmt.println(n.string() + " items")` (`fmt.fixed(f, 2)` for decimals) | No format-string language to learn or get wrong. |
| `m[k]++` on a missing key | `m[k] = m.getOr(k, 0) + 1` | A missing key has no hidden zero value; the default is written where it is used. |
| `String()` on an `iota` type | Every enum has `name()`: `Direction.North.name()` is `"North"` | Printing a case needs no hand-written switch. |
| Capitalised names are exported | `pub` keyword; naming style is fixed by the compiler | Visibility doesn't fight with naming rules, and making a name public doesn't rename it. |
| Struct fields exported by capital letter | Fields are private unless `pub` | A package can protect its invariants. |
| Package-level `var`, `init()` | Only `const` at package level; nothing runs on import | No global mutable state, no hidden start-up code. |
| `go f()` anywhere | `spawn f()` only inside a `group` | Structured concurrency: goroutines can't leak. |
| Shared memory, `sync.Mutex`, `WaitGroup`, `errgroup`, `context` | Goroutines share nothing; `group` waits and returns the first error, stopping the others | No data races; failures propagate like in Erlang. |
| `select` with `default` | `select` always waits; timeouts are channels | One way to wait. |
| `_test.go` files, `func TestX(t *testing.T)` | `<package>/tests/`, `test "description" { ... }`, `assert` | Tests stay out of the package's code, and nothing depends on naming conventions. |
| `go vet`, linters, warnings | One strict compiler: unused names, shadowing, unreachable code, naming | Everything worth flagging is an error, so nothing gets ignored. |
| `gofmt` is a separate step | Formatting is part of the language | Unformatted code doesn't compile. |
| Garbage collector | Reference counting, no cycles possible | Memory is freed at once, with no pauses. |

## Things that look the same but aren't

- **Declarations put the type first**, also for parameters (`fn add(int a, int b) int`) and switch bindings (`case Shape.Circle(float r):`).
- **Conditions need parentheses**: `if (x > 0) {`. And `if` has no init statement.
- **Integer division by zero crashes**, but float division by zero gives `inf` or `nan`, as IEEE says.
- **`s[i]` is a `byte`**, and `byte.string()` gives its number (`'a'.string()` is `"97"`). Use `string([]byte{b})` or `s.slice(i, i + 1)` to get text.
- **A map lookup returns `V?`**, not a zero value with a `bool`. Store it in a variable and check it, compare it with a value (`m[k] == 3` is false when `k` is missing), or use `m.getOr(k, default)`.
- **`ch.receive()` returns `T?`**; `null` means the channel is closed and empty.
- **`main` returns an `error`** and receives the arguments: `fn main([]string args) error`.
- **Function parameters can be reassigned but not changed in place**: `args = args.slice(1, args.len())` is fine (only the function's copy changes), while `xs[0] = 1` or `xs.push(1)` on a parameter is an error, because in Go it would change the caller's slice. Copy into a local to change one.
- **An `error` must be checked before `err.message()`**, like a `T?`: the compiler rejects `message()` on an error that may be null.
- **`defer` with an error result** needs `defer _ = f()`.

## What you give up, and what you get

You give up some convenience: a few more characters for declarations, no closures for quick callbacks, no type switches, and a little more ceremony around maps and nullable values. In return, a Seed program reads the same no matter who wrote it, a function call can't change your variables behind your back, nothing is `nil` unless its type says so, no error is silently dropped, goroutines can't race or leak, and the compiler tells you about most mistakes before you run anything.

Next: [Crashes and debugging](14-crashes-and-debugging.md).
