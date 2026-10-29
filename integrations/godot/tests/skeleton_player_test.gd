extends Node
@export var robot_scene: PackedScene
func _ready() -> void:
    var result := preload("res://tests/skeleton_test.gd").new().run(self, robot_scene)
    var arguments := OS.get_cmdline_user_args()
    var render := arguments.find("--render-output")
    if result == 0 and render >= 0 and render + 1 < arguments.size():
        result = await preload("res://tests/skeleton_render_test.gd").new().run(self, robot_scene, arguments[render + 1])
    print("Poseweaver Godot exported player: %s" % ("passed" if result == 0 else "FAILED"))
    get_tree().quit(result)
