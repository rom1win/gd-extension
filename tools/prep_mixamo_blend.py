"""Normalize a Mixamo-style FBX-imported .blend to meter scale, headless.

Problem this solves: Mixamo FBX imports carry a 0.01 object scale on the
Armature (rig data is authored in centimeters). Godot's .blend import keeps
that structure, so the imported Skeleton3D's model space -- the space the
TressFX simulation runs in -- would be in CENTIMETERS, making every
meter-tuned physics parameter (gravity, collision margins, clamp distances)
wrong by 100x. This script bakes the scale into the data so armature-local
space becomes meters, while leaving every world-space position bit-identical.

What it does (armature object world matrix M = rotation R * uniform scale S,
e.g. Mixamo's +90degX rotation * 0.01):
  1. Bake M into the armature's rest-bone data (Armature.transform(M)):
     the rest skeleton becomes meters, oriented like Blender WORLD space.
     Baking the ROTATION too is essential, not cosmetic: Blender's glTF
     exporter axis-converts the JOINT data itself (measured 2026-07-23:
     glTF joint translation == yup_conversion @ armature_local), so Godot's
     skeleton model space == yup(armature-local). Only when armature-local
     is Blender-world-oriented does that come out Y-up -- the space the
     sim's hardcoded (0,-1,0) gravity expects. Leaving Mixamo's 90degX on
     the object gives a skeleton space with character-up along -Z: binding
     would still be self-consistent, but gravity would pull hair SIDEWAYS.
  2. Armature object transform -> identity.
  3. Every mesh child of the armature: bake its own old world matrix into
     its vertex data, transforms -> identity (their data was in rig units).
  4. Every action's pose-bone location f-curves: keyframes *= S (bone-local
     translations are in rig units; bone-local frames rotate WITH the baked
     rig, so rotations and directions stay valid -- only the scale of
     translations changes). Without this the animation translates 100x off.
  5. Saves a NEW file next to the input: <name>_prep.blend. Never overwrites.

Curves (hair) objects keep their point data untouched (it is already in
meters in their own object space), but their matrix_parent_inverse is
recomputed: it was authored against the parent's OLD centimeter-scale world
transform, and reusing it against the rescaled parent would multiply the
hair's world transform by 1/S (measured: hair at 177 m). The script restores
each curves object's exact pre-prep world transform.

Invocation:
    blender --background <file.blend> --python tools/prep_mixamo_blend.py

Verification is external: tools/inspect_blend-style checks that world-space
positions (rest AND posed) match the original file. See CLAUDE.md Phase B.
"""
import bpy
import os
from mathutils import Matrix, Vector

EPS = 1e-9


def main():
    # Find the (single) armature.
    armatures = [ob for ob in bpy.data.objects if ob.type == 'ARMATURE']
    if len(armatures) != 1:
        raise SystemExit(f"[PREP] expected exactly 1 armature, found {len(armatures)}")
    arm = armatures[0]

    s = arm.scale
    if abs(s.x - s.y) > EPS or abs(s.x - s.z) > EPS:
        raise SystemExit(f"[PREP] armature scale is non-uniform {tuple(s)}; refusing")
    S = s.x
    M = arm.matrix_world.copy()
    if all(abs(M[i][j] - (1.0 if i == j else 0.0)) < 1e-6 for i in range(4) for j in range(4)):
        raise SystemExit("[PREP] armature world transform is already identity; nothing to do")
    print(f"[PREP] armature '{arm.name}' scale={S}, baking full world matrix (rotation+scale) into data")

    # Snapshot every object's world transform BEFORE touching anything, so
    # transforms that must survive unchanged can be restored afterwards.
    world_before = {ob.name: ob.matrix_world.copy() for ob in bpy.data.objects}

    # -- 1. Bake the armature's world matrix into its rest-bone data.
    # Armature.transform() handles head/tail/roll consistently. --
    arm.data.transform(M)
    print(f"[PREP] baked world matrix into {len(arm.data.bones)} rest bones")

    # -- 2. Armature object transform -> identity. --
    arm.matrix_basis = Matrix.Identity(4)
    arm.matrix_parent_inverse = Matrix.Identity(4)

    # -- 3. Bake each armature-child mesh's old world matrix into its
    # vertex data, then clear its transforms (world stays bit-identical:
    # new world = identity_parent @ identity_local @ (old_world @ data)). --
    for ob in bpy.data.objects:
        if ob.type != 'MESH':
            continue
        if ob.parent is not arm:
            continue
        ob.data.transform(world_before[ob.name])
        ob.matrix_parent_inverse = Matrix.Identity(4)
        ob.matrix_basis = Matrix.Identity(4)
        print(f"[PREP] baked world matrix into mesh data '{ob.name}' ({len(ob.data.vertices)} verts)")

    # -- 3b. Restore curves (hair) objects' world transforms: their
    # matrix_parent_inverse was authored against the parent's old cm-scale
    # world matrix and is now stale. world = parent_world @ parent_inverse @
    # basis, so solve for the parent_inverse that reproduces the old world.
    bpy.context.view_layer.update()
    for ob in bpy.data.objects:
        if ob.type != 'CURVES' or ob.parent is None:
            continue
        ob.matrix_parent_inverse = (
            ob.parent.matrix_world.inverted()
            @ world_before[ob.name]
            @ ob.matrix_basis.inverted()
        )
        print(f"[PREP] restored world transform of curves object '{ob.name}'")

    # -- 4. Scale location f-curves of every action (bone-local units). --
    for act in bpy.data.actions:
        n_keys = 0
        for fc in act.fcurves:
            if not fc.data_path.endswith('.location'):
                continue
            for kp in fc.keyframe_points:
                kp.co.y *= S
                kp.handle_left.y *= S
                kp.handle_right.y *= S
            n_keys += len(fc.keyframe_points)
        print(f"[PREP] action '{act.name}': scaled {n_keys} location keys by {S}")

    # -- 5. Save as _prep.blend, never overwrite the source. --
    src = bpy.data.filepath
    base, ext = os.path.splitext(src)
    dst = base + "_prep" + ext
    bpy.ops.wm.save_as_mainfile(filepath=dst)
    print(f"[PREP] wrote: {dst}")


main()
