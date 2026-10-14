// Goroutines: 20,000 goroutines in batches of 1,000 (each batch one
// WaitGroup), each sending a result on a channel.
package main

import (
	"fmt"
	"sync"
)

func main() {
	total := 0
	for batch := 0; batch < 20; batch++ {
		out := make(chan int, 1000)
		var wg sync.WaitGroup
		for i := 0; i < 1000; i++ {
			wg.Add(1)
			go func(i int) {
				defer wg.Done()
				out <- i * i % 1000
			}(batch*1000 + i)
		}
		wg.Wait()
		close(out)
		for v := range out {
			total += v
		}
	}
	fmt.Println(total)
}
