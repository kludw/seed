#!/bin/sh
# Golden tests for `seed fmt`.
#
#   tests/fmt/run.sh <seed binary> [--update]
#
# For every tests/fmt/NAME.sd, `seed fmt` must turn it into exactly
# NAME.expected, and NAME.expected must already be formatted (formatting is
# idempotent). With --update, NAME.expected is (re)written from the current
# output instead; review the diff before committing it.
seed=$1
update=$2
dir=$(dirname "$0")
tmp=$(mktemp -d "${TMPDIR:-/tmp}/seedfmt.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
pass=0; fail=0
failed() { fail=$((fail+1)); echo "FAIL $1: $2"; }
for src in "$dir"/*.sd; do
  [ -e "$src" ] || continue
  name=$(basename "$src" .sd)
  want="$dir/$name.expected"
  cp "$src" "$tmp/$name.sd"
  if ! "$seed" fmt "$tmp/$name.sd" 2> "$tmp/err"; then
    failed "$src" "seed fmt failed: $(head -c 300 "$tmp/err")"; continue
  fi
  if [ "$update" = "--update" ]; then
    cp "$tmp/$name.sd" "$want"
  fi
  if [ ! -f "$want" ]; then
    failed "$src" "missing $want"; continue
  fi
  if ! cmp -s "$tmp/$name.sd" "$want"; then
    failed "$src" "output differs from $want:"; diff "$want" "$tmp/$name.sd" | head -20; continue
  fi
  cp "$want" "$tmp/$name.sd"
  if ! "$seed" fmt --check "$tmp/$name.sd" 2> "$tmp/err"; then
    failed "$want" "not a fixed point: $(head -c 300 "$tmp/err")"; continue
  fi
  pass=$((pass+1))
done
echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
