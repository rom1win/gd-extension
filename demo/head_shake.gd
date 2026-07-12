extends Skeleton3D
## Testing tool for A3: procedurally shakes the head bone every frame so
## hair/skinning can be validated under real motion. babylon.tscn has no
## AnimationPlayer (the source glTF's one clip was never carried over when
## this scene was hand-assembled), so this substitutes for a walk/idle clip
## -- and doubles as the "violent head-shake" stress test the A3 gate itself
## calls for. Not part of the TressFX product.

@export var bone_name: String = "frenchHornMonster_head_JNT"
@export var amplitude_degrees: float = 35.0
@export var frequency_hz: float = 2.0 # violent shake
@export var shaking: bool = true

var _bone_idx: int = -1
var _rest_rotation: Quaternion
var _t: float = 0.0

func _ready() -> void:
	_bone_idx = find_bone(bone_name)
	if _bone_idx < 0:
		push_warning("HeadShake: bone not found: " + bone_name)
		return
	_rest_rotation = get_bone_pose_rotation(_bone_idx)

func _process(delta: float) -> void:
	if _bone_idx < 0 or not shaking:
		return
	_t += delta
	var yaw := deg_to_rad(amplitude_degrees) * sin(_t * TAU * frequency_hz)
	var pitch := deg_to_rad(amplitude_degrees) * 0.4 * sin(_t * TAU * frequency_hz * 1.3 + 0.7)
	var shake := Quaternion(Vector3.UP, yaw) * Quaternion(Vector3.RIGHT, pitch)
	set_bone_pose_rotation(_bone_idx, _rest_rotation * shake)
