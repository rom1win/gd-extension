#!/usr/bin/env python3
"""Regression check: compare freshly captured simulation dumps against the
protected reference dumps (CLAUDE.md decision 2).

Workflow:
  1. Reference baseline (done once, at Gate 2 green): run gdscript_hair.tscn
     with `capture_reference = true` on the HairSim node, then copy
     reference_new/ -> reference/ and commit reference/.
  2. After ANY change to kernels, packing, or host code: re-capture (same
     scene, same switch) and run:  python tools/compare_dump.py
     PASS = the change did not alter the physics.

Dump format: u32 vps, u32 guides, u32 stride, u32 frame, then the raw
positions buffer (float4 per vertex, interleaved guide+follow slots).
Determinism context: fixed dt=1/60, wind off, identity bones, same GPU.
"""
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REFERENCE = ROOT / "reference"
CANDIDATE = ROOT / "reference_new"
TOLERANCE = 1e-5  # meters; same-GPU reruns are usually bit-identical


def load(path: Path):
    raw = path.read_bytes()
    vps, guides, stride, frame = struct.unpack_from("<IIII", raw, 0)
    n = (len(raw) - 16) // 4
    return (vps, guides, stride, frame), struct.unpack_from(f"<{n}f", raw, 16)


def main() -> int:
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
            print(f"FAIL {ref.name}: no candidate in {CANDIDATE}/ — re-capture with capture_reference=true")
            failures += 1
            continue
        rh, rf = load(ref)
        ch, cf = load(cand)
        if rh != ch or len(rf) != len(cf):
            print(f"FAIL {ref.name}: header/size mismatch (ref {rh} n={len(rf)}, cand {ch} n={len(cf)})")
            failures += 1
            continue
        max_diff = 0.0
        arg = -1
        for i, (a, b) in enumerate(zip(rf, cf)):
            d = abs(a - b)
            if d > max_diff:
                max_diff, arg = d, i
        status = "PASS" if max_diff <= TOLERANCE else "FAIL"
        if status == "FAIL":
            failures += 1
            v, c = divmod(arg, 4)
            print(f"{status} {ref.name}: max diff {max_diff:.8f} at vertex {v} component {c} "
                  f"(ref {rf[arg]:.6f} vs cand {cf[arg]:.6f})")
        else:
            print(f"{status} {ref.name}: max diff {max_diff:.8f}")

    print()
    print("REGRESSION:", "PASS — physics unchanged" if failures == 0 else f"FAIL — {failures} dump(s) differ")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
