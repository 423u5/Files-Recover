#!/bin/sh
# Makes small audio files with independent encoders (LAME, FFmpeg, faac,
# fdkaac, SoX, Python's wave module), for the samples embedded in
# tests/support/audio_samples.cpp and for the reference corpus of
# AudioReferenceTest.
#
# Usage: make_audio_samples.sh <output directory> [large]
#   Without "large", the files last a fraction of a second so they can be
#   embedded in the tests. With "large", they last 20 seconds, for
#   RECOVERY_AUDIO_REFERENCE_DIR (see docs/testing/testing.md).
#
# Needs sox, lame, ffmpeg, faac, fdkaac and python3 on PATH.
# tests/reference/make_audio_samples.ps1 adds files written by Windows' own
# encoders (Media Foundation, the speech synthesizer).
set -eu

out=$1
size=${2:-small}
mkdir -p "$out"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

if [ "$size" = large ]; then
    seconds=20
else
    seconds=0.25
fi

# Source signals: a chord, in stereo at 44.1 kHz and in mono at 8 kHz.
sox -n -r 44100 -c 2 -b 16 "$work/stereo.wav" synth "$seconds" sine 440 sine 660 gain -6
sox -n -r 8000 -c 1 -b 16 "$work/mono8k.wav" synth "$seconds" sine 440 gain -6
sox -n -r 22050 -c 1 -b 16 "$work/mono22k.wav" synth "$seconds" sine 440 gain -6
ff() { ffmpeg -nostdin -loglevel error -y "$@"; }

# MP3: LAME (with its Info or Xing tag unless the frames are too small for it)
# and FFmpeg's MP3 muxer around libmp3lame.
lame --quiet -b 64 "$work/stereo.wav" "$out/lame_cbr.mp3"
lame --quiet -V 5 "$work/stereo.wav" "$out/lame_vbr.mp3"
lame --quiet -p -b 96 "$work/stereo.wav" "$out/lame_crc.mp3"
lame --quiet -t -b 64 "$work/stereo.wav" "$out/lame_notag.mp3"
lame --quiet -m m -b 32 "$work/mono22k.wav" "$out/lame_mpeg2_mono.mp3"
lame --quiet -V 7 "$work/mono8k.wav" "$out/lame_mpeg25_vbr.mp3"
lame --quiet -b 64 --add-id3v2 --tt Title --ta Artist --tl Album "$work/stereo.wav" "$out/lame_tags.mp3"
lame --quiet -b 64 --id3v1-only --tt Title "$work/stereo.wav" "$out/lame_id3v1.mp3"
ff -i "$work/stereo.wav" -c:a libmp3lame -b:a 64k -metadata title=Title "$out/ffmpeg_tagged.mp3"
ff -i "$work/stereo.wav" -c:a libmp3lame -b:a 64k -write_xing 0 -id3v2_version 0 "$out/ffmpeg_bare.mp3"

# Raw AAC in ADTS: FFmpeg's encoder, faac and the Fraunhofer encoder.
ff -i "$work/stereo.wav" -c:a aac -b:a 64k "$out/ffmpeg_adts.aac"
faac -q 50 -o "$out/faac_adts.aac" "$work/mono22k.wav" 2>/dev/null
fdkaac -b 48 -f 2 -o "$out/fdkaac_adts.aac" "$work/stereo.wav" 2>/dev/null

# M4A: FFmpeg with moov after and before mdat, a generic brand, fragments
# and ALAC; mp4v2 through faac; fdkaac's own muxer.
ff -i "$work/stereo.wav" -c:a aac -b:a 64k "$out/ffmpeg_moov_last.m4a"
ff -i "$work/stereo.wav" -c:a aac -b:a 64k -movflags +faststart "$out/ffmpeg_faststart.m4a"
ff -i "$work/mono8k.wav" -c:a aac -b:a 24k -f mp4 -brand mp42 "$out/ffmpeg_brand_mp42.m4a"
ff -i "$work/mono8k.wav" -c:a aac -b:a 24k -f mp4 -movflags frag_keyframe+empty_moov "$out/ffmpeg_fragmented.m4a"
ff -i "$work/mono8k.wav" -c:a alac "$out/ffmpeg_alac.m4a"
faac -q 50 -w -o "$out/faac_mp4v2.m4a" "$work/mono22k.wav" 2>/dev/null
fdkaac -b 48 -o "$out/fdkaac.m4a" "$work/mono22k.wav" 2>/dev/null

# WAV: SoX (PCM, extensible, float, mu-law, A-law, IMA and MS ADPCM),
# FFmpeg (PCM with a LIST INFO chunk, float) and Python's wave module.
sox "$work/mono8k.wav" -b 8 "$out/sox_u8.wav"
sox "$work/mono8k.wav" "$out/sox_s16.wav"
sox "$work/mono8k.wav" -b 24 "$out/sox_s24_extensible.wav"
sox "$work/mono8k.wav" -e floating-point -b 32 "$out/sox_float.wav"
sox "$work/mono8k.wav" -e u-law "$out/sox_ulaw.wav"
sox "$work/mono8k.wav" -e a-law "$out/sox_alaw.wav"
sox "$work/mono8k.wav" -e ima-adpcm "$out/sox_ima_adpcm.wav"
sox "$work/mono8k.wav" -e ms-adpcm "$out/sox_ms_adpcm.wav"
ff -i "$work/mono8k.wav" -c:a pcm_s16le -metadata title=Title "$out/ffmpeg_info.wav"
ff -i "$work/mono8k.wav" -c:a pcm_f32le "$out/ffmpeg_float.wav"
python3 - "$work/mono8k.wav" "$out/python_wave.wav" <<'PY'
import sys, wave
with wave.open(sys.argv[1], "rb") as source, wave.open(sys.argv[2], "wb") as target:
    target.setparams(source.getparams())
    target.writeframes(source.readframes(source.getnframes()))
PY

ls -l "$out"
