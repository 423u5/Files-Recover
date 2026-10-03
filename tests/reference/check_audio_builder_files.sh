#!/bin/sh
# Checks the audio test builders' files with independent decoders, so that a
# builder and a format module cannot share a misreading of a specification.
# The files come from AudioBuilderExport.WritesFiles (see docs/testing/testing.md):
#
#   $env:RECOVERY_AUDIO_EXPORT_DIR = "<dir>"
#   ctest --preset msvc-debug -R AudioBuilderExport
#
# Usage: check_audio_builder_files.sh <directory of builder_*.mp3/.aac/.m4a/.wav>
# Needs ffmpeg, mpg123 and sox on PATH. FFmpeg decodes every file and stops at
# the first error it detects (-xerror, with CRC checks: a damaged frame or a
# chunk offset outside the file fails); mpg123 decodes the MP3 files once more
# (it does not check CRCs) and SoX reads the WAV files.
set -u

failed=0
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

report() {
    if [ "$1" -eq 0 ] && [ ! -s "$work/out" ]; then
        echo "  ok       $2"
    else
        echo "  FAILED   $2"
        sed 's/^/             /' "$work/out" | head -5
        failed=1
    fi
}

for file in "$1"/*.mp3 "$1"/*.aac "$1"/*.m4a "$1"/*.wav; do
    [ -e "$file" ] || continue
    ffmpeg -nostdin -v error -xerror -err_detect crccheck+bitstream+buffer -i "$file" -f null - > "$work/out" 2>&1
    report $? "ffmpeg $(basename "$file")"
done

for file in "$1"/*.mp3; do
    [ -e "$file" ] || continue
    # mpg123 prints its warnings and errors on stderr; -t decodes without output.
    mpg123 -q -t "$file" > "$work/out" 2>&1
    report $? "mpg123 $(basename "$file")"
done

for file in "$1"/*.wav; do
    [ -e "$file" ] || continue
    sox -V1 "$file" -n > "$work/out" 2>&1
    report $? "sox $(basename "$file")"
done

if [ "$failed" -ne 0 ]; then
    echo "some files were refused by an independent decoder"
    exit 1
fi
echo "every builder file was accepted"
