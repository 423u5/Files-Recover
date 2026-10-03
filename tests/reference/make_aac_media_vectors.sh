#!/usr/bin/env bash
# Makes ADTS files with CRC protection (fdkaac -C) for the AAC media decoder
# tests: mono and stereo at constant and variable bitrates, 5.1 (SCE, CPE,
# CPE, LFE), digital silence (channel streams of the zero codebook, read to
# their ends), clicks (short windows), and a low sampling rate. FFmpeg must
# decode every file. (The libfdk-aac of Debian's fdkaac has no SBR, so there
# is no HE-AAC file.)
#
# Usage: make_aac_media_vectors.sh <output directory>
# Needs sox, fdkaac and ffmpeg (tests/reference/make_audio_samples.sh says
# where they come from) and python3. Then
#   python3 embed_media_vectors.py ../validation/aac_media_vectors.cpp \
#       validation/aac_media_vectors.hpp aac_vectors aac <output directory>

set -euo pipefail
out="$1"
mkdir -p "$out"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

sox -n -r 44100 -c 1 -b 16 "$work/mono.wav" synth 0.25 sine 440 sine 1300 vol 0.5
sox -n -r 48000 -c 2 -b 16 "$work/stereo.wav" synth 0.25 pinknoise pinknoise vol 0.6
sox -n -r 32000 -c 6 -b 16 "$work/surround.wav" synth 0.2 sine 300 sine 400 sine 500 sine 600 sine 80 sine 700 vol 0.4
sox -n -r 44100 -c 2 -b 16 "$work/silence.wav" trim 0 0.15
sox -n -r 44100 -c 2 -b 16 "$work/clicks.wav" synth 0.4 square 3 square 5 vol 0.5
sox -n -r 8000 -c 1 -b 16 "$work/low.wav" synth 0.5 sine 300 vol 0.5

fdkaac -S -f 2 -C -p 2 -b 96000 "$work/mono.wav" -o "$out/fdk_crc_mono.aac"
fdkaac -S -f 2 -C -p 2 -m 3 "$work/mono.wav" -o "$out/fdk_crc_mono_vbr.aac"
fdkaac -S -f 2 -C -p 2 -b 128000 "$work/stereo.wav" -o "$out/fdk_crc_stereo.aac"
fdkaac -S -f 2 -C -p 2 -m 4 "$work/stereo.wav" -o "$out/fdk_crc_stereo_vbr.aac"
fdkaac -S -f 2 -C -p 2 -b 192000 "$work/surround.wav" -o "$out/fdk_crc_surround.aac"
fdkaac -S -f 2 -C -p 2 -b 96000 "$work/silence.wav" -o "$out/fdk_crc_silence.aac"
fdkaac -S -f 2 -C -p 2 -b 128000 "$work/clicks.wav" -o "$out/fdk_crc_clicks.aac"
fdkaac -S -f 2 -C -p 2 -b 24000 "$work/low.wav" -o "$out/fdk_crc_8khz.aac"

for file in "$out"/*.aac; do
    ffmpeg -v error -i "$file" -f null -
done
ls -l "$out"
