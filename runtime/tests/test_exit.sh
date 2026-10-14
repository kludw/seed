#!/bin/sh
# Runs build/.../test_exit in each mode and checks stdout (exact), a piece of
# stderr and the exit code.  Usage: test_exit.sh BINARY [nostack]
bin=$1
nostack=$2
fail=0
count=0
out=$(mktemp)
err=$(mktemp)
trap 'rm -f "$out" "$err"' EXIT

check() { # mode expected_stdout stderr_substring expected_code
  count=$((count + 1))
  "$bin" "$1" >"$out" 2>"$err" </dev/null
  code=$?
  want=$(printf "$2")
  got=$(cat "$out")
  errok=1
  if [ -n "$3" ] && ! grep -qF -- "$3" "$err"; then errok=0; fi
  if [ "$got" != "$want" ] || [ "$code" != "$4" ] || [ $errok = 0 ]; then
    fail=$((fail + 1))
    echo "test_exit $1: FAILED"
    echo "  stdout: $(printf %s "$got" | tr '\n' '|')  (want $(printf %s "$want" | tr '\n' '|'))"
    echo "  stderr: $(tr '\n' '|' <"$err")  (want to contain: $3)"
    echo "  exit:   $code (want $4)"
  fi
}

check crash 'before\ndeferred' 'crash: index out of range: 5 with length 0' 2
check error 'main body\nmain deferred' 'error: shutting down' 1
check exit 'buffered' '' 3
check deadlock 'waiting' 'crash: deadlock: all goroutines are blocked' 2
check deadlock_group '' 'crash: deadlock' 2
check deadlock 'waiting' '  main(): receiving from a channel' 2
check deadlock_group '' '3 goroutines are blocked:' 2
check deadlock_group '' '    a goroutine: receiving from a channel' 2
check group_crash_body 'child cleanup\nmain cleanup' 'crash: division by zero' 2
check eprint 'out1 out2' 'err' 0
check timeout 'timeout\nslept' '' 0
check args "$bin args" '' 0
if [ "$nostack" != nostack ]; then
  check stack 'deferred' 'crash: stack overflow or invalid memory access' 2
  check stack_goroutine 'group error: crash: stack overflow or invalid memory access' '' 0
fi

if [ $fail -ne 0 ]; then
  echo "test_exit: $fail of $count FAILED"
  exit 1
fi
echo "test_exit: all $count modes passed"
