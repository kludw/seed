// Channels: 100,000 round trips between two goroutines over unbuffered
// channels, then 1 million values through a buffered channel.
package main

import (
	"fmt"
	"sync"
)

func main() {
	ping := make(chan int)
	pong := make(chan int)
	results := make(chan int, 2)
	var wg sync.WaitGroup
	wg.Add(2)
	go func() {
		defer wg.Done()
		for v := range ping {
			pong <- v + 1
		}
		close(pong)
	}()
	go func() {
		defer wg.Done()
		sum := 0
		for i := 0; i < 100000; i++ {
			ping <- i
			sum += <-pong
		}
		close(ping)
		results <- sum
	}()
	wg.Wait()
	buffered := make(chan int, 1024)
	wg.Add(2)
	go func() {
		defer wg.Done()
		for i := 0; i < 1000000; i++ {
			buffered <- i
		}
		close(buffered)
	}()
	go func() {
		defer wg.Done()
		sum := 0
		for v := range buffered {
			sum += v
		}
		results <- sum
	}()
	wg.Wait()
	fmt.Println(<-results + <-results)
}
