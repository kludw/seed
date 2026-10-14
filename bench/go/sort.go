// Lists: push 2 million pseudo-random ints, sort them, sort structs
// stably, and read them back.
package main

import (
	"fmt"
	"sort"
)

type Item struct {
	key  int
	name string
}

func main() {
	xs := []int{}
	x := 12345
	for i := 0; i < 2000000; i++ {
		x = (x*1103515245 + 12345) % 2147483648
		xs = append(xs, x)
	}
	sort.Ints(xs)
	check := 0
	for i := 0; i < len(xs); i += 1000 {
		check = (check + xs[i]) % 1000000007
	}
	items := []Item{}
	for i := 0; i < 300000; i++ {
		x = (x*1103515245 + 12345) % 2147483648
		items = append(items, Item{key: x % 1000, name: "n"})
	}
	sort.SliceStable(items, func(a, b int) bool { return items[a].key < items[b].key })
	for i := 0; i < len(items); i += 1000 {
		check = (check + items[i].key) % 1000000007
	}
	fmt.Println(check)
}
