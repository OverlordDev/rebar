extends Node2D
# CPU-bound game-engine workload: 2D rigid bodies in a box, stepped as fast as possible (headless), then a 3D transform/array workload.
var frames := 0
var t0 := 0
func _ready():
	var rng := RandomNumberGenerator.new(); rng.seed = 7
	for w in [Rect2(-10, 600, 1300, 20), Rect2(-10, -10, 20, 700), Rect2(1270, -10, 20, 700)]:
		var b := StaticBody2D.new(); var c := CollisionShape2D.new(); var s := RectangleShape2D.new(); s.size = w.size; c.shape = s; b.add_child(c); b.position = w.position + w.size / 2; add_child(b)
	for i in 250:
		var b := RigidBody2D.new(); var c := CollisionShape2D.new(); var s := CircleShape2D.new(); s.radius = 8 + rng.randi() % 6; c.shape = s; b.add_child(c)
		b.position = Vector2(rng.randf_range(50, 1200), rng.randf_range(-300, 400)); add_child(b)
	t0 = Time.get_ticks_msec()
func _physics_process(_d):
	frames += 1
	if frames == 240:
		print("physics ", Time.get_ticks_msec() - t0, " ms")
		var t1 := Time.get_ticks_msec()
		var xf := Transform3D(); var acc := Vector3.ZERO
		for i in 200000:
			xf = xf.rotated(Vector3.UP, 0.001); acc += xf * Vector3(i, 1, 2)
		print("transform3d ", Time.get_ticks_msec() - t1, " ms")
		get_tree().quit()
