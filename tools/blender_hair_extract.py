"""Production headless hair extractor: .blend (Blender Curves object) -> .ghair v1.

Invocation (Blender must open the file itself, not this script):
    blender --background <file.blend> --python tools/blender_hair_extract.py -- \
        --out <file.ghair> [--object NAME] [--vps 16] [--space world]

Args after the bare `--` are parsed with argparse:
    --out      required. Output .ghair path.
    --object   Curves object name. Default: the first object of type CURVES
               found in the file.
    --vps      Vertices per strand after resampling. One of 8/16/32/64.
               Default 16.
    --space    "world" (default, apply the object's matrix_world to every
               point before resampling), "local" (raw curve-local points),
               or "armature" (armature-local space of the surface mesh's
               ARMATURE-modifier object -- USE THIS FOR RIGGED CHARACTERS:
               it is exactly the Godot-imported Skeleton3D's model space,
               the space the sim and root binding run in. Requires a
               meter-scale rig; run tools/prep_mixamo_blend.py first on
               cm-scale FBX imports).

Extraction always happens in REST pose (armatures are forced to rest before
evaluation), so a file saved mid-animation still yields the groom as
authored on the un-posed character.

Reads the EVALUATED object (via the depsgraph) so Geometry Nodes output is
included, not just the authored control points -- same approach as the B1
prototype (tools/extract_hair_prototype.py, kept as historical reference,
never modified by this script).

RESAMPLING
----------
Every strand is resampled to exactly `vps` points by uniform-arc-length
parameterization of its original polyline (linear interpolation between the
original points, arc length measured in the OUTPUT space -- i.e. after the
world-space transform, if --space=world, so spacing is uniform in the space
the numbers are written in). Point index 0 (root) and point index vps-1
(tip) are copied from the original polyline's first/last point verbatim --
never touched by interpolation -- because the root is the future bind
anchor. Per-point attributes (currently only `radius`, if present) are
resampled with the exact same segment/fraction as position, so they line up
sample-for-sample. Per-curve attributes (surface UV, twist) pass through
unresampled, one value per surviving strand.

Strands with fewer than 2 points, or whose total polyline length is ~zero
(< DEGENERATE_LENGTH_EPS in the output space), are skipped entirely (not
written to the output) and counted in the run's warning/summary output and
in META's "skipped_strand_count".

.ghair v1 FILE FORMAT
----------------------
Little-endian throughout.

Header (20 bytes):
    8 bytes   magic              ASCII "GHAIRv01"
    u32       numStrands         surviving (non-degenerate) strand count
    u32       vps                points per strand, constant for the file
    u32       numChunks

Then `numChunks` chunks, back to back:
    u32       fourcc             4 ASCII bytes, NOT nul-terminated
    u64       payloadByteLength
    <payloadByteLength bytes>

READERS MUST SKIP ANY FOURCC THEY DON'T RECOGNIZE (seek/read past
payloadByteLength and continue) -- this is what keeps the format
forward-compatible; do not assume a fixed chunk count or order.

Chunks always written:
    POS0  f32 x,y,z per point, strand-major: strand 0 point 0..vps-1, then
          strand 1 point 0..vps-1, ... . numStrands*vps*3 floats.
    RTUV  f32 u,v per strand, from the `surface_uv_coordinate` per-curve
          attribute (root UV on the scalp mesh). numStrands*2 floats. If the
          source object has no such attribute, this chunk is still written
          but filled with zeros, and META notes "surface_uv_present": false.
    META  UTF-8 JSON blob. See below for keys.

Chunks written only when the source data exists:
    WID0  f32 per point, from Blender's `radius` point attribute, resampled
          with the same parameterization as POS0. numStrands*vps floats.
          Omitted entirely if the source curves have no `radius` attribute.
    TWST  f32 per strand, from a per-curve FLOAT attribute literally named
          "twist" (NOT "test_twist" -- test_* attributes in the reference
          asset are probes for exercising the extractor and are never baked
          into the output; their presence is only noted in META).
          Omitted entirely if no "twist" attribute exists.

META JSON keys: source_blend, blender_version, object, space, vps,
num_strands (surviving), skipped_strand_count, original_point_counts (list,
one entry per ORIGINAL curve including skipped ones), matrix_world (4x4,
row-major, the object's matrix_world regardless of --space),
surface_uv_present, radius_present, twist_present, test_attributes_present
(dict of probe attribute name -> bool, informational only).
"""

import argparse
import json
import math
import struct
import sys

import bpy
import mathutils

MAGIC = b"GHAIRv01"
VALID_VPS = (8, 16, 32, 64)
DEGENERATE_LENGTH_EPS = 1e-8


