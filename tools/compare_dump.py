#!/usr/bin/env python3
"""Regression gate: compare freshly captured simulation dumps against the
protected reference dumps (CLAUDE.md decision 2).

Workflow:
  1. Reference baseline lives in reference/ (committed; captured by the
     GDScript harness at Gate 2: fixed dt=1/60, wind off, identity bones).
  2. After ANY change to kernels, packing, or host code: re-capture and run
     `python tools/compare_dump.py`. Two capture paths write the same format:
       - C++ demo (primary since A1): set gate_capture_mode=true on the
         TressFXCharacter node in demo/babylon.tscn, F5, wait for the three
         "GATE A1 dump written" lines, close Godot, run this script.
         Set gate_capture_mode back to false afterwards.
       - GDScript harness: capture_reference=true on the HairSim node in
         demo/gdscript_hair.tscn.
     PASS = the change did not alter the physics.

Dump format: u32 vps, u32 guides, u32 stride, u32 frame, then the raw
positions buffer (float4 per vertex, interleaved guide+follow slots; stride =
follow_per_guide + 1 strand slots, the guide occupies slot 0 of each group).
The "gdscript_ref_frame_*" filename prefix is historical; the C++ capture
writes the same names on purpose so this script pairs files by name.

GUIDE SLOTS ONLY: the C++ host dispatches UpdateFollowHairVertices, which
overwrites the follow slots with derived positions; the GDScript reference
never ran that kernel, so its follow slots hold rest positions. Follow hairs
are a pure function of the guides, so comparing guide slots compares the
physics. This also lets reference and candidate use different follow counts
(different stride) as long as vps/guides/frame match.

WHY A STATISTICAL GATE (Gate A1 finding, 2026-07-11):
The solver is NOT bit-reproducible run-to-run on the same GPU (measured on an
RX 7900 XTX): two F5 runs of the identical build diverge, because tiny GPU
floating-point scheduling noise is amplified by the chaotic solver — by frame
120 two identical runs differ by up to ~2.4 cm at the worst strand tip. An
exact tolerance therefore cannot work past the first frame: the committed
reference would FAIL against a re-capture of itself.

The gate instead checks two things that DO hold:
  - Frame 1 (a reset step, before chaos can amplify anything) must match the
    reference to float noise. This proves initial buffers + first-step
    dynamics are identical.
  - Frames 30/120 must stay within 3x the solver's measured run-to-run noise
    envelope (RMS and p95 of per-vertex Euclidean distance over guide slots).
    A real physics change (wrong stiffness, dt, rest lengths, bone matrices,
    kernel order, ...) is systematic and exceeds this by an order of
    magnitude; chaos does not.

Noise floor provenance: two identical C++ A1 captures, 2026-07-11, RX 7900
XTX, Godot 4.4 Forward+. If you change GPU and the gate fails marginally,
re-measure: capture twice into two directories, then
`python tools/compare_dump.py --noise <dirA> <dirB>` and set each frame's
FRAME_GATES limits to the suggested (3x) values. Re-baselining or re-deriving
limits requires maintainer approval (CLAUDE.md).
"""
import math
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REFERENCE = ROOT / "reference"
CANDIDATE = ROOT / "reference_new"

# Per-frame limits, in meters, on the distribution of per-vertex Euclidean
# distances (guide slots only).
#   frame 1:    strict — deterministic reset step, must match to float noise
#               (measured: RMS ~1e-6, max ~1.2e-5).
#   frames 30+: 3x the measured run-to-run noise floor (see docstring):
#               frame 30:  noise RMS 0.000685, p95 0.001581
#               frame 120: noise RMS 0.002376, p95 0.004600
FRAME_GATES = {
    1:   {"rms": 1e-5,    "p95": 5e-5},
    30:  {"rms": 0.00206, "p95": 0.00474},
    120: {"rms": 0.00713, "p95": 0.01380},
}


def load(path: Path):
    raw = path.read_bytes()
    vps, guides, stride, frame = struct.unpack_from("<IIII", raw, 0)
    n = (len(raw) - 16) // 4
    return (vps, guides, stride, frame), struct.unpack_from(f"<{n}f", raw, 16)


