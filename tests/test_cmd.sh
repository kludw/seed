#!/bin/sh
# Tests of the seed command itself: `seed test`, `seed build`, `seed run`,
# finding the root through $PATH and a symbolic link, temporary
# directories, the runtime cache, stdout on a terminal, and make install.
#
#   tests/test_cmd.sh <seed>        (make test-full runs it with build/seed2)
#
# For every tests/testcmd/NAME/ project, `seed test testcmd/NAME` (run in
# tests/) must print exactly NAME.out on stdout and exit with NAME.exit.
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
seed=${1:-build/seed2}
case $seed in /*) ;; *) seed=$root/$seed ;; esac
tmp=$(mktemp -d "${TMPDIR:-/tmp}/seedcmd.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
pass=0; fail=0
failed() { fail=$((fail+1)); echo "FAIL $1: $2"; }

for dir in "$here"/testcmd/*/; do
  name=$(basename "$dir")
  (cd "$here" && "$seed" test "testcmd/$name" > "$tmp/out" 2> "$tmp/err"); code=$?
  want=$(cat "$here/testcmd/$name.exit")
  if [ "$code" != "$want" ]; then
    failed "seed test testcmd/$name" "exit code $code, want $want: $(head -c 300 "$tmp/err")"; continue
  fi
  if ! cmp -s "$tmp/out" "$here/testcmd/$name.out"; then
    failed "seed test testcmd/$name" "stdout differs:"; diff "$here/testcmd/$name.out" "$tmp/out" | head -20; continue
  fi
  pass=$((pass+1))
done

