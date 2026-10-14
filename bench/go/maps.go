// Maps: insert 1 million string keys, look each up twice, update and
// delete some.
package main

import (
	"fmt"
	"strconv"
)

func main() {
	m := map[string]int{}
	n := 1000000
	for i := 0; i < n; i++ {
		m["key"+strconv.Itoa(i)] = i
	}
	sum := 0
	for round := 0; round < 2; round++ {
		for i := 0; i < n; i++ {
			if v, ok := m["key"+strconv.Itoa(i)]; ok {
				sum += v % 10
			}
		}
	}
	for i := 0; i < n; i += 2 {
		delete(m, "key"+strconv.Itoa(i))
	}
	counts := map[int]int{}
	for i := 0; i < n; i++ {
		counts[i%1000]++
	}
	fmt.Println(sum + len(m) + len(counts))
}
