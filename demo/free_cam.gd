extends Camera3D
## Free-fly debug camera: WASD to move, mouse to look, Shift to move faster,
## Escape to release the mouse cursor (click the view to recapture it).
## Testing tool only -- not part of the TressFX product.

@export var move_speed: float = 3.0
@export var fast_multiplier: float = 4.0
@export var mouse_sensitivity: float = 0.003

var _yaw: float = 0.0
var _pitch: float = 0.0
var _mouse_captured: bool = true

func _ready() -> void:
	_yaw = rotation.y
	_pitch = rotation.x
	Input.mouse_mode = Input.MOUSE_MODE_CAPTURED

func _input(event: InputEvent) -> void:
	if event is InputEventMouseButton and event.pressed and not _mouse_captured:
		Input.mouse_mode = Input.MOUSE_MODE_CAPTURED
		_mouse_captured = true
		return

	if event is InputEventKey and event.pressed and event.keycode == KEY_ESCAPE:
		Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
		_mouse_captured = false
		return

	if event is InputEventMouseMotion and _mouse_captured:
		_yaw -= event.relative.x * mouse_sensitivity
		_pitch -= event.relative.y * mouse_sensitivity
		_pitch = clamp(_pitch, -1.5, 1.5)
		rotation = Vector3(_pitch, _yaw, 0.0)

func _process(delta: float) -> void:
	var dir := Vector3.ZERO
	if Input.is_key_pressed(KEY_W):
		dir -= transform.basis.z
	if Input.is_key_pressed(KEY_S):
		dir += transform.basis.z
	if Input.is_key_pressed(KEY_A):
		dir -= transform.basis.x
	if Input.is_key_pressed(KEY_D):
		dir += transform.basis.x
	if Input.is_key_pressed(KEY_E):
		dir += Vector3.UP
	if Input.is_key_pressed(KEY_Q):
		dir -= Vector3.UP

	if dir.length_squared() > 0.0:
		dir = dir.normalized()
		var speed := move_speed
		if Input.is_key_pressed(KEY_SHIFT):
			speed *= fast_multiplier
		global_position += dir * speed * delta
