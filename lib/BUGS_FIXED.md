# Library and runtime bugs found and fixed (QA pass)

One entry each: symptom; cause; fix; regression test. How the library was
tested is at the end.

1. **A path with a NUL byte named a different file, and `fs.removeAll` could
   delete the wrong directory.** `fs.removeAll(dir + "/\x00")` removed `dir`
   itself; `fs.write("a.txt\x00.png", ...)` wrote `a.txt`; `fs.exists`,
   `fs.stat`, `fs.list`, `fs.mkdir`, `fs.rename`, `fs.remove`,
   `fs.makeTemp`, `fs.open` and `fs.create` likewise acted on the part
   before the NUL. Cause: Seed strings may hold NUL bytes, and the natives
   passed `path->data` to the OS as a C string, which ends at the first
   one. Fix: every `fs` function checks its paths first (`fs_has_nul` in
   `fs/fs.c`) and fails with `fs.<fn>: <path>: path contains a NUL byte`
   (the subject shows each NUL as `\x00`; `exists` and `isDir` return
   false), as Go fails with `invalid argument`. Test: `fs/tests/qa_test.sd`,
   "a path with a NUL byte is an error and touches nothing".
2. **`os.run` and `os.output` ran a different command line when an argument
   held a NUL byte** (`os.run(["sh\x00-c", ...])` ran `sh`; `"a\x00b"`
   became `"a"`). Cause: the same C-string truncation in the argv given to
   `posix_spawnp`. Fix: `os_check_argv` (`os/os.c`) rejects such an argv
   before anything starts: `os.run: sh\x00-c: program name contains a NUL
   byte`, `os.output: echo: argument 1 contains a NUL byte`. (`os.env`
   already returned null for such a name; `net` already rejected such an
   address.) Test: `os/tests/qa_test.sd`.
3. **The HTTP server sent a stray `HTTP/1.1 100 Continue` on a kept-alive
   connection** after answering a request that had `expect: 100-continue`
   and whose body arrived together with its head (as many clients send
   it): the next request on the connection was preceded by an unsolicited
   interim response. Cause: `MessageReader.expectContinue` was set when the
   head was parsed and cleared only when `serveConn` sent the 100, so when
   no 100 was needed it stayed set into the next `NeedMore`. Fix:
   `MessageReader.finish` clears it when the request is complete. Tests:
   `http/tests/qa_test.sd`, "expect 100-continue is not carried over to the
   next request" (reader level) and "no stray 100 Continue after a request
   whose body came with its head" (a real server and socket).
4. **`fs` tests failed when two test programs ran at once** (found by the
   soak, `seed test lib/fs` in parallel: `list` saw the other run's files).
   Cause: "write, read, list and remove" used the fixed directory
   `$TMPDIR/seed-lib-fs-test`. Fix: it uses `fs.makeTemp`, as
   docs/stdlib.md recommends; the other tests already did. Test: the soak
   (300 parallel runs of the fs tests, no failure).

Also changed (not bugs, found by the same testing):

- **`net.listen` asks for the longest accept queue the OS allows** (65535,
  which the kernel clamps to `kern.ipc.somaxconn` / `net.core.somaxconn`,
  as Go does) instead of 128. On macOS the limit is 128 anyway: a burst of
  more than about 150 connections arriving at once can get `connection
  reset by peer` (macOS drops a queued connection to make room). Go's
  `net/http` behaves the same on this machine (measured: 300 clients in one
  process, 19 of 20 runs fail in both); raising `kern.ipc.somaxconn` is the
  fix. On Linux the larger backlog takes effect.
- **`rand`'s per-goroutine generator** is reseeded by each goroutine: now
  that threads are reused (runtime/README.md, section 10), `rand.c`
  registers an `sd_goroutine_atexit` function that forgets the seed, so a
  goroutine never continues the stream of the one that ran before it on
  the same thread.

## How the library was tested

- **Differential fuzzing against reference implementations** (about
  200,000 random and mutated inputs over several seeds; no mismatch after
  the fixes): hex and base64 (standard and URL, with and without padding,
  mutated padding and alphabets) against Python's `binascii`/`base64` with
  strict-padding rules; `urlEncode`/`urlDecode` against a strict reference;
  `json.parse` + `string()` and `pretty()` against Python's `json` with the
  docs/stdlib.md printing rules (RFC 8259 strictness, surrogates, invalid
  UTF-8, control characters, numbers at the float limits, nesting limit);
  `sha256` and `hmacSha256` against `hashlib`/`hmac`; `path.clean`, `base`,
  `dir`, `ext` and `join` against Go's `path` package; `strings.fields` and
  `lastIndex` against Python's bytes methods; `time.parse` against a strict
  RFC 3339 reference over years 0001-9999 and `time.format` against
  `datetime`.
- **Edge cases by hand**: JSON documents of 5.8 MB and strings of 1.2 MB,
  2,000-deep values built in code (printing 100,000 levels crashes cleanly
  with a stack overflow, as any deep recursion does), `1e400`/`1e-400`,
  lone and reversed surrogates; 6 MB base64 and hex round trips; `fs` on
  missing paths, permission-denied files and directories, directories
  given to `read`/`write`, `mkdir` over and under files, `rename` of
  missing paths; `io` line reading on 200 random inputs with CR, LF, NUL
  and no final newline, and through a pipe that delivers slowly;
  `time.after`/`sleep` with zero and negative durations, dates before year
  1 and at year 9999; `rand.range` beyond 2^64; `math.powInt` of big
  values.
- **HTTP**: a raw-socket probe against `http.serveListener` (expect
  100-continue, bodies of exactly 8 MiB and 8 MiB + 1, chunked bodies over
  the limit, headers over 64 KiB, a request sent one byte at a time, bad
  chunk sizes, `content-length` variants, NUL and CR in headers, request
  smuggling with both framings, a client that disconnects mid-body or
  never reads a 10 MB response, 200 idle connections, 6,000 requests from
  300 concurrent clients); the client against Go's `net/http` server
  (chunked and 70 MB responses, 5 MB bodies, redirects, 204, HEAD,
  repeated headers, a `host` header set by the program) and against
  malformed URLs, refused ports, unknown names and port 0. Bursts of
  simultaneous connections: see `net.listen` above.
