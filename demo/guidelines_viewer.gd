extends CanvasLayer

@export var character_path: NodePath
@export var refresh_every_frame: bool = true
@export var show_overlay: bool = false

@onready var texture_rect: TextureRect = $TextureRect

func _ready() -> void:
	# Default to hidden now that we have 3D in-world debug lines.
	visible = show_overlay
	texture_rect.visible = show_overlay

	var character = _resolve_character()
	if character != null:
		# Force GPU mode for demo bring-up.
		if character.has_method("set_debug_draw_hair_lines"):
			character.call("set_debug_draw_hair_lines", false)

	if show_overlay:
		_update_texture()
	if (not show_overlay) or (not refresh_every_frame):
		set_process(false)

func _process(_delta: float) -> void:
	if show_overlay:
		_update_texture()

func _resolve_character() -> Node:
	if character_path != NodePath(""):
		var n = get_node_or_null(character_path)
		if n != null:
			return n

	# Fallback: look for a node named exactly "TressFXCharacter".
	return get_tree().current_scene.find_child("TressFXCharacter", true, false)

func _update_texture() -> void:
	var character = _resolve_character()
	if character == null:
		return

	if not character.has_method("get_gpu_guide_lines_texture"):
		return

	var tex = character.call("get_gpu_guide_lines_texture")
	if tex is Texture2D:
		texture_rect.texture = tex
