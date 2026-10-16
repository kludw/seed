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
| `bigint` | 3^2,000,000 by repeated squaring and fib(1,000,000) by fast doubling, each printed (954,243 and 208,988 digits), parsed back and divided |
| `regex` | a 1.5 MB log, 5 rounds of: `findAll` with groups (`\b(\w+)@(\w+)\.(com\|org)\b`, `(?m)^(\d{4})-(\d{2})-(\d{2})`, `[a-z]+ing\b`), `matches` on each of 20,000 lines (`(?i)error\|warn`), `replace` (`\s+`) and `split` (`[,;]\s*`). Measured on Linux x86-64: 0.58 s, Go 2.4 s (lib/README.md, "regex") |

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
two concurrency benchmarks are where goroutines being threads shows (they
were; with the M:N scheduler they are within about 1.1-1.4x of Go: section
"Goroutine scheduler (M:N)").

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

**`pingpong`: waking threads** (before the M:N scheduler). Goroutines were OS threads, so a goroutine
blocked on a channel sleeps on a condition variable, and waking it costs a
system call to signal and the scheduler's latency to resume it, several
microseconds each on macOS. An unbuffered round trip needs two such
hand-offs. Go switches goroutines in user space in about 100-200 ns.

**`spawn`: starting threads** (before the M:N scheduler). Each `spawn` created a thread
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
   data-less enums never are; no benchmark changed measurably. (Ints were
   left out too at first; they are in frames now, so that a crash does not
   leak a big integer: the compiler compiling itself executes 2.8% more
   instructions, the benchmarks less than 1% more, `spawn` 2.1%;
   compiler/README.md, "Decisions".) The
   compiler compiling itself, which copies many structs with strings into
   locals across calls, is about 7% slower (112 ms to 120 ms): a thread-local
   access per call of a function with a frame (about 1%), its values kept in
   memory across calls (about 3.5%), and zeroing and nulling its fields.

Not done: floats in concatenations (`f.string()` costs its formatting, not
its allocation), tighter bounds for lengths (`i + 1` with `i < xs.len()` is
not proven small, because a length is only known to be below 2^62), and
intervals for fields and parameters (`fib`).

What needed more than the compiler: goroutines in user space, to get
`spawn` and `pingpong` to Go's level. That is the M:N scheduler of the
last section.

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

## Goroutine scheduler (M:N)

Goroutines used to be OS threads (reused by later goroutines, with
spinning before sleeping). They are now scheduled M:N, as in Go: each
goroutine has a stack of its own (8 MB reserved, committed as it grows,
pooled) and is switched in user space (a hand-written register swap for
x86_64 and aarch64) onto one worker thread per CPU. Each processor has a
run queue; idle workers steal. Channels, selects and group waits park the
goroutine instead of a thread; descriptors and timers are watched by one
poller thread (epoll or kqueue). runtime/README.md, section 10, has the
design, including the rule for thread-local storage that a goroutine
moving between threads requires.

**Results.** Linux x86_64, 4 cores, GCC 13, `-O2`, on a machine shared
with other builds. "Before" is the runtime with the allocator and a thread
per goroutine (branch `seed-v2-xdbfs9`); "after" is this scheduler. The
three were run alternately, best of 7; the after/before ratio is the
stable number (the absolute times moved by 20-50% between windows).

| Benchmark | Before (s) | After (s) | After / before | Go (s) | After / Go |
|---|---|---|---|---|---|
| `fib` | 1.77 | 1.75 | 0.99 | 0.58 | 3.0 |
| `intloop` | 0.84 | 0.90 | 1.06 | 0.42 | 2.1 |
| `json` | 1.16 | 1.20 | 1.04 | 1.11 | 1.1 |
| `maps` | 1.20 | 1.21 | 1.01 | 2.05 | 0.6 |
| `pingpong` | 0.59 | **0.13** | **0.23** | 0.12 | **1.1** (was 5.0) |
| `sort` | 0.51 | 0.50 | 0.98 | 0.98 | 0.5 |
| `spawn` | 1.01 | **0.10** | **0.10** | 0.075 | **1.3** (was 13) |
| `strings` | 0.80 | 0.78 | 0.97 | 2.14 | 0.4 |

The sequential benchmarks do not change beyond noise: a second window
gave `intloop` 0.97, `json` 1.08, `fib` 1.02 and `strings` 1.06. Instruction
counts (`valgrind --tool=callgrind`) are steadier: `strings` executes the
same instructions, `json` 1.3% more and the compiler compiling itself 1.5%
more (376 ms before, 367 ms after). That is the cleanup frames: a function
with a frame now finds its goroutine's chain with a call
(`sd_lf_top_`), where it used to read a thread-local variable, because the
goroutine may continue on another thread. `spawn` measured 1.2-1.6x Go
in other windows, `pingpong` 1.1-1.2x.

