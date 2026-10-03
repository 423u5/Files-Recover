#!/usr/bin/env bash
# Makes MP4 files whose AVC and HEVC video exercises the media checks beyond
# what the small embedded samples do (they are Baseline AVC and plain HEVC):
#
#   AVC (libx264): CAVLC and CABAC, B pictures with pyramid references,
#   weighted prediction, interlaced (MBAFF) coding, several slices per
#   picture, 4:2:2 and 4:4:4 at 10 bits, lossless (transform bypass),
#   custom scaling matrices, HRD parameters, and parameter sets in the
#   samples ('avc3');
#   HEVC (libx265): wavefront entry points, several slices, Main 10, 4:4:4
#   (range extensions), lossless, weighted prediction, an open GOP (CRA and
#   RASL pictures), 16x16 coding tree blocks, temporal sub-layers, HRD
#   parameters, default scaling lists, and parameter sets in the samples
#   ('hev1').
#
# FFmpeg must decode every file without an error.
#
# Usage: make_video_media_vectors.sh <output directory>
# Needs ffmpeg with libx264 and libx265 (FFmpeg 6.1.1 of Ubuntu 24.04;
# tests/reference/make_mp4_samples.sh says where it comes from) and python3.
# Then
#   python3 embed_media_vectors.py ../validation/video_media_vectors.cpp \
#       validation/video_media_vectors.hpp video_vectors mp4 <output directory> \
#       "FFmpeg 6.1.1, libx264 and libx265"

set -euo pipefail
out="$1"
mkdir -p "$out"

# The inputs: a test pattern, and the same pattern fading in (for weighted prediction).
input() {
    if [ "$1" = fade ]; then
        echo "testsrc2=size=128x96:rate=10:duration=1,fade=in:0:10"
    else
        echo "testsrc2=size=128x96:rate=10:duration=1"
    fi
}

# encode <encoder> <name> <src|fade> <ffmpeg output options...>
encode() {
    local encoder="$1" name="$2" kind="$3"
    shift 3
    ffmpeg -hide_banner -loglevel error -y -f lavfi -i "$(input "$kind")" -an "$@" -c:v "$encoder"         -movflags +faststart "$out/$name.mp4"
}

avc() {
    encode libx264 "$@"
}

hevc() {
    encode libx265 "$@"
}

avc avc_cavlc_baseline src -profile:v baseline -x264-params keyint=4:log-level=error
avc avc_cabac_bframes src -profile:v high -bf 3 -x264-params b-pyramid=normal:ref=4:keyint=10:log-level=error
avc avc_weighted fade -profile:v high -bf 2 -x264-params weightp=2:weightb=1:log-level=error
avc avc_interlaced src -flags +ildct+ilme -x264-params interlaced=1:tff=1:log-level=error
avc avc_slices src -x264-params slices=4:log-level=error
avc avc_422_10bit src -pix_fmt yuv422p10le -profile:v high422 -x264-params log-level=error
avc avc_444_10bit src -pix_fmt yuv444p10le -profile:v high444 -x264-params log-level=error
avc avc_lossless src -pix_fmt yuv444p -qp 0 -x264-params log-level=error
avc avc_scaling src -profile:v high -x264-params \
    "cqm4=6,12,19,26,12,19,26,31,19,26,31,35,26,31,35,40:cqm8=6,10,13,16,18,23,25,27,10,11,16,18,23,25,27,29,13,16,18,23,25,27,29,31,16,18,23,25,27,29,31,33,18,23,25,27,29,31,33,36,23,25,27,29,31,33,36,38,25,27,29,31,33,36,38,40,27,29,31,33,36,38,40,42:log-level=error"
avc avc_hrd src -b:v 200k -maxrate 200k -bufsize 400k -x264-params nal-hrd=vbr:log-level=error
avc avc_avc3 src -tag:v avc3 -x264-params repeat-headers=1:keyint=5:log-level=error

hevc hevc_wpp src -x265-params ctu=32:wpp=1:log-level=error
hevc hevc_slices src -x265-params ctu=16:slices=3:log-level=error
hevc hevc_main10 src -pix_fmt yuv420p10le -x265-params log-level=error
hevc hevc_444 src -pix_fmt yuv444p -x265-params log-level=error
hevc hevc_lossless src -x265-params lossless=1:log-level=error
hevc hevc_weighted fade -x265-params weightp=1:weightb=1:bframes=2:log-level=error
hevc hevc_opengop src -x265-params keyint=4:min-keyint=4:open-gop=1:bframes=3:scenecut=0:log-level=error
hevc hevc_ctu16 src -x265-params ctu=16:min-cu-size=8:sao=0:wpp=0:amp=1:log-level=error
hevc hevc_temporal src -x265-params temporal-layers=3:bframes=3:log-level=error
hevc hevc_hrd src -b:v 200k -maxrate 200k -bufsize 400k -x265-params hrd=1:log-level=error
hevc hevc_scaling src -x265-params scaling-list=default:log-level=error
hevc hevc_hev1 src -tag:v hev1 -x265-params repeat-headers=1:keyint=5:log-level=error

for file in "$out"/*.mp4; do
    if ! ffmpeg -v error -i "$file" -f null - 2>"$out/decode.log" || [ -s "$out/decode.log" ]; then
        echo "FFmpeg does not decode $file cleanly:" >&2
        cat "$out/decode.log" >&2
        exit 1
    fi
done
rm -f "$out/decode.log"
ls -l "$out"
