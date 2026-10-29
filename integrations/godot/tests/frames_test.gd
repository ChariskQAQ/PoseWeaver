extends SceneTree

var failures := 0

func check(condition: bool, message: String) -> void:
	if not condition:
		failures += 1
		push_error(message)

func _initialize() -> void:
	run.call_deferred()

func run() -> void:
	var doc: Dictionary = JSON.parse_string(FileAccess.get_file_as_string("res://example/arm.pwframes"))
	var scene := load("res://example/arm.pwframes") as PackedScene
	if scene == null:
		push_error("Poseweaver scene did not import")
		quit(1)
		return
	var sprite := scene.instantiate() as AnimatedSprite2D
	check(sprite != null, "Expected native AnimatedSprite2D")
	if sprite == null:
		quit(1)
		return
	root.add_child(sprite)
	var expected_filter := CanvasItem.TEXTURE_FILTER_NEAREST if doc.get("filter", "linear") == "nearest" else CanvasItem.TEXTURE_FILTER_LINEAR
	check(sprite.texture_filter == expected_filter, "Wrong pixel texture filter")
	for animation in doc.animations:
		check(sprite.sprite_frames.has_animation(animation.name), "Missing animation")
		check(sprite.sprite_frames.get_frame_count(animation.name) == animation.frames.size(), "Wrong frame count")
		check(sprite.sprite_frames.get_animation_loop(animation.name) == animation.loop, "Wrong loop flag")
		var duration := 0.0
		for index in animation.frames.size():
			var frame: Dictionary = animation.frames[index]
			var texture := sprite.sprite_frames.get_frame_texture(animation.name, index) as AtlasTexture
			check(texture != null and texture.region == Rect2(frame.x, frame.y, frame.width, frame.height), "Wrong atlas region")
			check(texture != null and texture.atlas.resource_path == "res://example/" + doc.pages[frame.page].file, "Wrong atlas page")
			duration += sprite.sprite_frames.get_frame_duration(animation.name, index) / sprite.sprite_frames.get_animation_speed(animation.name)
		check(abs(duration - animation.duration) < 0.00001, "Wrong animation duration")
	check(sprite.offset.is_equal_approx(Vector2(doc.frameWidth * 0.5 - doc.originX, doc.frameHeight * 0.5 - doc.originY)), "Origin shifted")
	var name: String = doc.animations[0].name
	sprite.play(name)
	await create_timer(0.15).timeout
	check(sprite.is_playing() and sprite.frame > 0, "Animation did not advance")
	sprite.pause()
	var paused_frame := sprite.frame
	await create_timer(0.1).timeout
	check(sprite.frame == paused_frame, "Paused animation advanced")
	sprite.sprite_frames.set_animation_loop(name, false)
	sprite.stop()
	sprite.play(name)
	await create_timer(float(doc.animations[0].duration) + 0.15).timeout
	check(not sprite.is_playing() and sprite.frame == sprite.sprite_frames.get_frame_count(name) - 1, "Nonlooping animation did not hold last frame")
	sprite.sprite_frames.set_animation_loop(name, true)
	sprite.play(name)
	await create_timer(float(doc.animations[0].duration) + 0.15).timeout
	check(sprite.is_playing(), "Looping animation stopped")
	sprite.queue_free()
	print("Poseweaver Godot frames: %d failure(s)" % failures)
	quit(0 if failures == 0 else 1)