def parse_args():
    argv = sys.argv
    if "--" in argv:
        argv = argv[argv.index("--") + 1:]
    else:
        argv = []
    p = argparse.ArgumentParser(description="Extract hair curves from a .blend to .ghair v1")
    p.add_argument("--out", required=True)
    p.add_argument("--object", default=None)
    p.add_argument("--vps", type=int, default=16, choices=VALID_VPS)
    p.add_argument("--space", choices=("world", "local", "armature"), default="world")
    return p.parse_args(argv)


def find_bind_armature(obj):
    """The armature whose local space becomes Godot's skeleton model space:
    the ARMATURE-modifier object of the curves' surface mesh (fallback: the
    file's single armature). Used by --space armature."""
    surf = getattr(obj.data, "surface", None)
    if surf is not None:
        for m in surf.modifiers:
            if m.type == 'ARMATURE' and m.object is not None:
                return m.object
    armatures = [o for o in bpy.data.objects if o.type == 'ARMATURE']
    if len(armatures) == 1:
        return armatures[0]
    raise SystemExit(
        "[EXTRACT] --space armature: no armature found via the curves' surface "
        f"mesh, and the file has {len(armatures)} armatures (need exactly 1)")


def find_object(name):
    if name:
        obj = bpy.data.objects.get(name)
        if obj is None or obj.type != "CURVES":
            raise SystemExit(f"[EXTRACT] object '{name}' not found or not a CURVES object")
        return obj
    obj = next((o for o in bpy.data.objects if o.type == "CURVES"), None)
    if obj is None:
        raise SystemExit("[EXTRACT] no CURVES object found in file")
    return obj


def attr_values(attrs, name):
    """Flat list of tuples (vector attrs) or floats (scalar attrs), or None if absent."""
    a = attrs.get(name)
    if a is None:
        return None
    out = []
    for d in a.data:
        if hasattr(d, "vector"):
            out.append(tuple(d.vector))
        elif hasattr(d, "value"):
            out.append(d.value)
        elif hasattr(d, "color"):
            out.append(tuple(d.color))
    return out


def curve_point_offsets(curves):
    offsets = []
    acc = 0
    for c in curves:
        try:
            npts = c.points_length
        except AttributeError:
            npts = len(c.points)
        offsets.append((acc, npts))
        acc += npts
    return offsets


def dist3(a, b):
    return math.sqrt(sum((a[k] - b[k]) ** 2 for k in range(3)))


def arc_targets(points, vps):
    """Cumulative arc length + evenly spaced target lengths for `vps` samples."""
    cum = [0.0]
    for i in range(len(points) - 1):
        cum.append(cum[-1] + dist3(points[i], points[i + 1]))
    total = cum[-1]
    targets = [total * i / (vps - 1) for i in range(vps)]
    return cum, total, targets


def resample_along(values, cum, targets):
    """Resample per-point `values` (tuples or scalars) at arc-length `targets`,
    using segment/fraction derived from `cum` (built from positions) so
    attributes line up sample-for-sample with POS0. First/last samples are
    copied verbatim, never interpolated."""
    n = len(values)
    out = []
    seg = 0
    last = len(targets) - 1
    for i, t in enumerate(targets):
        if i == 0:
            out.append(values[0])
            continue
        if i == last:
            out.append(values[-1])
            continue
        while seg < n - 2 and cum[seg + 1] < t:
            seg += 1
        l0, l1 = cum[seg], cum[seg + 1]
        frac = 0.0 if l1 <= l0 else (t - l0) / (l1 - l0)
        v0, v1 = values[seg], values[seg + 1]
        if isinstance(v0, tuple):
            out.append(tuple(v0[k] + (v1[k] - v0[k]) * frac for k in range(len(v0))))
        else:
            out.append(v0 + (v1 - v0) * frac)
    return out


def pack_chunk(fourcc, payload):
    assert len(fourcc) == 4
    return struct.pack("<4sQ", fourcc.encode("ascii"), len(payload)) + payload


def pack_floats(values):
    if not values:
        return b""
    return struct.pack("<%df" % len(values), *values)


