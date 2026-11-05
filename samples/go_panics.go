package main

import "fmt"

type T struct{ a, b int }

func try(f func()) (r any) { defer func() { r = recover() }(); f(); return nil }

func main() {
	var p *T
	fmt.Println("nil deref:", try(func() { fmt.Println(p.b) }))
	var m map[string]int
	fmt.Println("nil map:", try(func() { m["x"] = 1 }))
	var s []int
	fmt.Println("index:", try(func() { _ = s[3] }))
	a, b := 1, 0
	fmt.Println("div:", try(func() { fmt.Println(a / b) }))
	var i interface{ M() }
	fmt.Println("nil iface:", try(func() { i.M() }))
	var ptr *int
	fmt.Println("write nil:", try(func() { *ptr = 3 }))
	n := 0 // (faults in code that has been translated: hot loop)
	for i := 0; i < 3000; i++ {
		if try(func() { _ = p.b }) != nil {
			n++
		}
	}
	fmt.Println("hot nil derefs:", n)
}
