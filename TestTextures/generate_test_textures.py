#!/usr/bin/env python3
"""Generates the test textures used to verify TextureChannelPacker.

Pure standard library (zlib + struct), so it runs anywhere Python 3 does.
Run from this directory to regenerate every PNG:

    python3 generate_test_textures.py

See README.md for what each texture is meant to exercise.
"""

import math
import os
import struct
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


def main():
    out = os.path.dirname(os.path.abspath(__file__))

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
