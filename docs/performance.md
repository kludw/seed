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
and maps (the queue that makes freeing non-recursive). The runtime's own
allocator has since replaced `malloc` for these objects (section
"Allocator").

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

## Allocator

Counted objects (strings, lists, maps and their arrays, boxes, big
integers, channels) used to be `malloc` blocks. They now come from the
runtime's own small-object allocator: 24 size classes up to 2 KB,
per-thread free lists, and a central pool that the lists overflow to and
refill from in batches. Blocks freed on another thread join that thread's
list. runtime/README.md, section 15, has the design and the thread-local
rule that keeps it correct when goroutines move between threads.

**Where allocation time went.** Instruction counts (`valgrind
--tool=callgrind`, Linux x86_64, glibc 2.39) before the change: glibc's
`malloc`, `free` and their internals (`_int_malloc`, `_int_free`,
`malloc_consolidate`, `unlink_chunk`) were 27% of the instructions of
`json`, 45% of `strings` and 19% of `maps`. After the change, `sd_mem_alloc`
plus `sd_mem_free` are 9%, 15% and 8%, and total instructions fell by 31%
(`json`), 45% (`strings`) and 18% (`maps`). The free fast path is about 15
instructions: header, thread pointer, list head, push, room counter, with
no lock and no atomic operation.

**Results.** These are from a different machine from the table above:
Linux x86_64, 4 cores, GCC 13, `-O2`, shared with other builds while it
ran. "Before" and "after" were run alternately in the same window, best of
9 each. The ratio is the stable number; the absolute times moved by up to
50% between windows for the thread-heavy benchmarks.

| Benchmark | Before (s) | After (s) | After / before | Go (s) |
|---|---|---|---|---|
| `fib` | 1.70 | 1.76 | 1.04 | 0.58 |
| `intloop` | 0.83 | 0.85 | 1.03 | 0.42 |
| `json` | 1.43 | 1.07 | **0.75** | 0.93 |
| `maps` | 1.20 | 1.10 | **0.92** | 1.81 |
| `pingpong` | 0.56 | 0.48 | **0.86** | 0.12 |
| `sort` | 0.55 | 0.50 | **0.91** | 0.91 |
| `spawn` | 1.02 | 0.98 | 0.96 | 0.08 |
| `strings` | 1.36 | 0.69 | **0.51** | 2.00 |

In three separate windows, the ratios were: `strings` 0.48-0.55, `json`
0.74-0.75, `maps` 0.92-0.96, `sort` 0.88-0.99, `pingpong` 0.79-0.97.
`fib`, `intloop` and `spawn` allocate (almost) no counted objects, so they
measure noise. `fib` executes the same 14.4 billion instructions before
and after; its 2-5% is where the code lands in the binary. The compiler
compiling itself (a whole-program build of the runtime) went from 394 ms to
374 ms, with peak resident memory 26.4 MB before and 27.2 MB after.

What made the difference:

- **No `malloc` bookkeeping for small blocks.** glibc's free path merges
  neighbouring chunks and its allocation path sorts bins. A size class's
  free list is a stack.
- **Strings and lists use their whole slot as capacity.** A 30-byte string
  sits in a 48-byte slot with room to grow. `strings` appends in place more
  often, and a short list grows without moving.
- **Maps' entry and index arrays come from the allocator too.** These are
  three blocks per small map instead of three `malloc` calls.
- **Boxes, lists and maps whose elements hold nothing counted skip the free
  queue** (the thread-local queue that makes freeing non-recursive), so
  freeing them reads no thread-local state besides the allocator's.

Not done:

- **Short-string optimisation.** The string representation is not opaque.
  `sd_str` is a pointer that generated code compares with `NULL`, literals
  are static structs built by `SD_STR_LIT` in generated code, and natives
  read `s->data` directly. Inlining short strings would change the C type
  of every string in generated code. runtime/README.md, decision 23, has
  the details.
- **Returning memory to the system.** Freed slots are reused, so memory
  stays flat at the peak of live objects, but chunks are never unmapped.
