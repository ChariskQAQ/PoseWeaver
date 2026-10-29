class_name PoseweaverSkeleton
extends Node2D
## Real-time bones, meshes, skins, constraints, tracks and event signals from the shared native core.
signal animation_event(event: Dictionary)
signal runtime_failed(message: String)
const SkeletonData = preload("res://addons/poseweaver_runtime/skeleton_data.gd")
@export var source: SkeletonData
@export_file("*.pwskeleton") var skeleton_path := ""
@export var autoplay := ""
@export var playing := true
var bone_names: Array = []
var skin_names: Array = []
var animation_names: Array = []
var last_error := ""
var _native: RefCounted
var _images: Array[Texture2D] = []
var _parts: Array[MeshInstance2D] = []
var _world: Array = []
var _materials: Array[Material] = []

func _init() -> void:
	for mode in [CanvasItemMaterial.BLEND_MODE_MIX, CanvasItemMaterial.BLEND_MODE_ADD]:
		var material := CanvasItemMaterial.new()
		material.blend_mode = mode
		_materials.append(material)
	var multiply := ShaderMaterial.new()
	multiply.shader = preload("res://addons/poseweaver_runtime/multiply.gdshader")
	_materials.append(multiply)

func _ready() -> void:
	if source != null: load_source(source)
	elif not skeleton_path.is_empty(): load_skeleton(skeleton_path)

func _call(request: Dictionary) -> Dictionary:
	if _native == null: return _fail("The Windows x64 native runtime is unavailable")
	var result = JSON.parse_string(_native.call("exchange", JSON.stringify(request)))
	if not result is Dictionary or not result.get("ok", false):
		return _fail(str(result.get("error", "Invalid native reply")) if result is Dictionary else "Invalid native reply")
	last_error = ""
	return result

func _fail(message: String) -> Dictionary:
	last_error = message
	runtime_failed.emit(message)
	return {"ok": false, "error": message}

func load_skeleton(path: String) -> bool:
	if ResourceLoader.exists(path, "PackedScene"):
		var packed := load(path) as PackedScene
		if packed == null: return bool(_fail("Invalid skeleton scene").ok)
		var node := packed.instantiate()
		var data = node.get("source")
		node.free()
		if not data is SkeletonData: return bool(_fail("Missing skeleton source").ok)
		if not load_source(data): return false
		skeleton_path = path
		return true
	if not ClassDB.class_exists("PoseweaverNative"): return bool(_fail("Install poseweaver_runtime.dll and restart Godot").ok)
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null or file.get_length() > 16 * 1024 * 1024: return bool(_fail("Missing or oversized skeleton").ok)
	var text := file.get_as_text()
	var document = JSON.parse_string(text)
	if not document is Dictionary or not document.get("images") is Array: return bool(_fail("Invalid skeleton document").ok)
	var images: Array[Texture2D] = []
	var directory := path.get_base_dir()
	if not directory.ends_with("/"): directory += "/"
	for image in document.images:
		if not image is Dictionary or not image.get("path") is String: return bool(_fail("Invalid image reference").ok)
		var stored: String = image.path
		var resolved := (directory + stored).simplify_path()
		if stored.is_absolute_path() or ":" in stored or "\\" in stored or not resolved.begins_with(directory):
			return bool(_fail("Image path escapes skeleton directory").ok)
		var texture := load(resolved) as Texture2D
		if texture == null: return bool(_fail("Cannot load image: " + resolved).ok)
		if texture.get_width() != int(image.get("width", 0)) or texture.get_height() != int(image.get("height", 0)):
			return bool(_fail("Image size changed; re-export the skeleton").ok)
		images.append(texture)
	var data := SkeletonData.new()
	data.document = text; data.images = images
	if not load_source(data): return false
	skeleton_path = path
	return true