def main():
    args = parse_args()
    obj = find_object(args.object)

    # Always evaluate in REST pose: the binder and the sim consume the groom
    # as authored on the un-animated character, but files are often saved
    # posed mid-animation, and a Surface-Deform'ed groom evaluates DEFORMED
    # at whatever pose is active. Forcing REST here makes extraction
    # pose-independent (proven on the Mixamo capoeira asset, 2026-07-23).
    rest_forced = []
    for ob in bpy.data.objects:
        if ob.type == 'ARMATURE' and ob.data.pose_position != 'REST':
            ob.data.pose_position = 'REST'
            rest_forced.append(ob.name)
    if rest_forced:
        bpy.context.view_layer.update()
        print(f"[EXTRACT] forced REST pose on armature(s): {rest_forced}")

    deps = bpy.context.evaluated_depsgraph_get()
    ev = obj.evaluated_get(deps)
    cu = ev.data

    n_curves = len(cu.curves)
    offsets = curve_point_offsets(cu.curves)

    positions_flat = attr_values(cu.attributes, "position")
    radius_flat = attr_values(cu.attributes, "radius")
    surface_uv_per_curve = attr_values(cu.attributes, "surface_uv_coordinate")
    twist_per_curve = attr_values(cu.attributes, "twist")
    test_twist_present = cu.attributes.get("test_twist") is not None
    test_width_present = cu.attributes.get("test_width") is not None

    bind_armature = None
    if args.space == "world":
        mat = obj.matrix_world.copy()
    elif args.space == "armature":
        # Godot's skeleton model space -- the space the TressFX sim and root
        # binding run in -- is the glTF-exported joint space, and Blender's
        # glTF exporter axis-converts the JOINT DATA itself (measured
        # 2026-07-23 on the Mixamo asset: glTF joint translation ==
        # yup_conversion @ armature_local). So the correct .ghair space is
        # yup(armature-local), NOT raw armature-local. For the sim's
        # hardcoded -Y gravity to point down the character, the rig must be
        # meter-scale with an identity armature object transform (run
        # tools/prep_mixamo_blend.py on FBX imports first).
        yup = mathutils.Matrix(((1, 0, 0, 0), (0, 0, 1, 0), (0, -1, 0, 0), (0, 0, 0, 1)))
        bind_armature = find_bind_armature(obj)
        mat = yup @ bind_armature.matrix_world.inverted() @ obj.matrix_world
    else:
        mat = None

    vps = args.vps
    pos_out = []
    uv_out = []
    width_out = [] if radius_flat is not None else None
    twist_out = [] if twist_per_curve is not None else None
    skipped = 0
    original_point_counts = [npts for (_, npts) in offsets]

    for i in range(n_curves):
        off, npts = offsets[i]
        if npts < 2:
            print(f"[EXTRACT] WARNING strand {i}: {npts} point(s), skipping (degenerate)")
            skipped += 1
            continue

        pts = positions_flat[off:off + npts]
        if mat is not None:
            pts = [tuple(mat @ mathutils.Vector(p)) for p in pts]

        cum, total, targets = arc_targets(pts, vps)
        if total < DEGENERATE_LENGTH_EPS:
            print(f"[EXTRACT] WARNING strand {i}: near-zero length ({total:.3e}), skipping (degenerate)")
            skipped += 1
            continue

        resampled_pts = resample_along(pts, cum, targets)
        for p in resampled_pts:
            pos_out.extend(p)

        if surface_uv_per_curve is not None:
            uv_out.extend(surface_uv_per_curve[i])
        else:
            uv_out.extend((0.0, 0.0))

        if width_out is not None:
            radii = radius_flat[off:off + npts]
            resampled_radii = resample_along(radii, cum, targets)
            width_out.extend(resampled_radii)

        if twist_out is not None:
            twist_out.append(twist_per_curve[i])

    num_strands = n_curves - skipped

    meta = {
        "source_blend": bpy.data.filepath,
        "blender_version": bpy.app.version_string,
        "object": obj.name,
        "space": args.space,
        "vps": vps,
        "num_strands": num_strands,
        "skipped_strand_count": skipped,
        "original_point_counts": original_point_counts,
        "matrix_world": [list(row) for row in obj.matrix_world],
        "rest_pose_forced_on": rest_forced,
        "bind_armature": bind_armature.name if bind_armature is not None else None,
        "surface_uv_present": surface_uv_per_curve is not None,
        "radius_present": radius_flat is not None,
        "twist_present": twist_per_curve is not None,
        "test_attributes_present": {
            "test_twist": test_twist_present,
            "test_width": test_width_present,
        },
    }
    meta_bytes = json.dumps(meta).encode("utf-8")

    chunks = [
        pack_chunk("POS0", pack_floats(pos_out)),
        pack_chunk("RTUV", pack_floats(uv_out)),
        pack_chunk("META", meta_bytes),
    ]
    if width_out is not None:
        chunks.append(pack_chunk("WID0", pack_floats(width_out)))
    if twist_out is not None:
        chunks.append(pack_chunk("TWST", pack_floats(twist_out)))

    header = struct.pack("<8sIII", MAGIC, num_strands, vps, len(chunks))

    with open(args.out, "wb") as f:
        f.write(header)
        for c in chunks:
            f.write(c)

    print("[EXTRACT] blender:", bpy.app.version_string)
    print("[EXTRACT] object:", obj.name)
    print("[EXTRACT] space:", args.space, " vps:", vps)
    print("[EXTRACT] strands total:", n_curves, " skipped:", skipped, " written:", num_strands)
    print("[EXTRACT] surface_uv_present:", meta["surface_uv_present"],
          " radius_present:", meta["radius_present"],
          " twist_present:", meta["twist_present"])
    print("[EXTRACT] wrote:", args.out, " bytes:", 20 + sum(len(c) for c in chunks))


if __name__ == "__main__":
    main()
