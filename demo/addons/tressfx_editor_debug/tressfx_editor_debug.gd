@tool
extends EditorPlugin

var _last_root_id: int = 0
var _last_tick_ms: int = 0

func _enter_tree() -> void:
	_last_root_id = 0
	_last_tick_ms = 0

func _process(_delta: float) -> void:
	# Poll lightly; editor callbacks vary across versions and this keeps it robust.
	var now := Time.get_ticks_msec()
	if now - _last_tick_ms < 250:
		return
	_last_tick_ms = now

	var root := get_editor_interface().get_edited_scene_root()
	if root == null:
		_last_root_id = 0
		return

	var root_id := root.get_instance_id()
	var scene_changed := root_id != _last_root_id
	_last_root_id = root_id

	# Rebuild CPU debug visuals for all TressFXCharacter nodes that have debug enabled.
	# We do it on scene change and also occasionally (to catch toggles / instance overrides).
	if not scene_changed and (now % 1000) > 250:
		return

	var nodes := root.find_children("", "TressFXCharacter", true, false)
	for n in nodes:
		if n == null:
			continue
		var enabled := false
		if n.has_method("get_debug_draw_hair_lines"):
			enabled = bool(n.call("get_debug_draw_hair_lines"))
		else:
			# fallback to property access
			enabled = bool(n.get("debug_draw_hair_lines"))

		if enabled and n.has_method("rebuild_cpu_debug_visuals"):
			n.call("rebuild_cpu_debug_visuals")
