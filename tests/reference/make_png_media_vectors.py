"""Writes tests/validation/png_media_vectors.cpp: small PNG files whose zlib
streams Python's zlib made with every block type and strategy (fixed and
dynamic Huffman codes, stored blocks, a 512-byte window, many small blocks,
long matches), Adam7 images, and PNG files around DEFLATE bit streams built
here by hand to break one rule each. Every chunk CRC is correct, so only the
media decoder can tell the broken ones.

Usage: python3 make_png_media_vectors.py <output .cpp>
"""

import random
import struct
import sys
import zlib

ADAM7 = [(0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4), (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2)]
CHANNELS = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}


def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)


def png(width, height, depth, color, idat, interlaced=False, palette=None, split=0, iend=True):
    out = b"\x89PNG\r\n\x1a\n"
    out += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, depth, color, 0, 0, 1 if interlaced else 0))
    if palette is not None:
        out += chunk(b"PLTE", palette)
    if split:
        for start in range(0, len(idat), split):
            out += chunk(b"IDAT", idat[start:start + split])
    else:
        out += chunk(b"IDAT", idat)
    if iend:
        out += chunk(b"IEND", b"")
    return out


def scanlines(width, height, depth, color, interlaced, seed, repetitive=False, alphabet=256):
    rng = random.Random(seed)
    bpp = CHANNELS[color] * depth
    passes = ADAM7 if interlaced else [(0, 0, 1, 1)]
    raw = bytearray()
    row_index = 0
    for x0, y0, dx, dy in passes:
        columns = (width - x0 + dx - 1) // dx if width > x0 else 0
        rows = (height - y0 + dy - 1) // dy if height > y0 else 0
        if columns == 0 or rows == 0:
            continue
        row_bytes = (columns * bpp + 7) // 8
        first = bytes(rng.randrange(alphabet) for _ in range(row_bytes))
        for _ in range(rows):
            raw.append(row_index % 5)
            raw += first if repetitive else bytes(rng.randrange(alphabet) for _ in range(row_bytes))
            row_index += 1
    return bytes(raw)


def compress(raw, level=6, wbits=15, mem=8, strategy=zlib.Z_DEFAULT_STRATEGY):
    c = zlib.compressobj(level, zlib.DEFLATED, wbits, mem, strategy)
    return c.compress(raw) + c.flush()


class Bits:
    """DEFLATE bit order: values from their least significant bit, Huffman codes from their first bit."""

    def __init__(self):
        self.out = bytearray()
        self.acc = 0
        self.count = 0

    def put(self, value, n):
        for i in range(n):
            self.acc |= ((value >> i) & 1) << self.count
            self.count += 1
            if self.count == 8:
                self.out.append(self.acc)
                self.acc = 0
                self.count = 0

    def code(self, code, n):
        for i in range(n - 1, -1, -1):
            self.put((code >> i) & 1, 1)

    def align(self):
        if self.count:
            self.out.append(self.acc)
            self.acc = 0
            self.count = 0

    def fixed_literal(self, symbol):
        if symbol < 144:
            self.code(0x30 + symbol, 8)
        elif symbol < 256:
            self.code(0x190 + symbol - 144, 9)
        elif symbol < 280:
            self.code(symbol - 256, 7)
        else:
            self.code(0xC0 + symbol - 280, 8)


def zlib_stream(body, adler):
    return b"\x78\x01" + body + struct.pack(">I", adler)


def broken_fixed(build):
    bits = Bits()
    bits.put(1, 1)  # BFINAL
    bits.put(1, 2)  # fixed Huffman codes
    build(bits)
    bits.align()
    return zlib_stream(bytes(bits.out), 1)


def broken_dynamic(lengths_order_values, symbols):
    """A dynamic block header: HLIT 257, HDIST 1, the code length code
    lengths in transmission order, then (code, length) pairs."""
    bits = Bits()
    bits.put(1, 1)
    bits.put(2, 2)
    bits.put(0, 5)  # HLIT: 257
    bits.put(0, 5)  # HDIST: 1
    bits.put(len(lengths_order_values) - 4, 4)
    for value in lengths_order_values:
        bits.put(value, 3)
    for code, n in symbols:
        bits.code(code, n)
    bits.align()
    return zlib_stream(bytes(bits.out), 1)


vectors = []


def add(name, expect, detail, data):
    vectors.append((name, expect, detail, data))


# Valid streams of every kind.
raw = scanlines(16, 8, 8, 2, False, 1)
add("fixed_codes", "passed", "", png(16, 8, 8, 2, compress(raw, 6, 15, 8, zlib.Z_FIXED)))
raw = scanlines(48, 20, 8, 6, False, 2, repetitive=True)
add("dynamic_codes_split", "passed", "", png(48, 20, 8, 6, compress(raw, 9), split=37))
raw = scanlines(20, 10, 8, 0, False, 3)
add("window_512", "passed", "", png(20, 10, 8, 0, compress(raw, 6, 9)))
raw = scanlines(24, 12, 8, 2, False, 4)
add("huffman_only", "passed", "", png(24, 12, 8, 2, compress(raw, 6, 15, 8, zlib.Z_HUFFMAN_ONLY)))
raw = scanlines(24, 12, 8, 2, False, 5, repetitive=True)
add("run_lengths", "passed", "", png(24, 12, 8, 2, compress(raw, 6, 15, 8, zlib.Z_RLE)))
raw = scanlines(64, 64, 8, 0, False, 6, alphabet=12)
add("many_blocks", "passed", "", png(64, 64, 8, 0, compress(raw, 6, 15, 1)))
raw = scanlines(64, 64, 8, 0, False, 6, repetitive=True)
add("stored_blocks", "passed", "", png(64, 64, 8, 0, compress(raw, 0)))
palette = bytes(range(48))
raw = scanlines(13, 11, 4, 3, True, 7)
add("adam7_palette4", "passed", "", png(13, 11, 4, 3, compress(raw, 9), interlaced=True, palette=palette))
raw = scanlines(9, 7, 16, 2, True, 8)
add("adam7_rgb16", "passed", "", png(9, 7, 16, 2, compress(raw, 9), interlaced=True))
raw = scanlines(33, 5, 1, 0, False, 9)
add("gray1", "passed", "", png(33, 5, 1, 0, compress(raw, 9)))
raw = scanlines(200, 4, 8, 2, False, 10, repetitive=True)
add("long_matches", "passed", "", png(200, 4, 8, 2, compress(raw, 9)))
raw = scanlines(1, 1, 8, 0, False, 11)
add("one_pixel", "passed", "", png(1, 1, 8, 0, compress(raw, 9)))

