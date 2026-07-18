#!/usr/bin/env python3
"""Generate small stdlib-only PNG test textures for hair_ribbon.gdshader's
texture modes v1 (tile/fit). No PIL/Pillow -- raw PNG via struct + zlib.

Run: python tools/make_test_textures.py
Writes:
  demo/textures/strand_tile_test.png   (128x128,  opaque, for tile mode)
  demo/textures/feather_fit_test.png   (128x256,  alpha silhouette, for fit mode)
"""
import os
import struct
import zlib
import math

OUT_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "demo", "textures")


def write_png(path, width, height, rows):
    """rows: list of `height` bytes objects, each width*4 RGBA8 bytes."""
    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xffffffff)

    sig = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)  # 8-bit RGBA, no interlace
    raw = bytearray()
    for row in rows:
        raw.append(0)  # per-scanline filter type: none
        raw.extend(row)
    idat = zlib.compress(bytes(raw), 9)

    with open(path, "wb") as f:
        f.write(sig)
        f.write(chunk(b"IHDR", ihdr))
        f.write(chunk(b"IDAT", idat))
        f.write(chunk(b"IEND", b""))


def make_strand_tile_test():
    """128x128, vertical light/dark brown streaks simulating painted strand
    clumps. Fully opaque -- validates tile mode's UV.y (across the ribbon) x
    fract(fraction_along_strand * texture_tiling) (along the ribbon, repeating)
    mapping."""
    width, height = 128, 128
    light = (150, 100, 60)
    dark = (70, 40, 20)
    rows = []
    for y in range(height):
        row = bytearray()
        for x in range(width):
            streak = (math.sin(x * 0.35) + math.sin(x * 0.11 + 1.7) * 0.6 + 1.6) / 3.2
            t = max(0.0, min(1.0, streak))
            r = int(dark[0] + (light[0] - dark[0]) * t)
            g = int(dark[1] + (light[1] - dark[1]) * t)
            b = int(dark[2] + (light[2] - dark[2]) * t)
            row.extend((r, g, b, 255))
        rows.append(bytes(row))
    write_png(os.path.join(OUT_DIR, "strand_tile_test.png"), width, height, rows)


def make_feather_fit_test():
    """128x256, symmetric leaf/feather silhouette: opaque teal interior with a
    dark central shaft line, transparent outside the silhouette. Row 0 (top,
    V=0) is the root end, row height-1 (bottom, V=1) is the tip -- validates
    fit mode's one-shot root->tip stretch (no repeat)."""
    width, height = 128, 256
    fill = (30, 140, 125, 255)
    shaft = (10, 45, 40, 255)
    transparent = (0, 0, 0, 0)
    rows = []
    for y in range(height):
        v = y / (height - 1)
        # Leaf envelope: narrow at the root, bulges in the middle, tapers to a
        # sharp point at the tip.
        half_width = 0.46 * (math.sin(math.pi * v) ** 0.6) * (1.0 - 0.15 * v)
        row = bytearray()
        for x in range(width):
            u = (x + 0.5) / width - 0.5  # -0.5..0.5 across the ribbon
            if abs(u) <= half_width:
                px = shaft if (abs(u) <= half_width * 0.08 and v > 0.03) else fill
            else:
                px = transparent
            row.extend(px)
        rows.append(bytes(row))
    write_png(os.path.join(OUT_DIR, "feather_fit_test.png"), width, height, rows)


if __name__ == "__main__":
    os.makedirs(OUT_DIR, exist_ok=True)
    make_strand_tile_test()
    make_feather_fit_test()
    print("Wrote %s" % os.path.join(OUT_DIR, "strand_tile_test.png"))
    print("Wrote %s" % os.path.join(OUT_DIR, "feather_fit_test.png"))
