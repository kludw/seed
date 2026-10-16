#!/bin/sh
# Tests of `seed lsp`, the language server (compiler/lsp*.sd), driven over
# stdin and stdout with scripted JSON-RPC sessions.
#
#   tests/test_lsp.sh <seed> [--update]
#
# tests/lsp/driver/ (a Seed program, built first) turns a session script
# into framed messages and checks what the server answers: every message
# correctly framed, every request answered exactly once.
#
# - For every tests/lsp/NAME.lsp (the script format is in
#   tests/lsp/driver/main.sd), the transcript of the server's messages must
#   equal NAME.out (--update rewrites it), the server must exit with 0
#   after shutdown and exit, and its stderr must be empty, or contain the
#   text of NAME.err when there is one.
# - Fuzz sessions: each file of the fixture project, and the program of
#   tests/lsp/defaults (struct fields with default values), is typed in,
#   three bytes at a time, with hover, definition, completion, symbols and
#   formatting asked at every step. Every request must be answered, and
#   nothing may crash (a crash would be contained, but is logged to
#   stderr, which must stay empty).
#
# The documents are in tests/lsp/proj (a program and its package shop of
# two files, with tests), tests/lsp/edits (versions of them for
# didChange), tests/lsp/programs and tests/lsp/defaults.
set -u
seed=$1
update=${2:-}
case $seed in /*) ;; *) seed=$(pwd)/$seed ;; esac
dir=$(cd "$(dirname "$0")/lsp" && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/seedlsp.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
pass=0
fail=0
failed() { fail=$((fail+1)); echo "FAIL $1: $2"; }

if ! "$seed" build -o "$tmp/driver" "$dir/driver" > "$tmp/build.out" 2>&1; then
  echo "FAIL: tests/lsp/driver does not compile:"; cat "$tmp/build.out"; exit 1
fi

# run <label> <session...>: frames the session, runs the server on it and
# checks the transcript (into $tmp/transcript). Returns 1 after reporting a
# failure.
run() {
  label=$1; shift
  if ! "$tmp/driver" frame "$@" > "$tmp/in" 2> "$tmp/derr"; then
    failed "$label" "driver: $(cat "$tmp/derr")"; return 1
  fi
  "$seed" lsp < "$tmp/in" > "$tmp/out" 2> "$tmp/err"; code=$?
  if [ "$code" != 0 ]; then
    failed "$label" "seed lsp exited with $code; stderr: $(head -c 500 "$tmp/err")"; return 1
  fi
  if ! "$tmp/driver" unframe "$tmp/out" "$@" > "$tmp/transcript" 2> "$tmp/derr"; then
    failed "$label" "$(cat "$tmp/derr")"; return 1
  fi
  return 0
}

for script in "$dir"/*.lsp; do
  name=${script%.lsp}
  base=$(basename "$name")
  run "$base" script "$script" "$dir" || continue
  if [ "$update" = --update ]; then
    cp "$tmp/transcript" "$name.out"
  fi
  if ! cmp -s "$tmp/transcript" "$name.out"; then
    failed "$base" "the transcript differs from $base.out:"
    diff "$name.out" "$tmp/transcript" | head -40
    continue
  fi
  if [ -f "$name.err" ]; then
    if ! grep -qF "$(cat "$name.err")" "$tmp/err"; then
      failed "$base" "stderr does not contain '$(cat "$name.err")': $(head -c 500 "$tmp/err")"; continue
    fi
  elif [ -s "$tmp/err" ]; then
    failed "$base" "stderr: $(head -c 500 "$tmp/err")"; continue
  fi
  pass=$((pass+1))
done

for f in proj/main.sd proj/shop/cart.sd proj/shop/item.sd proj/shop/tests/cart_test.sd defaults/main.sd closures/main.sd; do
  run "fuzz $f" fuzz "$dir/$f" "$dir" 3 || continue
  if [ -s "$tmp/err" ]; then
    failed "fuzz $f" "stderr: $(head -c 500 "$tmp/err")"; continue
  fi
  pass=$((pass+1))
done

echo "test_lsp: $pass passed, $fail failed"
[ "$fail" = 0 ]
