#!/usr/bin/env bash
# Makes WebP files that exercise what the small embedded samples do not:
# VP8L images large enough for libwebp to use a color cache and meta prefix
# codes (an entropy image), color-indexed images packed 8, 4 and 2 pixels to
# a byte or not at all, lossless alpha with filtering, and lossy frames with
# segmentation and loop filter deltas. dwebp must decode every file.
#
# Usage: make_webp_media_vectors.sh <output directory>
# Needs cwebp and dwebp (libwebp 1.3.2; tests/reference/make_image_samples.sh
# says where they come from) and python3. Then
#   python3 embed_webp_media_vectors.py ../validation/webp_media_vectors.cpp <output directory>

set -euo pipefail
out="$1"
mkdir -p "$out"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

python3 - "$work" <<'PY'
import math
import random
import struct
import sys
import zlib

def png(path, width, height, pixels, alpha=False):
    channels = 4 if alpha else 3
    raw = bytearray()
    for y in range(height):
        raw.append(0)
        for x in range(width):
            raw += bytes(pixels(x, y)[:channels])
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    data = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6 if alpha else 2, 0, 0, 0))
    data += chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b"")
    open(path, "wb").write(data)

work = sys.argv[1]
rng = random.Random(7)
noise = [[rng.randrange(6) for _ in range(160)] for _ in range(120)]
def photo(x, y):
    r = int(128 + 100 * math.sin(x / 13.0) * math.cos(y / 17.0)) + noise[y][x]
    g = int(128 + 90 * math.sin((x + y) / 23.0)) + noise[y][(x * 7) % 160]
    b = (x * 3 + y * 2) % 256 if (x // 20 + y // 20) % 2 else 40 + noise[(y * 3) % 120][x]
    return (min(r, 255), min(g, 255), b, 255)
png(f"{work}/photo.png", 160, 120, photo)
for colors in (2, 3, 16, 200):
    palette = [(rng.randrange(256), rng.randrange(256), rng.randrange(256)) for _ in range(colors)]
    png(f"{work}/palette{colors}.png", 99, 37, lambda x, y: palette[(x // 3 + y // 2 * 5) % colors] + (255,))
def translucent(x, y):
    r, g, b, _ = photo(x, y)
    a = int(255 * (0.5 + 0.5 * math.sin(x / 9.0 + y / 11.0)))
    return (r, g, b, a)
png(f"{work}/alpha.png", 120, 90, translucent, alpha=True)
PY

cwebp -quiet -lossless -m 6 -q 100 "$work/photo.png" -o "$out/lossless_photo.webp"
for colors in 2 3 16 200; do
    cwebp -quiet -lossless -m 6 "$work/palette$colors.png" -o "$out/lossless_palette$colors.webp"
done
cwebp -quiet -q 80 -alpha_filter best -alpha_method 1 "$work/alpha.png" -o "$out/lossy_alpha_filtered.webp"
cwebp -quiet -lossless -exact "$work/alpha.png" -o "$out/lossless_alpha.webp"
cwebp -quiet -q 70 -segments 4 -sns 80 -f 60 -sharpness 3 "$work/photo.png" -o "$out/lossy_segments.webp"
cwebp -quiet -q 90 -segments 1 -sns 0 -f 0 "$work/photo.png" -o "$out/lossy_plain.webp"

for file in "$out"/*.webp; do
    dwebp -quiet "$file" -o /dev/null
done
ls -l "$out"
