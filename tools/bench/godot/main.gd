extends Node
func fib(n: int) -> int:
	return n if n < 2 else fib(n - 1) + fib(n - 2)
func _ready():
	var t0 := Time.get_ticks_msec()
	var s := 0
	for i in 3000000: s += i % 7
	print("loop ", Time.get_ticks_msec() - t0, " ms")
	t0 = Time.get_ticks_msec()
	print("fib ", fib(26), " ", Time.get_ticks_msec() - t0, " ms")
	t0 = Time.get_ticks_msec()
	var a := PackedFloat32Array(); a.resize(100000)
	for k in 10: for i in a.size(): a[i] = sin(i * 0.001) * k
	var d := {}
	for i in 100000: d[str(i)] = i
	print("array/dict ", Time.get_ticks_msec() - t0, " ms")
	get_tree().quit()
