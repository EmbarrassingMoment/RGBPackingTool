#!/usr/bin/env python3
"""Generates the test textures used to verify TextureChannelPacker.

Pure standard library (zlib + struct), so it runs anywhere Python 3 does.
Run from this directory to regenerate the standard PNGs:

    python3 generate_test_textures.py

The 16384x16384 stress-test textures are not built by default (they take a
while and are not needed for everyday checks). Add --huge for those:

    python3 generate_test_textures.py --huge

See README.md for what each texture is meant to exercise.
"""

import math
import os
import struct
import sys
import zlib

# Colour types from the PNG spec.
COLOR_GRAY = 0
COLOR_RGBA = 6


def write_png(path, width, height, rows, color_type, bit_depth=8):
    """Writes a PNG. `rows` is a list of bytes objects, one per scanline."""

    def chunk(tag, payload):
        data = tag + payload
        return struct.pack(">I", len(payload)) + data + struct.pack(">I", zlib.crc32(data) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", width, height, bit_depth, color_type, 0, 0, 0)
    # Filter type 0 (None) in front of every scanline keeps the writer trivial.
    raw = b"".join(b"\x00" + row for row in rows)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", ihdr))
        f.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        f.write(chunk(b"IEND", b""))


def write_png_streaming(path, width, height, row_iter, color_type, bit_depth=8, level=1):
    """Writes a PNG without ever holding the whole image in memory.

    Scanlines are pulled from `row_iter` and fed through an incremental zlib
    compressor, emitting an IDAT chunk per block (the PNG spec allows any number
    of IDAT chunks). Peak memory is a few MB regardless of image size, which is
    what makes the 16K x 16K textures (1 GiB of raw RGBA) practical to build.

    Level 1 is used by default: the synthetic patterns here are so repetitive
    that the higher levels cost a lot of time for almost no size gain.
    """

    def chunk(tag, payload):
        data = tag + payload
        return struct.pack(">I", len(payload)) + data + struct.pack(">I", zlib.crc32(data) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", width, height, bit_depth, color_type, 0, 0, 0)
    compressor = zlib.compressobj(level)
    pending = bytearray()
    flush_at = 4 * 1024 * 1024

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", ihdr))

        for row in row_iter:
            pending += b"\x00"  # filter type 0 (None)
            pending += row
            if len(pending) >= flush_at:
                block = compressor.compress(bytes(pending))
                pending.clear()
                if block:
                    f.write(chunk(b"IDAT", block))

        if pending:
            block = compressor.compress(bytes(pending))
            if block:
                f.write(chunk(b"IDAT", block))

        tail = compressor.flush()
        if tail:
            f.write(chunk(b"IDAT", tail))

        f.write(chunk(b"IEND", b""))


def clamp8(value):
    return 0 if value < 0 else (255 if value > 255 else int(value))


# ---------------------------------------------------------------------------
# Patterns. Each returns 0-255 and is deliberately unmistakable in isolation,
# so a mis-routed channel is obvious the moment the preview grid appears.
# ---------------------------------------------------------------------------

def pattern_gradient_h(x, y, w, h):
    """Smooth left-to-right ramp."""
    return clamp8(x * 255 / max(w - 1, 1))


def pattern_gradient_v(x, y, w, h):
    """Smooth top-to-bottom ramp."""
    return clamp8(y * 255 / max(h - 1, 1))


def pattern_checker(x, y, w, h, size=64):
    """Hard-edged checkerboard."""
    return 255 if ((x // size) + (y // size)) % 2 == 0 else 0


def pattern_rings(x, y, w, h):
    """Smooth concentric rings centred on the image."""
    dx = x - w / 2.0
    dy = y - h / 2.0
    dist = math.sqrt(dx * dx + dy * dy)
    period = max(w, h) / 8.0
    return clamp8(127.5 + 127.5 * math.sin(dist / period * 2.0 * math.pi))


def pattern_diagonal(x, y, w, h, size=32):
    """Hard-edged diagonal stripes."""
    return 255 if ((x + y) // size) % 2 == 0 else 0


def pattern_vignette(x, y, w, h):
    """Bright centre falling off to dark corners (stands in for AO)."""
    dx = (x - w / 2.0) / (w / 2.0)
    dy = (y - h / 2.0) / (h / 2.0)
    dist = min(math.sqrt(dx * dx + dy * dy), 1.0)
    return clamp8(255 * (1.0 - dist * dist))


def pattern_blobs(x, y, w, h):
    """Soft overlapping blobs (stands in for roughness variation)."""
    total = 0.0
    for cx, cy, radius in ((0.30, 0.30, 0.22), (0.70, 0.40, 0.18), (0.45, 0.75, 0.25)):
        dx = (x / w) - cx
        dy = (y / h) - cy
        dist = math.sqrt(dx * dx + dy * dy)
        if dist < radius:
            total += math.cos(dist / radius * math.pi / 2.0) ** 2
    return clamp8(40 + 215 * min(total, 1.0))


def pattern_circle_mask(x, y, w, h):
    """Hard circle: fully metallic inside, non-metallic outside."""
    dx = (x - w / 2.0) / (w / 2.0)
    dy = (y - h / 2.0) / (h / 2.0)
    return 255 if math.sqrt(dx * dx + dy * dy) < 0.6 else 0


def pattern_constant(value):
    return lambda x, y, w, h: value


# ---------------------------------------------------------------------------
# Builders
# ---------------------------------------------------------------------------

def build_rgba(path, width, height, channels):
    """channels: (r, g, b, a) pattern callables."""
    rows = []
    for y in range(height):
        row = bytearray(width * 4)
        for x in range(width):
            offset = x * 4
            for index, fn in enumerate(channels):
                row[offset + index] = fn(x, y, width, height)
        rows.append(bytes(row))
    write_png(path, width, height, rows, COLOR_RGBA, 8)
    print(f"  {os.path.basename(path):<38} {width}x{height} RGBA8")


def build_gray8(path, width, height, fn):
    rows = []
    for y in range(height):
        rows.append(bytes(fn(x, y, width, height) for x in range(width)))
    write_png(path, width, height, rows, COLOR_GRAY, 8)
    print(f"  {os.path.basename(path):<38} {width}x{height} Gray8")


def build_gray16(path, width, height, fn16):
    """fn16 returns 0-65535 so the 16-bit source path gets real precision."""
    rows = []
    for y in range(height):
        row = bytearray(width * 2)
        for x in range(width):
            value = fn16(x, y, width, height)
            row[x * 2] = (value >> 8) & 0xFF
            row[x * 2 + 1] = value & 0xFF
        rows.append(bytes(row))
    write_png(path, width, height, rows, COLOR_GRAY, 16)
    print(f"  {os.path.basename(path):<38} {width}x{height} Gray16")


def build_rgba_max(path, size=16384):
    """16384x16384 RGBA8 — the tool's maximum supported dimensions.

    Source data is 16384*16384*4 = 1 GiB, which still fits a 32-bit byte count,
    so this exercises the full 16K processing path rather than the oversize
    rejection. (Tripping that rejection needs more than 2 GiB of source, i.e. a
    16K RGBA32F at 4 GiB — see README.)

    Rows are assembled with extended-slice assignment on a bytearray (a C-level
    copy per channel) instead of a per-pixel Python loop, which is the
    difference between seconds and hours at this size.
    """
    w = h = size
    block = w // 16

    ramp = bytes(x * 255 // (w - 1) for x in range(w))                              # R: horizontal ramp
    checker_even = bytes(255 if (x // block) % 2 == 0 else 0 for x in range(w))     # G: coarse checkerboard
    checker_odd = bytes(255 - value for value in checker_even)
    opaque = b"\xff" * w                                                            # A: constant

    def rows():
        row = bytearray(w * 4)
        row[0::4] = ramp
        row[3::4] = opaque
        for y in range(h):
            row[1::4] = checker_even if (y // block) % 2 == 0 else checker_odd
            row[2::4] = bytes((y * 255 // (h - 1),)) * w                            # B: vertical ramp
            # Safe to hand out the buffer itself: the writer copies immediately.
            yield row

    write_png_streaming(path, w, h, rows(), COLOR_RGBA, 8)
    print(f"  {os.path.basename(path):<38} {w}x{h} RGBA8   ({os.path.getsize(path) / 1024 / 1024:.1f} MB on disk)")


def build_gray_max(path, size=16384):
    """16384x16384 Gray8 — 256 MB of source data, i.e. large but within int32.

    Unlike the RGBA one this can be run end to end through the packer, so it
    tests that 16K actually works rather than that it is rejected cleanly.
    """
    w = h = size
    ramp = bytes(x * 255 // (w - 1) for x in range(w))

    def rows():
        for y in range(h):
            offset = y * 255 // (h - 1)
            # Diagonal gradient expressed as a 256-entry remap of the horizontal
            # ramp, so each row is one C-level translate instead of 16384 steps.
            yield ramp.translate(bytes((value + offset) // 2 for value in range(256)))

    write_png_streaming(path, w, h, rows(), COLOR_GRAY, 8)
    print(f"  {os.path.basename(path):<38} {w}x{h} Gray8   ({os.path.getsize(path) / 1024 / 1024:.1f} MB on disk)")


def main():
    out = os.path.dirname(os.path.abspath(__file__))

    if "--huge" in sys.argv:
        print("Stress-test textures (16384x16384) — this takes a few minutes:")
        build_rgba_max(os.path.join(out, "T_StressTest_16K_ORM.png"))
        build_gray_max(os.path.join(out, "T_StressTest_16K_Gray.png"))
        return

    print("Unpack test textures:")

    # Four unmistakable patterns, one per channel. Unpacking this should yield
    # gradient / checkerboard / rings / stripes in that order.
    build_rgba(
        os.path.join(out, "T_UnpackTest_ORM.png"), 512, 512,
        (pattern_gradient_h, pattern_checker, pattern_rings, pattern_diagonal),
    )

    # Blue is uniformly 0 and Alpha uniformly 255: both should be badged
    # "Uniform" and unchecked automatically, while R and G stay checked.
    build_rgba(
        os.path.join(out, "T_UnpackTest_Uniform_ORM.png"), 256, 256,
        (pattern_vignette, pattern_gradient_v, pattern_constant(0), pattern_constant(255)),
    )

    # Non-square source: the 2x2 preview grid and the outputs must keep 4:1.
    build_rgba(
        os.path.join(out, "T_UnpackTest_NonSquare_ORM.png"), 512, 128,
        (pattern_gradient_h, pattern_checker, pattern_rings, pattern_diagonal),
    )

    print("Pack test textures:")

    build_gray8(os.path.join(out, "T_TestMaterial_AO.png"), 512, 512, pattern_vignette)
    # Deliberately half resolution to exercise auto-resize during packing.
    build_gray8(os.path.join(out, "T_TestMaterial_Roughness.png"), 256, 256, pattern_blobs)
    build_gray8(os.path.join(out, "T_TestMaterial_Metallic.png"), 512, 512, pattern_circle_mask)

    # 16-bit source, so the G16 path is covered.
    build_gray16(
        os.path.join(out, "T_TestMaterial_Height16.png"), 512, 512,
        lambda x, y, w, h: int(65535 * (0.5 + 0.5 * math.sin(x / w * 4.0 * math.pi) * math.cos(y / h * 4.0 * math.pi))),
    )


if __name__ == "__main__":
    main()
