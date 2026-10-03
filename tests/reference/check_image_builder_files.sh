#!/bin/sh
# Checks the image test builders' files with independent decoders, so that a
# builder and a format module cannot share a misreading of a specification.
# The files come from ImageBuilderExport.WritesFiles (see docs/testing/testing.md):
#
#   $env:RECOVERY_IMAGE_EXPORT_DIR = "<dir>"
#   ctest --preset msvc-debug -R ImageBuilderExport
#
# Usage: check_image_builder_files.sh <directory of builder_*.jpg/.png/.gif/.bmp/.webp>
# Needs djpeg, pngcheck, giftext, dwebp, webpinfo and cjpeg on PATH.
# check_image_builder_files.ps1 adds Windows' own decoder (GDI+), which is the
# only one here that reads RLE-compressed BMP files.
set -u

failed=0
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

report() {
    if [ "$1" -eq 0 ]; then
        echo "  ok       $2"
    else
        echo "  FAILED   $2"
        sed 's/^/             /' "$work/out" | head -5
        failed=1
    fi
}

for file in "$1"/*.jpg; do
    [ -e "$file" ] || continue
    djpeg -outfile "$work/out.ppm" "$file" > "$work/out" 2>&1
    report $? "djpeg $(basename "$file")"
done

for file in "$1"/*.png; do
    [ -e "$file" ] || continue
    pngcheck -q "$file" > "$work/out" 2>&1
    report $? "pngcheck $(basename "$file")"
done

for file in "$1"/*.gif; do
    [ -e "$file" ] || continue
    giftext "$file" > "$work/out" 2>&1
    report $? "giftext $(basename "$file")"
done

for file in "$1"/*.webp; do
    [ -e "$file" ] || continue
    webpinfo "$file" > "$work/out" 2>&1
    report $? "webpinfo $(basename "$file")"
    case "$(basename "$file")" in
        *animation*) ;;  # dwebp decodes still images only
        *)
            dwebp -quiet "$file" -o "$work/out.png" > "$work/out" 2>&1
            report $? "dwebp $(basename "$file")"
            ;;
    esac
done

# cjpeg's BMP reader takes 8, 24 and 32 bits per pixel with a core or
# BITMAPINFOHEADER header, and no compression. The other files (1, 4 and 16
# bits, V4 and V5 headers, RLE) are checked by GDI+ in the .ps1 instead.
for file in "$1"/*.bmp; do
    [ -e "$file" ] || continue
    case "$(basename "$file")" in
        *1bit*|*4bit*|*16bit*|*v4*|*v5*|*rle*) continue ;;
    esac
    cjpeg -outfile "$work/out.jpg" "$file" > "$work/out" 2>&1
    report $? "cjpeg $(basename "$file")"
done

if [ "$failed" -ne 0 ]; then
    echo "some files were refused by an independent decoder"
    exit 1
fi
echo "every builder file was accepted"
