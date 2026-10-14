// Integer arithmetic in a tight loop.
package main

import "fmt"

func main() {
	n := 300000000
	sum := 0
	for i := 0; i < n; i++ {
		sum = sum + (i*i)%7 + (i & 3)
	}
	fmt.Println(sum)
}
