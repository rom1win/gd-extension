#!/usr/bin/env python3
"""Gate B part 1 answer-key experiment (B3.1, prototype only — no C++/Blender).

Binds Ratboy's hair roots to Ratboy's body collision mesh by nearest-triangle +
barycentric skin-weight blending, then measures agreement against AMD's authored
`.tfxbone` weights (the "answer key" — hand-authored in Maya, shipped with RatBoy).

This does NOT decide pass/fail. It prints a `GATE-B1 PROVISIONAL:` summary line;
the architect sets thresholds from the measured numbers.

Context for interpreting results: AMD's .tfxbone was very likely authored against
their RENDER mesh (unknown to us) rather than the `.tfxmesh` COLLISION mesh used
here for SDF collision — small, spatially-smooth disagreement is expected and is
part of what this experiment measures. Large, scattered (non-smooth) disagreement
would instead point at a bug in our own binding math or file parsing.

Formats reproduced here (see NOTES.md §5 and src/SDF.cpp — NOT the header struct
in AMD's TressFXFileFormat.h, which NOTES.md documents as wrong for .tfxbone,
finding H9):

  .tfx      little-endian: f32 version; u32 numHairStrands; u32 numVerticesPerStrand;
            u32 offsetVertexPosition; u32 offsetStrandUV; u32 offsetVertexUV;
            u32 offsetStrandThickness; u32 offsetVertexColor; u32 reserved[32].
            Positions at offsetVertexPosition: float4[numHairStrands * vps],
            strand-major (w = movability, unused here).

  .tfxbone  little-endian: i32 numBones; repeat numBones { i32 boneIndex(unused,
            file order IS the index); i32 nameLen(incl NUL); char name[nameLen] };
            i32 numStrands; repeat numStrands { i32 strandIndex(unused);
            repeat 4 { i32 boneIndex; f32 weight } }  <-- INTERLEAVED per slot,
            confirmed against thirdparty/tressfx/src/TressFX/TressFXAsset.cpp
            LoadBoneData (reads boneIndex then weight per j, four times), NOT the
            "boneIndex[4] block then weight[4] block" layout one might guess from
            a casual reading of the struct name — parsing it as two 4-wide blocks
            desyncs immediately (weights stop summing to ~1). Verified two ways
            on Ratboy_mohawk.tfxbone: per-strand weight sums are ~1.0 and the
            parse lands exactly on EOF (84369 bytes) with this layout.

  .tfxmesh  text, matches src/SDF.cpp's tokenizer exactly: `numOfBones N` + N
            lines "idx name" (idx == sequential file order, verified on
            Ratboy_body.tfxmesh); `numOfVertices N` + N lines
            "id px py pz nx ny nz b0 b1 b2 b3 w0 w1 w2 w3"; `numOfTriangles N`
            + N lines "id v0 v1 v2". `#`-prefixed and blank lines are comments.

Bone names are the binding currency throughout: .tfx has none, .tfxbone and
.tfxmesh each have their OWN local bone index table (different sizes: 105 vs 86,
different order) that must never be compared by raw index — only by resolved
name string.
"""

import math
import struct
import sys
import time
from pathlib import Path

try:
    import numpy as np
    HAS_NUMPY = True
except ImportError:
    HAS_NUMPY = False

ASSET_DIR = Path(__file__).resolve().parent.parent / "demo" / "Meshes" / "HairAsset" / "Ratboy"
TFX_PATH = ASSET_DIR / "Ratboy_mohawk.tfx"
TFXBONE_PATH = ASSET_DIR / "Ratboy_mohawk.tfxbone"
TFXMESH_PATH = ASSET_DIR / "Ratboy_body.tfxmesh"

# Broad-phase candidate count: nearest-K mesh VERTICES considered per root before
# the exact point-triangle test is run on every triangle touching them. Generous
# on purpose (correctness over speed — runtime budget is ~2 min, we're nowhere
# close). Cross-checked against full brute force on a random sample below.
K_NEAREST_VERTICES = 40
BRUTE_FORCE_SAMPLE = 40  # roots to cross-check KNN result against full O(T) search


def fail(msg):
    print(f"FATAL: {msg}", file=sys.stderr)
    sys.exit(1)


# ---------------------------------------------------------------------------
# .tfx parsing
# ---------------------------------------------------------------------------

