# Performance

How fast Seed programs are compared with Go, where their time goes, what
the runtime and the compiler do about it. The numbers
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

Best of 5, in seconds. "Start" is the runtime at the start of the runtime
work, "runtime" after it (section "What was optimised"), "compiler" after
the compiler work of the last section (small ints, numbers written straight
into concatenations, and the cleanup frames, which cost nothing measurable
here).

| Benchmark | Seed start | Seed runtime | Seed compiler | Go | Seed / Go |
|---|---|---|---|---|---|
| `fib` | 0.39 | 0.39 | 0.39 | 0.33 | 1.2 |
| `intloop` | 0.75 | 0.75 | **0.26** | 0.31 | 0.8 (was 2.4) |
| `json` | 0.48 | 0.48 | 0.49 | 0.42 | 1.2 |
| `maps` | 0.49 | 0.50 | **0.41** | 0.54 | 0.8 |
| `pingpong` | 0.35 | **0.10** | 0.10 | 0.05 | 2.0 (was 7.0) |
| `sort` | 0.24 | 0.24 | 0.23 | 0.36 | 0.6 |
| `spawn` | 1.24 | **0.15** | 0.15 | 0.04 | 3.8 (was 31) |
| `strings` | 0.57 | 0.57 | **0.45** | 0.55 | 0.8 |

Sequential code is within 1.2x of Go; tight integer loops and string
building are now faster than Go's versions, as are maps and sorting. The
two concurrency benchmarks are where goroutines being threads shows.

Runtime micro-benchmarks (`runtime/tests/run.sh bench`):

| Operation | Before | After |
|---|---|---|
| spawn + join, one goroutine at a time | 15.4 us | 1.4 us |
| unbuffered ping-pong round trip | 3.3 us | 0.7 us |
| `chan int(0)` producer -> consumer | 1.5 us per value | 0.25 us per value |
| `chan int(1024)` producer -> consumer | 45 ns per value | 50 ns per value |

## Where the time goes

**`intloop`, `fib`: tagged integers.** An `int` is a tagged word that
becomes a big integer instead of overflowing, so each general operation
tests both tags and checks for overflow, and each temporary is released
with a tag test. In `intloop` the counter is bounded by a constant, so the
compiler proves `i`, `i * i`, `% 7` and `& 3` small and emits plain C on
`int64_t`; only `sum`, which grows without bound, stays general (one
inline addition per iteration). `fib`'s argument is a parameter, which the
analysis cannot bound, so `fib` still pays the tag tests; it is dominated
by calls (1.2x).

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
free what a crash leaked (runtime/README.md, section 17): it cost 18-80% on
allocation-heavy goroutines. The compiler's cleanup frames replaced it.

## What the compiler does

Three proposals of the runtime work were implemented in the compiler
(compiler/README.md, "Memory management"):

1. **Unboxed small ints** (compiler/gen_int.sd). An interval analysis of
   every int local of a function finds the ones that are always small:
   loop counters bounded by constants or lengths, lengths, `int(byte)`, and
   locals assigned only such values. Those are `int64_t` C locals, never
   retained or released; comparisons and `+ - * / % &` whose operands and
   result are small are plain C. In an int `+` chain the small operands are
   summed in C first. `intloop` went from 0.75 s to 0.26 s; `maps` and
   `strings` gained from their loop counters.
2. **Numbers written into concatenations.** `"item-" + i.string()` is one
   `sd_str_concat_parts`, which writes the digits of `i` into the result:
   one allocation and one copy fewer per number (`strings`: 0.57 s to
   0.45 s, with the unboxed counters).
3. **Cleanup frames**, so that a crash or a stopped goroutine releases what
   the unwound frames held (runtime/README.md, sections 9 and 17). Only
   values that live across a call that may crash are kept in a frame, and
   ints and data-less enums never are; no benchmark changed measurably. The
   compiler compiling itself, which copies many structs with strings into
   locals across calls, is about 7% slower (112 ms to 120 ms): a thread-local
   access per call of a function with a frame (about 1%), its values kept in
   memory across calls (about 3.5%), and zeroing and nulling its fields.