Runtime micro-benchmarks (`runtime/tests/run.sh bench`, same machine, the
test's goroutines started from a thread outside the scheduler):

| Operation | Before | After |
|---|---|---|
| `chan int(0)` producer -> consumer | 1.0-1.2 us per value | 0.30-0.36 us per value |
| `chan int(1024)` producer -> consumer | 385-530 ns per value | 74-107 ns per value |
| buffered send + receive, one goroutine | 79-135 ns | 71 ns |
| spawn + join, one goroutine at a time | 15 us | 2.0-2.3 us |

**Where the time went, and what made the difference.**

- **Hand-offs.** A goroutine readied by another (a channel hand-off, the
  last member of a group waking its waiter) goes to the readier's
  `runnext` slot and runs on the same CPU as soon as the readier parks:
  two user-space switches through the worker's scheduler context, no
  system call. `pingpong`'s unbuffered half (100,000 round trips) went
  from 0.18 s to 0.05 s (0.5 us per round trip; Go: 0.055 s), its
  buffered half (1 million values through `chan int(1024)`) from 0.44 s
  to 0.08 s (Go: 0.07-0.09 s).
- **Not stealing a hand-off too early.** At first, an idle worker stole a
  readied goroutine from `runnext` after 5 us of spinning. That split
  producer/consumer pairs over two CPUs: the buffered half of `pingpong`
  took 0.25 s (Go: 0.09 s), with both CPUs fighting over the channel's
  lock, and the unbuffered half 0.11 s, because every hand-off between
  CPUs wakes a thread. Go waits `usleep(3)` before stealing `runnext`,
  which Linux's timer slack makes about 50-80 us. Seed now does the same
  and also leaves it if the owning processor ran anything meanwhile (a
  goroutine can be back in `runnext` after a whole round, which the first
  version mistook for one that had waited): the buffered half takes
  0.09 s, the steals fell from about 280 to 15 per run.
- **Spawning.** A goroutine's task and stack come from the processor's
  free list (a few hundred stacks are mapped at the start of `spawn`, then
  reused), its arguments are copied into the task when they fit in 64
  bytes, and it is queued with two atomic operations. One processor runs
  `spawn` in 0.07 s, as fast as Go; with four, the goroutines of a batch
  run on other CPUs and fight over the group's and the channel's locks,
  hence 0.10 s.
- **Locks.** Channel and group locks are glibc adaptive mutexes, which
  spin on a load before sleeping; with the default mutexes, contended
  operations went to the kernel at once (a third of `pingpong`'s buffered
  half was system time). A first version that looped on `trylock` made
  `spawn` slower: the loop's writes took the lock's cache line away from
  the holder.
- **False sharing.** The count of running goroutines (for the deadlock
  check) is updated by every thread; a counter used to seed each new
  goroutine's select generator sat on the same cache line. New goroutines
  now take their seed from their parent's generator.

What was measured and not kept: spinning in `park` before switching away
(the thread-per-goroutine design needed it; with user-space switches it
only burns CPU), and checking every pooled stack for deep use (touching a
second page of each stack at exit cost about 10% of `spawn`; a processor
now checks every fourth stack it keeps).

## Refcount elision

Measured on Linux x86_64 (4 cores, GCC 13), the machine shared with other
builds, so wall times are interleaved A/B runs (best of 7 to 15) and
reported as ratios; instruction counts come from callgrind and do not
depend on the load. compiler/README.md ("Memory management") has the rules.

1. **Parameters were already borrowed.** Parameters cannot be changed in
   place in Seed, so a callee never retains or releases one and a caller
   passes its values as they are; nothing to do there. (A parameter that
   the function assigns, allowed since, takes a retained copy on entry.)
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
time instead). Unboxing an unbounded accumulator like `sum`, which needs a
second copy of the loop to continue in when it overflows, came later
(section "Loop accumulators").

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

### On the M:N scheduler and the new allocator

The numbers above were measured on the thread-per-goroutine runtime. On the
runtime with the M:N scheduler and the small-object allocator, the same
code generator against the one before these three sections (interleaved,
best of 7; Linux x86_64, GCC 13):

| Benchmark | Before | After | After / before | `bench/run.sh` (best of 5): seed / Go |
|---|---|---|---|---|
| `fib` | 1.74 | 0.41 | 0.24 | 0.46 / 0.58 = 0.8 |
| `intloop` | 0.90 | 0.67 | 0.74 | 0.69 / 0.41 = 1.7 |
| `json` | 1.16 | 1.12 | 0.96 | 1.06 / 1.03 = 1.0 |
| `maps` | 1.20 | 1.20 | 1.00 | 1.27 / 1.95 = 0.7 |
| `pingpong` | 0.128 | 0.131 | 1.02 | 0.131 / 0.120 = 1.1 |
| `sort` | 0.52 | 0.57 | 1.10 (same instructions: noise) | 0.59 / 0.94 = 0.6 |
| `spawn` | 0.094 | 0.081 | 0.86 | 0.109 / 0.074 = 1.5 |
| `strings` | 0.73 | 0.73 | 0.99 | 0.77 / 2.09 = 0.4 |

