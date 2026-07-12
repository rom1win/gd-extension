extends OmniLight3D
## Testing tool: orbits this light around the character so the hair's
## anisotropic highlight and shadow casting/receiving are visible without
## needing to move anything by hand. Not part of the TressFX product.

@export var center: Vector3 = Vector3(0.0, 1.0, -0.16)
@export var radius: float = 2.0
@export var height: float = 1.6
@export var angular_speed: float = 0.6 # radians/sec

var _t: float = 0.0

func _process(delta: float) -> void:
	_t += delta * angular_speed
	global_position = center + Vector3(cos(_t) * radius, height, sin(_t) * radius)