Not done: floats in concatenations (`f.string()` costs its formatting, not
its allocation), tighter bounds for lengths (`i + 1` with `i < xs.len()` is
not proven small, because a length is only known to be below 2^62), and
intervals for fields and parameters (`fib`).

What would need more than the compiler: goroutines in user space (an M:N
scheduler with growable stacks) to get `spawn` and `pingpong` to Go's
level. Threads are a deliberate simplification (runtime/README.md, decision
8); the worker pool and spinning recover most of the cost for the patterns
programs use (a goroutine per request, short hand-offs).

## Refcount elision

Measured on Linux x86_64 (4 cores, GCC 13), the machine shared with other
builds, so wall times are interleaved A/B runs (best of 7 to 15) and
reported as ratios; instruction counts come from callgrind and do not
depend on the load. compiler/README.md ("Memory management") has the rules.

1. **Parameters were already borrowed.** Parameters are read-only in Seed,
   so a callee never retains or releases one and a caller passes its
   values as they are; nothing to do there.
2. **Mut-call arguments are borrowed** when they read another variable
   than the receiver's (`c.error(pos, message)` no longer copies `pos` and
   `message`), and literals are never retained into a temporary.
3. **Move on last use.** A store of a local that is its last use, in a
   simple statement of the local's own block, moves the value: no retain,
   no release at the end of the block, and the list or map keeps a count of
   1, so changing the copy later changes it in place.
4. **Borrowed locals.** `Expr x = g.c.ast.exprs[e]` holds the element's bits
   without owning them when nothing changes `x` and nothing changes the
   place where `x` could see it while `x` is in scope; the mut methods
   called meanwhile are judged by their *mod sets* (the receiver fields
   they may change), so `g.emit(...)` does not stop it.

The compiler compiling itself (`seed2 c compiler/*.sd`):

| | Before | After |
|---|---|---|
| instructions | 2,240 M | 2,040 M (-8.9%) |
| wall time (interleaved, best of 15) | 1.00 | 0.90 |
| `sd_rt_` struct copies in its C (static) | 902 | 269 |
| `sd_str_retain` / `sd_list_retain` / `sd_int_retain` (static) | 851 / 197 / 1610 | 293 / 124 / 770 |

