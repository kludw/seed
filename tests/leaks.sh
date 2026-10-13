#!/bin/sh
# Run Seed test programs under macOS `leaks --atExit` and report every
# program that leaks.
#
#   tests/leaks.sh "<compile command>" <dir>...
#
# For example: tests/leaks.sh "build/seed2 c" tests/lang tests/full
# Programs that crash are expected to leak (the locals of the unwound frames,
# see runtime/README.md section 15); they are reported separately.
CC=${CC:-cc}
compile=$1; shift
tmp=$(mktemp -d "${TMPDIR:-/tmp}/seedleaks.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
clean=0; leaky=0; crashed=0
for dir in "$@"; do
  for src in "$dir"/*.sd; do
    [ -e "$src" ] || continue
    name=${src%.sd}
    $compile "$src" > "$tmp/t.c" 2> /dev/null || continue
    $CC -std=c11 -O1 -g -w -o "$tmp/t" "$tmp/t.c" -lm -lpthread || continue
    input=/dev/null; [ -f "$name.in" ] && input="$name.in"
    MallocStackLogging=1 leaks --atExit -- "$tmp/t" < "$input" > "$tmp/report" 2>&1
    summary=$(grep -E 'leaks? for' "$tmp/report")
    case "$summary" in
      *" 0 leaks for"*) clean=$((clean+1)) ;;
      *)
        if [ -f "$name.exit" ] && [ "$(cat "$name.exit")" = 2 ]; then
          crashed=$((crashed+1)); echo "crash (expected to leak) $src: $summary"
        else
          leaky=$((leaky+1)); echo "LEAK $src: $summary"
        fi ;;
    esac
  done
done
echo "$clean without leaks, $leaky leaking, $crashed crashing"
[ "$leaky" = 0 ]
