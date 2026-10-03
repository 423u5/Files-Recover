#!/bin/sh
# Makes small MP4 (ISO base media) files with independent writers, FFmpeg and
# GPAC's MP4Box, for the samples embedded in tests/support/mp4_samples.cpp
# and for the reference corpus of Mp4ReferenceTest.
#
# Usage: make_mp4_samples.sh <output directory> [large]
#   Without "large", the files last half a second at 64x48 so they can be
#   embedded in the tests. With "large", they last 20 seconds at 640x360, for
#   RECOVERY_MP4_REFERENCE_DIR (see docs/testing/testing.md).
#
# Needs ffmpeg (with libx264 and libx265) and MP4Box on PATH.
# tests/reference/make_mp4_samples.ps1 adds files written by Windows' own
# MP4 writer (Media Foundation).
set -eu

out=$1
size=${2:-small}
mkdir -p "$out"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

if [ "$size" = large ]; then
    seconds=20
    dims=640x360
else
    seconds=0.5
    dims=64x48
fi
ff() { ffmpeg -nostdin -loglevel error -y "$@"; }

# Sources: a test pattern at 10 frames per second, a chord in stereo at
# 44.1 kHz, a tone in mono at 8 kHz, and two subtitles.
ff -f lavfi -i "testsrc=size=$dims:rate=10:duration=$seconds" -pix_fmt yuv420p "$work/video.y4m"
ff -f lavfi -i "sine=frequency=440:sample_rate=44100:duration=$seconds" \
   -f lavfi -i "sine=frequency=660:sample_rate=44100:duration=$seconds" \
   -filter_complex "[0][1]amerge=inputs=2" -c:a pcm_s16le "$work/stereo.wav"
ff -f lavfi -i "sine=frequency=330:sample_rate=8000:duration=$seconds" -c:a pcm_s16le "$work/mono.wav"
printf '1\n00:00:00,000 --> 00:00:00,200\nOne\n\n2\n00:00:00,250 --> 00:00:00,450\nTwo\n' > "$work/text.srt"

x264="-c:v libx264 -preset ultrafast -tune zerolatency -g 5 -crf 40"
aac="-c:a aac -b:a 24k"

# FFmpeg's MP4 muxer: moov after mdat (its default) and before it, video
# only, audio only, several audio tracks, B-frames (ctts and an edit list),
# a subtitle track, fragments, H.265 and MPEG-4 Part 2; its QuickTime and 3GP
# muxers.
ff -i "$work/video.y4m" -i "$work/stereo.wav" $x264 $aac "$out/ffmpeg_h264_aac.mp4"
ff -i "$work/video.y4m" -i "$work/stereo.wav" $x264 $aac -movflags +faststart "$out/ffmpeg_faststart.mp4"
ff -i "$work/video.y4m" $x264 "$out/ffmpeg_video_only.mp4"
ff -i "$work/stereo.wav" $aac -f mp4 "$out/ffmpeg_audio_only.mp4"
ff -i "$work/video.y4m" -i "$work/stereo.wav" -i "$work/mono.wav" -map 0:v -map 1:a -map 2:a $x264 $aac \
   "$out/ffmpeg_two_audio.mp4"
ff -i "$work/video.y4m" -c:v libx264 -preset medium -bf 2 -g 5 -crf 40 "$out/ffmpeg_bframes.mp4"
ff -i "$work/video.y4m" -i "$work/text.srt" $x264 -c:s mov_text "$out/ffmpeg_subtitles.mp4"
ff -i "$work/video.y4m" -i "$work/stereo.wav" $x264 $aac -movflags frag_keyframe+empty_moov \
   "$out/ffmpeg_fragmented.mp4"
# Movie fragments three more ways (P12): data offsets from the moof box
# (CMAF), the first samples in moov and the rest in fragments, and a moof
# per track.
ff -i "$work/video.y4m" -i "$work/stereo.wav" $x264 $aac \
   -movflags frag_keyframe+empty_moov+default_base_moof "$out/ffmpeg_fragmented_cmaf.mp4"
ff -i "$work/video.y4m" -i "$work/stereo.wav" $x264 -g 2 $aac -movflags frag_keyframe \
   "$out/ffmpeg_fragmented_moov.mp4"
ff -i "$work/video.y4m" -i "$work/stereo.wav" $x264 $aac \
   -movflags frag_keyframe+empty_moov+separate_moof "$out/ffmpeg_fragmented_separate.mp4"
ff -i "$work/video.y4m" -c:v libx265 -preset ultrafast -crf 45 -tag:v hvc1 -x265-params log-level=error \
   "$out/ffmpeg_hevc.mp4"
ff -i "$work/video.y4m" -c:v mpeg4 -q:v 20 "$out/ffmpeg_mpeg4.mp4"
ff -i "$work/video.y4m" -i "$work/mono.wav" $x264 $aac -f mov "$out/ffmpeg_quicktime.mov"
ff -i "$work/video.y4m" -i "$work/mono.wav" $x264 $aac -f 3gp "$out/ffmpeg_3gp.3gp"

# GPAC's MP4Box, from raw H.264 and ADTS: interleaved with moov first (its
# default), flat (media data first, not interleaved), 64-bit chunk offsets,
# compact sample sizes, padding after moov, and fragments.
ff -i "$work/video.y4m" $x264 -f h264 "$work/video.h264"
ff -i "$work/stereo.wav" $aac -f adts "$work/audio.aac"
box() { MP4Box -quiet "$@" >/dev/null 2>&1; }
box -add "$work/video.h264:fps=10" -add "$work/audio.aac" -new "$out/gpac_h264_aac.mp4"
box -flat -add "$work/video.h264:fps=10" -add "$work/audio.aac" -new "$out/gpac_flat.mp4"
box -co64 -add "$work/video.h264:fps=10" -add "$work/audio.aac" -new "$out/gpac_co64.mp4"
box -add "$work/audio.aac:stz2" -new "$out/gpac_stz2.mp4"
box -moovpad 64 -add "$work/video.h264:fps=10" -new "$out/gpac_moovpad.mp4"
box -frag 200 -add "$work/video.h264:fps=10" -add "$work/audio.aac" -new "$out/gpac_fragmented.mp4"

ls -l "$out"