def load_tfx_roots(path):
    data = path.read_bytes()
    version, num_strands, vps, off_pos = struct.unpack_from("<f3I", data, 0)
    if num_strands != 2228:
        fail(f".tfx numHairStrands={num_strands}, expected 2228 (spec mismatch — stop)")
    if vps != 32:
        fail(f".tfx numVerticesPerStrand={vps}, expected 32 (spec mismatch — stop)")
    if off_pos != 0xA0:
        fail(f".tfx offsetVertexPosition=0x{off_pos:x}, expected 0xA0 (spec mismatch — stop)")

    v0 = struct.unpack_from("<4f", data, off_pos)
    v1 = struct.unpack_from("<4f", data, off_pos + 16)
    if v0[3] != 0.0 or v1[3] != 0.0:
        fail(f"first two vertices of strand 0 expected w=0 (pinned), got w={v0[3]},{v1[3]}")

    roots = []
    stride = 16  # float4
    strand_stride = stride * vps
    for s in range(num_strands):
        off = off_pos + s * strand_stride  # root = element s*vps + 0
        x, y, z, w = struct.unpack_from("<4f", data, off)
        roots.append((x, y, z))
    return roots, num_strands, vps


# ---------------------------------------------------------------------------
# .tfxbone parsing (answer key)
# ---------------------------------------------------------------------------

def load_tfxbone(path, expected_num_strands):
    data = path.read_bytes()
    off = 0
    (num_bones,) = struct.unpack_from("<i", data, off); off += 4
    if num_bones != 105:
        fail(f".tfxbone numBones={num_bones}, expected 105 (spec mismatch — stop)")

    names = []
    for _ in range(num_bones):
        off += 4  # boneIndex field, unused (file order IS the index)
        (name_len,) = struct.unpack_from("<i", data, off); off += 4
        raw = data[off:off + name_len]; off += name_len
        names.append(raw.split(b"\0")[0].decode("ascii"))

    (num_strands,) = struct.unpack_from("<i", data, off); off += 4
    if num_strands != expected_num_strands:
        fail(f".tfxbone numStrands={num_strands}, expected {expected_num_strands} (.tfx mismatch)")

    answers = []  # list of {bone_name: weight}, renormalized to sum 1
    for _ in range(num_strands):
        off += 4  # strandIndex, unused
        entries = []
        for _ in range(4):
            (bone_idx,) = struct.unpack_from("<i", data, off); off += 4
            (weight,) = struct.unpack_from("<f", data, off); off += 4
            if bone_idx < 0 or weight <= 0.0:
                continue
            entries.append((names[bone_idx], weight))
        total = sum(w for _, w in entries)
        d = {}
        if total > 0.0:
            for name, w in entries:
                d[name] = d.get(name, 0.0) + w / total
        answers.append(d)

    if off != len(data):
        fail(f".tfxbone parse ended at byte {off}, file is {len(data)} bytes — layout is wrong")

    return answers


# ---------------------------------------------------------------------------
# .tfxmesh parsing (collision mesh: positions, triangles, per-vertex skin weights)
# ---------------------------------------------------------------------------

def load_tfxmesh(path):
    lines = path.read_text().splitlines()
    bone_names = []
    positions = []
    normals = []
    vertex_bone_idx = []  # list of 4-tuples (local mesh bone index)
    vertex_bone_w = []    # list of 4-tuples
    triangles = []

    i = 0
    n = len(lines)

    def next_tokens():
        nonlocal i
        while i < n:
            line = lines[i].strip()
            i += 1
            if not line or line.startswith("#"):
                continue
            return line.split()
        return None

    while i < n:
        tok = next_tokens()
        if tok is None:
            break
        if tok[0] == "numOfBones":
            count = int(tok[1])
            for _ in range(count):
                t = next_tokens()
                bone_names.append(t[1])
            if len(bone_names) != count:
                fail(".tfxmesh: numOfBones count mismatch")
        elif tok[0] == "numOfVertices":
            count = int(tok[1])
            for _ in range(count):
                t = next_tokens()
                if len(t) < 15:
                    fail(f".tfxmesh: malformed vertex line {t}")
                positions.append((float(t[1]), float(t[2]), float(t[3])))
                normals.append((float(t[4]), float(t[5]), float(t[6])))
                vertex_bone_idx.append(tuple(int(t[7 + k]) for k in range(4)))
                vertex_bone_w.append(tuple(float(t[11 + k]) for k in range(4)))
            if len(positions) != count:
                fail(".tfxmesh: numOfVertices count mismatch")
        elif tok[0] == "numOfTriangles":
            count = int(tok[1])
            for _ in range(count):
                t = next_tokens()
                triangles.append((int(t[1]), int(t[2]), int(t[3])))
            if len(triangles) != count:
                fail(".tfxmesh: numOfTriangles count mismatch")

    if len(bone_names) != 86:
        fail(f".tfxmesh numOfBones={len(bone_names)}, expected 86 (spec mismatch — stop)")
    if len(positions) != 12909:
        fail(f".tfxmesh numOfVertices={len(positions)}, expected 12909 (spec mismatch — stop)")
    if len(triangles) != 25814:
        fail(f".tfxmesh numOfTriangles={len(triangles)}, expected 25814 (spec mismatch — stop)")

    return bone_names, positions, vertex_bone_idx, vertex_bone_w, triangles


