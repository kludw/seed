#!/bin/sh
# Builds and runs the runtime's tests.
#
#   runtime/tests/run.sh [all|asan|tsan|plain|bench]     (default: all)
#
#   asan   every test under -fsanitize=address,undefined, with object counting
#   tsan   the concurrency and blocking tests, the exit tests and the example under -fsanitize=thread
#   plain  every test at -O2 (runtime compiled as separate units), including the
#          long allocation stress test with its resident-memory check and the
#          stack-overflow tests (which need the runtime's own SIGSEGV handler)
#   bench  performance numbers (not part of all)
#
# Outputs go to build/runtime-tests/. Exits non-zero if anything failed.
set -u
here=$(cd "$(dirname "$0")" && pwd)
rt=$(cd "$here/.." && pwd)
root=$(cd "$rt/.." && pwd)
out="$root/build/runtime-tests"
CC=${CC:-cc}
mode=${1:-all}
failed=0

warn="-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-infinite-recursion"
libs="-lpthread -lm"

step() { # name command...
  name=$1
  shift
  if "$@"; then :; else
    echo "FAILED: $name"
    failed=$((failed + 1))
  fi
}

# build_test DIR FLAGS NAME EXTRA_SOURCES...
build_test() {
  dir=$1 flags=$2 name=$3
  shift 3
  # shellcheck disable=SC2086
  $CC -std=c11 $flags $warn -I"$rt" "$here/$name.c" "$@" "$rt/seed_all.c" -o "$dir/$name" $libs
}

run_example() { # binary
  "$1" >"$out/example.stdout" 2>/dev/null &&
    cmp -s "$out/example.stdout" "$here/example.out" &&
    echo "example: output matches example.out" ||
    { echo "example: output differs from example.out"; diff "$out/example.stdout" "$here/example.out"; return 1; }
}

run_asan() {
  d="$out/asan"
  mkdir -p "$d"
  f="-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=undefined -DSD_COUNT_OBJECTS"
  echo "== asan+ubsan"
  # shellcheck disable=SC2086
  step "build seed0_float" $CC -std=c11 $f -w -c "$here/seed0_float.c" -o "$d/seed0_float.o" || return
  for t in test_int test_mem test_pkg test_conc test_block test_exit example; do
    step "build $t" build_test "$d" "$f" $t
  done
  step "build test_str" build_test "$d" "$f" test_str "$d/seed0_float.o"
  for t in test_int test_str test_mem test_pkg test_conc test_block; do step "$t (asan)" "$d/$t"; done
  step "test_exit (asan)" "$here/test_exit.sh" "$d/test_exit" nostack
  step "example (asan)" run_example "$d/example"
}

run_tsan() {
  d="$out/tsan"
  mkdir -p "$d"
  f="-O1 -g -fsanitize=thread"
  echo "== tsan"
  for t in test_conc test_block test_exit example; do step "build $t" build_test "$d" "$f" $t; done
  step "test_conc (tsan)" "$d/test_conc" 20000
  step "test_block (tsan)" "$d/test_block" 1000
  step "test_exit (tsan)" "$here/test_exit.sh" "$d/test_exit" nostack
  step "example (tsan)" run_example "$d/example"
}

run_plain() {
  d="$out/plain"
  mkdir -p "$d"
  f="-O2 -DSD_COUNT_OBJECTS"
  echo "== plain -O2"
  # The runtime as separate translation units, warnings as errors.
  objs=""
  for u in seed_core seed_int seed_str seed_list seed_task seed_pkg; do
    # shellcheck disable=SC2086
    step "compile $u.c" $CC -std=c11 $f -Wall -Wextra -Werror -Wno-unused-parameter -c "$rt/$u.c" -o "$d/$u.o"
    objs="$objs $d/$u.o"
  done
  # shellcheck disable=SC2086
  step "build seed0_float" $CC -std=c11 $f -w -c "$here/seed0_float.c" -o "$d/seed0_float.o"
  for t in test_int test_str test_mem test_pkg test_conc test_block test_exit example; do
    extra=""
    [ $t = test_str ] && extra="$d/seed0_float.o"
    # shellcheck disable=SC2086
    step "build $t" $CC -std=c11 $f $warn -I"$rt" "$here/$t.c" $extra $objs -o "$d/$t" $libs
  done
  for t in test_int test_str test_pkg; do step "$t (plain)" "$d/$t"; done
  step "test_mem (plain, 2M rounds)" "$d/test_mem" 2000000
  step "test_conc (plain, 100k ping-pongs)" "$d/test_conc" 100000
  step "test_block (plain, 20k goroutines in one group)" "$d/test_block" 20000
  step "test_exit (plain)" "$here/test_exit.sh" "$d/test_exit"
  step "example (plain)" run_example "$d/example"
}

run_bench() {
  d="$out/bench"
  mkdir -p "$d"
  step "build bench" build_test "$d" "-O2" bench && "$d/bench"
}

mkdir -p "$out"
case $mode in
  all) run_asan; run_tsan; run_plain ;;
  asan) run_asan ;;
  tsan) run_tsan ;;
  plain) run_plain ;;
  bench) run_bench ;;
  *) echo "usage: $0 [all|asan|tsan|plain|bench]"; exit 2 ;;
esac

if [ $failed -ne 0 ]; then
  echo "runtime tests: $failed step(s) FAILED"
  exit 1
fi
echo "runtime tests: all passed ($mode)"
