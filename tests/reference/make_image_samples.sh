#!/bin/sh
# Makes small image files with independent encoders (libjpeg-turbo, libwebp,
# giflib), for the samples embedded in tests/support/image_samples.cpp and
# for the reference corpus of ImageReferenceTest.
#
# Usage: make_image_samples.sh <output directory> [large]
#   Without "large", the images are tiny (16x16 to 32x24) so they can be
#   embedded in the tests. With "large", they are 640x480 and more varied,
#   for RECOVERY_IMAGE_REFERENCE_DIR (see docs/testing/testing.md).
#
# Needs cjpeg, djpeg, jpegtran, cwebp, dwebp, img2webp, webpmux, gif2rgb and
# python3 on PATH. tests/reference/make_image_samples.ps1 adds files written
# by Windows' own encoders (GDI+).
set -eu

out=$1
size=${2:-small}
mkdir -p "$out"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

if [ "$size" = large ]; then
    w=640; h=480; jw=640; jh=480
else
    w=16; h=16; jw=32; jh=24
fi

# Source images: smooth gradients with some texture, as PPM (RGB), PAM (RGBA)
# and raw RGB, plus a second animation frame.
python3 - "$work" "$w" "$h" "$jw" "$jh" <<'PY'
import math, os, sys
work, w, h, jw, jh = sys.argv[1], *map(int, sys.argv[2:])

def pixel(x, y, width, height, phase=0.0):
    r = int(255 * x / max(1, width - 1))
    g = int(255 * y / max(1, height - 1))
    b = int(127 + 127 * math.sin((x + y) / 3.0 + phase))
    return r, g, b

def rgb(width, height, phase=0.0):
    return bytes(c for y in range(height) for x in range(width) for c in pixel(x, y, width, height, phase))

def ppm(name, width, height, phase=0.0):
    with open(os.path.join(work, name), "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (width, height) + rgb(width, height, phase))

ppm("src.ppm", w, h)
ppm("frame2.ppm", w, h, 1.5)
ppm("jpeg.ppm", jw, jh)
with open(os.path.join(work, "src.rgb"), "wb") as f:
    f.write(rgb(w, h))
alpha = bytes(c for y in range(h) for x in range(w)
              for c in (*pixel(x, y, w, h), int(255 * (x + y) / max(1, w + h - 2))))
with open(os.path.join(work, "src.pam"), "wb") as f:
    f.write(b"P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n" % (w, h) + alpha)
with open(os.path.join(work, "exif.bin"), "wb") as f:
    # A minimal little-endian TIFF structure: header and one empty IFD.
    f.write(b"II*\x00\x08\x00\x00\x00\x00\x00\x00\x00\x00\x00")
with open(os.path.join(work, "xmp.xml"), "wb") as f:
    f.write(b'<x:xmpmeta xmlns:x="adobe:ns:meta/"><rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"/></x:xmpmeta>')
PY

src=$work/src.ppm
jsrc=$work/jpeg.ppm

# JPEG: libjpeg-turbo.
cjpeg -quality 85 -outfile "$out/cjpeg_baseline.jpg" "$jsrc"
cjpeg -quality 85 -progressive -outfile "$out/cjpeg_progressive.jpg" "$jsrc"
cjpeg -quality 85 -restart 1B -outfile "$out/cjpeg_restart.jpg" "$jsrc"
cjpeg -quality 85 -grayscale -optimize -outfile "$out/cjpeg_gray_optimized.jpg" "$jsrc"
cjpeg -quality 85 -sample 1x1 -outfile "$out/cjpeg_444.jpg" "$jsrc"
cjpeg -quality 85 -arithmetic -outfile "$out/cjpeg_arithmetic.jpg" "$jsrc"
jpegtran -progressive -restart 1B -outfile "$out/jpegtran_progressive_restart.jpg" "$out/cjpeg_baseline.jpg"

# BMP: libjpeg-turbo's BMP writer (Windows and OS/2 headers, 24 and 8 bits).
djpeg -bmp -outfile "$out/djpeg_24.bmp" "$out/cjpeg_444.jpg"
djpeg -os2 -outfile "$out/djpeg_os2.bmp" "$out/cjpeg_444.jpg"
djpeg -bmp -colors 16 -outfile "$out/djpeg_8.bmp" "$out/cjpeg_444.jpg"

# WEBP: libwebp.
cwebp -quiet -q 80 "$src" -o "$out/cwebp_lossy.webp"
cwebp -quiet -lossless "$src" -o "$out/cwebp_lossless.webp"
cwebp -quiet -q 80 "$work/src.pam" -o "$out/cwebp_alpha.webp"
cwebp -quiet -lossless "$work/src.pam" -o "$out/cwebp_lossless_alpha.webp"
webpmux -set icc "$work/exif.bin" "$out/cwebp_lossy.webp" -o "$work/icc.webp"
webpmux -set exif "$work/exif.bin" "$work/icc.webp" -o "$work/exif.webp"
webpmux -set xmp "$work/xmp.xml" "$work/exif.webp" -o "$out/webpmux_metadata.webp"
img2webp -loop 0 -lossy -d 100 "$src" -lossless -d 100 "$work/frame2.ppm" -o "$out/img2webp_animated.webp"

# PNG and BMP: libwebp's decoder writes them through libpng and its own BMP writer.
dwebp -quiet "$out/cwebp_lossless_alpha.webp" -o "$out/dwebp_alpha.png"
dwebp -quiet "$out/cwebp_lossless_alpha.webp" -bmp -o "$out/dwebp_32.bmp"

# GIF: giflib (quantized from raw RGB to 2^4 colors).
gif2rgb -1 -c 4 -s "$w" "$h" "$work/src.rgb" > "$out/giflib.gif"

ls -l "$out"
