#!/usr/bin/env python3
"""Gate 1 comparison: C++ asset loader vs GDScript tfx_asset.gd parser.

Usage (from the repo root):
    python tools/compare_gate1.py

Reads demo/gate1_cpp.txt (written by the C++ demo on F5) and
demo/gate1_gdscript.txt (written by `godot --headless --path demo -s
res://tests/gate1_dump.gd`), compares them line by line, and prints PASS or
FAIL. Numbers must match within 1e-4 (the C++ computes in 32-bit floats,
GDScript in 64-bit, so the last decimals can legitimately differ).
"""
import sys
from pathlib import Path

TOL = 1e-4


def tokens(line: str):
    out = []
    for t in line.split():
        try:
            out.append(float(t))
        except ValueError:
            out.append(t)
    return out


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    cpp_path = root / "demo" / "gate1_cpp.txt"
    gd_path = root / "demo" / "gate1_gdscript.txt"

    for p, hint in ((cpp_path, "run the demo in Godot (F5) first"),
                    (gd_path, "run: <godot> --headless --path demo -s res://tests/gate1_dump.gd")):
        if not p.exists():
            print(f"FAIL: missing {p} — {hint}")
            return 1

    cpp_lines = cpp_path.read_text().strip().splitlines()
    gd_lines = gd_path.read_text().strip().splitlines()

    if len(cpp_lines) != len(gd_lines):
        print(f"FAIL: line count differs (C++ {len(cpp_lines)} vs GDScript {len(gd_lines)})")
        return 1

    mismatches = 0
    for i, (cl, gl) in enumerate(zip(cpp_lines, gd_lines), 1):
        ct, gt = tokens(cl), tokens(gl)
        if len(ct) != len(gt):
            print(f"FAIL line {i}: token count differs\n  C++: {cl}\n  GD : {gl}")
            mismatches += 1
            continue
        for c, g in zip(ct, gt):
            ok = (abs(c - g) <= TOL) if isinstance(c, float) and isinstance(g, float) else (c == g)
            if not ok:
                print(f"FAIL line {i}: {c!r} != {g!r}\n  C++: {cl}\n  GD : {gl}")
                mismatches += 1
                break

    if mismatches:
        print(f"\nFAIL: {mismatches} mismatching line(s) out of {len(cpp_lines)}")
        return 1
    print(f"PASS: all {len(cpp_lines)} lines match (tolerance {TOL})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
