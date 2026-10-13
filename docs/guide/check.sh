#!/bin/sh
# Checks every example program of the guide against its expected output.
#
#   docs/guide/check.sh            (from anywhere; uses build/seed2)
#   SEED="build/seed2" docs/guide/check.sh
#
# For every examples/NAME.sd (compiled with `seed c --strict`):
#   NAME.out   expected stdout (required, may be empty)
#   NAME.exit  expected exit code (optional, default 0)
#   NAME.err   text that must appear in stderr (optional)
# For every examples/errors/NAME.sd: compiling with --strict must fail, and
# stderr must be exactly NAME.err (the compiler runs inside examples/errors,
# so the messages name the file without a directory, as the guide shows them).
#
# For examples/fmt/NAME.sd (deliberately unformatted): `seed fmt --check`
# must print exactly NAME.check, and `seed fmt` must produce NAME.expected.
# Every other example, pending ones included, must be formatted.
# examples/pending/ holds programs for features the compiler does not
# implement yet (concurrency, packages, tests); they are not run.
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

for src in "$dir"/*.sd; do
  [ -e "$src" ] || continue
  name=${src%.sd}
  short=${src#"$root"/}
  if ! "$seed" c --strict "$src" > "$tmp/t.c" 2> "$tmp/cerr"; then
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
  if [ -f "$name.err" ] && ! grep -qF "$(cat "$name.err")" "$tmp/err"; then
    failed "$short" "stderr missing '$(cat "$name.err")', got: $(head -c 300 "$tmp/err")"; continue
  fi
  pass=$((pass+1))
done

for src in "$dir"/errors/*.sd; do
  [ -e "$src" ] || continue
  name=${src%.sd}
  base=$(basename "$src")
  short=${src#"$root"/}
  if (cd "$dir/errors" && "$seed" c --strict "$base" > /dev/null 2> "$tmp/cerr"); then
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
# syntax errors and cannot be formatted. pending/11_shop/cart/cart.sd uses
# `pub` struct fields, which the parser does not accept yet.
find "$dir" -name '*.sd' ! -name '*syntax*' ! -path '*/examples/fmt/*' ! -path '*/11_shop/cart/cart.sd' | sort > "$tmp/files"
if ! xargs "$seed" fmt --check < "$tmp/files" 2> "$tmp/fmterr"; then
  failed "seed fmt --check" "$(cat "$tmp/fmterr")"
fi

echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
