#!/bin/sh
# Debugging a Seed program with gdb: seed build -g, then a scripted gdb
# session (a breakpoint at a Seed line, the locals through the pretty
# printers of runtime/seed-gdb.py, a step into a call, a backtrace, next,
# finish). Skipped when gdb is not installed, or cannot run programs (no
# ptrace in a container).
#
#   tests/test_gdb.sh build/seed2
seed=${1:-build/seed2}
if ! command -v gdb > /dev/null 2>&1; then
  echo "gdb not found: tests/test_gdb.sh skipped"
  exit 0
fi
tmp=$(mktemp -d "${TMPDIR:-/tmp}/seedgdb.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
here=$(cd "$(dirname "$0")" && pwd)
if ! "$seed" build -g -o "$tmp/prog" "$here/gdb/prog.sd"; then
  echo "FAIL tests/test_gdb.sh: seed build -g failed"
  exit 1
fi
cat > "$tmp/cmds" << 'EOF'
set pagination off
set confirm off
break prog.sd:19
run
info locals
step
bt
next
next
next
p l_xs
p l_label
finish
continue
EOF
# One processor: the program's goroutines then run on one thread, which
# keeps gdb's next on the goroutine being followed.
SEED_PROCS=1 gdb -q -batch -nx -x "$here/../runtime/seed-gdb.py" -x "$tmp/cmds" "$tmp/prog" > "$tmp/out" 2>&1
if ! grep -q "^Breakpoint 1, " "$tmp/out"; then
  if grep -qi "ptrace\|not permitted\|couldn't get registers" "$tmp/out"; then
    echo "gdb cannot run programs here: tests/test_gdb.sh skipped"
    exit 0
  fi
fi
pass=0
fail=0
expect() {
  if grep -qF -- "$1" "$tmp/out"; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    echo "FAIL tests/test_gdb.sh: the session has no '$1'"
  fi
}
expect "Breakpoint 1, sd_f_main"          # the breakpoint at a Seed line, in main
expect "prog.sd:19"
expect "int t = total(xs, name)"          # gdb shows the Seed source
expect "l_xs = [3, 4, 5] (length 3)"      # locals, through the pretty printers
expect 'l_name = "numbers"'
expect 'l_seen = {"a": 1} (length 1)'
expect 'sd_f_total (l_xs=[3, 4, 5] (length 3), l_label="numbers") at '   # step into the call
expect "prog.sd:6"
expect "#1  "                             # the backtrace: total called from main, line 19
expect "in sd_f_main ("
expect "sum = sum + xs[i]"                # next, through the loop
expect '$1 = [3, 4, 5] (length 3)'
expect '$2 = "numbers"'
expect "Value returned is"                # finish: total returned 12
expect "= 12"
expect "numbers: 12"
if [ "$fail" != 0 ]; then
  echo "--- the gdb session:"
  cat "$tmp/out"
fi
echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
