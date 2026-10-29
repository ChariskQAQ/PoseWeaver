extends RefCounted
func run(root: Node, packed: PackedScene, output: String) -> int:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(640, 480)
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	viewport.world_2d = World2D.new()
	root.add_child(viewport)
	var background := ColorRect.new()
	background.color = Color(0.05, 0.05, 0.05)
	background.size = Vector2(640, 480)
	viewport.add_child(background)
	var character = packed.instantiate()
	character.playing = false
	character.autoplay = "wave"
	character.position = Vector2(320, 350)
	viewport.add_child(character)
	if not character.advance(0.5): viewport.free(); return 1
	var strip := ColorRect.new()
	strip.color = Color(0.8, 0.5, 0.3)
	strip.position = Vector2(16, 16); strip.size = Vector2(144, 24)
	viewport.add_child(strip)
	for i in 3:
		var patch := ColorRect.new()
		patch.position = Vector2(16 + i * 40, 16); patch.size = Vector2(32, 24)
		patch.color = Color(0.5, 0.4, 0.8, 0.4)
		patch.material = character._materials[i]
		viewport.add_child(patch)
	await RenderingServer.frame_post_draw
	await RenderingServer.frame_post_draw
	var image := viewport.get_texture().get_image()
	var failures := 0
	var expected := [Color(0.68, 0.46, 0.50), Color(1.0, 0.66, 0.62), Color(0.64, 0.38, 0.276)]
	for i in 3:
		var actual := image.get_pixel(32 + 40 * i, 24)
		if absf(actual.r - expected[i].r) > 0.03 or absf(actual.g - expected[i].g) > 0.03 or absf(actual.b - expected[i].b) > 0.03:
			printerr("FAIL: blend %d actual=%s expected=%s" % [i, actual, expected[i]])
			failures += 1
	var visible := 0
	for y in range(80, 470, 4):
		for x in range(200, 440, 4):
			var pixel := image.get_pixel(x, y)
			if maxf(pixel.r, maxf(pixel.g, pixel.b)) > 0.15: visible += 1
	if visible < 200: failures += 1; printerr("FAIL: character rendered too few visible pixels")
	if image.save_png(output) != OK: failures += 1; printerr("FAIL: cannot save rendered evidence")
	viewport.free()
	print("Poseweaver Godot rendered player: %d failure(s)" % failures)
	return 1 if failures else 0