func load_source(data: SkeletonData) -> bool:
	if data == null or data.document.to_utf8_buffer().size() > 16 * 1024 * 1024: return bool(_fail("Invalid skeleton source").ok)
	if not ClassDB.class_exists("PoseweaverNative"): return bool(_fail("Install the native runtime and restart Godot").ok)
	var document = JSON.parse_string(data.document)
	if not document is Dictionary or not document.get("images") is Array or document.images.size() != data.images.size(): return bool(_fail("Skeleton texture count changed").ok)
	for i in data.images.size():
		var image = document.images[i]
		var texture := data.images[i]
		if not image is Dictionary or texture == null or texture.get_width() != int(image.get("width", 0)) or texture.get_height() != int(image.get("height", 0)): return bool(_fail("Missing or resized skeleton texture").ok)
	var previous := _native
	_native = ClassDB.instantiate("PoseweaverNative")
	var response := _call({"op": "load", "data": data.document})
	if not response.ok: _native = previous; return false
	if not autoplay.is_empty() and not response.animations.has(autoplay):
		_native = previous
		return bool(_fail("Unknown autoplay animation: " + autoplay).ok)
	if not _call({"op": "step", "dt": 0.0}).ok: _native = previous; return false
	_images = data.images.duplicate(); source = data
	bone_names = response.bones; skin_names = response.skins; animation_names = response.animations
	if not autoplay.is_empty(): play(autoplay)
	return advance(0.0)

func play(animation: String, loop := true, track := 0, mix := 0.2) -> bool:
	return bool(_call({"op": "play", "name": animation, "loop": loop, "track": track, "mix": mix}).ok)

func queue_animation(animation: String, loop := true, track := 0, delay := 0.0, mix := 0.2) -> bool:
	return bool(_call({"op": "queue", "name": animation, "loop": loop, "track": track, "delay": delay, "mix": mix}).ok)

func set_skins(names: Array[String]) -> bool:
	return bool(_call({"op": "skin", "names": names}).ok)

func set_bone_position(bone: String, position_in_skeleton: Vector2) -> bool:
	return bool(_call({"op": "bone", "name": bone, "position": true, "x": position_in_skeleton.x, "y": -position_in_skeleton.y}).ok)

func set_bone_rotation(bone: String, degrees: float) -> bool:
	return bool(_call({"op": "bone", "name": bone, "rotate": true, "angle": -degrees}).ok)

func clear_bone(bone: String) -> bool:
	return bool(_call({"op": "clear_bone", "name": bone}).ok)

func bone_position(bone: String) -> Vector2:
	var index := bone_names.find(bone)
	if index < 0 or _world.size() < (index + 1) * 6: return Vector2.ZERO
	return Vector2(_world[index * 6 + 4], -_world[index * 6 + 5])

func set_attachment(slot: String, attachment: String) -> bool:
	return bool(_call({"op": "attachment", "name": slot, "attachment": attachment}).ok)

func clear_attachment(slot: String) -> bool:
	return bool(_call({"op": "clear_attachment", "name": slot}).ok)

func set_track_alpha(track: int, alpha: float) -> bool:
	return bool(_call({"op": "alpha", "track": track, "alpha": alpha}).ok)

func fade_track(track: int, mix := 0.2) -> bool:
	return bool(_call({"op": "empty", "track": track, "mix": mix}).ok)

func set_speed(speed: float) -> bool:
	return bool(_call({"op": "speed", "speed": speed}).ok)

func clear_tracks() -> bool:
	return bool(_call({"op": "clear"}).ok)

func _process(delta: float) -> void:
	if playing and _native != null: advance(minf(delta, 1.0))

func advance(delta: float) -> bool:
	var frame := _call({"op": "step", "dt": delta})
	if not frame.ok: return false
	_world = frame.world
	for part in _parts: part.visible = false
	for i in frame.batches.size():
		var batch: Dictionary = frame.batches[i]
		if i == _parts.size():
			var node := MeshInstance2D.new()
			_parts.append(node); add_child(node)
		var node := _parts[i]
		var vertices := PackedVector3Array()
		var uv := PackedVector2Array()
		var colors := PackedColorArray()
		var tint := Color(batch.color[0], batch.color[1], batch.color[2], batch.color[3])
		for v in range(batch.xy.size() / 2):
			vertices.append(Vector3(batch.xy[v * 2], -batch.xy[v * 2 + 1], 0))
			uv.append(Vector2(batch.uv[v * 2], batch.uv[v * 2 + 1])); colors.append(tint)
		var arrays := []; arrays.resize(Mesh.ARRAY_MAX)
		arrays[Mesh.ARRAY_VERTEX] = vertices; arrays[Mesh.ARRAY_TEX_UV] = uv
		arrays[Mesh.ARRAY_COLOR] = colors; arrays[Mesh.ARRAY_INDEX] = PackedInt32Array(batch.triangles)
		var mesh := ArrayMesh.new()
		if not batch.triangles.is_empty(): mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
		node.mesh = mesh; node.texture = _images[batch.image]; node.visible = true
		node.material = _materials[batch.blend]
	for event in frame.events: animation_event.emit(event)
	return true
