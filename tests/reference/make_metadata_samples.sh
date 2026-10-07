#!/bin/sh
# Makes small media files that carry metadata (P17), written by independent
# tools, for tests/support/metadata_samples.cpp (embed_metadata_samples.py
# records what exiftool and ffprobe read in them):
#
#   * Exif in JPEG (both byte orders, with a thumbnail), PNG and WebP:
#     orientation, camera make and model, date taken with its offset;
#   * animations: GIF, APNG;
#   * ID3v2.3 and 2.4 tags with cover art (FFmpeg), ID3v2 in ADTS, an ID3
#     chunk in WAV (mutagen), RIFF INFO (FFmpeg);
#   * MP4 and M4A tags with cover art, creation time, rotation, and a
#     QuickTime file with its own text atoms.
#
# Usage: make_metadata_samples.sh <output directory>
#
# Needs ffmpeg (libx264, libmp3lame), exiftool, cwebp and python3 with
# mutagen on PATH (in WSL: source ~/audiotools/env.sh, the imgtools PATH,
# and exiftool as an alias; see docs/testing/testing.md).
set -eu

out=$1
mkdir -p "$out"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

ff() { ffmpeg -nostdin -loglevel error -y "$@"; }
exif() { exiftool -q -overwrite_original "$@"; }

# Sources: a test picture, a smaller one for thumbnails and covers, a tone.
ff -f lavfi -i testsrc=size=64x48:rate=1 -frames:v 1 "$work/picture.png"
ff -f lavfi -i testsrc=size=32x24:rate=1 -frames:v 1 -q:v 5 "$work/cover.jpg"
ff -f lavfi -i testsrc=size=16x12:rate=1 -frames:v 1 -q:v 8 "$work/thumbnail.jpg"
ff -f lavfi -i "sine=frequency=440:duration=0.5" -ac 2 -ar 44100 "$work/tone.wav"

# Exif in JPEG: big-endian (exiftool's default), and little-endian.
ff -i "$work/picture.png" -q:v 4 "$work/plain.jpg"
cp "$work/plain.jpg" "$out/exiftool_exif_mm.jpg"
exif -Orientation#=6 -Make=Canon "-Model=Canon EOS 5D Mark IV" "-DateTimeOriginal=2024:05:17 14:23:05" \
    "-OffsetTimeOriginal=+02:00" "-ThumbnailImage<=$work/thumbnail.jpg" "$out/exiftool_exif_mm.jpg"
cp "$work/plain.jpg" "$out/exiftool_exif_ii.jpg"
exif -ExifByteOrder=Little-endian -Orientation#=8 -Make=NIKON "-Model=COOLPIX P1000" \
    "-DateTimeOriginal=2019:12:31 23:59:58" "$out/exiftool_exif_ii.jpg"
# A date and no offset, no orientation.
cp "$work/plain.jpg" "$out/exiftool_date_only.jpg"
exif "-DateTimeOriginal=2001:02:03 04:05:06" "$out/exiftool_date_only.jpg"

# Exif in PNG (an eXIf chunk) and WebP (an EXIF chunk in an extended file).
cp "$work/picture.png" "$out/exiftool_exif.png"
exif -Orientation#=3 -Make=Apple "-Model=iPhone 15" "-DateTimeOriginal=2023:07:04 12:00:00" \
    "$out/exiftool_exif.png"
cwebp -quiet -q 50 "$work/picture.png" -o "$work/picture.webp"
cp "$work/picture.webp" "$out/exiftool_exif.webp"
exif -Orientation#=8 -Make=Google "-Model=Pixel 8" "$out/exiftool_exif.webp"

# Animations: 4 frames at 10 per second, looping.
ff -f lavfi -i testsrc=size=40x30:rate=10 -frames:v 4 -loop 0 "$out/ffmpeg_animated.gif"
ff -f lavfi -i testsrc=size=40x30:rate=10 -frames:v 3 -plays 2 -f apng "$out/ffmpeg_animated.png"

