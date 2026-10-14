#!/bin/sh
# Runs the benchmarks in bench/seed (and their Go versions in bench/go, when
# `go` is installed) and prints a table of the best wall-clock time of each.
#
#   bench/run.sh                 every benchmark, best of 3 runs
#   RUNS=5 bench/run.sh fib json only these, best of 5
#
# Programs are built into build/bench/ with `seed build` (cc -O2) and
# `go build`. Each pair must print the same output (a checksum).
set -u
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
seed=${SEED:-$root/build/seed2}
out=$root/build/bench
runs=${RUNS:-3}
mkdir -p "$out"
[ -x "$seed" ] || { echo "bench: $seed not found (run make first)" >&2; exit 2; }
havego=false
command -v go > /dev/null 2>&1 && havego=true

if [ $# -gt 0 ]; then
  names="$*"
else
  names=$(cd "$here/seed" && ls *.sd | sed 's/\.sd$//')
fi

# best BINARY OUTFILE: the best "real" time of $runs runs, in seconds.
best() {
  b=""
  i=0
  while [ $i -lt "$runs" ]; do
    /usr/bin/time -p "$1" > "$2" 2> "$2.time" || { echo "FAILED"; return; }
    t=$(awk '/^real/ {print $2}' "$2.time")
    if [ -z "$b" ] || awk -v t="$t" -v b="$b" 'BEGIN { exit !(t < b) }'; then b=$t; fi
    i=$((i + 1))
  done
  echo "$b"
}

printf "%-10s %10s %10s %8s  %s\n" "benchmark" "seed (s)" "go (s)" "seed/go" "same output"
for n in $names; do
  if ! "$seed" build -o "$out/$n.seed" "$here/seed/$n.sd" > "$out/$n.build" 2>&1; then
    echo "$n: seed build failed:"; cat "$out/$n.build"; continue
  fi
  ts=$(best "$out/$n.seed" "$out/$n.seed.out")
  tg="-"; ratio="-"; same="-"
  if $havego && [ -f "$here/go/$n.go" ]; then
    if (cd "$here/go" && go build -o "$out/$n.go" "$n.go") > "$out/$n.gobuild" 2>&1; then
      tg=$(best "$out/$n.go" "$out/$n.go.out")
      if cmp -s "$out/$n.seed.out" "$out/$n.go.out"; then same=yes; else same=NO; fi
      case "$ts$tg" in
        *FAILED*) ;;
        *) ratio=$(awk -v s="$ts" -v g="$tg" 'BEGIN { if (g < 0.01) g = 0.01; printf "%.1f", s / g }') ;;
      esac
    else
      tg="build failed"
    fi
  fi
  printf "%-10s %10s %10s %8s  %s\n" "$n" "$ts" "$tg" "$ratio" "$same"
done
