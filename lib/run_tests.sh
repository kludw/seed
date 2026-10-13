#!/bin/sh
# Runs the tests of the pure-Seed standard library packages.
#
#   lib/run_tests.sh [--update] [pkg...]
#
# The compiler does not support user packages, imports of them, or `test`
# blocks yet, so each package is tested as part of a main program. For every
# package this script:
#
#   1. copies lib/<pkg>/*.sd, the files of the packages it imports (see deps
#      below), lib/<pkg>/tests/*_test.sd and lib/testkit/testkit.sd into a
#      temporary directory, rewriting `package <pkg>` to `package main`,
#      deleting `import "<dep>"` lines and the `<dep>.` qualifiers of the
#      lib packages it uses;
#   2. generates a main.sd that runs every `fn testX() TestLog` in the test
#      files, and runs one `fn crashX()` when its name is the first argument;
#   3. compiles everything with `seed c --strict` and the C compiler, runs it,
#      then runs each crash test in its own process, recording its exit code
#      and stderr;
#   4. compares the whole output with lib/<pkg>/tests/expected.out
#      (--update rewrites that file instead).
#
# It also checks that every .sd file under lib/ is formatted (seed fmt).
# Environment: SEED (default build/seed2), CC (default cc), KEEP=1 keeps
# the temporary directory (each package's merged sources, C and binary).
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SEED=${SEED:-$ROOT/build/seed2}
CC=${CC:-cc}
ALL="strings path math encoding rand crypto json http"

# The lib packages each package imports (not the core ones).
deps() {
  case $1 in
    http) echo "encoding json" ;;
    *) echo "" ;;
  esac
}

update=0
if [ "${1:-}" = "--update" ]; then
  update=1
  shift
fi
pkgs=${*:-$ALL}

if [ ! -x "$SEED" ]; then
  echo "run_tests.sh: $SEED not found; run make build/seed2 first" >&2
  exit 2
fi

tmp=$(mktemp -d "${TMPDIR:-/tmp}/seedlib.XXXXXX")
if [ "${KEEP:-}" = 1 ]; then
  echo "keeping $tmp"
else
  trap 'rm -rf "$tmp"' EXIT
fi
pass=0
fail=0

if ! "$SEED" fmt --check "$ROOT"/lib/*/*.sd "$ROOT"/lib/*/tests/*.sd; then
  echo "FAIL formatting: run  build/seed2 fmt lib/*/*.sd lib/*/tests/*.sd"
  fail=$((fail + 1))
fi

# rewrite SRC DST DEPS: copies a source file into the merged program.
rewrite() {
  DEPS="$3" perl -pe '
    BEGIN { @d = split " ", $ENV{DEPS}; }
    s/^package [a-z]+$/package main/ if $. == 1;
    for my $d (@d) {
      $_ = "" if /^import "\Q$d\E"$/;
      s/(?<![\w.])\Q$d\E\.(?=[A-Za-z])//g;
    }
  ' "$1" > "$2"
}

for pkg in $pkgs; do
  dir="$ROOT/lib/$pkg"
  if [ ! -d "$dir" ]; then
    echo "FAIL $pkg: no directory lib/$pkg"
    fail=$((fail + 1))
    continue
  fi
  work="$tmp/$pkg"
  mkdir -p "$work"
  d=$(deps "$pkg")
  for f in "$dir"/*.sd; do
    rewrite "$f" "$work/$pkg-$(basename "$f")" "$d"
  done
  for dep in $d; do
    for f in "$ROOT/lib/$dep"/*.sd; do
      rewrite "$f" "$work/dep-$dep-$(basename "$f")" "$(deps "$dep")"
    done
  done
  for f in "$dir"/tests/*_test.sd; do
    rewrite "$f" "$work/test-$(basename "$f")" "$d"
  done
  cp "$ROOT/lib/testkit/testkit.sd" "$work/testkit.sd"

  tests=$(cat "$dir"/tests/*_test.sd | sed -n 's/^fn \(test[A-Z][A-Za-z0-9]*\)() TestLog {$/\1/p')
  crashes=$(cat "$dir"/tests/*_test.sd | sed -n 's/^fn \(crash[A-Z][A-Za-z0-9]*\)() {$/\1/p')
  {
    echo "package main"
    echo
    echo "import \"fmt\""
    echo
    echo "fn main([]string args) error {"
    if [ -n "$crashes" ]; then
      echo "    if (args.len() > 1) {"
      for c in $crashes; do
        echo "        if (args[1] == \"$c\") {"
        echo "            $c()"
        echo "        }"
      done
      echo "        return null"
      echo "    }"
    fi
    echo "    int failed = 0"
    echo "    int total = 0"
    for t in $tests; do
      echo "    failed = failed + testReport(\"$t\", $t())"
      echo "    total++"
    done
    echo "    fmt.println(total.string() + \" tests, \" + failed.string() + \" failed\")"
    echo "    return null"
    echo "}"
  } > "$work/main.sd"

  if ! "$SEED" c --strict "$work"/*.sd > "$work/prog.c" 2> "$work/cerr"; then
    echo "FAIL $pkg: seed compile failed:"
    head -40 "$work/cerr"
    fail=$((fail + 1))
    continue
  fi
  if ! $CC -std=c11 -O2 -w -o "$work/prog" "$work/prog.c" -lm -lpthread 2> "$work/ccerr"; then
    echo "FAIL $pkg: C compile failed:"
    head -20 "$work/ccerr"
    fail=$((fail + 1))
    continue
  fi
  "$work/prog" > "$work/out" 2> "$work/err" < /dev/null
  code=$?
  if [ "$code" != 0 ]; then
    echo "exit $code: $(head -c 500 "$work/err")" >> "$work/out"
  fi
  for c in $crashes; do
    "$work/prog" "$c" > /dev/null 2> "$work/cerr" < /dev/null
    echo "crash $c: exit $?: $(head -c 300 "$work/cerr")" >> "$work/out"
  done

  expected="$dir/tests/expected.out"
  if [ "$update" = 1 ]; then
    cp "$work/out" "$expected"
    echo "updated lib/$pkg/tests/expected.out ($(grep -c '^ok ' "$work/out") passing tests)"
    pass=$((pass + 1))
    continue
  fi
  if [ -f "$expected" ] && cmp -s "$work/out" "$expected" && ! grep -q '^FAIL' "$work/out"; then
    echo "ok   $pkg: $(grep -c '^ok ' "$work/out") tests, $(echo $crashes | wc -w | tr -d ' ') crash tests"
    pass=$((pass + 1))
  else
    echo "FAIL $pkg: output differs from lib/$pkg/tests/expected.out"
    if [ -f "$expected" ]; then
      diff "$expected" "$work/out" | head -40
    else
      cat "$work/out"
    fi
    fail=$((fail + 1))
  fi
done

echo "$pass packages passed, $fail failed"
[ "$fail" = 0 ]
