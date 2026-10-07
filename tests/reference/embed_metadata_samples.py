"""Writes tests/metadata/metadata_samples.cpp: what exiftool and ffprobe read
in media files, as the expectations of the metadata tests (P17).

The files are the samples of make_metadata_samples.sh (their bytes are
embedded too), and, with --embedded, the samples already embedded in
tests/support/image_samples.cpp, audio_samples.cpp and mp4_samples.cpp
(their bytes are read back from those files; the tests find them there by
name).

Expectations are key/value pairs, in the engine's terms:

    image.width image.height image.orientation (1-8) image.make image.model
    image.dateTaken (ISO 8601) image.frames image.loopCount
    thumbnail.offset thumbnail.length
    audio.codec audio.profile audio.sampleRate audio.channels
    video.codec video.profile video.level video.width video.height
    video.frameRate (n/d) video.rotation (degrees clockwise)
    track.<n>.kind track.<n>.codec track.<n>.language
    tags.title tags.artist tags.album tags.genre tags.date tags.track tags.trackTotal
    movie.majorBrand movie.created cover.width cover.height duration (seconds)

Images come from exiftool (-n), audio and video from ffprobe; a video's
rotation from exiftool and ffprobe both (they must agree).

Usage (in WSL, with exiftool and ffprobe on PATH):
    python3 embed_metadata_samples.py <output .cpp> <samples directory> [--embedded <.cpp>...]
"""

import json
import os
import re
import subprocess
import sys
import tempfile

FORMATS = {".jpg": "jpeg", ".png": "png", ".gif": "gif", ".bmp": "bmp", ".webp": "webp", ".mp3": "mp3",
           ".wav": "wav", ".aac": "aac", ".m4a": "m4a", ".mp4": "mp4", ".mov": "mp4", ".3gp": "mp4"}
PRODUCERS = {"ffmpeg": "FFmpeg 6.1.1", "exiftool": "ExifTool 12.76 (on FFmpeg and cwebp output)",
             "mutagen": "mutagen 1.46 (on FFmpeg output)"}
MAX_SAMPLE = 16384
CODECS = {"pcm_u8": "pcm", "pcm_s16le": "pcm", "pcm_s24le": "pcm", "pcm_s32le": "pcm", "pcm_f32le": "pcm_float",
          "pcm_f64le": "pcm_float", "pcm_alaw": "alaw", "pcm_mulaw": "mulaw", "adpcm_ima_wav": "adpcm_ima",
          "adpcm_ms": "adpcm_ms"}
AAC_PROFILES = {"LC": "AAC LC", "Main": "AAC Main", "LTP": "AAC LTP", "SSR": "AAC SSR", "HE-AAC": "HE-AAC",
                "HE-AACv2": "HE-AAC v2"}


def run_json(command):
    return json.loads(subprocess.run(command, check=True, capture_output=True, text=True).stdout)


def iso_exif(date, offset):
    match = re.fullmatch(r"(\d{4}):(\d{2}):(\d{2}) (\d{2}):(\d{2}):(\d{2})", date.strip())
    if not match:
        return None
    text = "%s-%s-%sT%s:%s:%s" % match.groups()
    return text + offset.strip() if offset else text


def iso_tag(date):
    """ffprobe's date tag ("2019", "2024-05-17") or creation time as the engine prints it."""
    date = date.strip()
    match = re.fullmatch(r"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2})(\.\d+)?Z", date)
    if match:
        return match.group(1) + "Z"
    return date


def image_fields(path, data):
    info = run_json(["exiftool", "-j", "-n", "-ImageWidth", "-ImageHeight", "-Orientation", "-Make", "-Model",
                     "-DateTimeOriginal", "-OffsetTimeOriginal", "-CreateDate", "-ModifyDate",
                     "-ThumbnailOffset", "-ThumbnailLength", "-FrameCount", "-AnimationFrames",
                     "-AnimationPlays", "-AnimationIterations", "-AnimationLoopCount", "-Duration", path])[0]
    fields = {"image.width": info["ImageWidth"], "image.height": info["ImageHeight"]}
    for key, name in (("image.orientation", "Orientation"), ("image.make", "Make"), ("image.model", "Model")):
        if name in info:
            fields[key] = info[name]
    if "DateTimeOriginal" in info:
        date = iso_exif(str(info["DateTimeOriginal"]), str(info.get("OffsetTimeOriginal", "")))
        if date:
            fields["image.dateTaken"] = date
    for name in ("FrameCount", "AnimationFrames"):
        if name in info:
            fields["image.frames"] = info[name]
    for name in ("AnimationIterations", "AnimationPlays", "AnimationLoopCount"):
        if name in info:
            fields["image.loopCount"] = info[name]
    if "Duration" in info:
        fields["duration"] = "%.6f" % float(info["Duration"])
    if "ThumbnailOffset" in info:
        offset = int(info["ThumbnailOffset"])
        length = int(info["ThumbnailLength"])
        # exiftool gives the offset in the file; check it holds a JPEG.
        if data[offset:offset + 3] != b"\xff\xd8\xff":
            sys.exit(f"{path}: no JPEG at the thumbnail offset {offset}")
        fields["thumbnail.offset"] = offset
        fields["thumbnail.length"] = length
    return fields


