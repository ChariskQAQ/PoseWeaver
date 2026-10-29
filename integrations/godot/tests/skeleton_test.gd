extends RefCounted
const SkeletonPlayer = preload("res://addons/poseweaver_runtime/skeleton.gd")
var failures := 0
func check(condition: bool, message: String) -> void:
	if not condition: failures += 1; printerr("FAIL: " + message)
func run(root: Node, packed: PackedScene) -> int:
	var native: PoseweaverNative = PoseweaverNative.new()
	check(not JSON.parse_string(native.exchange('{"op":"step","dt":0}')).ok, "typed native call")
	native = null
	check(packed != null, "imported PackedScene")
	if packed == null: return 1
	var imported = packed.instantiate()
	imported.playing = false
	root.add_child(imported)
	check(imported.source != null and imported.source.images.size() == 8 and imported.source.audio.size() == 1, "embedded dependencies")
	check(imported.bone_names.size() == 15 and imported.advance(0.1), "scene ready playback")
	imported.free()
	var root_scene := load("res://root-robot.pwskeleton") as PackedScene
	check(root_scene != null, "skeleton at project root")
	if root_scene != null:
		var root_character = root_scene.instantiate()
		root_character.playing = false
		root.add_child(root_character)
		check(root_character.bone_names.size() == 15 and root_character.advance(0.0), "project-root dependency paths")
		root_character.free()
	var player := SkeletonPlayer.new()
	player.playing = false
	root.add_child(player)
	check(player.load_skeleton("res://example/robot.pwskeleton"), "load " + player.last_error)
	if player.bone_names.size() != 15: printerr(player.last_error); player.free(); return 1
	check(player.skin_names.size() == 2 and player.animation_names.size() == 3, "metadata")
	check(player.play("wave", false, 0, 0.0), "play")
	var events: Array = []
	player.animation_event.connect(func(event: Dictionary): events.append(event))
	check(player.advance(0.5), "advance")
	check(player.get_child_count() == 12 and not events.is_empty(), "meshes and events")
	var image: Texture2D = player.get_child(0).texture
	check(player.set_skins(["ember"]), "skin")
	check(player.advance(0.0) and player.get_child(0).texture != image, "live skin texture")
	check(player.set_bone_position("foot_target_l", Vector2(65, 80)), "bone override")
	check(player.advance(0.0) and player.bone_position("foot_target_l").distance_to(Vector2(65, 80)) < 0.01, "IK target position")
	check(player.clear_bone("foot_target_l"), "clear override")
	check(player.play("walk", true, 0, 0.1) and player.play("wave", false, 1, 0.1), "mix tracks")
	check(player.set_track_alpha(1, 0.5) and player.advance(0.2), "track alpha")
	check(player.queue_animation("idle", true, 0), "queue")
	check(player.fade_track(1, 0.1) and player.advance(0.2), "fade")
	check(not player.play("missing"), "unknown animation rejected")
	check(not player.load_skeleton("res://example/missing.pwskeleton") and player.advance(0.0), "failed load preserves previous instance")
	var source_before = player.source
	var broken = source_before.duplicate()
	broken.document = "{}"
	check(not player.load_source(broken) and player.source == source_before and player.advance(0.0), "invalid source preserves loaded character")
	player.autoplay = "missing"
	check(not player.load_source(source_before) and player.source == source_before and player.advance(0.0), "invalid autoplay preserves loaded character")
	player.autoplay = ""
	player.free()
	print("Poseweaver Godot skeleton: %d failure(s)" % failures)
	return 1 if failures else 0
