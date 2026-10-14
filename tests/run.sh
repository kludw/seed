#!/bin/sh
# Run Seed test programs.
#
#   tests/run.sh "<compile command>" <dir>...
#
# The compile command is given a .sd path and must write C to stdout, e.g.
#   tests/run.sh "build/seed2 c" tests/lang
#
# For every <dir>/NAME.sd:
#   NAME.out   expected stdout (required, may be empty)
#   NAME.exit  expected exit code (optional, default 0)
#   NAME.err   text that must appear in stderr (optional)
#   NAME.in    stdin for the program (optional)
# For every <dir>/errors/NAME.sd: compiling must fail, and stderr must contain
# the text after "// expect-error: " on the file's first line.
# A program made of packages is a directory <dir>/NAME/ (with NAME.out next
# to it): the compile command gets the directory, which holds package main
# and the packages it imports (NAME/cart/, ...). <dir>/errors/NAME/ is a
# directory that must fail to compile; the expected text is on the first
# line of its main.sd.
CC=${CC:-cc}
compile=$1; shift
tmp=$(mktemp -d "${TMPDIR:-/tmp}/seedtest.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
pass=0; fail=0
failed() { fail=$((fail+1)); echo "FAIL $1: $2"; }
for dir in "$@"; do
  for src in "$dir"/*.sd "$dir"/*/; do
    [ -e "$src" ] || continue
    case $src in
      */) src=${src%/}; [ -f "$src.out" ] || continue; name=$src ;;
      *) name=${src%.sd} ;;
    esac
    if ! $compile "$src" > "$tmp/t.c" 2> "$tmp/cerr"; then
      failed "$src" "compile failed: $(head -c 300 "$tmp/cerr")"; continue
    fi
    if ! $CC -std=c11 -O1 -w -o "$tmp/t" "$tmp/t.c" -lm 2> "$tmp/ccerr"; then
      failed "$src" "C compile failed: $(head -c 300 "$tmp/ccerr")"; continue
    fi
    input=/dev/null; [ -f "$name.in" ] && input="$name.in"
    "$tmp/t" < "$input" > "$tmp/out" 2> "$tmp/err"; code=$?
    want=0; [ -f "$name.exit" ] && want=$(cat "$name.exit")
    if [ "$code" != "$want" ]; then
      failed "$src" "exit code $code, want $want. stderr: $(head -c 300 "$tmp/err")"; continue
    fi
    if ! cmp -s "$tmp/out" "$name.out"; then
      failed "$src" "stdout differs:"; diff "$name.out" "$tmp/out" | head -20; continue
    fi
    if [ -f "$name.err" ] && ! grep -qF "$(cat "$name.err")" "$tmp/err"; then
      failed "$src" "stderr missing '$(cat "$name.err")', got: $(head -c 300 "$tmp/err")"; continue
    fi
    pass=$((pass+1))
  done
  for src in "$dir"/errors/*.sd "$dir"/errors/*/; do
    [ -e "$src" ] || continue
    first=$src
    case $src in */) src=${src%/}; first=$src/main.sd ;; esac
    want=$(head -1 "$first" | sed -n 's|^// expect-error: ||p')
    if $compile "$src" > /dev/null 2> "$tmp/cerr"; then
      failed "$src" "compiled, but should fail with '$want'"; continue
    fi
    if ! grep -qF "$want" "$tmp/cerr"; then
      failed "$src" "error should contain '$want', got: $(head -c 300 "$tmp/cerr")"; continue
    fi
    pass=$((pass+1))
  done
done
echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