def media_fields(path, format_id):
    report = run_json(["ffprobe", "-v", "error", "-show_streams", "-show_format", "-of", "json", path])
    fields = {}
    tags = {k.lower(): v for k, v in report["format"].get("tags", {}).items()}
    streams = sorted(report["streams"], key=lambda s: s["index"])
    cover = [s for s in streams if s.get("disposition", {}).get("attached_pic") == 1]
    media = [s for s in streams if s.get("disposition", {}).get("attached_pic") != 1]
    if cover:
        fields["cover.width"] = cover[0]["width"]
        fields["cover.height"] = cover[0]["height"]
    if "duration" in report["format"]:
        fields["duration"] = report["format"]["duration"]
    audio = [s for s in media if s["codec_type"] == "audio"]
    video = [s for s in media if s["codec_type"] == "video"]
    if audio:
        a = audio[0]
        fields["audio.codec"] = CODECS.get(a["codec_name"], a["codec_name"])
        if a["codec_name"] == "aac" and a.get("profile") in AAC_PROFILES:
            fields["audio.profile"] = AAC_PROFILES[a["profile"]]
        fields["audio.sampleRate"] = int(a["sample_rate"])
        fields["audio.channels"] = int(a["channels"])
    if video:
        v = video[0]
        fields["video.codec"] = v["codec_name"]
        fields["video.width"] = v["width"]
        fields["video.height"] = v["height"]
        # Profiles and levels: the engine reads them for AVC and HEVC.
        if v.get("profile") and v["codec_name"] in ("h264", "hevc"):
            fields["video.profile"] = v["profile"]
        level = int(v.get("level", -99))
        if level > 0 and v["codec_name"] == "h264" and level != 9:
            fields["video.level"] = "%d.%d" % (level // 10, level % 10)
        elif level > 0 and v["codec_name"] == "hevc":
            fields["video.level"] = "%d.%d" % (level // 30, (level % 30) // 3)
        rate = v.get("avg_frame_rate", "0/0")
        if rate not in ("0/0", "0/1"):
            fields["video.frameRate"] = rate
        counter = 0
        for side in v.get("side_data_list", []):
            if "rotation" in side:
                counter = int(side["rotation"])
        rotation = (360 - counter) % 360
        exif = run_json(["exiftool", "-j", "-n", "-Rotation", path])[0]
        if int(exif.get("Rotation", 0)) != rotation:
            sys.exit(f"{path}: exiftool's rotation {exif.get('Rotation')} is not ffprobe's {rotation}")
        fields["video.rotation"] = rotation
    if format_id in ("mp4", "m4a"):
        # Tracks: ffprobe's streams but cover art (an ilst item, not a track).
        mac = macintosh_languages(open(path, "rb").read())
        for number, stream in enumerate(media, 1):
            kind = {"video": "video", "audio": "audio"}.get(stream["codec_type"], "unknown")
            fields["track.%d.kind" % number] = kind
            language = stream.get("tags", {}).get("language")
            # The engine leaves Macintosh language codes untranslated.
            if language and not (number - 1 < len(mac) and mac[number - 1]):
                fields["track.%d.language" % number] = language
        if "major_brand" in tags:
            fields["movie.majorBrand"] = tags["major_brand"]
        if "creation_time" in tags:
            fields["movie.created"] = iso_tag(tags["creation_time"])
    for key in ("title", "artist", "album", "genre"):
        if key in tags:
            fields["tags." + key] = tags[key]
    if "date" in tags:
        fields["tags.date"] = iso_tag(tags["date"])
    if "track" in tags:
        number, _, total = tags["track"].partition("/")
        fields["tags.track"] = int(number)
        if total:
            fields["tags.trackTotal"] = int(total)
    return fields


def boxes(data, start, end):
    position = start
    while end - position >= 8:
        size = int.from_bytes(data[position:position + 4], "big")
        kind = data[position + 4:position + 8]
        header = 8
        if size == 1:
            size = int.from_bytes(data[position + 8:position + 16], "big")
            header = 16
        elif size == 0:
            size = end - position
        if size < header or position + size > end:
            return
        yield kind, position + header, position + size
        position += size


def macintosh_languages(data):
    """Per trak of the first moov, whether its mdhd holds a Macintosh language code."""
    for kind, begin, end in boxes(data, 0, len(data)):
        if kind != b"moov":
            continue
        result = []
        for trak, trak_begin, trak_end in boxes(data, begin, end):
            if trak != b"trak":
                continue
            mac = False
            for mdia, mdia_begin, mdia_end in boxes(data, trak_begin, trak_end):
                if mdia != b"mdia":
                    continue
                for mdhd, payload, mdhd_end in boxes(data, mdia_begin, mdia_end):
                    if mdhd == b"mdhd":
                        at = payload + (32 if data[payload] == 1 else 20)
                        mac = int.from_bytes(data[at:at + 2], "big") < 0x400
            result.append(mac)
        return result
    return []


def embedded(cpp):
    """(name, bytes) of every sample embedded in a generated samples .cpp."""
    text = open(cpp, encoding="utf-8").read()
    found = []
    for match in re.finditer(r"// (\S+)\nconstexpr std::array<std::uint8_t, (\d+)> kSample\d+ = \{\n(.*?)\n\};",
                             text, re.S):
        values = bytes(int(v, 16) for v in re.findall(r"0x([0-9A-F]{2})", match.group(3)))
        if len(values) != int(match.group(2)):
            sys.exit(f"{cpp}: {match.group(1)} does not hold {match.group(2)} bytes")
        found.append((match.group(1), values))
    return found


def cpp_string(value):
    out = []
    for byte in str(value).encode("utf-8"):
        if byte in (0x22, 0x5C) or byte < 0x20 or byte > 0x7E:
            out.append("\\x%02X\" \"" % byte)
        else:
            out.append(chr(byte))
    return '"' + "".join(out) + '"'


def main():
    out_path = sys.argv[1]
    directory = sys.argv[2]
    sources = sys.argv[4:] if len(sys.argv) > 3 and sys.argv[3] == "--embedded" else []
    samples = []
    for name in sorted(os.listdir(directory)):
        ext = os.path.splitext(name)[1]
        if ext not in FORMATS:
            continue
        path = os.path.join(directory, name)
        data = open(path, "rb").read()
        if len(data) > MAX_SAMPLE:
            sys.exit(f"{name} is larger than {MAX_SAMPLE} bytes; samples must stay small")
        samples.append((name, FORMATS[ext], PRODUCERS[name.split("_")[0]], data, True))
    with tempfile.TemporaryDirectory() as work:
        for cpp in sources:
            for name, data in embedded(cpp):
                ext = os.path.splitext(name)[1]
                if ext in FORMATS:
                    samples.append((name, FORMATS[ext], "", data, False))
        records = []
        for name, format_id, producer, data, own in samples:
            path = os.path.join(work, "sample" + os.path.splitext(name)[1])
            with open(path, "wb") as f:
                f.write(data)
            if format_id in ("jpeg", "png", "gif", "bmp", "webp"):
                fields = image_fields(path, data)
            else:
                fields = media_fields(path, format_id)
            records.append((name, format_id, producer, data if own else None, fields))

    lines = [
        "// Generated by tests/reference/embed_metadata_samples.py from the output of",
        "// tests/reference/make_metadata_samples.sh and the samples embedded in tests/support. Do not edit.",
        "",
        '#include "metadata_samples.hpp"',
        "",
        "#include <array>",
        "",
        "namespace recovery::test::metadata_samples {",
        "",
        "namespace {",
        "",
    ]
    for index, (name, _, _, data, fields) in enumerate(records):
        lines.append(f"// {name}")
        if data is not None:
            lines.append(f"constexpr std::array<std::uint8_t, {len(data)}> kBytes{index} = {{")
            for start in range(0, len(data), 16):
                lines.append("    " + ", ".join(f"0x{b:02X}" for b in data[start:start + 16]) + ",")
            lines.append("};")
        lines.append(f"constexpr std::array<Field, {len(fields)}> kFields{index} = {{{{")
        for key in sorted(fields):
            lines.append(f"    {{{cpp_string(key)}, {cpp_string(fields[key])}}},")
        lines.append("}};")
        lines.append("")
    lines.append("}  // namespace")
    lines.append("")
    lines.append("const std::vector<Sample>& all() {")
    lines.append("    static const std::vector<Sample> samples = {")
    for index, (name, format_id, producer, data, _) in enumerate(records):
        bytes_ref = f"kBytes{index}" if data is not None else "{}"
        lines.append(f'        {{{cpp_string(name)}, "{format_id}", {cpp_string(producer)}, {bytes_ref}, '
                     f"kFields{index}}},")
    lines.append("    };")
    lines.append("    return samples;")
    lines.append("}")
    lines.append("")
    lines.append("}  // namespace recovery::test::metadata_samples")
    lines.append("")
    with open(out_path, "w", newline="\n", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print(f"wrote {len(records)} samples to {out_path}")


main()
