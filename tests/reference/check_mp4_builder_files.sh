#!/bin/sh
# Checks the MP4 builder's files (exported by Mp4BuilderExport into
# RECOVERY_MP4_EXPORT_DIR) with demuxers this project did not write:
#
#  * FFmpeg: every sample it lists (with edit lists ignored) must be at the
#    offset and of the size the builder wrote in <name>.samples, and no
#    other; and it must remux the file without an error;
#  * GPAC: MP4Box must read the file (-info) without an error.
#
# The builder's video samples are placeholder bytes, not H.264: what FFmpeg's
# video decoder says about them while probing is set aside; any other
# message is shown.
#
# Usage: check_mp4_builder_files.sh <export directory>
# Needs ffprobe, ffmpeg and MP4Box on PATH. Prints one line per file and
# exits with 1 when any check fails.
set -u

dir=$1
failed=0
for file in "$dir"/builder_*.mp4; do
    name=$(basename "$file" .mp4)
    expected="$dir/$name.samples"
    listed=$(mktemp)
    messages=$(mktemp)
    ffprobe -v error -ignore_editlist 1 -show_entries packet=stream_index,pos,size -of csv=p=0 "$file" \
        2> "$messages" | awk -F, '{ print $1, $3, $2 }' | sort -n -k1,1 -s > "$listed"
    if grep -v '^\[h264 @' "$messages" | grep -q .; then
        echo "$name: ffprobe reports"
        grep -v '^\[h264 @' "$messages" | head -5
        failed=1
    elif ! sort -n -k1,1 -s "$expected" | cmp -s - "$listed"; then
        echo "$name: FFmpeg lists other samples than the builder wrote"
        sort -n -k1,1 -s "$expected" | diff - "$listed" | head -5
        failed=1
    elif ! ffmpeg -nostdin -v error -i "$file" -map 0 -c copy -f mp4 -y /dev/null 2> "$messages" ||
         grep -v '^\[h264 @' "$messages" | grep -q .; then
        echo "$name: FFmpeg cannot remux it"
        grep -v '^\[h264 @' "$messages" | head -5
        failed=1
    elif ! MP4Box -info "$file" >/dev/null 2>&1; then
        echo "$name: MP4Box cannot read it"
        failed=1
    else
        echo "$name: ok ($(wc -l < "$expected") samples)"
    fi
    rm -f "$listed" "$messages"
done
exit $failed
