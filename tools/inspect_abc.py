# Phase B feasibility inspector — run headless in Blender:
#   blender --background --python tools/inspect_abc.py
# Answers, with evidence:
#  1. Do the custom attributes (test_twist / test_width) exist in the .blend at all?
#  2. What survives the .abc round trip (structure, widths, attributes)?
#  3. Do the Alembic and glTF exporters agree on axes/scale? (sphere registration)
# All verdict lines are prefixed [INSPECT] for easy grepping.

import os
import sys

import bpy
from mathutils import Vector

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BLEND = os.path.join(REPO, "test_assets", "hair_test.blend")
ABC = os.path.join(REPO, "test_assets", "hair_test.abc")
GLB = os.path.join(REPO, "test_assets", "hair_test.glb")

PROBES = ("test_twist", "test_width")


def say(msg):
    print("[INSPECT] " + msg)


def world_bbox(obj):
    pts = [obj.matrix_world @ Vector(c) for c in obj.bound_box]
    lo = Vector((min(p[i] for p in pts) for i in range(3)))
    hi = Vector((max(p[i] for p in pts) for i in range(3)))
    return lo, hi


def fmt_v(v):
    return "(%.4f, %.4f, %.4f)" % (v[0], v[1], v[2])


def describe_curves_obj(obj, label):
    data = obj.data
    say("%s: object '%s' type=%s data=%s" % (label, obj.name, obj.type, type(data).__name__))
    # New-style hair Curves
    if obj.type == "CURVES":
        try:
            n_curves = len(data.curves)
            n_points = len(data.points)
            say("%s: curves=%d total_points=%d" % (label, n_curves, n_points))
            sizes = {}
            counts = []
            for c in data.curves:
                try:
                    npts = c.points_length
                except AttributeError:
                    npts = len(c.points)
                counts.append(npts)
                sizes[npts] = sizes.get(npts, 0) + 1
            say("%s: points-per-curve min=%d max=%d histogram=%s"
                % (label, min(counts), max(counts), sorted(sizes.items())))
        except Exception as e:
            say("%s: FAILED to read curve structure: %r" % (label, e))
        try:
            names = []
            for a in data.attributes:
                names.append("%s(domain=%s,type=%s)" % (a.name, a.domain, a.data_type))
            say("%s: attributes: %s" % (label, ", ".join(names) if names else "NONE"))
            for probe in PROBES:
                a = data.attributes.get(probe)
                if a is None:
                    say("%s: PROBE %s: ABSENT" % (label, probe))
                else:
                    vals = [round(d.value, 4) for d in list(a.data)[:5]]
                    say("%s: PROBE %s: PRESENT domain=%s first_values=%s"
                        % (label, probe, a.domain, vals))
        except Exception as e:
            say("%s: FAILED to read attributes: %r" % (label, e))
    # Legacy curve object (what the Alembic importer may produce)
    elif obj.type == "CURVE":
        counts = []
        for sp in data.splines:
            counts.append(len(sp.points) if len(sp.points) else len(sp.bezier_points))
        if counts:
            say("%s: legacy CURVE splines=%d points-per-spline min=%d max=%d"
                % (label, len(counts), min(counts), max(counts)))
        else:
            say("%s: legacy CURVE with no splines" % label)
        attrs = getattr(data, "attributes", None)
        if attrs is None:
            say("%s: legacy CURVE has no generic-attribute API -> custom attrs unreadable here" % label)
        else:
            names = ["%s(%s,%s)" % (a.name, a.domain, a.data_type) for a in attrs]
            say("%s: attributes: %s" % (label, ", ".join(names) if names else "NONE"))
    else:
        say("%s: unexpected object type %s" % (label, obj.type))


def reset_empty():
    bpy.ops.wm.read_factory_settings(use_empty=True)


# ---------- Phase 1: the .blend (did the authoring actually happen?) ----------
say("=" * 60)
say("PHASE 1: source .blend inspection: " + BLEND)
bpy.ops.wm.open_mainfile(filepath=BLEND)
say("blender version: %s" % (bpy.app.version_string,))
for obj in bpy.data.objects:
    say("blend object: '%s' type=%s" % (obj.name, obj.type))
hair = None
for obj in bpy.data.objects:
    if obj.type == "CURVES":
        hair = obj
if hair is None:
    say("blend: NO new-style hair CURVES object found — was legacy particle hair used?")
else:
    # Evaluate through depsgraph so Geometry Nodes results (Store Named Attribute) count
    deps = bpy.context.evaluated_depsgraph_get()
    hair_eval = hair.evaluated_get(deps)
    describe_curves_obj(hair_eval, "blend-hair(evaluated)")
    describe_curves_obj(hair, "blend-hair(original)")

# ---------- Phase 2: .abc round trip ----------
say("=" * 60)
say("PHASE 2: .abc reimport inspection: " + ABC)
reset_empty()
try:
    bpy.ops.wm.alembic_import(filepath=ABC, as_background_job=False)
except TypeError:
    bpy.ops.wm.alembic_import(filepath=ABC)
abc_sphere = None
for obj in bpy.data.objects:
    say("abc object: '%s' type=%s parent=%s" % (obj.name, obj.type,
        obj.parent.name if obj.parent else "-"))
    if obj.type in ("CURVES", "CURVE"):
        describe_curves_obj(obj, "abc-hair")
    if obj.type == "MESH":
        abc_sphere = obj
if abc_sphere is not None:
    lo, hi = world_bbox(abc_sphere)
    say("abc-sphere '%s': verts=%d bbox %s .. %s"
        % (abc_sphere.name, len(abc_sphere.data.vertices), fmt_v(lo), fmt_v(hi)))

# ---------- Phase 3: .glb registration ----------
say("=" * 60)
say("PHASE 3: .glb registration: " + GLB)
try:
    bpy.ops.import_scene.gltf(filepath=GLB)
    glb_sphere = None
    for obj in bpy.data.objects:
        if obj.type == "MESH" and obj is not abc_sphere:
            glb_sphere = obj
    if glb_sphere is not None and abc_sphere is not None:
        lo_g, hi_g = world_bbox(glb_sphere)
        lo_a, hi_a = world_bbox(abc_sphere)
        say("glb-sphere '%s': verts=%d bbox %s .. %s"
            % (glb_sphere.name, len(glb_sphere.data.vertices), fmt_v(lo_g), fmt_v(hi_g)))
        say("REGISTRATION: abc bbox %s..%s vs glb bbox %s..%s"
            % (fmt_v(lo_a), fmt_v(hi_a), fmt_v(lo_g), fmt_v(hi_g)))
        delta = max(abs(lo_a[i] - lo_g[i]) + abs(hi_a[i] - hi_g[i]) for i in range(3))
        say("REGISTRATION VERDICT: %s (max bbox deviation %.4f m)"
            % ("MATCH — same space after Blender import conversions" if delta < 1e-3
               else "MISMATCH — importer must correct axes/scale", delta))
    else:
        say("REGISTRATION: could not identify both spheres")
except Exception as e:
    say("PHASE 3 FAILED: %r" % e)

say("=" * 60)
say("DONE")