Of the gain, moves and borrowed arguments give about 5% (the baseline
compiler's source built by the new generator runs 2,124 M instructions)
and borrowed locals the rest; the analyses themselves cost about 1.5% of
the compile. The benchmarks do not change: `fib`, `intloop`, `pingpong`,
`spawn` and `strings` compile to the same C as before, and `json` (1.01),
`maps` (0.99) and `sort` (0.98) are within noise; their time goes to
allocation and to the runtime, not to copies of locals.

## Integer fast paths

On Linux x86_64 with GCC, `fib` was 2.9x Go and `intloop` 2.2x, against
1.2x and 0.8x on macOS arm64. The generated code is the same; the
difference is in what the C compiler makes of it. On x86 every general
int operation becomes a tag test, the operation with an overflow jump,
and, for each temporary, a release that tests the tag again (two compares
and two branches). GCC cannot remove those releases, because the slow path
(a big integer) also reaches them; clang on arm64 schedules the same
branches better. `fib`'s hot path was 45 instructions with five saved
registers; `intloop`'s loop 27. Three changes (compiler/README.md, "Int
fast paths"):

1. **In-place int updates.** `sum = sum + e`, `x = x - e`, `x++` and `x--`
   on an int that is not unboxed replace the word when both values are
   small and the add does not overflow; only the slow path makes a new
   value and releases the old one. `intloop`'s loop went from 27 to 22
   instructions; what is left against Go (17) is the tag test and overflow
   jump of the unbounded `sum` and the shift that boxes the small part.
2. **Fast versions of pure int functions.** A function of ints and bools
   with a body of int arithmetic, comparisons, `if`, `for` and calls of
   other such functions gets a version on the words of small ints
   (`sd_fs_fib`: add, compare, overflow jump, no tags, no releases), tried
   when every int argument is small; when a result leaves the small range
   or a division by zero would crash, it gives up and the general version
   computes the same result. `fib`'s fast version is about 25 instructions
   with two saved registers (GCC also drops the overflow checks of `n - 1`
   and `n - 2`, knowing `n >= 2` there).
3. **Enum comparisons by tag.** `x.kind == ExprKind.Ident` compared the two
   boxes with `sd_box_equal`, which walks the descriptor whenever the boxes
   differ; comparing with a case without data now compares the tag. This
   is not in the benchmarks (they compare no enums in their loops), but it
   was 20% of the compiler's own time.

Tried and not kept: unsigned `/` and `%` when the interval analysis knows
the dividend is not negative (GCC already proves it and emits the same
code). Not done: intervals for parameters and fields (a parameter can hold
any int; bounding it needs every call site, which exported functions and
function values do not have; the fast versions check the arguments at run
time instead), and unboxing an unbounded accumulator like `sum` (it needs
a second copy of the loop to continue in when it overflows).

Interleaved runs (best of 9) against the code before these changes, and Go
on the same machine:

| Benchmark | Before | After | After / before | Go | After / Go |
|---|---|---|---|---|---|
| `fib` | 1.66 | 0.63 | 0.38 | 0.59 | 1.1 (was 2.9) |
| `intloop` | 0.86 | 0.69 | 0.80 | 0.41 | 1.7 (was 2.2) |
| `json` | 1.52 | 1.47 | 0.97 | 1.07 | 1.4 |

The other benchmarks run the same instructions as before (callgrind:
`strings` 9,512 M before and after; `json` 9,076 M to 8,902 M); their
wall times moved by up to 8% either way between identical binaries on this
shared machine.

The compiler compiling itself, after the refcount work and these changes:

| | Start | Refcount elision | Integer fast paths |
|---|---|---|---|
| instructions | 2,240 M | 2,040 M | 1,544 M |
| wall time (interleaved, best of 11) | 1.00 | 0.95 | 0.77 |

## Build speed

Building a program was almost all C compiler: the front end and generator
take 20 to 70 ms for the examples, `cc -O2` 0.4 to 4 s, and most of the C
is the standard library (`examples/server.sd`: 300 of 8,000 lines are its
own; `http`, `json`, `time` and `encoding` are the rest). Two changes
(compiler/README.md, "Decisions"):

1. **`-O1` for `seed run` and `seed test`**, `-O2` for `seed build` (and
   for the runtime's cached object, compiled once). `-O1` takes 35 to 50%
   less time than `-O2` on the generated C.
2. **Cached package objects.** `build`, `run` and `test` generate one C
   unit per package and keep each imported package's object in the cache
   (`build/cache/pkg-<path>-<key>.o`, next to the runtime's), keyed by a
   hash of the unit's text, so only the root package is compiled when
   nothing it imports changed. Missing objects are compiled in parallel.

Wall time in ms (best of 3; "cold" removes the package objects before every
command, so it includes compiling them):

| Command | Before | After, cold | After, cached |
|---|---|---|---|
| `seed run primes.sd` | 380 | 223 | 177 |
| `seed run bigmath.sd` | 949 | 449 | 442 |
| `seed run life.sd` | 552 | 288 | 285 |
| `seed run values.sd` | 461 | 296 | 297 |
| `seed run jsonfmt.sd` | 2,100 | 1,116 | 246 |
| `seed run bank` | 1,528 | 562 | 311 |
| `seed build server.sd` | 3,431 | 2,252 | 369 |
| `seed build httpapi` | 3,367 | 2,198 | 173 |
| `seed test examples/kvstore` | 2,424 | 1,034 | 616 |
| `seed test lib` (16 packages) | 27,171 | 16,530 | 16,439 |

(`seed run workers.sd`, about 2.5 s either way, is the program's own run
time.) `seed test lib` gains little from the cache: each package under test
is compiled with its tests, so only its imports come from the cache, and
most of the time is the test programs running. The compiler's own build
(`make`) uses `seed c` and the Makefile's `-O2`, unchanged.
