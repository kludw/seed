# Performance

How fast Seed programs are compared with Go, where their time goes, what
the runtime does about it, and what would need the compiler. The numbers
are from an Apple M-series laptop (10 cores, macOS, Apple clang `-O2`, Go
1.27); the benchmarks are in [bench/](../bench/).

## The benchmarks

`bench/run.sh` builds every program in `bench/seed/` with `seed build`
(C at `-O2`) and its counterpart in `bench/go/` with `go build`, runs each
three times (`RUNS=5` for five), and prints the best wall-clock time of
each. Each pair prints the same checksum, which the script compares.

| Benchmark | What it does |
|---|---|
| `intloop` | 300 million iterations of `sum = sum + (i * i) % 7 + (i & 3)` |
| `fib` | recursive `fib(40)`: 330 million calls |
| `strings` | 60 rounds of: 100,000 `"item-" + i.string()` pushed and joined, split again, appended one by one into a string, `contains` on each |
| `sort` | 2 million pseudo-random ints pushed and sorted; 300,000 structs sorted by key with `sortBy` (stable) |
| `maps` | 1 million string keys inserted, looked up twice, half deleted; 1 million `map[int]int` updates |
| `json` | a 1 MB document (6,000 objects) built from `json.Value`s, then printed and parsed back 30 times |
| `pingpong` | 100,000 round trips over two unbuffered channels, then 1 million ints through a `chan int(1024)` |
| `spawn` | 100,000 goroutines in groups of 1,000, each sending one int on a buffered channel |

The Go programs do the same work the idiomatic way (`strconv`, `sort.Ints`,
`sort.SliceStable`, `encoding/json` into `any`, `sync.WaitGroup`). One
difference: Go's `strings.go` uses a `strings.Builder` where Seed writes
`acc = acc + s`, because `+` in a loop is quadratic in Go, while Seed
appends in place to a string it alone holds.

## Results

Best of 5, in seconds. "Before" is the runtime at the start of this work,
"after" the current one.

| Benchmark | Seed before | Seed after | Go | Seed / Go |
|---|---|---|---|---|
| `fib` | 0.39 | 0.39 | 0.33 | 1.2 |
| `intloop` | 0.75 | 0.75 | 0.31 | 2.4 |
| `json` | 0.48 | 0.48 | 0.41 | 1.2 |
| `maps` | 0.49 | 0.50 | 0.54 | 0.9 |
| `pingpong` | 0.35 | **0.10** | 0.05 | 2.0 (was 7.0) |
| `sort` | 0.24 | 0.24 | 0.36 | 0.7 |
| `spawn` | 1.24 | **0.15** | 0.04 | 3.8 (was 31) |
| `strings` | 0.57 | 0.57 | 0.58 | 1.0 |

Sequential code is within 1.2x of Go, except tight integer arithmetic
(2.4x). Maps and sorting are faster than Go's generic versions. The two
concurrency benchmarks were the outliers and are where the runtime changed.

Runtime micro-benchmarks (`runtime/tests/run.sh bench`):

| Operation | Before | After |
|---|---|---|
| spawn + join, one goroutine at a time | 15.4 us | 1.4 us |
| unbuffered ping-pong round trip | 3.3 us | 0.7 us |
| `chan int(0)` producer -> consumer | 1.5 us per value | 0.25 us per value |
| `chan int(1024)` producer -> consumer | 45 ns per value | 50 ns per value |

## Where the time goes

**`intloop`, `fib`: tagged integers.** An `int` is a tagged word that
becomes a big integer instead of overflowing. The generated C is tight (no
calls in the loop: the small-int fast paths of `sd_int_add`, `sd_int_mul`,
`sd_int_mod` are inline, `% 7` becomes a multiplication by a reciprocal),
but each operation tests both tags and checks for overflow
(`__builtin_mul_overflow`, `smulh` on arm64), and each temporary is
released with a tag test. Go does one instruction per operation. `fib` is
dominated by calls, so it is only 1.2x.

**`json`, `strings`, `maps`: allocation.** A profile of `json` (`sample`,
top of stack) puts over a third of the time in `malloc`/`free` and the
runtime's free paths, an eighth in `memmove` (string slices, joins and
list growth), 7% in float formatting and parsing (`snprintf`/`strtod` for
non-integral numbers), and the rest in the parser and printer and in map
hashing. Go's `encoding/json` with `any` allocates as much. About 3% goes
to thread-local lookups (`tlv_get_addr`) in the free path of boxes, lists
and maps (the queue that makes freeing non-recursive).

