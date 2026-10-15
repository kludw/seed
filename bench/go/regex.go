// Regular expressions over a 1.5 MB log: findAll with groups, matches on
// each line, replace and split.
package main

import (
	"fmt"
	"regexp"
	"strconv"
	"strings"
)

var words = []string{"alpha", "beta", "gamma", "delta", "running", "error", "Warning", "sing", "x", "data", "load", "ok", "timeout", "user", "WARN", "connecting"}

var hosts = []string{"example", "test", "mail", "corp"}

func pad2(n int) string {
	if n < 10 {
		return "0" + strconv.Itoa(n)
	}
	return strconv.Itoa(n)
}

func makeLog(lines int) string {
	out := []string{}
	x := 42
	for i := 0; i < lines; i++ {
		x = (x*1103515245 + 12345) % 2147483648
		month := 1 + x%12
		day := 1 + (x/16)%28
		line := []string{"2026-" + pad2(month) + "-" + pad2(day)}
		line = append(line, words[(x/7)%len(words)]+"@"+hosts[(x/3)%len(hosts)]+".com")
		n := 4 + x%6
		for k := 0; k < n; k++ {
			x = (x*1103515245 + 12345) % 2147483648
			sep := " "
			if x%5 == 0 {
				sep = ", "
			} else if x%7 == 0 {
				sep = ";  "
			}
			line = append(line, sep+words[(x/11)%len(words)]+strconv.Itoa(x%1000))
		}
		out = append(out, strings.Join(line, " "))
	}
	return strings.Join(out, "\n") + "\n"
}

func main() {
	text := makeLog(20000)
	lines := strings.Split(text, "\n")
	email := regexp.MustCompile(`\b(\w+)@(\w+)\.(com|org)\b`)
	alert := regexp.MustCompile(`(?i)error|warn`)
	date := regexp.MustCompile(`(?m)^(\d{4})-(\d{2})-(\d{2})`)
	space := regexp.MustCompile(`\s+`)
	sep := regexp.MustCompile(`[,;]\s*`)
	ing := regexp.MustCompile(`[a-z]+ing\b`)
	total := 0
	for round := 0; round < 5; round++ {
		for _, m := range email.FindAllStringSubmatch(text, -1) {
			total += len(m[1])
		}
		for _, l := range lines {
			if alert.MatchString(l) {
				total++
			}
		}
		for _, m := range date.FindAllStringSubmatch(text, -1) {
			v, _ := strconv.Atoi(m[2])
			total += v
		}
		total += len(space.ReplaceAllString(text, " "))
		total += len(sep.Split(text, -1))
		total += len(ing.FindAllString(text, -1))
	}
	fmt.Println(total)
}
