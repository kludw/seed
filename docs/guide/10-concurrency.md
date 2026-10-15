# 10. Concurrency

Seed's concurrency looks like Go's, with goroutines and channels, but three rules make it much harder to get wrong:

1. **Goroutines share nothing.** Every value a goroutine gets is its own copy, so data races are impossible.
2. **Goroutines live in groups.** `spawn` only works inside a `group` block, and the block waits for every goroutine it started. A goroutine can't outlive the code that started it, so goroutines can't leak.
3. **Failure spreads like in Erlang.** When one goroutine fails, the rest of its group is stopped, and the group returns the error.

## Groups and spawn

```seed
// Sums the halves of a list in two goroutines.
package main

import "fmt"

fn sum([]int xs, chan int out) error {
    int total = 0
    for (int i = 0; i < xs.len(); i++) {
        total = total + xs[i]
    }
    out.send(total)
    return null
}

fn main([]string args) error {
    []int numbers = []int{1, 2, 3, 4, 5, 6, 7, 8, 9, 10}
    chan int partials = chan int(2)
    error err = group {
        spawn sum(numbers.slice(0, 5), partials)
        spawn sum(numbers.slice(5, 10), partials)
    }
    if (err != null) {
        return err
    }
    int total = 0
    for (int i = 0; i < 2; i++) {
        int? part = partials.receive()
        if (part != null) {
            total = total + part
        }
    }
    fmt.println("total: " + total.string())
    return null
}
```

```
total: 55
```