# ---------------------------------------------------------------------------
# geometry: closest point on a triangle (Ericson, "Real-Time Collision
# Detection" 5.1.5) — pure Python, works with or without numpy since it
# operates on plain float tuples, one triangle at a time.
# ---------------------------------------------------------------------------

def _sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def _dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def _add(a, b):
    return (a[0] + b[0], a[1] + b[1], a[2] + b[2])


def _scale(a, s):
    return (a[0] * s, a[1] * s, a[2] * s)


def closest_point_on_triangle(p, a, b, c):
    """Returns (closest_point, (u, v, w)) barycentric coords wrt (a, b, c)."""
    ab = _sub(b, a)
    ac = _sub(c, a)
    ap = _sub(p, a)
    d1 = _dot(ab, ap)
    d2 = _dot(ac, ap)
    if d1 <= 0.0 and d2 <= 0.0:
        return a, (1.0, 0.0, 0.0)

    bp = _sub(p, b)
    d3 = _dot(ab, bp)
    d4 = _dot(ac, bp)
    if d3 >= 0.0 and d4 <= d3:
        return b, (0.0, 1.0, 0.0)

    vc = d1 * d4 - d3 * d2
    if vc <= 0.0 and d1 >= 0.0 and d3 <= 0.0:
        v = d1 / (d1 - d3)
        return _add(a, _scale(ab, v)), (1.0 - v, v, 0.0)

    cp = _sub(p, c)
    d5 = _dot(ab, cp)
    d6 = _dot(ac, cp)
    if d6 >= 0.0 and d5 <= d6:
        return c, (0.0, 0.0, 1.0)

    vb = d5 * d2 - d1 * d6
    if vb <= 0.0 and d2 >= 0.0 and d6 <= 0.0:
        w = d2 / (d2 - d6)
        return _add(a, _scale(ac, w)), (1.0 - w, 0.0, w)

    va = d3 * d6 - d5 * d4
    if va <= 0.0 and (d4 - d3) >= 0.0 and (d5 - d6) >= 0.0:
        w = (d4 - d3) / ((d4 - d3) + (d5 - d6))
        return _add(b, _scale(_sub(c, b), w)), (0.0, 1.0 - w, w)

    denom = 1.0 / (va + vb + vc)
    v = vb * denom
    w = vc * denom
    return _add(a, _add(_scale(ab, v), _scale(ac, w))), (1.0 - v - w, v, w)


# ---------------------------------------------------------------------------
# broad phase: nearest-K mesh vertices per root
# ---------------------------------------------------------------------------

def nearest_vertices_numpy(roots, mesh_positions, k):
    roots_arr = np.asarray(roots, dtype=np.float64)
    mesh_arr = np.asarray(mesh_positions, dtype=np.float64)
    result = np.empty((len(roots), k), dtype=np.int64)
    chunk = 200
    for start in range(0, len(roots), chunk):
        end = min(start + chunk, len(roots))
        diff = mesh_arr[None, :, :] - roots_arr[start:end, None, :]
        d2 = np.einsum("ijk,ijk->ij", diff, diff)
        idx = np.argpartition(d2, k - 1, axis=1)[:, :k]
        result[start:end] = idx
    return result


