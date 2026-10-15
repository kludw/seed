# 14. Crashes and debugging

A crash is a bug ([chapter 4](04-functions-and-errors.md#errors-and-crashes)), and the first thing to know about a bug is where it is. This chapter shows how to read what a crash prints, and how to run a Seed program in a debugger.

## Reading a crash

A crash prints its message and, below it, where it happened and the calls that led there, innermost first:

```seed
package main

import "fmt"

fn average([]int xs) int {
    int sum = 0
    for (int i = 0; i < xs.len(); i++) {
        sum = sum + xs[i]
    }
    return sum / xs.len()
}

fn report(string name, []int scores) error {
    fmt.println(name + ": " + average(scores).string())
    return null
}

fn main([]string args) error {
    try group {
        spawn report("ada", []int{90, 85})
        spawn report("bob", []int{})
    }
    return null
}
```

```
$ seed run 14_trace.sd
ada: 87
error: crash: division by zero
    at average (14_trace.sd:10:16)
    called from report (14_trace.sd:14:31)
    spawned from main (14_trace.sd:21:9)
$ echo $?
1
```

- `at` is where the program crashed: the function, then the file, line and column of the operation that failed (here the `/`).
- Each `called from` line is a call on the way there: the function that made it and the position of the call.
- A goroutine's trace ends with `spawned from` and the position of its `spawn`. Its crash became the group's error, which `try` passed on to `main`; `main` returned it, so the program printed `error: crash: ...` with the trace and exited with 1. A crash in the main goroutine prints `crash: ...` and exits with 2.

Paths are the ones given to the compiler (`seed run 14_trace.sd` here), and those of the standard library start with `lib/`. Methods are named after their type (`Shelf.book`), the functions of a package other than `main` after the package (`shapes.width`, `rand.Source.range`), and tests by their description (`test "parses numbers"`).

A stack overflow (a recursion that never ends) shows the first ten calls and the last five, with the number of calls between them:

```
crash: stack overflow or invalid memory access
    at down (main.sd:11:12)
    called from down (main.sd:11:12)
    ...
    ... 174735 more calls ...
    called from down (main.sd:11:12)
    called from main (main.sd:17:17)
```

A recursion of `int`s only (no lists, strings or other values) runs as a faster version of the function that has no positions; its calls are counted instead, and the function is named where the recursion was entered:

```
crash: stack overflow or invalid memory access
    ... 524251 calls without a position (optimised code) ...
    at depth (main.sd:8:4)
    called from main (main.sd:17:17)
```

The trace goes with the error of a crashed goroutine, not into its message: `err.message()` is still `crash: division by zero`, so comparing errors and printing them is unchanged. Only `main` returning that very error, or `seed test` reporting a test that crashed, prints the trace.

**Inlined functions.** The C compiler copies small functions into their callers (inlining), and a copied function leaves no trace of the call: the crash's own position is always shown, but the line of the caller that it was copied into may be missing. Programs built for a debugger are not inlined, so `seed run -g` shows every call.

**What it costs.** Nothing while the program runs: the trace is rebuilt from the program's stack only when it crashes, from tables that the compiler adds to the program (the runtime's README, "Crash traces", has the details).

## Debugging with gdb or lldb

`seed build -g` (and `seed run -g`) compile a program for a debugger: without optimisations, and with the Seed source's lines and files in the debugging information. Breakpoints, stepping and backtraces then follow the Seed source:

```seed
package main

import "fmt"

fn total([]int xs, string label) int {
    int sum = 0
    for (int i = 0; i < xs.len(); i++) {
        sum = sum + xs[i]
    }
    fmt.println(label + ": " + sum.string())
    return sum
}

fn main([]string args) error {
    []int xs = []int{3, 4, 5}
    string name = "numbers"
    map[string]int seen = map[string]int{"a": 1}
    int t = total(xs, name)
    fmt.println(t.string() + " " + seen.len().string())
    return null
}
```

```
$ seed build -g -o totals 14_debug.sd
$ SEED_PROCS=1 gdb -x $SEED_ROOT/runtime/seed-gdb.py ./totals
(gdb) break 14_debug.sd:18
(gdb) run
Breakpoint 1, sd_f_main (l_args=["./totals"] (length 1)) at 14_debug.sd:18
18	    int t = total(xs, name)
(gdb) seed-locals
args = ["./totals"] (length 1)
name = "numbers"
seen = {"a": 1} (length 1)
t = 46912496166976
xs = [3, 4, 5] (length 3)
(gdb) step
sd_f_total (l_xs=[3, 4, 5] (length 3), l_label="numbers") at 14_debug.sd:5
5	fn total([]int xs, string label) int {
(gdb) bt 2
#0  sd_f_total (l_xs=[3, 4, 5] (length 3), l_label="numbers") at 14_debug.sd:5
#1  sd_f_main (l_args=["./totals"] (length 1)) at 14_debug.sd:18
(gdb) next
6	    int sum = 0
(gdb) p l_xs
$1 = [3, 4, 5] (length 3)
(gdb) finish
Value returned is $2 = 12
```

(`$SEED_ROOT` is the directory of the Seed repository; `t` holds whatever was in its memory before its line runs.)

- **Names.** A Seed function `f` is the C function `sd_f_f`, a method `T.m` is `sd_m_T__m`, and a function of package `cart` is `sd_f_cart_f`: break on a line (`break main.sd:12`), or on a function by its C name (`break sd_f_total`).
- **Variables.** A local or parameter `x` is the C variable `l_x`, or the field `L.l_x` of the function's *cleanup frame* `L` (a struct that holds what a crash must release); a parameter the function assigns is `o_x`. Inside a loop, an `int` the loop adds to may be kept in a copy `r_x_N` until the loop ends. `seed-locals` lists them all by their Seed names.
- **Values.** `runtime/seed-gdb.py` adds pretty printers to gdb: an `int` prints as its number, a `string` as its text, a list and a map as their elements, an error or interface value as its dynamic type. Without them, an `int` is a struct `{w}` holding twice the number (`p l_n.w >> 1`), a string is `l_s->data`, and a list's length is `l_xs->len`.
- **Goroutines** run on several threads, so `next` may stop in another goroutine that reaches the same breakpoint. `SEED_PROCS=1` runs them all on one processor (one thread), which keeps a session on the goroutine you follow.

lldb works the same way, without the printers: `b 14_debug.sd:18`, `run`, `frame variable`, `p L.l_name->data`, `bt`, `step` and `next`. On macOS, lldb is the debugger that comes with the command-line tools.

Back to the [table of contents](README.md).
