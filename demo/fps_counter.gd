extends CanvasLayer
## Test tool, not product: live FPS overlay + periodic console average.
## Drop on any test scene as a CanvasLayer node. The console line gives
## gates a recorded perf reference (CLAUDE.md follow-up "FPS baseline
## logging"): "FPS avg (10.0s): 59.8  min-frame: 41.2".

@export var average_window_seconds := 10.0

var _label: Label
var _accum_time := 0.0
var _accum_frames := 0
var _worst_fps := INF


func _ready() -> void:
	_label = Label.new()
	_label.position = Vector2(8, 8)
	_label.add_theme_color_override("font_color", Color.YELLOW)
	_label.add_theme_color_override("font_outline_color", Color.BLACK)
	_label.add_theme_constant_override("outline_size", 4)
	add_child(_label)


func _process(delta: float) -> void:
	var fps := Engine.get_frames_per_second()
	_label.text = "%d FPS" % fps
	_accum_time += delta
	_accum_frames += 1
	if delta > 0.0:
		_worst_fps = minf(_worst_fps, 1.0 / delta)
	if _accum_time >= average_window_seconds:
		print("FPS avg (%.1fs): %.1f  min-frame: %.1f" %
				[_accum_time, _accum_frames / _accum_time, _worst_fps])
		_accum_time = 0.0
		_accum_frames = 0
		_worst_fps = INF