The compiler compiling itself (each compiler built by its own code
generator): 2,266 M instructions before, 1,384 M after (-39%); wall time
0.70x (391 ms to 273 ms, interleaved best of 11).

## Preemption

The M:N scheduler had no preemption: a goroutine ran until it parked. With
8 goroutines computing on 4 CPUs, one that slept 10 ms woke after 57 to
2,051 ms (five runs), whenever a computing goroutine happened to end; with
`SEED_PROCS=1`, after 14.3 s. The thread-per-goroutine runtime had the
kernel's time slices. Now (runtime/README.md, "The scheduler";
compiler/README.md, "Preemption points"):

- **The compiler** puts `SD_PREEMPT()` at the top of every loop iteration,
  before every call that may close a cycle of calls (a call within the
  caller's strongly connected component of the call graph, or an indirect
  call), and on entry to vtable methods. It is a relaxed load of a
  process-wide word and a branch not taken: `mov`, `test`, `jne` to a cold
  call, no thread-local storage. A function checks at most once per path
  through its top-level statements, so `fib(n - 1) + fib(n - 2)` checks
  once.
- **sysmon** (the monitor thread of the block sections, which now runs from
  the first `spawn` and sleeps while every P is idle) asks a P to yield
  when it has run the same goroutine for 10 ms while others wait; the word
  counts the Ps asked. The slow path yields the goroutine to the global
  queue's tail if its P was asked.
- **The wake-up queue.** Goroutines readied by the poller (timers, sockets)
  or another thread without a P go to a queue that a P takes from first at
  its next park or preemption, so a timer's goroutine waits for one
  preemption, not for a P's whole queue.

The starvation program (8 goroutines spinning, one sleeping 10 ms, 4 CPUs):

| | Default (4 Ps) | `SEED_PROCS=1` | `SEED_PROCS=2` |
|---|---|---|---|
| before | 57 to 2,051 ms | 14,175 to 14,624 ms | |
| after | 10 to 20 ms | 15 ms | 15 ms |

tests/full/preempt_sleep.sd checks that a 30 ms sleep beside 64 spinning
goroutines ends within 200 ms of its spawn (430 ms before, 46 to 66 ms
after, with any `SEED_PROCS`); `test_preempt` in runtime/tests/test_conc.c
does the same in C under the sanitizers (a 10 ms sleep takes 20.6 ms).

**Where the checks go, and what they cost.** A check on entry to every
function that may recurse was the first version: `fib` took twice as long
(0.42 to 0.83 s), because the check's cold call made GCC save registers
on entry to every call, where before it returned from the leaves
(`n < 2`) without touching the stack (shrink-wrapping). Checking before
the recursive calls instead keeps the leaves as they were. What is left is
a load and a branch per loop iteration and per recursive call. On Linux
x86_64 (GCC 13, Xeon, 4 cores, a shared VM), against the code before
(instructions from cachegrind; times are the mean of 20 runs of
`perf stat`, whose run-to-run noise is 1 to 2%, and the best of 7
interleaved runs):

| Benchmark | Instructions | Mean time | Best time |
|---|---|---|---|
| `fib` | 0.985 | 1.03 | 1.01 |
| `intloop` | 1.125 | 1.13 | 1.12 |
| `json` | 1.029 | 1.07 | 0.96 |
| `maps` | 1.005 | 1.01 | 0.95 |
| `sort` | 1.003 | 1.00 | 1.00 |
| `strings` | 1.007 | 1.00 | 1.04 |
| `pingpong` | | 1.01 | 0.99 |
| `spawn` | | 0.93 | 0.99 |

`intloop` is the worst case: its loop body is 24 instructions, and the
check adds 3 (the next section takes them back). `json`'s 3% more
instructions are the checks of its per-character loops (its time moves by
up to 7% either way between identical binaries). The compiler compiling
itself: 2.9% more instructions (with the analysis that places the checks),
the same wall time. Go pays a comparable price on every call that is not a
leaf (its stack-bound check in the prologue, a load, a compare and a
branch) and nothing in loops, which it preempts with signals.

A note on measuring on this machine: Skylake-derived CPUs (this one is a
Cascade Lake) penalise a jump that crosses or ends on a 32-byte boundary
(the JCC erratum), so moving code by a few bytes changes a tight loop's
time by up to 25%. `intloop` built as one C file runs in 0.68 s, 0.88 s
with the checks, and 0.58 s and 0.64 s with the assembler told to avoid
the boundaries (GNU as's `-mbranches-within-32B-boundaries`, which
`seed build` does not pass). The instruction counts above do not depend on
it.

## Loop accumulators

