// JSON: build a document of about 1 MB, then print and parse it back 30
// times (encoding/json with generic values, like Seed's json.Value).
package main

import (
	"encoding/json"
	"fmt"
	"strconv"
)

func document() any {
	users := []any{}
	for i := 0; i < 6000; i++ {
		users = append(users, map[string]any{
			"id":     float64(i),
			"name":   "user " + strconv.Itoa(i),
			"email":  "user" + strconv.Itoa(i) + "@example.com",
			"score":  float64(i) * 1.5,
			"active": i%3 == 0,
			"tags":   []any{"a", "b\n\"c\""},
		})
	}
	return map[string]any{"users": users, "count": 6000.0}
}

func main() {
	doc := document()
	total := 0
	for i := 0; i < 30; i++ {
		b, err := json.Marshal(doc)
		if err != nil {
			panic(err)
		}
		var back any
		if err := json.Unmarshal(b, &back); err != nil {
			panic(err)
		}
		total += len(b)
		id := back.(map[string]any)["users"].([]any)[i].(map[string]any)["id"].(float64)
		total += int(id)
	}
	fmt.Println(total)
}
