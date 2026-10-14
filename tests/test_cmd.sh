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
if [ "$out" = a ] && ls "$tmp"/tmp2/seed-cache-seedtest/runtime-*.o > /dev/null 2>&1; then
  pass=$((pass+1))
else
  failed "runtime cache fallback" "$out; $(ls -R "$tmp/tmp2")"
fi
chmod 755 "$tmp/fakeroot/build"

# On a terminal, stdout is line-buffered and flushed before reading stdin:
# testcmd/tty_flush.sd runs under script (a pseudo-terminal); "tick" must
# show while it sleeps, and its prompt while it waits for a line. The
# feeder watches the typescript, records what it saw in time, then types
# the line. (macOS script; skipped elsewhere.)
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
if [ "$(uname)" = Darwin ] && command -v script > /dev/null 2>&1; then
  if "$seed" build -o "$tmp/tty_flush" "$here/testcmd/tty_flush.sd" > "$tmp/tty.err" 2>&1; then
    tty_feeder "$tmp/tty.out" "$tmp/tty.result" | script -q -F "$tmp/tty.out" "$tmp/tty_flush" > /dev/null 2>&1
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