`intloop` was 1.6x Go on Linux x86_64: its counter is unboxed (the interval
analysis bounds it), but `sum` grows without bound, so each iteration
tested its tag, added the words with an overflow check, and shifted the
small part into a word first; with the preemption check, the loop was 27
instructions against Go's 17. Now a loop whose int local is only ever
updated by `sum = sum + ...`, `sum = sum - ...`, `sum++` or `sum--` as the
last thing an iteration does gets a fast copy in which `sum` is a raw
`int64_t` (compiler/README.md, "Loop accumulators"):

```c
for (;;) {
  if (SD_UNLIKELY((l_i & 1023) == 0)) SD_PREEMPT();
  if (!((l_i < l_n))) break;
  {
    int64_t _t4 = ((l_i * l_i) % ((int64_t)7)) + (l_i & ((int64_t)3));
    if (SD_UNLIKELY(__builtin_add_overflow(r_sum_2, _t4, &r_sum_2))) {
      sd_int _t6 = sd_int_from_i64((int64_t)((uint64_t)r_sum_2 - (uint64_t)_t4));
      l_sum = sd_int_add(_t6, SD_INT_S(_t4));    /* exact, then on in the */
      sd_int_release(_t6);                       /* general copy of the loop */
      goto _c2;
    }
  }
  _c3:;
  l_i++;
}
l_sum = sd_int_from_i64(r_sum_2);
```

On overflow (past 2^63, or adding a big int) the update computes the exact
result as a general int and continues with the next iteration in the
general copy of the loop, so results beyond 64 bits are the same as
before (tests/full/int_accumulators.sd, and 100 random programs of 25
accumulator loops each, starting near the edges of the small and 64-bit
ranges, compared with the previous compiler's output: 80 at `-O1`, 20 at
`-O2`). Three details made the loop 20 instructions:

- **The add in place.** After an overflow, `__builtin_add_overflow` leaves
  the result modulo 2^64 in `r_sum_2`, from which the old value comes back
  by a subtraction; keeping the old value apart cost two moves per
  iteration.
- **No hoisting across the overflow jump.** Both the fast path and the
  spill's path run `i++` next (in the fast copy, and after the general
  copy's continue label), so GCC hoisted it above the jump and kept the
  overflow flag with `seto`, `test`, `jne`; an empty `asm` that may change
  `i` after the general copy's label stops it (0.61 s to 0.57 s on its
  own, built with the JCC mitigation below).
- **A cheaper preemption check.** A counted loop (`i++` on an unboxed
  counter nothing else assigns) whose body is only small-int arithmetic,
  comparisons and `if` checks every 1024th iteration, with
  `test $0x3ff, %ebx` instead of a load: each iteration takes a short,
  bounded time, so a request waits at most 1024 of them.

| `intloop` | Instructions | Mean time (perf stat, 20 runs) | Built with the JCC mitigation |
|---|---|---|---|
| before preemption | 7,200 M (24 per iteration) | 0.680 s | 0.59 s |
| with preemption | 8,100 M (27) | 0.781 s | 0.62 s |
| with loop accumulators | 6,301 M (21) | 0.516 s | 0.51 s |
| Go | (17 per iteration) | 0.41 s | |

`bench/run.sh` (best of 5) now gives `intloop` 0.486 s against Go's
0.409 s: 1.2x (was 1.6x). What is left is the overflow jump on `sum` and
the sign correction of `% 7`, which GCC keeps because it cannot see that
`i * i` is not negative (Go does). The other benchmarks have no such loop
in their hot paths except `strings`, whose `if (s.contains("99")) { n++ }`
loop gets a fast copy and the same instructions within 0.2%.

Code size: the compiler has 11 such loops; its binary grew by 3.4%
(1.73 MB to 1.79 MB) and compiling itself takes 0.5% more instructions
(the analysis included). Of the full benchmark set, against the code
before both changes (mean of 20 runs; instructions where measured):

| Benchmark | Before | Preemption | Accumulators too | Instructions, final / before |
|---|---|---|---|---|
| `fib` | 0.443 | 0.438 | 0.461 | 0.985 (same C as with preemption alone) |
| `intloop` | 0.680 | 0.782 | 0.516 | 0.875 |
| `json` | 1.095 | 1.087 | 1.079 | 1.029 |
| `maps` | 1.140 | 1.138 | 1.146 | 1.004 |
| `pingpong` | 0.139 | 0.142 | 0.138 | |
| `sort` | 0.597 | 0.591 | 0.588 | 1.003 |
| `spawn` | 0.107 | 0.106 | 0.091 | |
| `strings` | 0.733 | 0.762 | 0.796 | 1.009 |

`fib`'s C is the same with and without the accumulators (only the
packages' C around it changed), so its 5% is layout, as is most of the
`strings` and `spawn` movement.

### Bounded loops

