#!/usr/bin/env python3
"""Convert a simple PNG into VibeOS wallpaper pixels.

The output format is intentionally tiny:
  magic "VWP1", little-endian u32 width, little-endian u32 height,
  followed by width*height little-endian XRGB pixels (0x00RRGGBB).

This keeps the default wallpaper deterministic at boot without putting a PNG
decoder in the kernel or depending on stb_image during the early desktop scene.
"""
import struct
import sys
import zlib


def die(msg):
    raise SystemExit("png_to_vwp: " + msg)


def paeth(a, b, c):
    p = a + b - c
    pa = abs(p - a)
    pb = abs(p - b)
    pc = abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def unfilter_row(ftype, row, prev, bpp):
    out = bytearray(row)
    for i, value in enumerate(out):
        left = out[i - bpp] if i >= bpp else 0
        up = prev[i] if prev else 0
        up_left = prev[i - bpp] if prev and i >= bpp else 0
        if ftype == 0:
            pass
        elif ftype == 1:
            out[i] = (value + left) & 0xFF
        elif ftype == 2:
            out[i] = (value + up) & 0xFF
        elif ftype == 3:
            out[i] = (value + ((left + up) >> 1)) & 0xFF
        elif ftype == 4:
            out[i] = (value + paeth(left, up, up_left)) & 0xFF
        else:
            die("unsupported PNG filter %d" % ftype)
    return out


def decode_png_rgb(data):
    if not data.startswith(b"\x89PNG\r\n\x1a\n"):
        die("not a PNG")

    pos = 8
    width = height = color_type = bit_depth = interlace = None
    idat = bytearray()

    while pos + 12 <= len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        ctype = data[pos + 4:pos + 8]
        payload = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if ctype == b"IHDR":
            width, height, bit_depth, color_type, _comp, _filter, interlace = struct.unpack(">IIBBBBB", payload)
        elif ctype == b"IDAT":
            idat.extend(payload)
        elif ctype == b"IEND":
            break

    if width is None:
        die("missing IHDR")
    if bit_depth != 8 or color_type not in (2, 6) or interlace != 0:
        die("supports only non-interlaced 8-bit RGB/RGBA PNGs")

    channels = 3 if color_type == 2 else 4
    stride = width * channels
    raw = zlib.decompress(bytes(idat))
    expected = (stride + 1) * height
    if len(raw) < expected:
        die("truncated decompressed image")

    pixels = bytearray()
    prev = None
    off = 0
    for _y in range(height):
        ftype = raw[off]
        row = unfilter_row(ftype, raw[off + 1:off + 1 + stride], prev, channels)
        off += stride + 1
        for x in range(width):
            i = x * channels
            pixels.extend(struct.pack("<I", (row[i] << 16) | (row[i + 1] << 8) | row[i + 2]))
        prev = row

    return width, height, pixels


def main():
    if len(sys.argv) != 3:
        die("usage: png_to_vwp.py <input.png> <output.vwp>")
    with open(sys.argv[1], "rb") as f:
        data = f.read()
    width, height, pixels = decode_png_rgb(data)
    with open(sys.argv[2], "wb") as f:
        f.write(b"VWP1")
        f.write(struct.pack("<II", width, height))
        f.write(pixels)
    print("png_to_vwp: wrote %s (%dx%d)" % (sys.argv[2], width, height))


if __name__ == "__main__":
    main()
