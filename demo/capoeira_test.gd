extends Node3D
## Test tool for demo/capoeira_test.tscn (Gate B part 2), not product:
## auto-plays the imported Mixamo character's animation clip so the hair sim
## gets real skeletal motion with no manual input (same spirit as
## head_shake.gd). Toggle `animate` off for a static rest-pose check.

@export var animate := true


func _ready() -> void:
	if not animate:
		return
	# owned=false: the AnimationPlayer lives inside the instanced .blend
	# scene, whose children are owned by that scene, not by this one.
	var player := find_child("AnimationPlayer", true, false) as AnimationPlayer
	if player == null:
		push_warning("capoeira_test: no AnimationPlayer found in the imported scene")
		return
	var clips := player.get_animation_list()
	if clips.is_empty():
		push_warning("capoeira_test: AnimationPlayer has no clips")
		return
	var clip: String = clips[0]
	player.get_animation(clip).loop_mode = Animation.LOOP_LINEAR
	player.play(clip)
	print("capoeira_test: playing animation clip '", clip, "' (looping)")