The two things left above are gone (compiler/README.md, "Bounded
accumulators" and "Small ints"), and `intloop` is now faster than Go:

- **No overflow check when the trip count bounds the sum.** The counter
  `i` is unboxed with the interval `0..300000000` (`n` is a local that is
  never assigned again), and it takes a different value in each iteration,
  so the loop runs at most 300,000,001 times; `(i * i) % 7 + (i & 3)` is
  in `0..9`. Whatever `sum` holds when the loop starts (a small int, below
  2^62 in magnitude), adding at most 2.7 * 10^9 cannot leave `int64_t`, so
  the update is `r_sum_2 += _t4`: no `jo`, and no spill code. The rule is
  general: in a counted loop whose counter's interval is `lo..hi`, an
  accumulator whose every update adds or subtracts at most `d` needs no
  check when `(hi - lo + 1) * d <= 2^62` (a `count++` over a list's length
  qualifies: `2^62 * 1`).
- **Division of non-negative ints by constants.** The interval of `i * i`
  is `0..9 * 10^16`, so `% 7` needs no correction for negative dividends;
  C's signed `%` has one (`mov`, `sar $63`, `sub`), and GCC keeps it even
  for `(uint64_t)x % 7` when it knows `x >= 0` (it turns the unsigned
  division back into the signed one, which it thinks cheaper, correction
  included). The generator now emits the multiplication by the magic
  number itself when the dividend is known `>= 0`: `sd_modc_(x, m, s, 7)`
  is `x - (hi64(x * m) >> s) * 7`, with `m` and `s` chosen for the
  dividend's upper bound (here `m = ceil(2^64 / 7)`, `s = 0`: no shift at
  all); a power of two is a shift or a mask.

```c
int64_t _t4 = sd_modc_((l_i * l_i), 2635249153387078803ull, 0, 7) + (l_i & ((int64_t)3));
r_sum_2 += _t4;
```

```asm
.L4168: mov  %rbx,%rcx          ; i * i
        imul %rbx,%rcx
        mov  %rcx,%rax          ; high word of (i * i) * m
        imul %rbp
        lea  0(,%rdx,8),%rax    ; q * 7
        sub  %rdx,%rax
        sub  %rax,%rcx          ; (i * i) % 7
        mov  %rbx,%rax
        add  $1,%rbx
        and  $3,%eax            ; i & 3
        add  %rax,%rcx
        add  %rcx,%rdi          ; sum
        test $0x3ff,%ebx        ; preemption check, every 1024th
        jne  .L4148
        ...
.L4148: cmp  $300000000,%rbx
        je   .L4150
```

