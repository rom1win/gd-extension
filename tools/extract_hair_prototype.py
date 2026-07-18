# B1 prototype: headless hair extraction from a .blend (no add-on, no export dialog).
#   blender --background --python tools/extract_hair_prototype.py
# Reads the evaluated hair Curves object (Geometry Nodes applied), pulls
# per-strand point positions, radii, and custom attributes, and writes a
# JSON intermediate next to the blend. Prints a summary for eyeballing.

import json
import os

import bpy

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BLEND = os.path.join(REPO, "test_assets", "hair_test.blend")
OUT = os.path.join(REPO, "test_assets", "hair_test_extracted.json")


def attr_values(attrs, name):
    a = attrs.get(name)
    if a is None:
        return None
    out = []
    for d in a.data:
        if hasattr(d, "vector"):
            out.append(tuple(round(c, 6) for c in d.vector))
        elif hasattr(d, "value"):
            out.append(round(d.value, 6))
        elif hasattr(d, "color"):
            out.append(tuple(round(c, 6) for c in d.color))
    return out


bpy.ops.wm.open_mainfile(filepath=BLEND)

hair = next(o for o in bpy.data.objects if o.type == "CURVES")
deps = bpy.context.evaluated_depsgraph_get()
ev = hair.evaluated_get(deps)
cu = ev.data

n_curves = len(cu.curves)
n_points = len(cu.points)
positions = attr_values(cu.attributes, "position")
radii = attr_values(cu.attributes, "radius")
twist = attr_values(cu.attributes, "test_twist")          # per-curve
width = attr_values(cu.attributes, "test_width")          # per-point
surface_uv = attr_values(cu.attributes, "surface_uv_coordinate")  # per-curve

# per-curve point ranges (offsets into the flat point arrays)
offsets = []
acc = 0
for c in cu.curves:
    try:
        npts = c.points_length
    except AttributeError:
        npts = len(c.points)
    offsets.append((acc, npts))
    acc += npts

mw = [list(row) for row in hair.matrix_world]

doc = {
    "source_blend": BLEND,
    "blender_version": bpy.app.version_string,
    "object": hair.name,
    "matrix_world": mw,
    "num_strands": n_curves,
    "num_points_total": n_points,
    "strand_offsets": offsets,
    "positions": positions,
    "radius": radii,
    "test_twist_per_curve": twist,
    "test_width_per_point": width,
    "surface_uv_per_curve": surface_uv,
}
with open(OUT, "w") as f:
    json.dump(doc, f)

print("[EXTRACT] blender:", bpy.app.version_string)
print("[EXTRACT] object:", hair.name)
print("[EXTRACT] strands:", n_curves, " points:", n_points)
print("[EXTRACT] strand 0 points:", positions[:offsets[0][1]] if positions else None)
print("[EXTRACT] radius first 8:", radii[:8] if radii else "ABSENT")
print("[EXTRACT] test_twist first 5:", twist[:5] if twist else "ABSENT")
print("[EXTRACT] test_width first 8:", width[:8] if width else "ABSENT")
print("[EXTRACT] surface_uv first 3:", surface_uv[:3] if surface_uv else "ABSENT")
print("[EXTRACT] wrote:", OUT, " bytes:", os.path.getsize(OUT))
