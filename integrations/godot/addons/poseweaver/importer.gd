@tool
extends EditorImportPlugin

func _get_importer_name() -> String: return "poseweaver.frames"
func _get_visible_name() -> String: return "Poseweaver Frames"
func _get_recognized_extensions() -> PackedStringArray: return ["pwframes"]
func _get_save_extension() -> String: return "scn"
func _get_resource_type() -> String: return "PackedScene"
func _get_preset_count() -> int: return 1
func _get_preset_name(_index: int) -> String: return "Default"
func _get_import_options(_path: String, _preset_index: int) -> Array[Dictionary]: return []
func _get_import_order() -> int: return 100

func _number(value: Variant) -> bool:
	return (value is int or value is float) and is_finite(float(value))

func _integer(value: Variant) -> bool:
	return _number(value) and float(value) == floor(float(value))

func _fail(message: String) -> Error:
	push_error("Poseweaver: " + message)
	return ERR_INVALID_DATA

func _import(source_file: String, save_path: String, _options: Dictionary,
		_platform_variants: Array[String], _gen_files: Array[String]) -> Error:
	var file := FileAccess.open(source_file, FileAccess.READ)
	if file == null or file.get_length() > 4 * 1024 * 1024:
		return _fail("Cannot read manifest, or manifest exceeds 4 MiB")
	var parser := JSON.new()
	if parser.parse(file.get_as_text()) != OK or not parser.data is Dictionary:
		return _fail("Invalid JSON")
	var doc: Dictionary = parser.data
	if doc.get("format") != "poseweaver.frames" or doc.get("version") != 1:
		return _fail("Unsupported format/version")
	if doc.get("filter", "linear") not in ["linear", "nearest"]:
		return _fail("Unsupported texture filter")
	for key in ["frameWidth", "frameHeight"]:
		if not _integer(doc.get(key)) or doc[key] < 1 or doc[key] > 4096:
			return _fail("Invalid frame dimensions")
	for key in ["fps", "pixelsPerUnit", "originX", "originY"]:
		if not _number(doc.get(key)):
			return _fail("Invalid rate, scale or origin")
	if doc.fps < 1 or doc.fps > 120 or doc.pixelsPerUnit <= 0 or doc.pixelsPerUnit > 10000 or \
			doc.originX < 0 or doc.originX > doc.frameWidth or doc.originY < 0 or doc.originY > doc.frameHeight:
		return _fail("Invalid rate, scale or origin")
	if not doc.get("pages") is Array or doc.pages.is_empty() or doc.pages.size() > 10000 or \
			not doc.get("animations") is Array or doc.animations.is_empty() or doc.animations.size() > 256:
		return _fail("Empty or oversized package")
	var textures: Array[Texture2D] = []
	var directory := source_file.get_base_dir()
	if not directory.ends_with("/"): directory += "/"
	for page in doc.pages:
		if not page is Dictionary or not page.get("file") is String or page.file.is_absolute_path():
			return _fail("Invalid page path")
		var path: String = (directory + page.file).simplify_path()
		if not path.begins_with(directory) or "\\" in page.file:
			return _fail("Page path escapes package directory")
		for key in ["width", "height"]:
			if not _integer(page.get(key)) or page[key] < 1 or page[key] > 4096:
				return _fail("Invalid page dimensions")
		var png := FileAccess.get_file_as_bytes(path)
		if png.size() < 24 or png[0] != 137 or png[1] != 80 or png[2] != 78 or png[3] != 71:
			return _fail("Invalid PNG page")
		var width := (png[16] << 24) | (png[17] << 16) | (png[18] << 8) | png[19]
		var height := (png[20] << 24) | (png[21] << 16) | (png[22] << 8) | png[23]
		if width != page.width or height != page.height:
			return _fail("PNG dimensions do not match manifest")
		if append_import_external_resource(path) != OK:
			return _fail("Could not import page: " + path)
		var texture := ResourceLoader.load(path, "Texture2D", ResourceLoader.CACHE_MODE_REPLACE) as Texture2D
		if texture == null: return _fail("Could not load page: " + path)
		textures.append(texture)
	var frames := SpriteFrames.new()
	frames.remove_animation("default")
	var total := 0
	for animation in doc.animations:
		if not animation is Dictionary or not animation.get("name") is String or animation.name.is_empty() or \
				frames.has_animation(animation.name) or not animation.get("loop") is bool or \
				not _number(animation.get("duration")) or animation.duration <= 0 or \
				not animation.get("frames") is Array or animation.frames.is_empty():
			return _fail("Invalid or duplicate animation")
		total += animation.frames.size()
		if total > 10000: return _fail("Package exceeds 10000 frames")
		frames.add_animation(animation.name)
		frames.set_animation_speed(animation.name, doc.fps)
		frames.set_animation_loop(animation.name, animation.loop)
		var duration := 0.0
		for frame in animation.frames:
			if not frame is Dictionary: return _fail("Invalid frame")
			for key in ["page", "x", "y", "width", "height"]:
				if not _integer(frame.get(key)): return _fail("Invalid frame rectangle")
			if frame.page < 0 or frame.page >= textures.size() or frame.x < 0 or frame.y < 0 or \
					frame.width != doc.frameWidth or frame.height != doc.frameHeight or \
					not _number(frame.get("duration")) or frame.duration <= 0:
				return _fail("Invalid frame rectangle or duration")
			var page: Texture2D = textures[frame.page]
			if frame.x > page.get_width() - frame.width or frame.y > page.get_height() - frame.height:
				return _fail("Frame outside page")
			var atlas := AtlasTexture.new()
			atlas.atlas = page
			atlas.region = Rect2(frame.x, frame.y, frame.width, frame.height)
			frames.add_frame(animation.name, atlas, frame.duration * doc.fps)
			duration += frame.duration
		if abs(duration - animation.duration) > max(0.0001, animation.duration * 0.00001):
			return _fail("Frame durations do not match animation")
	var sprite := AnimatedSprite2D.new()
	sprite.name = "Poseweaver"
	sprite.sprite_frames = frames
	sprite.animation = doc.animations[0].name
	sprite.autoplay = sprite.animation
	sprite.offset = Vector2(doc.frameWidth * 0.5 - doc.originX, doc.frameHeight * 0.5 - doc.originY)
	sprite.texture_filter = CanvasItem.TEXTURE_FILTER_NEAREST if doc.get("filter", "linear") == "nearest" else CanvasItem.TEXTURE_FILTER_LINEAR
	var scene := PackedScene.new()
	var result := scene.pack(sprite)
	sprite.free()
	if result != OK: return result
	return ResourceSaver.save(scene, save_path + ".scn")
