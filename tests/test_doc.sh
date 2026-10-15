#!/bin/sh
# Tests of `seed doc` (compiler/doc.sd, doc_html.sd).
#
#   tests/test_doc.sh <seed> [--update]
#
# - Text: every command of tests/doc/commands (one per line, run in
#   tests/doc, where proj/ is a project with the packages geo and units)
#   is run; its output (stdout and stderr, with the repository written as
#   $ROOT) and exit code must equal tests/doc/commands.out (--update
#   rewrites it). The commands cover a package, one name of it and its
#   members, a standard library package and function, a directory of
#   packages, --check, and the errors.
# - HTML: `seed doc --html` of the standard library and of proj must write
#   index.html, linking every package's page, and pages whose links all
#   lead to a page that exists and an anchor in it; geo's page must link to
#   units' Meter, http's to json's Value.
# - The standard library must pass `seed doc --check lib`.
set -u
seed=$1
update=${2:-}
case $seed in /*) ;; *) seed=$(pwd)/$seed ;; esac
dir=$(cd "$(dirname "$0")/doc" && pwd)
root=$(cd "$dir/../.." && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/seeddoc.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
pass=0
fail=0
failed() { fail=$((fail+1)); echo "FAIL $1"; }

# Text output.
: > "$tmp/out"
while IFS= read -r cmd; do
  case $cmd in ''|'#'*) continue ;; esac
  echo "== seed doc $cmd" >> "$tmp/out"
  # shellcheck disable=SC2086
  (cd "$dir" && "$seed" doc $cmd) > "$tmp/one" 2>&1
  code=$?
  sed "s|$root|\$ROOT|g" "$tmp/one" >> "$tmp/out"
  echo "-- exit $code" >> "$tmp/out"
done < "$dir/commands"
if [ "$update" = --update ]; then
  cp "$tmp/out" "$dir/commands.out"
fi
if cmp -s "$tmp/out" "$dir/commands.out"; then
  pass=$((pass+1))
else
  failed "text: the output differs from tests/doc/commands.out:"
  diff "$dir/commands.out" "$tmp/out" | head -40
fi

# HTML.
html=$tmp/html
if (cd "$dir" && "$seed" doc --html -o "$html" "$root/lib" proj) > "$tmp/html.out" 2>&1; then
  ok=1
  for page in $(sed -n 's/.*<td><a href="\([^"#]*\)">.*/\1/p' "$html/index.html"); do
    if [ ! -f "$html/$page" ]; then
      ok=0; echo "index.html links to $page, which is missing"
    fi
  done
  pages=$(ls "$html" | grep -c '\.html$')
  rows=$(grep -c '<tr>' "$html/index.html")
  if [ "$pages" != $((rows + 1)) ]; then
    ok=0; echo "index.html lists $rows packages, but there are $pages pages"
  fi
  for f in "$html"/*.html; do
    for link in $(grep -o 'href="[^"]*#[^"]*"' "$f" | sed 's/^href="//; s/"$//' | sort -u); do
      page=${link%%#*}
      anchor=${link#*#}
      [ -z "$page" ] && page=$(basename "$f")
      if [ ! -f "$html/$page" ] || ! grep -q "id=\"$anchor\"" "$html/$page"; then
        ok=0; echo "$(basename "$f") links to $link, which does not exist"
      fi
    done
  done
  grep -q 'href="units.html#Meter"' "$html/geo.html" || { ok=0; echo "geo.html does not link to units.html#Meter"; }
  grep -q 'href="json.html#Value"' "$html/http.html" || { ok=0; echo "http.html does not link to json.html#Value"; }
  grep -q '<h1>package fmt</h1>' "$html/fmt.html" || { ok=0; echo "fmt.html has no title"; }
  if [ $ok = 1 ]; then pass=$((pass+1)); else failed "html"; fi
else
  failed "html: seed doc --html failed:"; cat "$tmp/html.out"
fi

# The standard library's doc comments.
if "$seed" doc --check "$root/lib" > "$tmp/check.out" 2>&1; then
  pass=$((pass+1))
else
  failed "seed doc --check lib:"; cat "$tmp/check.out"
fi

echo "test_doc: $pass passed, $fail failed"
[ "$fail" = 0 ]