- `spawn f(args)` starts `f` in a new goroutine. The arguments are evaluated and copied first.
- A spawned function returns an `error` (or nothing): the error is how a goroutine reports failure to its group. Other results come back on channels, and a `mut` method can't be spawned, since the goroutine would only change its own copy.
- `group { ... }` is an expression of type `error`. It runs its block, then waits until every goroutine spawned in it has finished. It returns `null` if all of them succeeded, or the first error.
- A group block is ordinary code: `spawn` can appear in loops and `if`s, so the number of goroutines can depend on the data (see the [worker pool](#worker-pool)). The block can't be left early, though: `return` and `try` are not allowed inside it, because the group must wait for its goroutines first.

The group's error must be handled like any other error. If nothing handles it, it ends up as the error `main` returns, and the program exits with its message.

## Channels

A channel is how goroutines talk.

| Operation | Meaning |
|---|---|
| `chan T(n)` | A new channel of `T` with a buffer of `n` values. `n = 0` is unbuffered: a send waits for a receiver. |
| `ch.send(v)` | Sends a copy of `v`. Waits while the buffer is full. Sending on a closed channel crashes. |
| `ch.receive()` | Waits for a value and returns it as a `T?`. Returns `null` once the channel is closed and empty. |
| `ch.close()` | Closes the channel. Receivers get the buffered values, then `null`. |

**Sending copies the value.** The receiver gets its own copy, and the sender can keep changing its own. (As always, copy-on-write makes this cheap until someone changes the data.) That is why there are no data races: no two goroutines can ever see the same variable.

**Channels are the one exception to value semantics.** A channel is a reference: copying a channel value, into a variable, a struct, an enum or a message on another channel, gives another handle to the same channel. That is how a goroutine tells another one where to send its answer (see [reply channels](#shared-state-reply-channels)).

A receive loop runs until the channel is closed:

```seed
for (;;) {
    int? v = input.receive()
    if (v == null) {
        break        // closed and empty
    }
    out.send(v * v)
}
```

## select

`select` waits until one of several channel operations can go ahead, and runs that case. Each case is a send or a receive:

```seed
// select waits on several channel operations at once, sends included.
package main

import "fmt"

fn main([]string args) error {
    chan int jobs = chan int(3)
    chan string replies = chan string(2)
    replies.send("a")
    replies.send("b")
    int sent = 0
    int received = 0
    for (int next = 1; sent < 3 || received < 2; ) {
        select {
        case jobs.send(next):
            sent++
            next++
        case string? r = replies.receive():
            if (r != null) {
                received++
            }
        }
    }
    fmt.println("sent " + sent.string() + " jobs, received " + received.string() + " replies")
    return null
}
```

```
sent 3 jobs, received 2 replies
```

A receive case declares a new variable (`string? r = replies.receive()`), assigns an existing one, or drops the value (`replies.receive()` or `_ = replies.receive()`). As everywhere, a name with a type in front of it is declared, and a name without one is assigned: `case latest = updates.receive():` stores the value in `latest`, a variable of exactly the received type (`string?` for a `chan string`) declared before the `select`, which is handy for keeping the last value across the iterations of a loop:

```seed
string? latest = null
for (bool open = true; open; ) {
    select {
    case latest = updates.receive():
        open = latest != null
    case tick.receive():
        show(latest)
    }
}
```

On a `chan error` the received error must be handled like any other: `case error e = errs.receive():` (or `case err = errs.receive():` for an existing `error err`), or `case _ = errs.receive():` to discard it.

When several cases are ready, any one of them may run. There is no `default` case: `select` always waits. To wait with a limit, add a timer channel as one more case (see [timeouts](#timeouts)).

## When a goroutine fails

A goroutine fails when it returns an error or crashes. Then:

- the other goroutines in its group are **stopped**;
- each stopped goroutine's deferred calls still run;
- the group returns the **first** error. A crash becomes an error too, so a bug in one goroutine doesn't take down the program; the group reports it.

```seed
// When one goroutine fails, the group stops the others and returns the error.
package main

import "errors"
import "fmt"

fn listen(chan string messages) error {
    defer fmt.println("listener: cleaning up")
    for (;;) {
        string? m = messages.receive() // stopped here
        if (m == null) {
            return null
        }
    }
}

fn check() error {
    return errors.new("checker: disk is full")
}

fn main([]string args) error {
    chan string messages = chan string(0)
    error err = group {
        spawn listen(messages)
        spawn check()
    }
    if (err != null) {
        fmt.println("group failed: " + err.message())
    }
    fmt.println("main carries on")
    return null
}
```

```
listener: cleaning up
group failed: checker: disk is full
main carries on
```

A goroutine is never interrupted in the middle of its work. It is stopped only at a **stop point**: a channel send or receive (including in `select`), or a standard-library call that blocks, such as `time.sleep`, reading stdin with `io.readLine`, socket I/O in `net` (and so `http`), `net.dial`, `os.run` and `os.output`. Reading an open `fs.File` is one too, since it can wait on a FIFO. (The other `fs` operations, such as `fs.read` and `fs.write`, are not stop points: on regular files they don't block for long.) A goroutine that computes for a long time without reaching one finishes its work before it notices.

This is Erlang's "let it crash" idea, with the structure of the code deciding who cleans up: a failure is contained by the nearest enclosing `group`, and the code after that group decides what to do about it.

## Patterns

### Worker pool

A fixed number of workers take jobs from one channel and send results to another. The `for` loop inside the `group` spawns the workers:

```seed
// A fixed number of workers take jobs from one channel and send results to
// another.
package main

import "fmt"

const int workers = 3

fn produce(int count, chan int jobs) error {
    for (int i = 1; i <= count; i++) {
        jobs.send(i)
    }
    jobs.close()
    return null
}

fn work(chan int jobs, chan int results) error {
    for (;;) {
        int? job = jobs.receive()
        if (job == null) {
            return null // no more jobs
        }
        results.send(job * job)
    }
}

fn main([]string args) error {
    int count = 10
    chan int jobs = chan int(0)
    chan int results = chan int(count)
    error err = group {
        spawn produce(count, jobs)
        for (int i = 0; i < workers; i++) {
            spawn work(jobs, results)
        }
    }
    if (err != null) {
        return err
    }
    results.close()

    // Results arrive in any order; sort them for printing.
    []int squares = []int{}
    for (;;) {
        int? r = results.receive()
        if (r == null) {
            break
        }
        squares.push(r)
    }
    squares.sort()
    []string text = []string{}
    for (int i = 0; i < squares.len(); i++) {
        text.push(squares[i].string())
    }
    fmt.println(text.join(" "))
    return null
}
```

```
1 4 9 16 25 36 49 64 81 100
```

The `results` channel has room for every result, so workers never wait for `main`, which only reads after the group has finished.

### Pipeline

Stages connected by channels, each closing its output when its input runs dry:

```seed
// Three stages connected by channels: generate, square, sum.
package main

import "fmt"

fn generate(int n, chan int out) error {
    for (int i = 1; i <= n; i++) {
        out.send(i)
    }
    out.close()
    return null
}

fn square(chan int input, chan int out) error {
    for (;;) {
        int? v = input.receive()
        if (v == null) {
            break
        }
        out.send(v * v)
    }
    out.close()
    return null
}

fn total(chan int input, chan int result) error {
    int sum = 0
    for (;;) {
        int? v = input.receive()
        if (v == null) {
            break
        }
        sum = sum + v
    }
    result.send(sum)
    return null
}

fn main([]string args) error {
    chan int numbers = chan int(0)
    chan int squares = chan int(0)
    chan int result = chan int(1)
    error err = group {
        spawn generate(10, numbers)
        spawn square(numbers, squares)
        spawn total(squares, result)
    }
    if (err != null) {
        return err
    }
    int? sum = result.receive()
    if (sum != null) {
        fmt.println("sum of squares 1..10 = " + sum.string())
    }
    return null
}
```

```
sum of squares 1..10 = 385
```

If any stage fails, the group stops the others, even the ones waiting on a channel that will never be closed now.

### Shared state: reply channels

Goroutines can't share a variable, so shared state belongs to **one goroutine that owns it**, and the others send it messages. An enum makes a good message type. A request that needs an answer carries a channel to send it on:

```seed
// Shared state without sharing: one goroutine owns the counter, and the
// others talk to it with messages. A request that needs an answer carries
// its own reply channel.
package main

import "errors"
import "fmt"

type Op enum {
    Add(int n)
    Get(chan int reply)
    Done
}

// Owns the count. Returns it once every client has said Done.
fn counter(chan Op ops, int clients, chan int result) error {
    int count = 0
    int finished = 0
    for (; finished < clients; ) {
        Op? op = ops.receive()
        if (op == null) {
            return errors.new("counter: channel closed early")
        }
        switch (op) {
        case Op.Add(int n):
            count = count + n
        case Op.Get(chan int reply):
            reply.send(count)
        case Op.Done:
            finished++
        }
    }
    result.send(count)
    return null
}

fn client(chan Op ops, int step) error {
    for (int i = 0; i < 3; i++) {
        ops.send(Op.Add(step))
    }
    chan int reply = chan int(1)
    ops.send(Op.Get(reply))
    int? seen = reply.receive()
    if (seen == null || seen < 3 * step) {
        return errors.new("client: lost an update")
    }
    ops.send(Op.Done)
    return null
}

fn main([]string args) error {
    chan Op ops = chan Op(0)
    chan int result = chan int(1)
    error err = group {
        spawn counter(ops, 3, result)
        spawn client(ops, 1)
        spawn client(ops, 2)
        spawn client(ops, 3)
    }
    if (err != null) {
        return err
    }
    int? count = result.receive()
    if (count != null) {
        fmt.println("final count: " + count.string())
    }
    return null
}
```

```
final count: 18
```

The counter handles one message at a time, so its updates can't interleave: the channel does the job a mutex does in Go, and there is no lock to forget. Seed has no `sync` package.

### Timeouts

A timeout is just another channel. `time.after(d)` returns a channel that receives a value after `d` nanoseconds, so it can be a case in a `select`. Here the collector gives up after 500 ms; its error stops the group, which also stops the goroutine that is still sleeping:

```seed
// Waiting with a timeout. The collector gives up after 500 ms, its error stops
// the group, and the slow goroutine is stopped in time.sleep.
package main

import "errors"
import "fmt"
import "time"

fn slowSquare(int n, chan int out) error {
    time.sleep(n * 100 * time.millisecond)
    out.send(n * n)
    return null
}

fn collect(chan int results, int count) error {
    for (int i = 0; i < count; i++) {
        select {
        case int? r = results.receive():
            if (r != null) {
                fmt.println("got " + r.string())
            }
        case time.after(500 * time.millisecond).receive():
            return errors.new("timed out waiting for " + (count - i).string() + " more result(s)")
        }
    }
    return null
}

fn main([]string args) error {
    chan int results = chan int(0)
    return group {
        spawn slowSquare(1, results)
        spawn slowSquare(2, results)
        spawn slowSquare(60, results)
        spawn collect(results, 3)
    }
}
```

```
$ seed run 10_timeout.sd
got 1
got 4
error: timed out waiting for 1 more result(s)
$ echo $?
1
```

`main` returns the group's error, so the program exits with code 1 and prints the message.

The `select` with the timer has to run in a spawned goroutine, as `collect` does. A `select` written in the group block itself does see the timeout, but the block can't stop the group's goroutines (only a failing goroutine stops them) and can't leave early, so the group still waits for the slow goroutine before it returns. For one HTTP request there is a shortcut: `http.getWithin(url, timeout)` and `http.sendWithin(req, timeout)` take the limit themselves ([stdlib.md](../stdlib.md#client)).

## Compared with Go

| Go | Seed |
|---|---|
| `go f(x)` anywhere | `spawn f(x)` only inside `group { ... }` |
| `sync.WaitGroup`, `errgroup` | `group` waits, and returns the first error |
| Goroutines can leak | A group always waits for its goroutines |
| Shared memory plus `sync.Mutex` | No shared memory; one goroutine owns the state |
| Data races possible | Data races impossible: sends copy |
| A panic in any goroutine kills the program | A crash becomes the group's error |
| `context.Context` for cancellation | The group stops its goroutines at stop points |
| `v, ok := <-ch` | `T? v = ch.receive()` |
| `select` with `default` | No `default`; use a timer channel |

Next: [Packages and visibility](11-packages.md).
