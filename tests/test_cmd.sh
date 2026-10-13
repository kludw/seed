#!/bin/sh
# Tests of the seed command itself: `seed test`, `seed build`, `seed run`,
# and finding the root through $PATH and a symbolic link.
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

echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
