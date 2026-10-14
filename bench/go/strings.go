// String building: strconv, concatenation, appending in a loop, join,
// split and contains.
package main

import (
	"fmt"
	"strconv"
	"strings"
)

func main() {
	total := 0
	for round := 0; round < 60; round++ {
		parts := []string{}
		for i := 0; i < 100000; i++ {
			parts = append(parts, "item-"+strconv.Itoa(i))
		}
		joined := strings.Join(parts, ",")
		back := strings.Split(joined, ",")
		acc := ""
		var b strings.Builder // acc = acc + s is quadratic in Go; Seed appends in place
		for _, s := range back {
			b.WriteString(s)
			if strings.Contains(s, "99") {
				total++
			}
		}
		acc = b.String()
		total = total + len(acc) + len(joined)
	}
	fmt.Println(total)
}