# The same for the spec conformance projects in spec/testcmd/NAME/ (run in
# spec/), where NAME.err, if present, is text that must appear in stderr
# (for projects whose tests must not compile).
for dir in "$here"/spec/testcmd/*/; do
  [ -d "$dir" ] || continue
  name=$(basename "$dir")
  (cd "$here/spec" && "$seed" test "testcmd/$name" > "$tmp/out" 2> "$tmp/err"); code=$?
  want=$(cat "$here/spec/testcmd/$name.exit")
  if [ "$code" != "$want" ]; then
    failed "seed test spec/testcmd/$name" "exit code $code, want $want: $(head -c 300 "$tmp/err")"; continue
  fi
  if ! cmp -s "$tmp/out" "$here/spec/testcmd/$name.out"; then
    failed "seed test spec/testcmd/$name" "stdout differs:"; diff "$here/spec/testcmd/$name.out" "$tmp/out" | head -20; continue
  fi
  if [ -f "$here/spec/testcmd/$name.err" ] && ! grep -qF "$(cat "$here/spec/testcmd/$name.err")" "$tmp/err"; then
    failed "seed test spec/testcmd/$name" "stderr missing '$(cat "$here/spec/testcmd/$name.err")', got: $(head -c 300 "$tmp/err")"; continue
  fi
  pass=$((pass+1))
done

# SPEC "Tests": seed test without a directory runs the tests under the
# current one.
out=$(cd "$here/spec/testcmd/rules" && "$seed" test 2>&1)
case $out in
  *'FAIL "byte comparison": tally/tests/tally_test.sd:35:'*"3 passed, 3 failed") pass=$((pass+1)) ;;
  *) failed "seed test in spec/testcmd/rules" "$out" ;;
esac

# SPEC "Toolchain": seed run skips a -- before the program's arguments (only
# the first one).
out=$("$seed" run "$here/spec/program_args.sd" -- -x -- two 2>&1)
if [ "$out" = "$(printf '4 true\n-x\n--\ntwo')" ]; then pass=$((pass+1)); else failed "seed run program_args.sd -- -x -- two" "$out"; fi

# SPEC "Concurrency": reading stdin is a stop point. spec/io_stdin_stop_point.sd
# reads from a FIFO whose writer stays open for 10 seconds without writing;
# the failing sibling must stop the reader long before that.
mkfifo "$tmp/stdin"
(sleep 10 > "$tmp/stdin" & echo $! > "$tmp/sleeper.pid")
out=$("$seed" run "$here/spec/io_stdin_stop_point.sd" < "$tmp/stdin" 2>&1)
kill "$(cat "$tmp/sleeper.pid")" 2>/dev/null
if [ "$out" = "$(printf 'enough\ntrue')" ]; then pass=$((pass+1)); else failed "io.readLine is a stop point" "$out"; fi

# docs/formatting.md "The command": seed fmt --check prints the first line
# that differs and the hint to stderr and exits with 1; it exits with 0 for
# a formatted file. A file with syntax errors is not rewritten, its errors
# are printed and the exit code is 1.
mkdir -p "$tmp/fmt"
cp "$here/full/errors/unformatted_code.sd" "$tmp/fmt/x.sd"
cp "$here/lang/hello.sd" "$tmp/fmt/ok.sd"
cp "$here/core/errors/statement_newline.sd" "$tmp/fmt/bad.sd"
out=$(cd "$tmp/fmt" && "$seed" fmt --check x.sd 2>&1 >/dev/null); code=$?
if [ "$code" = 1 ] && [ "$out" = "$(printf 'x.sd:9: not formatted\n    hint: run: seed fmt x.sd')" ]; then
  pass=$((pass+1))
else
  failed "seed fmt --check x.sd" "exit $code: $out"
fi
if (cd "$tmp/fmt" && "$seed" fmt --check ok.sd > /dev/null 2>&1); then pass=$((pass+1)); else failed "seed fmt --check ok.sd" "exit code not 0"; fi
out=$(cd "$tmp/fmt" && "$seed" fmt bad.sd 2>&1); code=$?
if [ "$code" = 1 ] && cmp -s "$tmp/fmt/bad.sd" "$here/core/errors/statement_newline.sd" &&
  case $out in *"bad.sd:5:15: error: expected the end of the statement"*) true ;; *) false ;; esac; then
  pass=$((pass+1))
else
  failed "seed fmt on a file with syntax errors" "exit $code: $out"
fi
if (cd "$tmp/fmt" && "$seed" fmt --check bad.sd > /dev/null 2>&1); then failed "seed fmt --check bad.sd" "exit code 0 for a file with syntax errors"; else pass=$((pass+1)); fi
# seed fmt takes files: a directory is an error that says so, rather than
# a failed read.
out=$(cd "$tmp/fmt" && "$seed" fmt . 2>&1); code=$?
if [ "$code" = 1 ] && [ "$out" = "error: seed fmt: . is a directory; name the .sd files, such as seed fmt ./*.sd" ]; then
  pass=$((pass+1))
else
  failed "seed fmt on a directory" "exit $code: $out"
fi

# seed test on a package's tests/ directory (which holds no package with
# tests of its own) names the package directory to use instead of reporting
# "0 passed, 0 failed".
out=$(cd "$here/testcmd" && "$seed" test pass/calc/tests 2>&1); code=$?
if [ "$code" = 1 ] && [ "$out" = "error: seed test: pass/calc/tests holds the tests of a package; name the package's directory instead: seed test pass/calc" ]; then
  pass=$((pass+1))
else
  failed "seed test on a tests directory" "exit $code: $out"
fi

# seed test -v prints each package and each test as it runs; --run runs
# only the tests whose description contains the text, and is an error when
# it matches none; unknown options (Go's -run) are usage errors.
out=$(cd "$here" && "$seed" test -v testcmd/pass 2>&1); code=$?
want='testing testcmd/pass/calc
PASS "sum adds every element"
PASS "private names are visible to tests"
PASS "assert narrows"
testing testcmd/pass/calc/inner
PASS "add"
PASS "odd error"
5 passed, 0 failed'
if [ "$code" = 0 ] && [ "$out" = "$want" ]; then pass=$((pass+1)); else failed "seed test -v" "exit $code: $out"; fi
out=$(cd "$here" && "$seed" test --run add testcmd/pass 2>&1); code=$?
if [ "$code" = 0 ] && [ "$out" = "2 passed, 0 failed" ]; then pass=$((pass+1)); else failed "seed test --run add" "exit $code: $out"; fi
out=$(cd "$here" && "$seed" test -v --run "comparison" testcmd/fail 2>&1); code=$?
want='testing testcmd/fail/checks
FAIL "int comparison": testcmd/fail/checks/tests/checks_test.sd:12: assert failed: n == 42 (41 == 42)
FAIL "string comparison": testcmd/fail/checks/tests/checks_test.sd:18: assert failed: s == "abc" ("a\tb\"c" == "abc")
0 passed, 2 failed'
if [ "$code" = 1 ] && [ "$out" = "$want" ]; then pass=$((pass+1)); else failed "seed test -v --run comparison" "exit $code: $out"; fi
out=$(cd "$here" && "$seed" test --run nothing-matches testcmd/pass 2>&1); code=$?
if [ "$code" = 1 ] && [ "$out" = "$(printf '0 passed, 0 failed\nseed test: no test description contains "nothing-matches"')" ]; then
  pass=$((pass+1))
else
  failed "seed test --run matching nothing" "exit $code: $out"
fi
out=$(cd "$here" && "$seed" test -run add testcmd/pass 2>&1); code=$?
case "$code:$out" in
  "2:seed test: unknown option -run; write --run <text>"*) pass=$((pass+1)) ;;
  *) failed "seed test -run" "exit $code: $out" ;;
esac
out=$(cd "$here" && "$seed" test testcmd/pass --run 2>&1); code=$?
case "$code:$out" in
  "2:seed test: --run needs the text"*) pass=$((pass+1)) ;;
  *) failed "seed test --run without text" "exit $code: $out" ;;
esac

# A deadlock keeps its first line, then says how many goroutines are
# blocked and on what, each named by its spawn (indented under the
# goroutine that waits for its group); in a test program the goroutine is
# the test's.
out=$("$seed" run "$here/full/group_deadlock.sd" 2>&1 >/dev/null); code=$?
want='crash: deadlock: all goroutines are blocked
3 goroutines are blocked:
  main(): waiting for its group'"'"'s goroutines
    ping(): receiving from a channel
    ping(): receiving from a channel'
if [ "$code" = 2 ] && [ "$out" = "$want" ]; then pass=$((pass+1)); else failed "deadlock report" "exit $code: $out"; fi
mkdir -p "$tmp/stuck/box/tests"
printf 'package box\n\npub fn one() int {\n    return 1\n}\n' > "$tmp/stuck/box/box.sd"
printf 'package box\n\ntest "one" {\n    assert(one() == 1)\n}\n\ntest "stuck" {\n    chan int c = chan int(0)\n    int? v = c.receive()\n    assert(v == null)\n}\n' > "$tmp/stuck/box/tests/box_test.sd"
out=$(cd "$tmp/stuck" && "$seed" test --run stuck 2>&1); code=$?
want='crash: deadlock: all goroutines are blocked
2 goroutines are blocked:
  the test runner: waiting for its group'"'"'s goroutines
    test "stuck": receiving from a channel
FAIL box: the test program ended with exit code 2
0 passed, 1 failed'
if [ "$code" = 1 ] && [ "$out" = "$want" ]; then pass=$((pass+1)); else failed "deadlock in a test" "exit $code: $out"; fi

# A file that is not formatted and has other errors reports those errors
# first, then that it is not formatted, so that formatting does not hide them.
mkdir -p "$tmp/unfmt"
printf 'package main\n\nimport "fmt"\n\nfn main([]string args) error {\n    []int xs = []int{1}\n    fmt.println(len(xs))\n    int n=2\n    return null\n}\n' > "$tmp/unfmt/u.sd"
out=$(cd "$tmp/unfmt" && "$seed" build u.sd 2>&1); code=$?
errs=$(printf '%s\n' "$out" | grep ': error: ')
want='u.sd:7:17: error: undefined function len
u.sd:8:9: error: n is declared but never used
u.sd:8:1: error: u.sd is not formatted'
if [ "$code" = 1 ] && [ "$errs" = "$want" ]; then pass=$((pass+1)); else failed "unformatted file with other errors" "exit $code: $out"; fi

# SPEC "Toolchain": seed build of a single file names the executable after
# the file, or as -o says; seed ast prints the syntax tree.
mkdir -p "$tmp/buildfile"
if (cd "$tmp/buildfile" && "$seed" build "$here/spec/program_args.sd" > /dev/null 2>&1) && [ "$("$tmp/buildfile/program_args")" = "1 true" ]; then
  pass=$((pass+1))
else
  failed "seed build program_args.sd" "no working executable $tmp/buildfile/program_args"
fi
if "$seed" build -o "$tmp/buildfile/named" "$here/spec/program_args.sd" > /dev/null 2>&1 && [ "$("$tmp/buildfile/named" a)" = "$(printf '2 true\na')" ]; then
  pass=$((pass+1))
else
  failed "seed build -o" "no working executable $tmp/buildfile/named"
fi
case $("$seed" ast "$here/lang/hello.sd" 2>&1) in
  *"(file"*"(fn main"*) pass=$((pass+1)) ;;
  *) failed "seed ast hello.sd" "no syntax tree" ;;
esac

# seed run <dir> passes the arguments and the exit code.
out=$(cd "$here/testcmd" && "$seed" run pass one two 2>&1)
if [ "$out" = "$(printf 'sum 6\narg one\narg two')" ]; then pass=$((pass+1)); else failed "seed run pass one two" "$out"; fi

# seed build without -o names the executable after the directory.
mkdir -p "$tmp/build"
if (cd "$tmp/build" && "$seed" build "$here/testcmd/pass" > /dev/null 2>&1) && [ "$("$tmp/build/pass")" = "sum 6" ]; then
  pass=$((pass+1))
else
  failed "seed build" "no working executable $tmp/build/pass"
fi

# seed found through $PATH as a symbolic link, without SEED_ROOT: it
# resolves its real path to find runtime/ and lib/.
mkdir -p "$tmp/bin"
ln -s "$seed" "$tmp/bin/seed"
out=$(cd "$tmp" && env -u SEED_ROOT PATH="$tmp/bin:$PATH" seed run "$here/full/os_crash.sd" 2>&1)
case $out in
  *"crash: check: n (-2) must not be negative"*) pass=$((pass+1)) ;;
  *) failed "seed through a symbolic link on PATH" "$out" ;;
esac

# Compiling an unformatted file is an error that names seed fmt.
out=$("$seed" build -o "$tmp/x" "$here/full/errors/unformatted_code.sd" 2>&1)
case $out in
  *"hint: run: seed fmt "*) pass=$((pass+1)) ;;
  *) failed "unformatted file" "$out" ;;
esac

# Two concurrent `seed run`s of programs with the same name use temporary
# directories of their own, and leave nothing behind in $TMPDIR.
mkdir -p "$tmp/same/a" "$tmp/same/b" "$tmp/tmpdir"
for x in a b; do
  printf 'package main\n\nimport "fmt"\nimport "time"\n\nfn main([]string args) error {\n    time.sleep(100 * time.millisecond)\n    fmt.println("%s")\n    return null\n}\n' "$x" > "$tmp/same/$x/prog.sd"
done
TMPDIR="$tmp/tmpdir" "$seed" run "$tmp/same/a/prog.sd" > "$tmp/same/a.out" 2>&1 &
TMPDIR="$tmp/tmpdir" "$seed" run "$tmp/same/b/prog.sd" > "$tmp/same/b.out" 2>&1 &
wait
if [ "$(cat "$tmp/same/a.out")" = a ] && [ "$(cat "$tmp/same/b.out")" = b ]; then pass=$((pass+1)); else failed "concurrent seed run" "$(cat "$tmp/same/a.out" "$tmp/same/b.out")"; fi
(cd "$here" && TMPDIR="$tmp/tmpdir" "$seed" test testcmd/pass > /dev/null 2>&1)
(cd "$tmp/build" && TMPDIR="$tmp/tmpdir" "$seed" build "$here/testcmd/pass" > /dev/null 2>&1)
left=$(ls "$tmp/tmpdir" | grep '^seed-' | grep -v '^seed-cache-')
if [ -z "$left" ]; then pass=$((pass+1)); else failed "temporary directories removed" "left: $left"; fi

# The runtime is compiled once into a cache: $SEED_ROOT/build/cache, or
# $TMPDIR/seed-cache-$USER when that cannot be written. A root whose build/
# is read-only uses the second.
if ls "$root"/build/cache/runtime-*.o > /dev/null 2>&1; then pass=$((pass+1)); else failed "runtime cache" "no $root/build/cache/runtime-*.o"; fi
mkdir -p "$tmp/fakeroot/build" "$tmp/tmp2"
ln -s "$root/runtime" "$tmp/fakeroot/runtime"
ln -s "$root/lib" "$tmp/fakeroot/lib"
chmod 555 "$tmp/fakeroot/build"
out=$(SEED_ROOT="$tmp/fakeroot" TMPDIR="$tmp/tmp2" USER=seedtest "$seed" run "$tmp/same/a/prog.sd" 2>&1)
if [ -w "$tmp/fakeroot/build" ]; then
  # root ignores the mode: build/ stays writable, so there is no fallback to see
  pass=$((pass+1))
elif [ "$out" = a ] && ls "$tmp"/tmp2/seed-cache-seedtest/runtime-*.o > /dev/null 2>&1; then
  pass=$((pass+1))
else
  failed "runtime cache fallback" "$out; $(ls -R "$tmp/tmp2")"
fi
chmod 755 "$tmp/fakeroot/build"

# On a terminal, stdout is line-buffered and flushed before reading stdin:
# testcmd/tty_flush.sd runs under script (a pseudo-terminal); "tick" must
# show while it sleeps, and its prompt while it waits for a line. The
# feeder watches the typescript, records what it saw in time, then types
# the line. (BSD script on macOS, util-linux script elsewhere.)
tty_feeder() { # TYPESCRIPT RESULT
  : > "$2"
  for want in "tick 15" "name? 30"; do
    text=${want% *}; tenths=${want##* }; i=0
    until grep -qF "$text" "$1" 2>/dev/null; do
      i=$((i+1))
      if [ "$i" -gt "$tenths" ]; then echo "missed '$text'" >> "$2"; break; fi
      sleep 0.1
    done
  done
  echo Ada
}
tty_script() { # TYPESCRIPT PROGRAM
  if [ "$(uname)" = Darwin ]; then script -q -F "$1" "$2"; else script -q -f -e -c "$2" "$1"; fi
}
if command -v script > /dev/null 2>&1; then
  if "$seed" build -o "$tmp/tty_flush" "$here/testcmd/tty_flush.sd" > "$tmp/tty.err" 2>&1; then
    tty_feeder "$tmp/tty.out" "$tmp/tty.result" | tty_script "$tmp/tty.out" "$tmp/tty_flush" > /dev/null 2>&1
    if [ ! -s "$tmp/tty.result" ] && grep -qF "hello Ada" "$tmp/tty.out"; then
      pass=$((pass+1))
    else
      failed "stdout on a terminal" "$(cat "$tmp/tty.result"); typescript: $(cat "$tmp/tty.out")"
    fi
  else
    failed "build testcmd/tty_flush.sd" "$(cat "$tmp/tty.err")"
  fi
fi

# make install links $(PREFIX)/bin/seed to the compiler; uninstall removes it.
if [ "$seed" = "$root/build/seed2" ]; then
  make -s -C "$root" install PREFIX="$tmp/prefix" > /dev/null 2>&1
  out=$(cd "$tmp" && env -u SEED_ROOT "$tmp/prefix/bin/seed" run "$tmp/same/b/prog.sd" 2>&1)
  make -s -C "$root" uninstall PREFIX="$tmp/prefix" > /dev/null 2>&1
  if [ "$out" = b ] && [ ! -e "$tmp/prefix/bin/seed" ]; then pass=$((pass+1)); else failed "make install/uninstall" "$out"; fi
fi

echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
