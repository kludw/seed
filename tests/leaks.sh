#!/bin/sh
# Run Seed test programs under a leak checker and report every program that
# leaks: macOS `leaks --atExit`, or LeakSanitizer (-fsanitize=leak)
# elsewhere.
#
#   tests/leaks.sh "<compile command>" <dir>...
#
# For example: tests/leaks.sh "build/seed2 c" tests/lang tests/full
# Programs that crash, or whose goroutines crash or are stopped, must not
# leak either: the cleanup frames release what the unwound frames held
# (runtime/README.md, section 9). A program with a NAME.leaks file is
# expected to leak, for the reason the file gives; it is reported
# separately (there are none at the moment).
CC=${CC:-cc}
compile=$1; shift
tmp=$(mktemp -d "${TMPDIR:-/tmp}/seedleaks.XXXXXX")
# SD_PLAIN_MALLOC (implied by SD_LEAK_CHECK): every object is its own
# malloc block, which the checkers can see (the runtime's small-object
# allocator would hide them; runtime/README.md, section 15).
if [ "$(uname)" = Darwin ]; then checker=leaks; sanflags=-DSD_PLAIN_MALLOC; else checker=lsan; sanflags="-fsanitize=leak -DSD_LEAK_CHECK"; fi
trap 'rm -rf "$tmp"' EXIT
clean=0; leaky=0; expected=0
for dir in "$@"; do
  for src in "$dir"/*.sd "$dir"/*/; do
    [ -e "$src" ] || continue
    case $src in
      */) src=${src%/}; [ -f "$src.out" ] || continue; name=$src ;;
      *) name=${src%.sd} ;;
    esac
    $compile "$src" > "$tmp/t.c" 2> /dev/null || continue
    $CC -std=c11 -O1 -g -w $sanflags -o "$tmp/t" "$tmp/t.c" -lm -lpthread || continue
    input=/dev/null; [ -f "$name.in" ] && input="$name.in"
    if [ $checker = leaks ]; then
      MallocStackLogging=1 leaks --atExit -- "$tmp/t" < "$input" > "$tmp/report" 2>&1
      summary=$(grep -E 'leaks? for' "$tmp/report")
    else
      LSAN_OPTIONS=exitcode=0 "$tmp/t" < "$input" > "$tmp/report" 2>&1
      # A leak report, or LeakSanitizer itself failing (it needs ptrace).
      summary=$(grep -E 'LeakSanitizer' "$tmp/report" | tail -1)
      [ -z "$summary" ] && summary=" 0 leaks for $src"
    fi
    case "$summary" in
      *" 0 leaks for"*) clean=$((clean+1)) ;;
      *)
        if [ -f "$name.leaks" ]; then
          expected=$((expected+1)); echo "expected to leak ($(cat "$name.leaks")) $src: $summary"
        else
          leaky=$((leaky+1)); echo "LEAK $src: $summary"
        fi ;;
    esac
  done
done
echo "$clean without leaks, $leaky leaking, $expected expected to leak"
[ "$leaky" = 0 ]