**`pingpong`: waking threads.** Goroutines are OS threads, so a goroutine
blocked on a channel sleeps on a condition variable, and waking it costs a
system call to signal and the scheduler's latency to resume it, several
microseconds each on macOS. An unbuffered round trip needs two such
hand-offs. Go switches goroutines in user space in about 100-200 ns.

**`spawn`: starting threads.** Each `spawn` created a thread
(`pthread_create` with an 8 MB stack), and the group joined it
(`pthread_join`): about 12 us per goroutine. After that, contention: when
1,000 goroutines end at once they all take the group's lock, which a
blocked `pthread_mutex_lock` turns into system calls.

## What was optimised

All in `runtime/seed_task.c`, with no change in behaviour (the full test
suites, the soak and the sanitizers pass; see lib/BUGS_FIXED.md):

1. **Worker threads are reused.** A goroutine that returns leaves its
   thread waiting for the next `spawn`; `sd_spawn` hands the goroutine to
   an idle worker, and creates a thread only when none is idle. Idle
   workers end after 2 s (at most 1024 are kept); a worker whose goroutine
   crashed or was stopped ends at once, so no thread-local state from a
   crash is reused. Groups no longer join threads: a finished goroutine's
   last use of its task is under the group's lock, so the group frees the
   task without waiting. (`rand`'s per-goroutine generator now forgets its
   seed when the goroutine ends, through `sd_goroutine_atexit`, so that the
   next goroutine on the thread seeds its own.)
2. **Spinning before sleeping.** A goroutine about to park watches its
   wake flag for up to 20 us first, and an idle worker its next task;
   a wake-up that arrives meanwhile costs no condition-variable signal and
   no thread wake-up. Spinning is skipped on one CPU, when more goroutines
   are running than there are CPUs, and when half the CPUs are spinning
   already, so it never competes with work.
3. **Fewer and shorter lock waits in `spawn`.** `sd_spawn` takes the
   group's lock once instead of twice (it collects the finished goroutines
   and registers the new one together). The group and pool locks are taken
   with a short `trylock` loop before blocking. (The same for channel locks
   made a busy buffered channel three times slower, so channels keep plain
   locks.) A new goroutine's `select` seed no longer reads the clock.

Together: `spawn` 1.24 s to 0.15 s, `pingpong` 0.35 s to 0.10 s.

What was tried and not kept: a per-goroutine registry of owned objects, to
free what a crash leaks (runtime/README.md, section 17): it cost 18-80% on
allocation-heavy goroutines.

## What would need the compiler

Proposals for the compiler team, in order of expected gain:

1. **Unboxed integer locals where the range is known.** For a loop
   `for (int i = A; i < B; i++)` whose body does not assign `i`, where `A`
   and `B` are small (constants, `.len()`, or ints the checker knows are
   small), keep `i` as an `int64_t` and emit `SD_INT(i)` only where it is
   used as an `int` value: the comparison and increment then cost one
   instruction each, and `xs[i]` can index with `i` directly (no tag test in
   `sd_int_index`). The same applies to results the generator knows are
   small: `x % c` and `x & c` for a small constant `c`, `.len()`,
   `int(byte)`. Releases of such values can be omitted entirely (today every
   temporary is released with a tag test). This is what separates
   `intloop` (2.4x) from Go.
2. **Cleanup frames for counted locals**, to free what a crash or a stopped
   goroutine leaks: the full design is in runtime/README.md, section 17.
   Not a speed-up, but the cost is small (a push and a pop per call of a
   function with string, list or map locals; nothing for scalar-only
   functions) and it removes the main known limitation.
3. **Fewer temporaries in string building.** `"user" + i.string() +
   "@example.com"` already becomes one `sd_str_concat_n` of three parts,
   but `i.string()` first makes a string of its own. A runtime entry point
   `sd_str_concat_n` that also takes ints (or a small "part" union: string,
   int, float) would let the generator write the digits straight into the
   result, saving an allocation and a copy per number in `strings` and
   `json`.

What would need more than the compiler: goroutines in user space (an M:N
scheduler with growable stacks) to get `spawn` and `pingpong` to Go's
level. Threads are a deliberate simplification (runtime/README.md, decision
8); the worker pool and spinning recover most of the cost for the patterns
programs use (a goroutine per request, short hand-offs).