# Streams that break one rule each.
raw = scanlines(16, 8, 8, 2, False, 12)
good = compress(raw, 9)
add("distance_too_far", "failed", "reaches before the data",
    png(16, 8, 8, 2, broken_fixed(lambda b: (b.fixed_literal(257), b.code(0, 5), b.fixed_literal(256)))))
bits = Bits()
bits.put(1, 1)
bits.put(3, 2)
bits.align()
add("block_type_3", "failed", "reserved type 3", png(16, 8, 8, 2, zlib_stream(bytes(bits.out), 1)))
bits = Bits()
bits.put(1, 1)
bits.put(0, 2)
bits.align()
add("stored_length_check", "failed", "length check",
    png(16, 8, 8, 2, zlib_stream(bytes(bits.out) + struct.pack("<HH", 5, 0x1234) + b"abcde", 1)))
add("distance_symbol_30", "failed", "invalid distance symbol 30",
    png(16, 8, 8, 2, broken_fixed(lambda b: (b.fixed_literal(65), b.fixed_literal(257), b.code(30, 5)))))
add("length_symbol_286", "failed", "invalid length symbol 286",
    png(16, 8, 8, 2, broken_fixed(lambda b: (b.fixed_literal(65), b.fixed_literal(286)))))
add("adler_mismatch", "failed", "Adler-32", png(16, 8, 8, 2, good[:-1] + bytes([good[-1] ^ 0x01])))
add("trailing_bytes", "failed", "more bytes of image data", png(16, 8, 8, 2, good + b"\x00\x01\x02"))
bad = bytearray(raw)
bad[(1 + 48) * 3] = 5
add("filter_type_5", "failed", "filter type 5", png(16, 8, 8, 2, compress(bytes(bad), 9)))
add("too_few_rows", "failed", "of the", png(16, 8, 8, 2, compress(raw[:-49], 9)))
add("too_much_data", "failed", "holds more than", png(16, 8, 8, 2, compress(raw + raw[:49], 9)))
add("code_lengths_oversubscribed", "failed", "code length code",
    png(16, 8, 8, 2, broken_dynamic([1] * 19, [])))
# Code length symbols 0 and 8 (one bit each): 256 literals of 8 bits, no end-of-block code.
add("no_end_of_block", "failed", "no end-of-block",
    png(16, 8, 8, 2, broken_dynamic([0, 0, 0, 1, 1], [(1, 1)] * 256 + [(0, 1), (0, 1)])))
# Code length symbols 8 and 16: a repeat first.
add("repeat_first", "failed", "no length before it",
    png(16, 8, 8, 2, broken_dynamic([1, 0, 0, 0, 1], [(1, 1)])))
add("preset_dictionary", "failed", "preset dictionary", png(16, 8, 8, 2, b"\x78\x20" + good[2:]))
add("header_check", "failed", "check bits", png(16, 8, 8, 2, b"\x78\x02" + good[2:]))
add("not_deflate", "failed", "not deflate", png(16, 8, 8, 2, b"\x77\x09" + good[2:]))

# The file ends inside the zlib stream (no IEND).
raw = scanlines(48, 20, 8, 6, False, 13)
whole = png(48, 20, 8, 6, compress(raw, 9), iend=False)
add("cut_in_stream", "truncated", "ends inside", whole[: len(whole) - 200])

lines = [
    "// Generated by tests/reference/make_png_media_vectors.py. Do not edit.",
    "",
    '#include "validation/png_media_vectors.hpp"',
    "",
    "#include <array>",
    "",
    "namespace recovery::test::png_vectors {",
    "",
    "namespace {",
    "",
]
for index, (name, _, _, data) in enumerate(vectors):
    lines.append(f"// {name}")
    lines.append(f"constexpr std::array<std::uint8_t, {len(data)}> kVector{index} = {{")
    for start in range(0, len(data), 16):
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in data[start:start + 16]) + ",")
    lines.append("};")
    lines.append("")
lines.append("}  // namespace")
lines.append("")
lines.append("const std::vector<Vector>& all() {")
lines.append("    static const std::vector<Vector> vectors = {")
for index, (name, expect, detail, _) in enumerate(vectors):
    lines.append(f'        {{"{name}", "{expect}", "{detail}", kVector{index}}},')
lines.append("    };")
lines.append("    return vectors;")
lines.append("}")
lines.append("")
lines.append("}  // namespace recovery::test::png_vectors")
with open(sys.argv[1], "w", newline="\n") as f:
    f.write("\n".join(lines) + "\n")
print(f"{len(vectors)} vectors")
