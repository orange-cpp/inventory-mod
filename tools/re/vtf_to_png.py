"""Converts a VTF texture's largest mip (DXT1 / DXT3 / DXT5, or 8-bit RGBA / BGRA / RGB / BGR) to a PNG, with Pillow.

    python -I tools/re/vtf_to_png.py <texture.vtf> <out.png>

load(bytes) gives the Pillow image (tools/make_rounds_addon.py uses it).
"""
import io
import struct
import sys

from PIL import Image

FORMATS = {0: ("RGBA", 4), 1: ("ABGR", 4), 2: ("RGB", 3), 3: ("BGR", 3), 12: ("BGRA", 4), 13: ("DXT1", 8),
           14: ("DXT3", 16), 15: ("DXT5", 16)}


def size_of(fmt, w, h):
    name, n = FORMATS[fmt]
    if name.startswith("DXT"):
        return max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * n
    return w * h * n


def load(d):
    if d[:4] != b"VTF\0":
        raise SystemExit("not a VTF")
    major, minor, header_size = struct.unpack_from("<III", d, 4)
    width, height, flags, frames, first_frame = struct.unpack_from("<HHIHH", d, 16)
    hi_format, = struct.unpack_from("<i", d, 52)
    mips = d[56]
    lo_format, = struct.unpack_from("<i", d, 57)
    lo_w, lo_h = d[61], d[62]
    if hi_format not in FORMATS:
        raise SystemExit(f"format {hi_format} is not handled")
    lo_size = size_of(lo_format, lo_w, lo_h) if lo_format >= 0 and lo_w and lo_h else 0
    # the high-res data follows the header and the low-res thumbnail, smallest mip first; one frame, one face
    data_start = header_size + lo_size
    if major == 7 and minor >= 3:  # resources: the high-res image's offset is in its resource entry (tag 0x30)
        num_resources, = struct.unpack_from("<I", d, 68)
        for r in range(num_resources):
            tag, rflags, offset = struct.unpack_from("<3sBI", d, 80 + r * 8)
            if tag == b"\x30\0\0":
                data_start = offset
    total = sum(size_of(hi_format, max(1, width >> m), max(1, height >> m)) * frames for m in range(mips))
    largest = size_of(hi_format, width, height)
    offset = data_start + total - largest * frames
    raw = d[offset: offset + largest]
    name, _ = FORMATS[hi_format]
    if name.startswith("DXT"):
        dds = bytearray(128)
        struct.pack_into("<4sIIIIIII", dds, 0, b"DDS ", 124, 0x1007 | 0x80000, height, width, largest, 0, 1)
        struct.pack_into("<II4s", dds, 76, 32, 4, name.encode())  # pixel format: FOURCC
        struct.pack_into("<I", dds, 108, 0x1000)
        image = Image.open(io.BytesIO(bytes(dds) + raw))
        image.load()
    else:
        image = Image.frombytes("RGBA" if len(name) == 4 else "RGB", (width, height), raw, "raw", name)
    image.info["vtf"] = f"{width}x{height} {name}, {mips} mips, v{major}.{minor}"
    return image


def main(argv):
    image = load(open(argv[1], "rb").read())
    image.save(argv[2])
    print(f"{image.info['vtf']} -> {argv[2]}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