16 instructions per iteration, against Go's 17 and the 21 before
(valgrind's instruction count, whole program):

| `intloop` | Instructions | `bench/run.sh` best of 7 |
|---|---|---|
| loop accumulators (before) | 6,301 M (21 per iteration) | 0.373-0.387 s |
| bounded, magic `% 7` | 4,801 M (16) | 0.302-0.314 s |
| Go | 5,100 M (17) | 0.333-0.385 s |

On this VM (Linux x86_64, Cascade Lake, GCC 13) timings move by 5-10%
between runs, hence the ranges; `intloop` is 0.8-0.9x Go in every run.
Each change alone gets about half of it (the same C, edited by hand, best
of 9): without the overflow check only, 0.34-0.38 s; with the magic
division only, 0.34 s; both, 0.29-0.31 s.

The JCC erratum mitigation (`-Wa,-mbranches-within-32B-boundaries`) was
measured again for all the benchmarks, with the runtime and packages
rebuilt with it: no benchmark moved by more than the noise (`maps` and
`spawn` were slower in both runs, `json` and `intloop` faster in one), so
it is not in the C flags. The new loop's branches do not cross or end at a
32-byte boundary anyway.

The other benchmarks have a few such divisions (`json`, `maps`, `sort`,
`strings`) but none in a hot loop: their instruction counts moved by
+0.0003% (`json`), -0.02% (`maps`), -0.46% (`sort`), -0.01% (`strings`),
and `fib`'s not at all. Two runs of `RUNS=5 bench/run.sh`, before and
after (seconds, Go in parentheses; the spread is the VM's):

| Benchmark | Before | After |
|---|---|---|
| `fib` | 0.384 (0.600) | 0.371-0.388 (0.599-0.609) |
| `intloop` | 0.387 (0.350) | 0.304-0.326 (0.349-0.355) |
| `json` | 0.677 (0.774) | 0.689-0.723 (0.781-0.826) |
| `maps` | 0.620 (1.303) | 0.721-0.854 (1.443-1.455) |
| `pingpong` | 0.105 (0.128) | 0.113-0.123 (0.121-0.126) |
| `sort` | 0.536 (0.805) | 0.579-0.610 (0.787-0.831) |
| `spawn` | 0.082 (0.063) | 0.085-0.089 (0.064-0.068) |
| `strings` | 0.465 (1.535) | 0.464-0.526 (1.635-1.759) |

The compiler compiling itself runs the same number of instructions
(1,530.4 M before, 1,530.8 M after) and its binary is 0.8% larger.

Correctness: tests/full/int_bounded_loops.sd (the edges of the rule, loops
whose bound or counter is assigned inside, nested runs, every divisor kind
against the same divisions on general ints, near the top of the small
range), the magic numbers checked in Python against exact division for
5,000 divisors at their bounds, the C with `__int128` disabled (the
fallback), on aarch64 under qemu and with clang, and 100 random programs of
20 accumulator loops and 5 division loops each, compared with the previous
compiler's output.

## Spawn

`spawn` was the one benchmark still slower than Go on Linux: 0.085 s
against 0.069 s (1.23x) in `bench/run.sh` (best of 7). The goroutines do
almost nothing (one send on a buffered channel), so the time is starting
and ending 100,000 of them, while the main goroutine spawns on one CPU and
the other three steal and run them. Only the runtime changed
(runtime/README.md, section 10); the generated C is the same.

**Where the time went.** `perf record` (no hardware counters on this VM:
`task-clock` samples, with DWARF call graphs), the runtime before:

- `sd_spawn` was 16% of all samples, and 85% of that was one instruction:
  the `memset` of `ctx_make`, writing the first frame at the top of the new
  goroutine's stack. A finished goroutine kept its stack, so each spawn
  wrote to the top of whichever 8 MB mapping the pooled goroutine came
  with, last used by another CPU: a TLB miss with a page walk (every stack
  is a mapping of its own, so every top page has its own page table) and
  cache misses. Resetting the goroutine for reuse was also the spawner's
  job (it took the group's dead list), so the dozen cache lines of each
  `sd_task`, last written by the CPU that ran it, came over one by one.
- `pthread_mutex_lock` and `_unlock` were a quarter of the samples. The
  group's lock was taken by every spawn and by every goroutine as it
  ended, which held it while it unlinked itself from a list threaded
  through its neighbours (running on other CPUs). Once the first two
  changes below had removed the misses, a trace of the `futex` calls
  (`perf record -e syscalls:sys_enter_futex`, 16,500 per run, most of them
  wake-ups of a lock word left contended) put half of them on the group's
  lock and 30% on the channel's.
- The count of running goroutines for the deadlock check (an atomic
  incremented by each spawn, decremented by each end) and the channel's
  reference count (retained by each spawn's argument copy, released at
  each end, next to the channel's lock) were lines that every CPU wrote.

**What changed.**

1. **Goroutines are freed where they end** (Go's `goexit0`): on the
   worker's scheduler stack, into that processor's free list. The spawner
   takes structures from its own list, refilled with a whole batch from
   the shared pool (an array of batches, so moving one touches no
   goroutine).
2. **Stacks are pooled apart, per processor.** A goroutine takes a stack,
   and makes its first context, when it first runs, from the free stacks
   of the processor running it, and gives it back to the processor it ends
   on. A worker that runs goroutines one after another reuses the same
   stack, hot in its cache and TLB. Together with 1: 0.086 s to 0.062 s
   (`perf stat`, mean of 10; the steps below are measured the same way).
3. **Groups without a lock.** The members sit in slots in fixed chunks:
   the spawner fills an empty one, an ending goroutine empties its own,
   and the running count (with the waiting owner's bit) is one atomic.
   The lock is left to cancellations (which pin each slot while they
   cancel its goroutine) and the first error. 0.062 s to 0.054 s.
4. **Fewer shared lines.** Ends and parks are counted off the running
   count when their processor goes idle (increments stay immediate, so the
   count never drops below the truth and still reaches zero exactly at a
   deadlock); the channel's reference count has a line of its own;
   `sd_task` is laid out so that a spawn writes two lines (the spawn's
   fields and the arguments), the running goroutine two more, and nothing
   writes the rest unless it is used. About 3-4% each (interleaved runs).

**Results.** Linux x86_64, 4 cores (a VM shared with other agents' builds
and benchmarks: single runs move by 5-10%). Before is the runtime at the
start of this work.

| `spawn` | Before | After | Go |
|---|---|---|---|
| `bench/run.sh` (best of 7) | 0.085 s | 0.046-0.049 s | 0.064-0.065 s |
| interleaved, 7 rounds, median (best) | 90.5 (84.9) ms | 60.5 (50.4) ms | 77.2 (69.7) ms |
| `perf stat`, 20 runs: elapsed | 82-86 ms | 48-49 ms | |
| task-clock (CPU time, all threads) | 187-192 ms | 137-141 ms | |
| context switches | 1,613 | 885-927 | |
| `clock_nanosleep` calls | 632 | 98-103 | |
| `SEED_PROCS=1` / `GOMAXPROCS=1` (mean of 10) | 72 ms | 41 ms | 54 ms |
| `SEED_PROCS=2` | 80 ms | 48 ms | 62 ms |
| spawn + join, one at a time (`runtime/tests/run.sh bench`) | 1.4-1.6 us | 0.4-0.5 us | |

`spawn` is now 0.7x Go. `futex` calls rose (10,500 to 16,500 per run):
with the group's lock gone and the goroutines running in parallel, two
thirds of them are the channel's lock (all goroutines send on one
channel, so the senders on four CPUs contend; Go has one lock per channel
too) and most of the rest the scheduler's (`sched.mu`, taken when a
processor looks at the global queue or goes idle). Those are what is
left: a profile now has the channel's lock (with the futex calls behind
it) and the first touch of a goroutine's structure on the CPU that runs it
(it was written by the spawner on another) at the top.

The full set (`bench/run.sh`, best of 5, after; the other benchmarks do
not spawn, and their times are within this machine's noise of before;
`json`, checked with cachegrind, executes the same instructions,
6,251 M):

| Benchmark | Seed (s) | Go (s) | Seed / Go |
|---|---|---|---|
| `fib` | 0.375 | 0.622 | 0.6 |
| `intloop` | 0.373 | 0.359 | 1.0 |
| `json` | 0.837 | 0.773 | 1.1 |
| `maps` | 0.611 | 1.348 | 0.5 |
| `pingpong` | 0.102 | 0.121 | 0.8 |
| `sort` | 0.569 | 0.848 | 0.7 |
| `spawn` | 0.044 | 0.067 | 0.7 |
| `strings` | 0.463 | 1.540 | 0.3 |

Interleaved before/after (7 rounds, median ms): `fib` 398/397, `intloop`
439/419, `json` 756/792 (15 rounds: 775/803), `maps` 758/664, `pingpong`
116/119 (15 rounds: 124/120), `sort` 588/611 (15 rounds: 607/602),
`strings` 544/546.

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

## JSON

`json` was the one benchmark at Go's level rather than below it on Linux:
0.71-0.81 s against Go's 0.75-0.79 s (0.9-1.1x, depending on the run).
It builds a 1 MB document of 6,000 objects and then, 30 times, prints it
with `doc.string()` and parses it back with `json.parse`.

**Where the time went.** `perf record` (task-clock samples) of the
benchmark before, flat:

| Function | Share |
|---|---|
| `JsonParser.value`, `.str`, `.object`, `.array`, `skipSpace` | 26% |
| `sd_mem_alloc` + `sd_mem_free` + `sd_release` | 19% |
| float formatting: glibc's `__printf_fp_buffer`, `__printf_buffer`, `__mpn_divrem`, `round_and_return`, `strlen` | 10% |
| `strtod` (parsing numbers, and checking each formatting attempt) | 4% |
| `memmove` | 6% |
| `JsonWriter.write`, `jsonQuote`, `sd_list_join`, `sd_str_concat_n` | 14% |
| maps (`map_probe`, `key_hash`, `sd_map_set`) | 5% |

Three things stood out. `f.string()` of the 3,000 non-integral scores per
document (`4498.5`) tried `%.*e` with 1, 2, ... digits and read each
attempt back with `strtod` until one round-tripped: about ten library
calls per number. The printer pushed every piece (`"{"`, a quoted key,
`":"`, a number's text) onto a `[]string` and joined it at the end, so each
quoted string and number was a string of its own, copied twice. And the
parser's common paths had cleanup frames (the string function's frame had
13 fields for its rare escape path, `value`'s 9 for its error messages and
the `true`/`false` boxes it built before checking the literal), and it
compared each literal by slicing it out of the text.

**What changed.**

1. **Float to text without printf** (runtime/README.md, section 6). An
   exact short decimal (`x = n / 10^k` with `n < 10^15`: integers, halves,
   `4498.5`) is its own digits; anything else goes through Ryu, with the
   printf loop kept as the definition for the rare value where the two
   differ (Ryu's `vr == vm`). Output is byte-for-byte the same, checked
   against seed0's formatter on 200 million doubles on x86-64 (four runs of
   50 million: random bit patterns, short decimals, decimal strings of 1-17
   digits rounded to doubles, neighbours of powers of two and ten), 50
   million with the 64-bit fallback for `unsigned __int128`
   (`-DSD_NO_INT128`), and 10 million on arm64 under qemu: no difference.
   About 0.2% of the values that reach Ryu take the printf loop.
2. **Text to float with Clinger's fast path**: short decimal text (at most
   19 digits, power of ten within 10^22) is one exact multiplication or
   division; the rest is `strtod`, as before. Checked bit for bit against
   `strtod` in the same runs (a quarter of the cases parse shortest texts,
   `%.*e` and `%.*f` texts and random digit strings).
3. **Appending to a string field in place** (compiler/README.md, "Memory
   management"). `w.out = w.out + a + b` on a field of a local struct or a
   mut receiver now appends in place, as `x = x + ...` on a local string
   did, and several parts (strings, or ints written as digits) are appended
   with one `sd_str_append_parts` instead of being concatenated first.
   `sd_str_append` and `sd_str_append_parts` are inline in `seed.h` for an
   unshared string with room; the general case is out of line.
4. **The json package** (lib/README.md, "json"): the printer appends to one
   string (no list, no join; a plain string is `"` + s + `"` straight into
   it, escaped strings are copied in runs between escapes, whole numbers
   are written as digits); the parser's string fast path is a function of
   its own without a frame, escapes and errors are in separate functions,
   literals are compared byte by byte, and escaped strings are built with
   in-place appends instead of a `[]string` and a join.
5. **String slices** check small-int bounds inline instead of calling
   `sd_int_to_i64` twice.

**After.** The same profile:

| Function | Share |
|---|---|
| parser (`object`, `str`, `value`, `strRest`, `skipSpace`, `number`, `array`, `skipDigits`) | 33% |
| printer (`quote`, `write`) | 16% |
| `sd_mem_alloc` + `sd_mem_free` + `sd_release` + `sd_flush_batch` | 17% |
| maps (`key_hash`, `map_probe`, `sd_map_set`) | 9% |
| `memmove` | 4% |
| `sd_str_slice` | 2.5% |

No printf, `strtod` or `sd_list_join` is left. Instructions (cachegrind):
6,251 M before, 4,196 M after (-33%); the time fell more than the
instructions (-40%), since the printf and strtod paths and the join's
copies were slow instructions.

`RUNS=7 bench/run.sh json`, three separate runs (Linux x86-64, 4 cores, a
VM shared with other work):

| Run | Seed (s) | Go (s) | Seed / Go |
|---|---|---|---|
| 1 | 0.417 | 0.783 | 0.53 |
| 2 | 0.461 | 0.752 | 0.61 |
| 3 | 0.435 | 0.763 | 0.57 |

The full set, `RUNS=5 bench/run.sh`, before and after (seconds; the
compiler and runtime of the previous section against this one, run one
after the other):

| Benchmark | Before | After | Go (before / after runs) |
|---|---|---|---|
| `fib` | 0.393 | 0.403 | 0.637 / 0.595 |
| `intloop` | 0.318 | 0.330 | 0.347 / 0.359 |
| `json` | 0.706 | 0.430 | 0.750 / 0.796 |
| `maps` | 0.698 | 0.667 | 1.319 / 1.338 |
| `pingpong` | 0.107 | 0.108 | 0.118 / 0.130 |
| `sort` | 0.555 | 0.594 | 0.839 / 0.710 |
| `spawn` | 0.045 | 0.047 | 0.068 / 0.069 |
| `strings` | 0.502 | 0.483 | 1.506 / 1.585 |

The other benchmarks run the same instructions as before (cachegrind:
`sort` 2,740.1 M both, `fib` 4,281.3 M, `intloop` 4,801.2 M, `maps`
2,875.4 M), except `strings`, 5,096 M to 5,012 M (its `acc = acc + s`
appends are inline now); their differences above are this machine's noise
(interleaved, best of 7: `sort` 586/556 ms, `fib` 392/377 ms, `intloop`
298/311 ms, old/new). The compiler compiling itself runs 1,543.3 M
instructions before and 1,536.2 M after, in the same time (best of 7:
228-249 ms before, 227-232 ms after); its binary is 1.7% larger (the
inline appends and Ryu's tables).

What is left in the profile is the work Go's `encoding/json` does too
(a box per value, a map per object, a string per key and value, freeing the
document) and the per-byte loops of the parser and printer: their index is
a general `int` (it starts at a struct field, which the interval analysis
cannot bound), so each byte costs a tag test, a bounds check and an
overflow check, about 18 instructions where C would take 5.

## Big integers

Big integers had only the schoolbook algorithms, so printing a number of a
million digits took 23 s. `seed_int.c` now multiplies long numbers with
Karatsuba's method (on 64-bit words where the C compiler has
`unsigned __int128`), divides them with Burnikel and Ziegler's recursive
division, and converts them to and from decimal text by divide and conquer
over the powers 10^(9 * 2^k); runtime/README.md, section 5, has the
details. On Linux x86_64 (one core, gcc `-O2`, Go 1.24), in seconds:

| Operation on 3^2,000,000 (954,243 digits) | Before | After | Go `math/big` |
|---|---|---|---|
| computing it by repeated squaring | 3.6 | 0.15 | 0.11 |
| `string()` | 22.9 | 0.45 | 0.26 |
| `toInt()` of the text | 4.3 | 0.2 | 1.5 |
| dividing by 7^666,666 | 5.1 | 0.15 | 0.13 |
| 100000! by a loop of `f = f * i`, then `string()` | 7.0 + 5.2 | 1.1 + 0.15 | 1.2 + 0.07 |

The `bigint` benchmark (bench/seed/bigint.sd) went from 57 s to 1.45 s;
Go's version takes 3.7 s, most of it in `SetString`, which is still
quadratic in Go. Multiplication and printing remain 1.5-2x slower than
Go's, whose inner loops are in assembly; Toom-Cook would gain little below
millions of digits.
