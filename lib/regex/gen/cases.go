//go:build ignore

// Generates lib/regex/tests/go_cases.sd, the differential test table of
// package regex: patterns and texts with the matches Go's regexp finds in
// them (FindAllStringSubmatchIndex), which tests/go_test.sd compares with
// regex.findAll. The patterns are a hand-picked list followed by random
// ones from a small grammar (seeded, so the file is reproducible):
//
//	go run lib/regex/gen/cases.go > lib/regex/tests/go_cases.sd
package main

import (
	"fmt"
	"math/rand"
	"os"
	"regexp"
	"strings"
)

var fixed = [][2]string{
	{`a+`, "baaac aa"},
	{`a*`, "baaac"},
	{`a*?`, "baaac"},
	{`(a|b)*c`, "xxababcab"},
	{`(a*)*`, "b"},
	{`(a*)+`, "b"},
	{`(a|)*`, "aab"},
	{`(|a)*`, "aab"},
	{`(a*)*b`, "aaab"},
	{`(a+|b+)*`, "ab"},
	{`(a?)*?b`, "aab"},
	{`(?i)hello`, "say HeLLo"},
	{`(?i)k`, "k K K"},
	{`(?i)s`, "s S ſ"},
	{`(?i)[a-z]+`, "MiXeD K"},
	{`(?i)\w+`, "Kſ x"},
	{`(?i)[^k]`, "kKx"},
	{`(?i)é`, "É é"},
	{`(?i)σ`, "Σ σ ς"},
	{`^\w+$`, "hello"},
	{`(?m)^(\d+)-(\d+)$`, "12-34\n56-78"},
	{`(?m)^$`, "a\n\nb\n"},
	{`(?m)$`, "a\nb"},
	{`^`, "ab"},
	{`$`, "ab\n"},
	{`\A`, "ab"},
	{`\z`, "ab"},
	{`\bfoo\b`, "foo foobar barfoo foo"},
	{`\B`, "aé"},
	{`\b`, "ab cd"},
	{`é+`, "aééb"},
	{`.`, "aé\xffb"},
	{`[^a]`, "aé\xffb"},
	{`\x{FFFD}`, "a\xff�b"},
	{`(?s).`, "a\nb"},
	{`.`, "a\nb"},
	{`(?P<year>\d{4})-(?P<m>\d{2})`, "on 2026-10 and 1999-01"},
	{`(?<y>\d+)`, "a1b22"},
	{`x{2,3}`, "xxxxxxx"},
	{`x{2,3}?`, "xxxxxxx"},
	{`x{2}`, "xxxxx"},
	{`x{2,}`, "xxxxx x xx"},
	{`x{0}y`, "xy"},
	{`x{0,0}`, "xx"},
	{`(ab){2}`, "abababab"},
	{`(a|ab)(c|bcd)(d*)`, "abcd"},
	{`(a+)(b+)?`, "aab aa"},
	{`(a)|(b)`, "ab"},
	{`(a)|b`, "ba"},
	{``, "aé"},
	{`a|`, "ab"},
	{`|a`, "ab"},
	{`()`, "ab"},
	{`(?:)`, "ab"},
	{`[[:alpha:]]+`, "ab1cd"},
	{`[[:^digit:]]+`, "ab1cd"},
	{`[\d\s]+`, "a1 2\tb"},
	{`[^\D]+`, "a12b"},
	{`\pL+`, "abc déf 123 ПРИВЕТ 日本"},
	{`\p{Greek}+`, "abc αβγ"},
	{`\PL+`, "ab12 cd"},
	{`\p{Lu}`, "aBcÉ"},
	{`(?i)\p{Lu}+`, "aBcÉ"},
	{`\p{^L}+`, "ab12cd"},
	{`[\p{N}x]+`, "a1x2 ٣"},
	{`\Qa.b\E+`, "a.bbb axb"},
	{`a\.b`, "a.b axb"},
	{`\x41\x{42}`, "AB"},
	{`\101`, "A"},
	{`[a\-z]+`, "a-z b"},
	{`[-a]+`, "--a"},
	{`[a-]+`, "--a"},
	{`[]a]+`, "]a]"},
	{`[^]a]+`, "]a]bc"},
	{`\d+`, "ab 123 4 ٣"},
	{`\s+`, "a \t\n\v\f\rb"},
	{`\W+`, "ab, cd!"},
	{`(?U)a+`, "aaa"},
	{`(?U)a+?`, "aaa"},
	{`a(?i)b|c`, "aB C c"},
	{`a(?i:b)c`, "aBc aBC"},
	{`(?i)a(?-i)b`, "Ab AB"},
	{`(?ms)^a.b$`, "x\na\nb\ny"},
	{`(a)(?:b)(c)`, "abc"},
	{`(\w+)@(\w+)\.com`, "bob@example.com, amy@test.com"},
	{`(a*)(b*)`, "aabb ab"},
	{`(a*?)(a*)`, "aaa"},
	{`a**`, ""},
	{`*a`, ""},
	{`a(b`, ""},
	{`a)b`, ""},
	{`[a`, ""},
	{`[z-a]`, ""},
	{`\1`, ""},
	{`(?=a)`, ""},
	{`x{1001}`, ""},
	{`x{2,1}`, ""},
	{`\pX`, ""},
	{`\`, ""},
	{`a{,5}`, "a{,5}"},
	{`a{01}`, "a{01}"},
	{`{`, "{"},
	{`a{`, "a{"},
	{`a{1`, "a{1"},
	{`\Z`, ""},
	{`(?i)Δ`, "δΔ"},
	{`[\x{10000}-\x{10FFFF}]+`, "a\U0001F600\U0010FFFFb"},
	{`[\x{80}-\x{10FFFF}]`, "a\xffé"},
	{`[^\x00-\x{10FFFF}]`, "abc"},
	{`a.c`, "a\xffc abc aéc"},
	{`\b\w+\b`, "héllo wörld"},
	{`(?i)straße`, "STRASSE straße STRAßE"},
	{`(a+)+$`, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaab"},
	{`(x+x+)+y`, "xxxxxxxxxxxxxxxxxxxxxxxxxx"},
	{`(?:a|aa)*b`, "aaaaaaaaaaaaaaaaaab"},
}

const alphabet = "aab b\nA1é_-xÉ\u212ak"

func randText(r *rand.Rand) string {
	n := r.Intn(24)
	rs := []rune(alphabet)
	var b strings.Builder
	for i := 0; i < n; i++ {
		b.WriteRune(rs[r.Intn(len(rs))])
	}
	return b.String()
}

func randAtom(r *rand.Rand, depth int) string {
	switch k := r.Intn(22); {
	case k < 6:
		return string("abAx-"[r.Intn(5)])
	case k == 6:
		return "é"
	case k == 7:
		return "."
	case k == 8:
		return []string{`\d`, `\w`, `\s`, `\W`, `\D`}[r.Intn(5)]
	case k == 9:
		return []string{`[ab]`, `[^a]`, `[a-c]`, `[^\n]`, `[[:alpha:]]`, `[é-ê]`, `[\w-]`}[r.Intn(7)]
	case k == 10:
		return []string{`^`, `$`, `\b`, `\B`, `\A`, `\z`}[r.Intn(6)]
	case k == 11:
		return " "
	case k == 12:
		return `\n`
	case k < 16 && depth < 3:
		return "(" + randAlt(r, depth+1) + ")"
	case k < 18 && depth < 3:
		return "(?:" + randAlt(r, depth+1) + ")"
	case k == 18 && depth < 3:
		return "(?" + []string{"i", "m", "s", "i-s", "U"}[r.Intn(5)] + ":" + randAlt(r, depth+1) + ")"
	default:
		return "a"
	}
}

func randRepeat(r *rand.Rand, atom string) string {
	if strings.HasPrefix(atom, "(?") && strings.HasSuffix(atom, ")") && !strings.Contains(atom, ":") {
		return atom
	}
	switch k := r.Intn(14); {
	case k < 6:
		return atom
	case k < 8:
		return atom + "*"
	case k == 8:
		return atom + "+"
	case k == 9:
		return atom + "?"
	case k == 10:
		return atom + []string{"*?", "+?", "??"}[r.Intn(3)]
	case k == 11:
		return atom + fmt.Sprintf("{%d}", r.Intn(3))
	case k == 12:
		return atom + fmt.Sprintf("{%d,}", r.Intn(3))
	default:
		lo := r.Intn(3)
		s := fmt.Sprintf("{%d,%d}", lo, lo+r.Intn(3))
		if r.Intn(3) == 0 {
			s += "?"
		}
		return atom + s
	}
}

func randConcat(r *rand.Rand, depth int) string {
	n := 1 + r.Intn(4)
	var b strings.Builder
	for i := 0; i < n; i++ {
		b.WriteString(randRepeat(r, randAtom(r, depth)))
	}
	return b.String()
}

// Flags without a group, (?i), only start a group: set in the middle of
// one, before a '|', Go's factoring of common prefixes can drop them
// (A(?i)x|Aa* is parsed as A(?i:X|A*)), which regex does not copy.
func randAlt(r *rand.Rand, depth int) string {
	s := randConcat(r, depth)
	for r.Intn(4) == 0 {
		s += "|" + randConcat(r, depth)
	}
	if r.Intn(8) == 0 {
		s = "(?" + []string{"i", "m", "s"}[r.Intn(3)] + ")" + s
	}
	return s
}

// A Seed string literal for any bytes.
func lit(s string) string {
	var b strings.Builder
	b.WriteByte('"')
	for i := 0; i < len(s); i++ {
		c := s[i]
		switch {
		case c == '"' || c == '\\':
			b.WriteByte('\\')
			b.WriteByte(c)
		case c == '\n':
			b.WriteString(`\n`)
		case c == '\t':
			b.WriteString(`\t`)
		case c >= 0x20 && c < 0x7f:
			b.WriteByte(c)
		default:
			fmt.Fprintf(&b, `\x%02x`, c)
		}
	}
	b.WriteByte('"')
	return b.String()
}

func expect(pat, text string) string {
	re, err := regexp.Compile(pat)
	if err != nil {
		return "error"
	}
	var ms []string
	for _, m := range re.FindAllStringSubmatchIndex(text, -1) {
		var xs []string
		for _, x := range m {
			xs = append(xs, fmt.Sprint(x))
		}
		ms = append(ms, strings.Join(xs, " "))
	}
	return strings.Join(ms, ";")
}

func main() {
	// Optional arguments, for a larger run that is not committed: the
	// number of random cases and the seed.
	count, seed := 3000, int64(20261015)
	if len(os.Args) > 2 {
		fmt.Sscan(os.Args[1], &count)
		fmt.Sscan(os.Args[2], &seed)
	}
	r := rand.New(rand.NewSource(seed))
	var cases [][2]string
	cases = append(cases, fixed...)
	seen := map[string]bool{}
	for len(cases) < len(fixed)+count {
		p := randAlt(r, 0)
		if seen[p] {
			continue
		}
		seen[p] = true
		if _, err := regexp.Compile(p); err != nil {
			continue
		}
		for k := 0; k < 3; k++ {
			cases = append(cases, [2]string{p, randText(r)})
		}
	}
	w := os.Stdout
	fmt.Fprintln(w, "package regex")
	fmt.Fprintln(w)
	fmt.Fprintln(w, "// Generated by lib/regex/gen/cases.go from Go's regexp: do not edit.")
	fmt.Fprintln(w, "// One case per line: the pattern, the text and Go's")
	fmt.Fprintln(w, "// FindAllStringSubmatchIndex, separated by \\x1f, then \\x1e. The")
	fmt.Fprintln(w, "// matches are separated by ';', each the start and end of the match and")
	fmt.Fprintln(w, "// of every group (-1 when a group did not take part); \"error\" when Go")
	fmt.Fprintln(w, "// rejects the pattern. (One constant string compiles much faster than a")
	fmt.Fprintln(w, "// list literal of thousands of strings.)")
	// Constants of at most 200 lines: an expression may not nest more
	// than 256 levels.
	var names []string
	for start := 0; start < len(cases); start += 200 {
		end := start + 200
		if end > len(cases) {
			end = len(cases)
		}
		name := fmt.Sprintf("goCases%d", len(names))
		names = append(names, name)
		fmt.Fprintf(w, "const string %s = \"\" +\n", name)
		for i, c := range cases[start:end] {
			sep := " +"
			if start+i == end-1 {
				sep = ""
			}
			fmt.Fprintf(w, "    %s%s\n", lit(c[0]+"\x1f"+c[1]+"\x1f"+expect(c[0], c[1])+"\x1e"), sep)
		}
		fmt.Fprintln(w)
	}
	fmt.Fprintln(w, "fn goCasesText() string {")
	fmt.Fprintf(w, "    return %s\n", strings.Join(names, " + "))
	fmt.Fprintln(w, "}")
}
