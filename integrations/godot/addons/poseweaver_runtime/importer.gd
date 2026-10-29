@tool
extends EditorImportPlugin
const Player = preload("res://addons/poseweaver_runtime/skeleton.gd")
const SkeletonData = preload("res://addons/poseweaver_runtime/skeleton_data.gd")
func _get_importer_name() -> String: return "poseweaver.skeleton"
func _get_visible_name() -> String: return "Poseweaver Skeleton"
func _get_recognized_extensions() -> PackedStringArray: return ["pwskeleton"]
func _get_save_extension() -> String: return "scn"
func _get_resource_type() -> String: return "PackedScene"
func _get_preset_count() -> int: return 1
func _get_preset_name(_index: int) -> String: return "Default"
func _get_import_options(_path: String, _preset: int) -> Array[Dictionary]: return []
func _get_import_order() -> int: return 100

func _fail(message: String) -> Error:
	push_error("Poseweaver skeleton: " + message)
	return ERR_INVALID_DATA

func _dependency(directory: String, stored: String) -> String:
	var path := (directory + stored).simplify_path()
	if stored.is_empty() or stored.is_absolute_path() or ":" in stored or "\\" in stored or not path.begins_with(directory): return ""
	return path

func _import(source_file: String, save_path: String, _options: Dictionary,
		_platforms: Array[String], _generated: Array[String]) -> Error:
	var file := FileAccess.open(source_file, FileAccess.READ)
	if file == null or file.get_length() > 16 * 1024 * 1024: return _fail("Missing or oversized skeleton")
	if not ClassDB.class_exists("PoseweaverNative"): return _fail("Install the native runtime and restart Godot")
	var data := SkeletonData.new()
	data.document = file.get_as_text()
	var native: RefCounted = ClassDB.instantiate("PoseweaverNative")
	var reply = JSON.parse_string(native.call("exchange", JSON.stringify({"op": "load", "data": data.document})))
	if not reply is Dictionary or not reply.get("ok", false): return _fail(str(reply))
	var document: Dictionary = JSON.parse_string(data.document)
	var directory := source_file.get_base_dir()
	if not directory.ends_with("/"): directory += "/"
	for image in document.get("images", []):
		var path := _dependency(directory, str(image.get("path", "")))
		if path.is_empty() or append_import_external_resource(path) != OK: return _fail("Invalid image dependency")
		var texture := ResourceLoader.load(path, "Texture2D", ResourceLoader.CACHE_MODE_REPLACE) as Texture2D
		if texture == null or texture.get_width() != int(image.width) or texture.get_height() != int(image.height): return _fail("Missing or resized image: " + path)
		data.images.append(texture)
	for event in document.get("events", []):
		var stored := str(event.get("audio", ""))
		if stored.is_empty(): continue
		var path := _dependency(directory, stored)
		if path.is_empty() or append_import_external_resource(path) != OK: return _fail("Invalid audio dependency")
		var audio := ResourceLoader.load(path, "AudioStream", ResourceLoader.CACHE_MODE_REPLACE) as AudioStream
		if audio == null: return _fail("Cannot import audio: " + path)
		data.audio.append(audio)
	var root := Player.new()
	root.name = source_file.get_file().get_basename().validate_node_name()
	root.source = data; root.skeleton_path = source_file
	if not reply.animations.is_empty(): root.autoplay = reply.animations[0]
	var scene := PackedScene.new()
	var result := scene.pack(root)
	root.free()
	if result != OK: return result
	return ResourceSaver.save(scene, save_path + ".scn")
