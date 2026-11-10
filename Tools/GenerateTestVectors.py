"""Writes the PNG and zlib test vectors of the tests: streams made by the Python zlib module and a minimal PNG writer
that covers every color type, bit depth, filter type and Adam7, with the expected RGBA8 CRC computed from the spec rules."""
import random
import struct
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ADAM7 = [(0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4), (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2)]
CHANNELS = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}


def zlib_input(n):
    return bytes(((i * i + (i >> 3)) & 0xFF) if i % 3 else (ord("a") + i % 13) for i in range(n))


def pack_row(samples, depth):
    if depth == 16:
        return b"".join(struct.pack(">H", s) for s in samples)
    if depth == 8:
        return bytes(samples)
    out, acc, bits = bytearray(), 0, 0
    for s in samples:
        acc = (acc << depth) | s
        bits += depth
        if bits == 8:
            out.append(acc)
            acc, bits = 0, 0
    if bits:
        out.append(acc << (8 - bits))
    return bytes(out)


def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    return a if pa <= pb and pa <= pc else b if pb <= pc else c


def filter_rows(rows, bpp, first_type):
    out, prior = bytearray(), None
    for y, row in enumerate(rows):
        t = (first_type + y) % 5
        out.append(t)
        for i, v in enumerate(row):
            left = row[i - bpp] if i >= bpp else 0
            up = prior[i] if prior else 0
            corner = prior[i - bpp] if prior and i >= bpp else 0
            pred = [0, left, up, (left + up) >> 1, paeth(left, up, corner)][t]
            out.append((v - pred) & 0xFF)
        prior = row
    return bytes(out)


def to8(v, depth):
    return {1: 255 * v, 2: 85 * v, 4: 17 * v, 8: v}.get(depth, (v * 255 + 32895) >> 16)


def rgba(px, ct, depth, palette, trns):
    if ct == 3:
        r, g, b = palette[px[0]]
        return (r, g, b, trns[px[0]] if trns is not None and px[0] < len(trns) else 255)
    if ct == 0:
        g = to8(px[0], depth)
        return (g, g, g, 0 if trns is not None and px[0] == trns[0] else 255)
    if ct == 2:
        return tuple(to8(c, depth) for c in px) + (0 if trns is not None and list(px) == list(trns) else 255,)
    if ct == 4:
        g = to8(px[0], depth)
        return (g, g, g, to8(px[1], depth))
    return tuple(to8(c, depth) for c in px)


def chunk(t, body):
    return struct.pack(">I", len(body)) + t + body + struct.pack(">I", zlib.crc32(t + body))


def make_png(name, w, h, ct, depth, interlace, seed, palette_size=0, with_trns=False):
    rnd = random.Random(seed)
    maxv = (1 << depth) - 1
    palette = [(rnd.randrange(256), rnd.randrange(256), rnd.randrange(256)) for _ in range(palette_size)]
    hi = palette_size - 1 if ct == 3 else maxv
    px = [[tuple(rnd.randint(0, hi) for _ in range(CHANNELS[ct])) for _ in range(w)] for _ in range(h)]
    trns = None
    if with_trns:
        if ct == 3:
            trns = [rnd.randrange(256) for _ in range(palette_size // 2)]
        else:
            trns = list(px[0][0][:3] if ct == 2 else px[0][0][:1])
    bpp = max(1, CHANNELS[ct] * depth // 8)
    if interlace:
        raw = b""
        for i, (sx, sy, dx, dy) in enumerate(ADAM7):
            rows = [pack_row([s for x in range(sx, w, dx) for s in px[y][x]], depth) for y in range(sy, h, dy)]
            if rows and rows[0]:
                raw += filter_rows([bytearray(r) for r in rows], bpp, i)
    else:
        raw = filter_rows([bytearray(pack_row([s for p in row for s in p], depth)) for row in px], bpp, 0)
    data = zlib.compress(raw, 9)
    out = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, depth, ct, 0, 0, interlace))
    out += chunk(b"tEXt", b"Comment\x00helix test vector")
    if ct == 3:
        out += chunk(b"PLTE", bytes(c for p in palette for c in p))
    if trns is not None:
        body = bytes(trns) if ct == 3 else b"".join(struct.pack(">H", v) for v in trns)
        out += chunk(b"tRNS", body)
    for i in range(0, len(data), 50):
        out += chunk(b"IDAT", data[i : i + 50])
    out += chunk(b"IEND", b"")
    expected = bytes(c for row in px for p in row for c in rgba(p, ct, depth, palette, trns))
    return name, out, w, h, zlib.crc32(expected)


def array(name, data):
    lines = [", ".join(f"0x{b:02x}" for b in data[i : i + 24]) for i in range(0, len(data), 24)]
    return f"    inline constexpr u8 {name}[] = {{\n        " + ",\n        ".join(lines) + "};\n"


def main():
    pngs = [
        make_png("Gray1", 13, 7, 0, 1, 0, 1),
        make_png("Gray4Trns", 9, 9, 0, 4, 1, 2, with_trns=True),
        make_png("Gray16Trns", 6, 5, 0, 16, 0, 3, with_trns=True),
        make_png("Palette2", 17, 5, 3, 2, 0, 4, palette_size=4, with_trns=True),
        make_png("Palette8", 20, 20, 3, 8, 1, 5, palette_size=200),
        make_png("Rgb8Trns", 7, 11, 2, 8, 0, 6, with_trns=True),
        make_png("Rgb16", 5, 6, 2, 16, 1, 7),
        make_png("GrayAlpha8", 8, 3, 4, 8, 0, 8),
        make_png("GrayAlpha16", 3, 3, 4, 16, 1, 9),
        make_png("Rgba8", 33, 17, 6, 8, 1, 10),
        make_png("Rgba16", 4, 4, 6, 16, 0, 11),
        make_png("Single", 1, 1, 6, 8, 1, 12),
    ]
    text = "#pragma once\n\n// Generated by Tools/GenerateTestVectors.py.\n\n#include <Helix/Core/Basics/Types.h>\n\nnamespace helix::image::vectors\n{\n"
    for name, data, w, h, crc in pngs:
        text += array(f"k{name}", data)
    text += "\n    struct PngVector\n    {\n        const char* name;\n        const u8* data;\n        usize size;\n        u32 width, height, rgbaCrc;\n    };\n\n"
    text += "    inline constexpr PngVector kPngVectors[] = {\n"
    text += "".join(f'        {{"{n}", k{n}, sizeof(k{n}), {w}, {h}, 0x{c:08x}u}},\n' for n, _, w, h, c in pngs)
    text += "    };\n\n"
    source = zlib_input(5000)
    fixed = zlib.compressobj(9, zlib.DEFLATED, 15, 9, zlib.Z_FIXED)
    streams = [("ZlibLevel1", zlib.compress(source, 1)), ("ZlibLevel9", zlib.compress(source, 9)), ("ZlibStored", zlib.compress(source, 0)),
               ("ZlibFixed", fixed.compress(source) + fixed.flush())]
    for name, data in streams:
        text += array(f"k{name}", data)
    text += f"    inline constexpr u32 kZlibInputSize = {len(source)};\n    inline constexpr u32 kZlibInputCrc = 0x{zlib.crc32(source):08x}u;\n}}\n"
    (ROOT / "Tests" / "Support" / "TestVectors.h").write_text(text, encoding="ascii", newline="\n")


if __name__ == "__main__":
    main()