def guide_points(header, floats):
    """(x, y, z) per guide-strand vertex, strand-major order."""
    vps, guides, stride, _frame = header
    floats_per_strand = vps * 4
    pts = []
    for g in range(guides):
        base = g * stride * floats_per_strand
        end = base + floats_per_strand
        if end > len(floats):
            raise ValueError(
                f"buffer too small: guide {g} needs floats [{base}:{end}) "
                f"but only {len(floats)} present")
        for v in range(vps):
            o = base + v * 4
            pts.append((floats[o], floats[o + 1], floats[o + 2]))
    return pts


def distance_stats(pts_a, pts_b):
    """RMS, p95 and max of per-vertex Euclidean distances."""
    d = sorted(math.dist(a, b) for a, b in zip(pts_a, pts_b))
    rms = math.sqrt(sum(x * x for x in d) / len(d))
    p95 = d[min(len(d) - 1, int(0.95 * len(d)))]
    return rms, p95, d[-1]


def compare_pair(ref: Path, cand: Path) -> bool:
    """Compare one dump pair; print one PASS/FAIL line; return True on pass."""
    rh, rf = load(ref)
    ch, cf = load(cand)
    r_vps, r_guides, r_stride, r_frame = rh
    c_vps, c_guides, c_stride, c_frame = ch
    if (r_vps, r_guides, r_frame) != (c_vps, c_guides, c_frame):
        print(f"FAIL {ref.name}: header mismatch (ref vps/guides/frame "
              f"{r_vps}/{r_guides}/{r_frame}, cand {c_vps}/{c_guides}/{c_frame})")
        return False
    if r_stride != c_stride:
        print(f"NOTE {ref.name}: stride differs (ref {r_stride} vs cand {c_stride}) "
              f"— comparing guide slots only, this is fine")

    gate = FRAME_GATES.get(r_frame)
    if gate is None:
        print(f"FAIL {ref.name}: frame {r_frame} has no entry in FRAME_GATES — "
              f"add one (docstring explains how limits are derived)")
        return False

    try:
        rms, p95, dmax = distance_stats(guide_points(rh, rf), guide_points(ch, cf))
    except ValueError as e:
        print(f"FAIL {ref.name}: {e}")
        return False

    ok = rms <= gate["rms"] and p95 <= gate["p95"]
    print(f"{'PASS' if ok else 'FAIL'} {ref.name}: "
          f"RMS {rms:.6f} (limit {gate['rms']:.6f})  "
          f"p95 {p95:.6f} (limit {gate['p95']:.6f})  max {dmax:.6f}")
    return ok


def noise_mode(dir_a: Path, dir_b: Path) -> int:
    """Measure the run-to-run noise floor between two capture directories."""
    for a in sorted(dir_a.glob("*.bin")):
        b = dir_b / a.name
        if not b.exists():
            print(f"SKIP {a.name}: missing in {dir_b}")
            continue
        ah, af = load(a)
        bh, bf = load(b)
        rms, p95, dmax = distance_stats(guide_points(ah, af), guide_points(bh, bf))
        print(f"{a.name}: RMS {rms:.6f}  p95 {p95:.6f}  max {dmax:.6f}"
              f"  -> suggested limits: rms {3 * rms:.5f}, p95 {3 * p95:.5f}")
    return 0


def main() -> int:
    if len(sys.argv) == 4 and sys.argv[1] == "--noise":
        return noise_mode(Path(sys.argv[2]), Path(sys.argv[3]))

    if not REFERENCE.exists():
        print(f"MISSING {REFERENCE}/ — adopt a baseline first (copy reference_new/ to reference/)")
        return 1
    refs = sorted(REFERENCE.glob("gdscript_ref_frame_*.bin"))
    if not refs:
        print(f"No reference dumps in {REFERENCE}/")
        return 1

    failures = 0
    for ref in refs:
        cand = CANDIDATE / ref.name
        if not cand.exists():
            print(f"FAIL {ref.name}: no candidate in {CANDIDATE}/ — re-capture "
                  f"(C++ gate_capture_mode or GDScript capture_reference)")
            failures += 1
            continue
        if not compare_pair(ref, cand):
            failures += 1

    print()
    print("REGRESSION:", "PASS — physics unchanged" if failures == 0 else f"FAIL — {failures} dump(s) differ")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