# Tags: ID3v2.3 and 2.4 with a front cover, ID3v2 in ADTS, RIFF INFO.
ff -i "$work/tone.wav" -i "$work/cover.jpg" -map 0 -map 1 -c:a libmp3lame -b:a 64k -c:v copy \
    -disposition:v attached_pic -id3v2_version 3 -metadata "title=Sample title" -metadata "artist=Sample artist" \
    -metadata "album=Sample album" -metadata genre=Jazz -metadata track=3/12 -metadata date=2019 \
    "$out/ffmpeg_id3v23.mp3"
ff -i "$work/tone.wav" -i "$work/cover.jpg" -map 0 -map 1 -c:a libmp3lame -b:a 64k -c:v copy \
    -disposition:v attached_pic -id3v2_version 4 -metadata "title=$(printf 'Gr\303\274\303\237e')" \
    -metadata "artist=Sample artist" -metadata date=2024-05-17 -metadata track=7 "$out/ffmpeg_id3v24.mp3"
ff -i "$work/tone.wav" -c:a aac -b:a 64k -write_id3v2 1 -metadata "title=ADTS title" \
    -metadata "artist=ADTS artist" "$out/ffmpeg_id3.aac"
ff -i "$work/tone.wav" -ac 1 -ar 8000 -c:a pcm_s16le -metadata "title=INFO title" -metadata "artist=INFO artist" \
    -metadata "album=INFO album" -metadata date=2018 -metadata genre=Blues -metadata track=5 "$out/ffmpeg_info.wav"
ff -i "$work/tone.wav" -ac 1 -ar 8000 -c:a pcm_s16le "$out/mutagen_id3.wav"
python3 - "$out/mutagen_id3.wav" <<'PY'
import sys
from mutagen.wave import WAVE
from mutagen.id3 import TIT2, TPE1, TALB, TDRC, TRCK, TCON
audio = WAVE(sys.argv[1])
audio.add_tags()
audio.tags.add(TIT2(encoding=1, text="Wave title"))
audio.tags.add(TPE1(encoding=3, text="Wave artist"))
audio.tags.add(TALB(encoding=0, text="Wave album"))
audio.tags.add(TDRC(encoding=0, text="2017-03-04"))
audio.tags.add(TRCK(encoding=0, text="2/9"))
audio.tags.add(TCON(encoding=0, text="(13)"))
audio.save()
PY

# MP4 and M4A: iTunes tags with a cover, creation time; rotation.
ff -i "$work/tone.wav" -i "$work/cover.jpg" -map 0 -map 1 -c:a aac -b:a 48k -c:v copy -disposition:v attached_pic \
    -metadata "title=M4A title" -metadata "artist=M4A artist" -metadata "album=M4A album" -metadata date=2016 \
    -metadata genre=Rock -metadata track=4/10 "$out/ffmpeg_tagged.m4a"
ff -f lavfi -i testsrc=size=64x48:rate=25 -f lavfi -i "sine=frequency=440:duration=0.4" -t 0.4 \
    -c:v libx264 -profile:v main -pix_fmt yuv420p -preset ultrafast -c:a aac -b:a 32k -metadata "title=Video title" \
    -metadata "artist=Video artist" -metadata creation_time=2024-05-17T14:23:05Z -metadata:s:a language=deu \
    "$work/video.mp4"
cp "$work/video.mp4" "$out/ffmpeg_tagged.mp4"
ff -display_rotation 90 -i "$work/video.mp4" -map 0 -c copy -map_metadata 0 "$out/ffmpeg_rotated.mp4"
ff -display_rotation 180 -i "$work/video.mp4" -map 0 -c copy -map_metadata 0 "$out/ffmpeg_rotated180.mp4"
ff -f lavfi -i testsrc=size=48x32:rate=30000/1001 -t 0.3 -c:v libx264 -pix_fmt yuv420p -preset ultrafast -f mov \
    -metadata "title=QuickTime title" -metadata "artist=QuickTime artist" -metadata date=2015 \
    -metadata creation_time=2015-06-07T08:09:10Z "$out/ffmpeg_tagged.mov"
ls -l "$out"
