#!/bin/sh
# Checks the example programs: builds every program, runs each case below
# and compares what it printed with examples/expected/CASE.out, runs the
# tests of every project directory, and checks that every file is formatted.
#
#   examples/run.sh [seed]          (default: build/seed2)
#   examples/run.sh [seed] --update (rewrites the expected files)
#
# An expected file holds the program's stdout, then, only when there was
# any, "--- stderr" and the stderr, then, only when it was not 0,
# "--- exit N". Programs run in examples/, so they see testdata/ and
# expected/ by relative paths, and with $TMPDIR set to a scratch directory.
#
# make test-examples runs this script after tests/examples.sh, which checks
# the examples that need a temporary file or a running server (wordcount,
# todo, server and fetch).
set -u
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
seed=${1:-build/seed2}
case $seed in /*) ;; *) seed=$root/$seed ;; esac
update=no
[ "${2:-}" = "--update" ] && update=yes
bin=$root/build/examples
tmp=$(mktemp -d "${TMPDIR:-/tmp}/seedex.XXXXXX")
tmp=$(cd "$tmp" && pwd)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$bin"
pass=0; fail=0
failed() { fail=$((fail+1)); echo "FAIL $1: $2"; }

if [ ! -x "$seed" ]; then
  echo "no compiler at $seed; run make first" >&2
  exit 2
fi

# --- build every single-file example and every project directory
# (newcomer/NAME/ projects are built as newcomer-NAME)
for src in "$here"/*.sd "$here"/*/main.sd "$here"/newcomer/*/main.sd; do
  [ -e "$src" ] || continue
  case $src in */main.sd) target=$(dirname "$src"); name=$(echo "${target#"$here"/}" | tr / -) ;;
    *) name=$(basename "$src" .sd); target=$src ;; esac
  if "$seed" build -o "$bin/$name" "$target" > "$tmp/build.out" 2>&1; then
    pass=$((pass+1))
  else
    failed "build $name" "$(head -c 800 "$tmp/build.out")"
  fi
done

# check CASE [<STDIN] PROGRAM ARGS...
#   Runs build/examples/PROGRAM with ARGS (in examples/, stdin from the file
#   named after <, else /dev/null) and compares with expected/CASE.out.
check() {
  case=$1; shift
  input=/dev/null
  case $1 in "<"*) input=${1#<}; shift ;; esac
  prog=$1; shift
  if [ ! -x "$bin/$prog" ]; then
    failed "$case" "$prog was not built"; return
  fi
  (cd "$here" && TMPDIR=$tmp "$bin/$prog" "$@" < "$input" > "$tmp/out" 2> "$tmp/err"); code=$?
  {
    cat "$tmp/out"
    if [ -s "$tmp/err" ]; then echo "--- stderr"; cat "$tmp/err"; fi
    if [ "$code" != 0 ]; then echo "--- exit $code"; fi
  } > "$tmp/got"
  if [ "$update" = yes ]; then
    cp "$tmp/got" "$here/expected/$case.out"
  fi
  if cmp -s "$tmp/got" "$here/expected/$case.out"; then
    pass=$((pass+1))
  else
    failed "$case" "output differs (want examples/expected/$case.out, got below)"
    diff "$here/expected/$case.out" "$tmp/got" | head -30
  fi
}

check bigmath bigmath
check primes primes
check primes-1000 primes 1000
check life life 4
check values values
check workers workers
check grep grep sea testdata/harbour.txt
check grep-i grep -i sea testdata/harbour.txt testdata/notes.txt
check grep-count grep -c -i todo testdata/harbour.txt testdata/notes.txt
check grep-names grep -l the testdata/harbour.txt testdata/missing.txt testdata/notes.txt
check grep-invert grep -v the testdata/notes.txt
check grep-none grep whale testdata/harbour.txt
check grep-dashes grep -- "- check" testdata/notes.txt
check jsonfmt "<testdata/order.json" jsonfmt
check jsonfmt-compact "<testdata/order.json" jsonfmt -c
check jsonfmt-quiet "<testdata/order.json" jsonfmt -q
check jsonfmt-bad "<testdata/bad.json" jsonfmt
check jsonfmt-empty jsonfmt
check calc "<testdata/calc.txt" calc
check bank bank
check kvstore "<testdata/kv1.txt" kvstore "$tmp/kv.json"
check kvstore-reload "<testdata/kv2.txt" kvstore "$tmp/kv.json"
check wc wc testdata
check wc-ext wc -e .json testdata
check wc-missing wc testdata/nope
check newcomer-todo-add newcomer-todo -f "$tmp/todo.json" add buy milk
check newcomer-todo-add2 newcomer-todo -f "$tmp/todo.json" add walk the dog
check newcomer-todo-done newcomer-todo -f "$tmp/todo.json" done 1
check newcomer-todo-list newcomer-todo -f "$tmp/todo.json" list
check newcomer-todo-missing newcomer-todo -f "$tmp/todo.json" remove 7
check newcomer-linkcheck newcomer-linkcheck --demo
check newcomer-logstat newcomer-logstat newcomer/logstat/access.log
check newcomer-vending "<newcomer/vending/script.txt" newcomer-vending

# --- the tests of every project directory with a tests/ directory in it
for dir in "$here"/*/; do
  name=$(basename "$dir")
  [ -n "$(find "$dir" -type d -name tests)" ] || continue
  if (cd "$here" && "$seed" test "$name" > "$tmp/out" 2>&1); then
    pass=$((pass+1))
  else
    failed "seed test $name" "$(cat "$tmp/out")"
  fi
done

# --- every example is formatted
find "$here" -name '*.sd' | sort > "$tmp/files"
if xargs "$seed" fmt --check < "$tmp/files" 2> "$tmp/fmterr"; then
  pass=$((pass+1))
else
  failed "seed fmt --check" "$(cat "$tmp/fmterr")"
fi

echo "examples: $pass passed, $fail failed"
[ "$fail" = 0 ]
