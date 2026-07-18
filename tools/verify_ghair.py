#!/usr/bin/env python3
"""Stdlib-only structural verifier / format reference for .ghair v1.

Usage:
    python tools/verify_ghair.py <file.ghair>

Exits 0 and prints a summary on a structurally valid file; exits nonzero
with a clear message on any structural error (truncation, bad magic, chunk
overrun, etc). No bpy dependency -- this is both the maintainer's sanity
check and the C++ importer's authoritative format reference. Uses only
struct/json/sys/os from the standard library.

.ghair v1 FORMAT (kept in sync with tools/blender_hair_extract.py, which is
the canonical doc comment -- read that if the two ever disagree).

Little-endian throughout.

Header (20 bytes):
    8 bytes   magic              ASCII "GHAIRv01"
    u32       numStrands
    u32       vps                points per strand
    u32       numChunks

Then `numChunks` chunks, back to back:
    u32       fourcc             4 ASCII bytes, NOT nul-terminated
    u64       payloadByteLength
    <payloadByteLength bytes>

Readers must skip any fourcc they don't recognize (this verifier does
exactly that for chunks it has no specific parsing for).

Chunks a compliant v1 writer produces:
    POS0  f32 x,y,z per point, strand-major. numStrands*vps*3 floats.
    RTUV  f32 u,v per strand (surface_uv_coordinate). numStrands*2 floats.
    META  UTF-8 JSON blob.
    WID0  (optional) f32 per point, from `radius`. numStrands*vps floats.
    TWST  (optional) f32 per strand, from a "twist" per-curve attribute.
"""

import json
import os
import struct
import sys

MAGIC = b"GHAIRv01"
HEADER_FMT = "<8sIII"
HEADER_SIZE = struct.calcsize(HEADER_FMT)
CHUNK_HDR_FMT = "<4sQ"
CHUNK_HDR_SIZE = struct.calcsize(CHUNK_HDR_FMT)


class GhairFormatError(Exception):
    pass


def read_ghair(path):
    """Parse a .ghair file, return (num_strands, vps, chunks) where chunks is
    an ordered list of (fourcc:str, payload:bytes). Raises GhairFormatError
    with a clear message on any structural problem."""
    with open(path, "rb") as f:
        data = f.read()

    if len(data) < HEADER_SIZE:
        raise GhairFormatError(
            f"file too short for header: {len(data)} bytes, need at least {HEADER_SIZE}")

    magic, num_strands, vps, num_chunks = struct.unpack_from(HEADER_FMT, data, 0)
    if magic != MAGIC:
        raise GhairFormatError(f"bad magic: {magic!r}, expected {MAGIC!r}")
    if vps == 0:
        raise GhairFormatError("vps is 0")

    chunks = []
    off = HEADER_SIZE
    for i in range(num_chunks):
        if off + CHUNK_HDR_SIZE > len(data):
            raise GhairFormatError(
                f"truncated file: chunk {i} header would read past EOF "
                f"(offset {off}, file size {len(data)})")
        fourcc_raw, length = struct.unpack_from(CHUNK_HDR_FMT, data, off)
        off += CHUNK_HDR_SIZE
        try:
            fourcc = fourcc_raw.decode("ascii")
        except UnicodeDecodeError:
            raise GhairFormatError(f"chunk {i} fourcc is not ASCII: {fourcc_raw!r}")
        if off + length > len(data):
            raise GhairFormatError(
                f"truncated file: chunk {i} ('{fourcc}') payload of {length} bytes "
                f"would read past EOF (offset {off}, file size {len(data)})")
        payload = data[off:off + length]
        off += length
        chunks.append((fourcc, payload))

    trailing = len(data) - off
    if trailing != 0:
        raise GhairFormatError(f"{trailing} trailing byte(s) after the last declared chunk")

    return num_strands, vps, chunks


def unpack_floats(payload):
    n = len(payload) // 4
    if n * 4 != len(payload):
        raise GhairFormatError(f"payload size {len(payload)} is not a multiple of 4 (f32)")
    return struct.unpack("<%df" % n, payload) if n else ()


def main():
    if len(sys.argv) != 2:
        print("usage: python tools/verify_ghair.py <file.ghair>", file=sys.stderr)
        return 2

    path = sys.argv[1]
    if not os.path.isfile(path):
        print(f"ERROR: no such file: {path}", file=sys.stderr)
        return 2

    try:
        num_strands, vps, chunks = read_ghair(path)
    except GhairFormatError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        return 1

    print("file:", path, " (", os.path.getsize(path), "bytes )")
    print("header: numStrands =", num_strands, " vps =", vps, " numChunks =", len(chunks))

    by_fourcc = {}
    for fourcc, payload in chunks:
        by_fourcc.setdefault(fourcc, []).append(payload)
        print(f"  chunk '{fourcc}': {len(payload)} bytes")

    errors = []

    pos_payloads = by_fourcc.get("POS0")
    if not pos_payloads:
        errors.append("missing required chunk POS0")
    else:
        pos = unpack_floats(pos_payloads[0])
        expected = num_strands * vps * 3
        if len(pos) != expected:
            errors.append(
                f"POS0 has {len(pos)} floats, expected numStrands*vps*3 = {expected}")
        else:
            if num_strands > 0:
                s0 = pos[0:vps * 3]
                root = s0[0:3]
                tip = s0[(vps - 1) * 3:(vps - 1) * 3 + 3]
                print("strand 0 root:", root)
                print("strand 0 tip: ", tip)

    uv_payloads = by_fourcc.get("RTUV")
    if not uv_payloads:
        errors.append("missing required chunk RTUV")
    else:
        uv = unpack_floats(uv_payloads[0])
        expected = num_strands * 2
        if len(uv) != expected:
            errors.append(f"RTUV has {len(uv)} floats, expected numStrands*2 = {expected}")

    meta_payloads = by_fourcc.get("META")
    if not meta_payloads:
        errors.append("missing required chunk META")
    else:
        try:
            meta = json.loads(meta_payloads[0].decode("utf-8"))
            print("META:", json.dumps(meta, indent=2))
        except (UnicodeDecodeError, json.JSONDecodeError) as e:
            errors.append(f"META is not valid UTF-8 JSON: {e}")

    if "WID0" in by_fourcc:
        wid = unpack_floats(by_fourcc["WID0"][0])
        expected = num_strands * vps
        if len(wid) != expected:
            errors.append(f"WID0 has {len(wid)} floats, expected numStrands*vps = {expected}")

    if "TWST" in by_fourcc:
        twst = unpack_floats(by_fourcc["TWST"][0])
        if len(twst) != num_strands:
            errors.append(f"TWST has {len(twst)} floats, expected numStrands = {num_strands}")

    if num_strands == 0:
        errors.append("numStrands is 0 (empty file?)")

    if errors:
        print("STRUCTURAL ERRORS:", file=sys.stderr)
        for e in errors:
            print("  -", e, file=sys.stderr)
        return 1

    print("OK: structurally valid .ghair v1 file")
    return 0


if __name__ == "__main__":
    sys.exit(main())
