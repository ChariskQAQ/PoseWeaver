extends SceneTree
func _initialize() -> void: call_deferred("_run")
func _run() -> void:
    var packed := load("res://example/robot.pwskeleton") as PackedScene
    var result := preload("res://tests/skeleton_test.gd").new().run(root, packed)
    quit(result)
