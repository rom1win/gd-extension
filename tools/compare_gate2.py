#!/usr/bin/env python3
"""Gate 2 side-by-side: does the GDScript sim behave like the C++ sim?

Compares the *residual segment stretch* distribution of both simulations at
frame 300 against the asset's rest lengths. Absolute positions are expected to
differ (the C++ demo skins to the skeleton; the GDScript Phase 2 run uses
identity bones) — what must match is the constraint solver's behavior: how far
segments deviate from their rest lengths once the hair has settled.

Usage (from the repo root, after both dumps exist):
    python tools/compare_gate2.py

Inputs:
    demo/gate2_cpp.bin  - written by the C++ demo (F5 main.tscn, wait ~6s)
    demo/gate2_gd.bin   - written by the GDScript scene (F6 gdscript_hair.tscn, wait ~6s)
"""
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TFX = ROOT / "demo" / "Meshes" / "HairAsset" / "Ratboy" / "Ratboy_mohawk.tfx"
TG = 64


def load_rest_guide_positions():
    data = TFX.read_bytes()
    _, nfile, vps, offpos = struct.unpack_from("<fIII", data, 0)
    guides = (nfile - nfile % TG) + TG
    pos = list(struct.unpack_from(f"<{nfile * vps * 4}f", data, offpos))
    pos += pos[(nfile - 1) * vps * 4:nfile * vps * 4] * (guides - nfile)
    return pos, guides, vps


def rest_lengths(pos, guides, vps):
    rest = [[0.0] * (vps - 1) for _ in range(guides)]
    for g in range(guides):
        b = g * vps * 4
        for j in range(vps - 1):
            a, c = b + j * 4, b + (j + 1) * 4
            rest[g][j] = ((pos[a] - pos[c]) ** 2 + (pos[a + 1] - pos[c + 1]) ** 2 + (pos[a + 2] - pos[c + 2]) ** 2) ** 0.5
    return rest


def read_cpp(path):
    raw = path.read_bytes()
    vps, guides = struct.unpack_from("<II", raw, 0)
    floats = struct.unpack_from(f"<{guides * vps * 4}f", raw, 8)
    return list(floats), guides, vps, 1  # guides packed consecutively


def read_gd(path):
    raw = path.read_bytes()
    vps, guides, stride = struct.unpack_from("<III", raw, 0)
    n = (len(raw) - 12) // 4
    floats = struct.unpack_from(f"<{n}f", raw, 12)
    return list(floats), guides, vps, stride  # full interleaved buffer


def stretch_stats(floats, guides, vps, stride, rest):
    sig_rel = []   # relative error, segments >= 0.1mm
    micro_abs = [] # absolute error, micro segments
    for g in range(guides):
        base = (g * stride) * vps * 4
        for j in range(vps - 1):
            a, c = base + j * 4, base + (j + 1) * 4
            ln = ((floats[a] - floats[c]) ** 2 + (floats[a + 1] - floats[c + 1]) ** 2 + (floats[a + 2] - floats[c + 2]) ** 2) ** 0.5
            r = rest[g][j]
            if r >= 1e-4:
                sig_rel.append(abs(ln - r) / r)
            elif r > 0:
                micro_abs.append(abs(ln - r))
    sig_rel.sort()
    micro_abs.sort()
    return sig_rel, micro_abs


def pct(sorted_list, p):
    if not sorted_list:
        return 0.0
    return sorted_list[min(len(sorted_list) - 1, int(len(sorted_list) * p))]


def describe(name, sig, micro):
    print(f"{name}:")
    print(f"  significant segments (rest >= 0.1mm): n={len(sig)}")
    print(f"    median={pct(sig, 0.5) * 100:.3f}%  p95={pct(sig, 0.95) * 100:.3f}%  p99={pct(sig, 0.99) * 100:.3f}%  max={sig[-1] * 100:.3f}%")
    print(f"  micro segments (rest < 0.1mm): n={len(micro)}")
    if micro:
        print(f"    median={pct(micro, 0.5) * 1000:.4f}mm  p99={pct(micro, 0.99) * 1000:.4f}mm  max={micro[-1] * 1000:.4f}mm")


def main() -> int:
    cpp_path = ROOT / "demo" / "gate2_cpp.bin"
    gd_path = ROOT / "demo" / "gate2_gd.bin"
    for p, hint in ((cpp_path, "F5 main.tscn and wait ~6 seconds"),
                    (gd_path, "F6 gdscript_hair.tscn and wait ~6 seconds")):
        if not p.exists():
            print(f"MISSING: {p} — {hint}")
            return 1

    pos, guides, vps = load_rest_guide_positions()
    rest = rest_lengths(pos, guides, vps)

    cf, cg, cv, cs = read_cpp(cpp_path)
    gf, gg, gv, gs = read_gd(gd_path)
    if (cg, cv) != (guides, vps) or (gg, gv) != (guides, vps):
        print(f"WARNING: count mismatch (asset {guides}x{vps}, cpp {cg}x{cv}, gd {gg}x{gv})")

    c_sig, c_micro = stretch_stats(cf, cg, cv, cs, rest)
    g_sig, g_micro = stretch_stats(gf, gg, gv, gs, rest)

    describe("C++ reference (main.tscn)", c_sig, c_micro)
    describe("GDScript sim (gdscript_hair.tscn)", g_sig, g_micro)

    # Verdict: the GDScript solver must not be looser than the C++ one
    # (allow 25% headroom on the p99 to absorb skinning/pose differences).
    c99, g99 = pct(c_sig, 0.99), pct(g_sig, 0.99)
    ok = g99 <= max(c99 * 1.25, c99 + 0.002)
    print()
    print(f"p99 residual stretch: C++ {c99 * 100:.3f}%  vs  GDScript {g99 * 100:.3f}%")
    print("GATE2 COMPARE:", "PASS — GDScript solver matches the C++ reference" if ok
          else "FAIL — GDScript is looser than the C++ reference; investigate")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
