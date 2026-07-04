# Gate 1 check — GDScript side.
# Loads the RatBoy mohawk with the same parameters as the C++ demo
# (follow=1, tip_separation=1.0, radius=0.0) and writes sampled values to
# res://gate1_gdscript.txt in the same format the C++ loader writes
# res://gate1_cpp.txt. Compare with tools/compare_gate1.py.
#
# Run (from the repo root, adjust the Godot binary path):
#   <godot> --headless --path demo -s res://tests/gate1_dump.gd
extends SceneTree

const TFXAsset := preload("res://addons/tressfx/tfx_asset.gd")

func _init() -> void:
	var asset = TFXAsset.new()
	if not asset.load_from_files(
			"res://Meshes/HairAsset/Ratboy/Ratboy_mohawk.tfx",
			"res://Meshes/HairAsset/Ratboy/Ratboy_mohawk.tfxbone",
			1, 1.0, 0.0):
		push_error("gate1_dump: asset load failed")
		quit(1)
		return

	var vps: int = asset.num_verts_per_strand
	var ts: int = asset.num_total_strands
	var lines := PackedStringArray()
	lines.append("counts guides=%d vps=%d follow=%d total_strands=%d total_vertices=%d" %
		[asset.num_guide_strands, vps, asset.num_follow_per_guide, ts, asset.num_total_vertices])

	var pos_samples := [
		[0, 0], [0, 1], [0, 2], [0, vps - 1],
		[1, 0], [1, vps - 1], [2, 0], [200, vps / 2],
		[ts - 2, 0], [ts - 1, vps - 1],
	]
	for sv in pos_samples:
		var i: int = (sv[0] * vps + sv[1]) * 4
		lines.append("pos s=%d v=%d %.6f %.6f %.6f %.6f" %
			[sv[0], sv[1], asset.positions[i], asset.positions[i + 1], asset.positions[i + 2], asset.positions[i + 3]])

	var rest_samples := [[0, 0], [0, vps - 2], [0, vps - 1], [200, vps / 2]]
	for sv in rest_samples:
		lines.append("rest s=%d v=%d %.6f" % [sv[0], sv[1], asset.rest_lengths[sv[0] * vps + sv[1]]])

	for s in [0, 1, 2, 3]:
		var i: int = s * 4
		lines.append("offset s=%d %.6f %.6f %.6f %.6f" %
			[s, asset.follow_root_offsets[i], asset.follow_root_offsets[i + 1],
			asset.follow_root_offsets[i + 2], asset.follow_root_offsets[i + 3]])

	var f := FileAccess.open("res://gate1_gdscript.txt", FileAccess.WRITE)
	f.store_string("\n".join(lines) + "\n")
	f.close()
	print("gate1_dump: wrote res://gate1_gdscript.txt (%d lines)" % lines.size())
	quit(0)
