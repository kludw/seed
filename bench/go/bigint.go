// Big integers: 3^2000000 by repeated squaring and fib(1000000) by fast
// doubling, each converted to decimal text, parsed back and divided.
package main

import (
	"fmt"
	"math/big"
)

func pow(b int64, e int) *big.Int {
	result := big.NewInt(1)
	x := big.NewInt(b)
	for n := e; n > 0; {
		if n&1 == 1 {
			result = new(big.Int).Mul(result, x)
		}
		n >>= 1
		if n > 0 {
			x = new(big.Int).Mul(x, x)
		}
	}
	return result
}

// fib(n) and fib(n + 1).
func fib(n int) (*big.Int, *big.Int) {
	if n == 0 {
		return big.NewInt(0), big.NewInt(1)
	}
	a, b := fib(n / 2)
	t := new(big.Int).Sub(new(big.Int).Lsh(b, 1), a)
	c := new(big.Int).Mul(a, t)
	d := new(big.Int).Add(new(big.Int).Mul(a, a), new(big.Int).Mul(b, b))
	if n%2 == 0 {
		return c, d
	}
	return d, new(big.Int).Add(c, d)
}

// The length and the first and last 12 digits of a number's text, and
// whether parsing the text gives the number back.
func summary(x *big.Int) string {
	s := x.String()
	back, _ := new(big.Int).SetString(s, 10)
	return fmt.Sprintf("%d %s...%s %v", len(s), s[:12], s[len(s)-12:], back.Cmp(x) == 0)
}

func main() {
	p := pow(3, 2000000)
	fmt.Println(summary(p))
	f, f1 := fib(1000000)
	fmt.Println(summary(f))
	q := new(big.Int).Quo(p, f)
	fmt.Println(summary(q))
	fmt.Println(len(new(big.Int).Rem(p, f).String()), len(new(big.Int).Sub(f1, f).String()))
}
