#!/bin/sh
# Builds every program in examples/ and runs the ones that can run without a
# person at the keyboard:
#
#   wordcount   on a fixed text given on stdin
#   todo        add, done, remove and list, on a list file in a temporary directory
#   server      on a free loopback port, with fetch (and curl, if installed) as clients
#
#   tests/examples.sh [seed]        (make test-examples runs it with build/seed2)
#
# Executables go to build/examples/. Exits non-zero if anything failed.
set -u
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
seed=${1:-build/seed2}
case $seed in /*) ;; *) seed=$root/$seed ;; esac
out=$root/build/examples
tmp=$(mktemp -d "${TMPDIR:-/tmp}/seedexamples.XXXXXX")
tmp=$(cd "$tmp" && pwd)
server_pid=
cleanup() {
  [ -n "$server_pid" ] && kill "$server_pid" 2>/dev/null
  rm -rf "$tmp"
}
trap cleanup EXIT
mkdir -p "$out"
pass=0; fail=0
ok() { pass=$((pass+1)); }
failed() { fail=$((fail+1)); echo "FAIL $1: $2"; }

# expect NAME WANT GOT
expect() {
  if [ "$2" = "$3" ]; then ok; else
    failed "$1" "output differs"
    printf 'want:\n%s\ngot:\n%s\n' "$2" "$3"
  fi
}

# --- build everything
for src in "$root"/examples/*.sd; do
  name=$(basename "$src" .sd)
  if (cd "$tmp" && "$seed" build -o "$out/$name" "$src") > "$tmp/build.out" 2>&1; then ok; else
    failed "build examples/$name.sd" "$(head -c 600 "$tmp/build.out")"
  fi
done

# --- wordcount: stdin, CRLF line ends, a last line without a newline
if [ -x "$out/wordcount" ]; then
  got=$(printf 'The cat and the dog.\r\n\nThe end, the END\nno newline at the end' | "$out/wordcount" 3 2>&1)
  want=$(printf '       5  the\n       3  end\n       1  and\n4 lines, 14 words, 8 distinct')
  expect "wordcount 3 < text" "$want" "$got"
  got=$("$out/wordcount" < /dev/null 2>&1)
  expect "wordcount < /dev/null" "0 lines, 0 words, 0 distinct" "$got"
fi

# --- todo: a list file in the temporary directory
if [ -x "$out/todo" ]; then
  export TODO_FILE="$tmp/todo.json"
  got=$("$out/todo" add buy milk && "$out/todo" add call mom && "$out/todo" done 1 && "$out/todo" remove 2 && "$out/todo" list)
  want=$(printf 'added item 1\nadded item 2\n  1. [x] buy milk\n  2. [ ] call mom\n  1. [x] buy milk\n  1. [x] buy milk')
  expect "todo" "$want" "$got"
  unset TODO_FILE
fi

# --- server and fetch on a free loopback port
if [ -x "$out/server" ] && [ -x "$out/fetch" ]; then
  "$out/server" 127.0.0.1:0 > "$tmp/server.out" 2> "$tmp/server.err" &
  server_pid=$!
  addr=
  for _ in $(seq 1 100); do
    addr=$(sed -n 's/^listening on //p' "$tmp/server.err")
    [ -n "$addr" ] && break
    sleep 0.05
  done
  if [ -z "$addr" ]; then
    failed "server" "did not start: $(head -c 300 "$tmp/server.err")"
  else
    base=http://$addr
    got=$("$out/fetch" "$base/" "$base/counter" "$base/nope" 2>&1 | sed 's/, [0-9]* ms$//; s/ in [0-9]* ms$//')
    want=$(printf '%s/: 200 OK, 16 bytes\n%s/counter: 200 OK, 11 bytes\n%s/nope: 404 Not Found, 17 bytes\n3 URLs' "$base" "$base" "$base")
    expect "fetch against server" "$want" "$got"
    got=$("$out/fetch" "http://127.0.0.1:1/" "https://example.com/" 2>&1 | sed 's/, [0-9]* ms$//; s/ in [0-9]* ms$//')
    want=$(printf 'http://127.0.0.1:1/: failed: http.get: http://127.0.0.1:1/: connection refused\nhttps://example.com/: failed: http.get: https://example.com/: https is not supported\n2 URLs')
    expect "fetch errors" "$want" "$got"
    if command -v curl > /dev/null 2>&1; then
      got=$(curl -s -X POST "$base/counter?by=5"; echo; curl -s "$base/counter"; echo
            curl -s -d '{"name": "Ada"}' "$base/greet"; echo; curl -s -d 'nope' "$base/greet"
            curl -s -o /dev/null -w '%{http_code}\n' -X DELETE "$base/counter")
      want=$(printf '{"value":5}\n{"value":5}\n{"greeting":"hello, Ada"}\njson.parse: line 1, column 1: invalid literal (expected null)\n405')
      expect "curl against server" "$want" "$got"
      got=$(curl -s "$base/time")
      case $got in
        '{"now":"'*'","uptimeSeconds":'*'}') ok ;;
        *) failed "curl /time" "unexpected $got" ;;
      esac
    fi
    kill "$server_pid"
    wait "$server_pid" 2>/dev/null
    server_pid=
  fi
fi

echo "examples: $pass passed, $fail failed"
[ "$fail" = 0 ]