def nearest_vertices_stdlib(roots, mesh_positions, k):
    result = []
    for r in roots:
        dists = []
        for vi, mp in enumerate(mesh_positions):
            d2 = (mp[0] - r[0]) ** 2 + (mp[1] - r[1]) ** 2 + (mp[2] - r[2]) ** 2
            dists.append((d2, vi))
        dists.sort()
        result.append([vi for _, vi in dists[:k]])
    return result


def brute_force_closest_triangle(root, positions, triangles):
    """O(numTriangles) exact search — used only as a spot-check on a small
    sample to confirm K_NEAREST_VERTICES is large enough for the broad phase."""
    best_d2 = math.inf
    best = None
    for ti, (i0, i1, i2) in enumerate(triangles):
        cp, bary = closest_point_on_triangle(root, positions[i0], positions[i1], positions[i2])
        d2 = (cp[0] - root[0]) ** 2 + (cp[1] - root[1]) ** 2 + (cp[2] - root[2]) ** 2
        if d2 < best_d2:
            best_d2 = d2
            best = (ti, cp, bary)
    return best_d2, best


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def main():
    t_start = time.time()

    if not TFX_PATH.exists() or not TFXBONE_PATH.exists() or not TFXMESH_PATH.exists():
        fail("one or more input assets missing under demo/Meshes/HairAsset/Ratboy/")

    roots, num_strands, vps = load_tfx_roots(TFX_PATH)
    print(f"Parsed .tfx: {num_strands} strands, vps={vps}, {len(roots)} roots")

    answers = load_tfxbone(TFXBONE_PATH, num_strands)
    print(f"Parsed .tfxbone: {len(answers)} strand entries (answer key)")

    mesh_bone_names, mesh_positions, mesh_bone_idx, mesh_bone_w, triangles = load_tfxmesh(TFXMESH_PATH)
    print(f"Parsed .tfxmesh: {len(mesh_bone_names)} bones, {len(mesh_positions)} verts, "
          f"{len(triangles)} triangles")
    print(f"Using {'numpy' if HAS_NUMPY else 'pure-Python stdlib'} broad phase "
          f"(K={K_NEAREST_VERTICES} nearest vertices)")

    # vertex -> touching triangle indices
    vertex_to_tris = [[] for _ in range(len(mesh_positions))]
    for ti, (i0, i1, i2) in enumerate(triangles):
        vertex_to_tris[i0].append(ti)
        vertex_to_tris[i1].append(ti)
        vertex_to_tris[i2].append(ti)

    t_broad0 = time.time()
    if HAS_NUMPY:
        knn = nearest_vertices_numpy(roots, mesh_positions, K_NEAREST_VERTICES)
    else:
        knn = nearest_vertices_stdlib(roots, mesh_positions, K_NEAREST_VERTICES)
    t_broad1 = time.time()
    print(f"Broad phase (KNN): {t_broad1 - t_broad0:.2f}s")

    results = []  # per root: dict with our_weights, dist_to_surface, bary, tri
    for ri, root in enumerate(roots):
        candidate_tris = set()
        for vi in knn[ri]:
            candidate_tris.update(vertex_to_tris[int(vi)])
        if not candidate_tris:
            fail(f"root {ri}: no candidate triangles found (K too small or mesh disconnected)")

        best_d2 = math.inf
        best_tri = None
        best_cp = None
        best_bary = None
        for ti in candidate_tris:
            i0, i1, i2 = triangles[ti]
            cp, bary = closest_point_on_triangle(root, mesh_positions[i0], mesh_positions[i1], mesh_positions[i2])
            d2 = (cp[0] - root[0]) ** 2 + (cp[1] - root[1]) ** 2 + (cp[2] - root[2]) ** 2
            if d2 < best_d2:
                best_d2 = d2
                best_tri = ti
                best_cp = cp
                best_bary = bary

        i0, i1, i2 = triangles[best_tri]
        u, v, w = best_bary
        blended = {}
        for corner_idx, bary_coord in ((i0, u), (i1, v), (i2, w)):
            for k in range(4):
                bidx = mesh_bone_idx[corner_idx][k]
                wgt = mesh_bone_w[corner_idx][k]
                if wgt <= 0.0:
                    continue
                name = mesh_bone_names[bidx]
                blended[name] = blended.get(name, 0.0) + bary_coord * wgt

        top4 = sorted(blended.items(), key=lambda kv: -kv[1])[:4]
        total = sum(w for _, w in top4)
        our_weights = {name: w / total for name, w in top4} if total > 0.0 else {}

        results.append({
            "root": root,
            "dist_to_surface": math.sqrt(best_d2),
            "our_weights": our_weights,
            "tri": best_tri,
        })

    t_narrow1 = time.time()
    print(f"Narrow phase (closest triangle + blend): {t_narrow1 - t_broad1:.2f}s")

    # cross-check broad phase against full brute force on a sample
    import random
    random.seed(42)
    sample_idx = random.sample(range(len(roots)), min(BRUTE_FORCE_SAMPLE, len(roots)))
    mismatches = 0
    for ri in sample_idx:
        bf_d2, _ = brute_force_closest_triangle(roots[ri], mesh_positions, triangles)
        knn_d = results[ri]["dist_to_surface"]
        if abs(math.sqrt(bf_d2) - knn_d) > 1e-6:
            mismatches += 1
    print(f"Brute-force cross-check on {len(sample_idx)} random roots: "
          f"{len(sample_idx) - mismatches}/{len(sample_idx)} exact matches "
          f"({'OK, K is large enough' if mismatches == 0 else 'MISMATCH — K_NEAREST_VERTICES too small!'})")

    # ---------------------------------------------------------------
    # compare against answer key
    # ---------------------------------------------------------------
    top1_matches = 0
    l1_list = []
    dist_list = []
    detail = []
    for ri, res in enumerate(results):
        our = res["our_weights"]
        ans = answers[ri]
        our_top1 = max(our.items(), key=lambda kv: kv[1])[0] if our else None
        ans_top1 = max(ans.items(), key=lambda kv: kv[1])[0] if ans else None
        top1_match = (our_top1 == ans_top1) and our_top1 is not None
        if top1_match:
            top1_matches += 1

        names = set(our) | set(ans)
        l1 = sum(abs(our.get(n, 0.0) - ans.get(n, 0.0)) for n in names)
        l1_list.append(l1)
        dist_list.append(res["dist_to_surface"])

        detail.append({
            "ri": ri,
            "root": res["root"],
            "our": our,
            "ans": ans,
            "l1": l1,
            "dist": res["dist_to_surface"],
            "top1_match": top1_match,
        })

    n = len(results)
    top1_pct = 100.0 * top1_matches / n

    def pct(values, p):
        s = sorted(values)
        idx = min(int(round(p * (len(s) - 1))), len(s) - 1)
        return s[idx]

    mean_l1 = sum(l1_list) / n
    median_l1 = pct(l1_list, 0.5)
    p95_l1 = pct(l1_list, 0.95)
    max_l1 = max(l1_list)

    mean_dist = sum(dist_list) / n
    max_dist = max(dist_list)

    print()
    print("=" * 70)
    print("SUMMARY")
    print("=" * 70)
    print(f"Roots compared: {n}")
    print(f"Top-1 bone match: {top1_matches}/{n} = {top1_pct:.1f}%")
    print(f"L1 weight-vector distance: mean={mean_l1:.4f} median={median_l1:.4f} "
          f"p95={p95_l1:.4f} max={max_l1:.4f}")
    print(f"Root-to-surface distance (m): mean={mean_dist:.5f} max={max_dist:.5f}")

    worst = sorted(detail, key=lambda d: -d["l1"])[:5]
    print()
    print("5 worst-disagreeing roots:")
    for d in worst:
        print(f"  strand {d['ri']}: root={tuple(round(c, 4) for c in d['root'])} "
              f"dist_to_surface={d['dist']:.5f} L1={d['l1']:.4f} top1_match={d['top1_match']}")
        print(f"    ours : {{{', '.join(f'{k}: {v:.3f}' for k, v in sorted(d['our'].items(), key=lambda kv: -kv[1]))}}}")
        print(f"    amd  : {{{', '.join(f'{k}: {v:.3f}' for k, v in sorted(d['ans'].items(), key=lambda kv: -kv[1]))}}}")

    t_end = time.time()
    print()
    print(f"Total runtime: {t_end - t_start:.2f}s")
    print()
    print(f"GATE-B1 PROVISIONAL: top1={top1_pct:.1f}% meanL1={mean_l1:.4f}")


if __name__ == "__main__":
    main()
