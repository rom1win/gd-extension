extends CanvasLayer

@export var character_path: NodePath
@export var refresh_every_frame: bool = true

@onready var texture_rect: TextureRect = $TextureRect

func _ready() -> void:
	_update_texture()
	if not refresh_every_frame:
		set_process(false)

func _process(_delta: float) -> void:
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

	# This property is backed by TressFXCharacter::get_gpu_guide_lines_texture().
	var tex = character.get("gpu_guide_lines_texture")
	if tex is Texture2D:
		texture_rect.texture = tex