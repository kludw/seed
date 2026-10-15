#!/bin/sh
# Checks every example program of the guide against its expected output.
#
#   docs/guide/check.sh            (from anywhere; uses build/seed2)
#   SEED="build/seed2" docs/guide/check.sh
#
# For every examples/NAME.sd (compiled with `seed c`):
#   NAME.out   expected stdout (required, may be empty)
#   NAME.exit  expected exit code (optional, default 0)
#   NAME.err   lines that must appear in stderr, each within a line, in
#              this order (optional)
# For every examples/NAME/ directory (a project: package main and the
# packages it imports): `seed run NAME` must print NAME/main.out, and
# `seed test NAME` must print NAME/test.out (both run in examples/).
# For every examples/errors/NAME.sd: compiling must fail, and
# stderr must be exactly NAME.err (the compiler runs inside examples/errors,
# so the messages name the file without a directory, as the guide shows them).
#
# For examples/fmt/NAME.sd (deliberately unformatted): `seed fmt --check`
# must print exactly NAME.check, and `seed fmt` must produce NAME.expected.
# Every other example, pending ones included, must be formatted.
# examples/pending/ (if it exists) holds programs that are not run.
root=$(cd "$(dirname "$0")/../.." && pwd) || exit 1
seed=${SEED:-$root/build/seed2}
case $seed in /*) ;; *) seed=$root/$seed ;; esac
CC=${CC:-cc}
dir=$root/docs/guide/examples
if [ ! -x "$seed" ]; then
  echo "no compiler at $seed; run make first" >&2
  exit 2
fi
tmp=$(mktemp -d "${TMPDIR:-/tmp}/seedguide.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
pass=0; fail=0
failed() { fail=$((fail+1)); echo "FAIL $1: $2"; }
# has_lines WANT GOT: every non-empty line of WANT is part of a line of GOT,
# in order (as in tests/run.sh).
has_lines() {
  awk 'NR == FNR { if ($0 != "") want[++n] = $0; next }
       j <= n && index($0, want[j]) { j++ }
       END { exit !(j > n) }' j=1 "$1" "$2"
}

for src in "$dir"/*.sd; do
  [ -e "$src" ] || continue
  name=${src%.sd}
  short=${src#"$root"/}
  if ! "$seed" c "$src" > "$tmp/t.c" 2> "$tmp/cerr"; then
    failed "$short" "compile failed: $(head -c 600 "$tmp/cerr")"; continue
  fi
  if ! $CC -std=c11 -O1 -w -o "$tmp/t" "$tmp/t.c" -lm -lpthread 2> "$tmp/ccerr"; then
    failed "$short" "C compile failed: $(head -c 300 "$tmp/ccerr")"; continue
  fi
  "$tmp/t" < /dev/null > "$tmp/out" 2> "$tmp/err"; code=$?
  want=0; [ -f "$name.exit" ] && want=$(cat "$name.exit")
  if [ "$code" != "$want" ]; then
    failed "$short" "exit code $code, want $want. stderr: $(head -c 300 "$tmp/err")"; continue
  fi
  if ! cmp -s "$tmp/out" "$name.out"; then
    failed "$short" "stdout differs:"; diff "$name.out" "$tmp/out" | head -20; continue
  fi
  if [ -f "$name.err" ] && ! has_lines "$name.err" "$tmp/err"; then
    failed "$short" "stderr missing '$(cat "$name.err")', got: $(head -c 300 "$tmp/err")"; continue
  fi
  pass=$((pass+1))
done

for proj in "$dir"/*/; do
  name=$(basename "$proj")
  case $name in errors|fmt|pending) continue ;; esac
  short=${proj#"$root"/}
  if ! (cd "$dir" && "$seed" run "$name" > "$tmp/out" 2> "$tmp/err" < /dev/null); then
    failed "$short" "seed run failed: $(head -c 600 "$tmp/err")"; continue
  fi
  if ! cmp -s "$tmp/out" "$proj/main.out"; then
    failed "$short" "stdout differs:"; diff "$proj/main.out" "$tmp/out" | head -20; continue
  fi
  if [ -f "$proj/test.out" ]; then
    (cd "$dir" && "$seed" test "$name" > "$tmp/out" 2> "$tmp/err")
    if ! cmp -s "$tmp/out" "$proj/test.out"; then
      failed "$short" "seed test output differs:"; diff "$proj/test.out" "$tmp/out" | head -20; continue
    fi
  fi
  pass=$((pass+1))
done

for src in "$dir"/errors/*.sd; do
  [ -e "$src" ] || continue
  name=${src%.sd}
  base=$(basename "$src")
  short=${src#"$root"/}
  if (cd "$dir/errors" && "$seed" c "$base" > /dev/null 2> "$tmp/cerr"); then
    failed "$short" "compiled, but should fail"; continue
  fi
  if ! cmp -s "$tmp/cerr" "$name.err"; then
    failed "$short" "compile errors differ:"; diff "$name.err" "$tmp/cerr" | head -20; continue
  fi
  pass=$((pass+1))
done

# examples/fmt/NAME.sd is deliberately unformatted: `seed fmt --check` must
# print exactly NAME.check, and `seed fmt` must turn it into NAME.expected.
for src in "$dir"/fmt/*.sd; do
  [ -e "$src" ] || continue
  name=${src%.sd}
  base=$(basename "$src")
  short=${src#"$root"/}
  mkdir -p "$tmp/fmt"
  cp "$src" "$tmp/fmt/$base"
  if (cd "$tmp/fmt" && "$seed" fmt --check "$base" > /dev/null 2> "$tmp/fmtcheck"); then
    failed "$short" "seed fmt --check accepted it"; continue
  fi
  if ! cmp -s "$tmp/fmtcheck" "$name.check"; then
    failed "$short" "seed fmt --check output differs:"; diff "$name.check" "$tmp/fmtcheck" | head -20; continue
  fi
  if ! (cd "$tmp/fmt" && "$seed" fmt "$base"); then
    failed "$short" "seed fmt failed"; continue
  fi
  if ! cmp -s "$tmp/fmt/$base" "$name.expected"; then
    failed "$short" "formatted text differs:"; diff "$name.expected" "$tmp/fmt/$base" | head -20; continue
  fi
  pass=$((pass+1))
done

# Every other example must be formatted. Files named *syntax* hold deliberate
# syntax errors and cannot be formatted.
find "$dir" -name '*.sd' ! -name '*syntax*' ! -path '*/examples/fmt/*' | sort > "$tmp/files"
if ! xargs "$seed" fmt --check < "$tmp/files" 2> "$tmp/fmterr"; then
  failed "seed fmt --check" "$(cat "$tmp/fmterr")"
fi

echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
