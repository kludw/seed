# Dogfooding: four realistic programs

DRAFT (in progress)

## Friction notes so far

- byte == 127: "mismatched types byte and int for ==" without a hint (write '\x7f' or byte(127)).
- defer s.close() takes its receiver when the defer runs; a struct whose handle is replaced later (store compaction) closes the stale handle. Spec says arguments are evaluated; receiver not mentioned.
- fs has no append-mode File: the kv log reopens by compacting on open (fs.create + rename).
- tests ran in the cwd of `seed test`, not the package dir: golden files unreachable. FIXED: --dir.
- `int start, _ = s.toInt()` error followed by a cascade "start has type int, which is never null".
- No closures: regex.replaceWith can't use state (mdhtml stash); a manual findAt loop instead.
- Testing one case of an enum with data needs a switch listing every case (or name() == "X").
- kv bench: 10.7k req/s 1 client, 51.5k 8 clients, 71k 32 clients; build 0.7 s; 555 KB.
- mdhtml: 280 KB markdown in 0.25 s (pandoc 1.6 s).
