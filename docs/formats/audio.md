# Audio formats (P10): MP3, WAV, M4A, raw AAC

The audio formats for the carving framework of P8, in the library `recovery_formats` next to the images of P9
([images.md](images.md)). As there, a format sees the bytes of one would-be file through `IContentReader` and
nothing else.

```cpp
carving::FormatRegistry registry;
if (Status added = formats::registerAudioFormats(registry); !added.ok()) { ... }
if (Status added = formats::registerImageFormats(registry); !added.ok()) { ... }  // both, if wanted
// carving as in ../recovery/carving.md: SignatureScanner::create(registry), FileCarver::run(...)
```

`registerAudioFormats` adds MP3, WAV, M4A and AAC in the plan's priority order (MP3, WAV, M4A/AAC). It adds
nothing when one of the ids is already registered, and it can be combined with the image formats: no id is taken
twice.

| Header | Format | Signatures | Minimum | Maximum | End detection |
| --- | --- | --- | --- | --- | --- |
| `mp3_format.hpp` | `Mp3Format` | `"ID3"`; MPEG-1, 2 and 2.5 Layer III frame syncs (`FF FA`, `FF F2`, `FF E2` under `FF FE`) | 96 | 1 GiB | StructureWalk: the last frame, then trailing tags |
| `wav_format.hpp` | `WavFormat` | `"RIFF" ???? "WAVE"` (masked) | 44 | 4 GiB + 8 | SizeField: 8 + the RIFF size |
| `m4a_format.hpp` | `M4aFormat` | `"ftyp"` at offset 4 | 32 | 4 GiB | StructureWalk: the last top-level box |
| `aac_format.hpp` | `AacFormat` | `"ID3"`; ADTS sync (`FF F0` under `FF F6`) | 32 | 1 GiB | StructureWalk: the last frame, then trailing tags |

MP3 and AAC are **self-synchronizing** (`FormatDescriptor::selfSynchronizing`): every frame of a stream matches
their signature, and a stream can be decoded from any frame on. The carver therefore skips their hits inside an
earlier candidate of the same format, whatever that candidate's verdict (L51,
[../recovery/carving.md](../recovery/carving.md)). Without that, a stream that is cut off or damaged would be
carved again from each of its thousands of frames. This was the one change P10 needed in the framework; it is
opt-in and the image formats, WAV and M4A do not use it.

Like the images, every format has one walk of its structure, used by both end detection and validation
(`src/formats/structure_walk.hpp`): the **layout** says where the file ends, and **problems** (checksums,
field values, element order) make it `Invalid` without moving its end. MP3 and AAC share the handling of metadata
tags (`src/formats/audio_tags.hpp`).

Validation is **structural**: frames, chunks, boxes and tags are checked, the audio is not decoded (that is media
validation, P14). Which is why some fragmentation goes unnoticed (L68-L71).

## Frame streams and their tags (MP3, AAC)

Neither format has a field that records the whole file's length. A file is:

1. optional **ID3v2 tags** at the start (versions 2.2, 2.3 and 2.4, one after another), and zero padding after
   them (at most 64 KiB);
2. **frames**, as long as their headers agree with the first one: for MP3 the version, layer, protection and
   sample rate, and whether the frame is mono; for ADTS the MPEG version, layer, protection, profile, sampling
   frequency and channel configuration. The bitrate, the padding and the channel mode may change from frame to
   frame (VBR files; FFmpeg writes its Info frame as stereo and the audio as joint stereo);
3. optional **tags after the frames**, in any order: APEv2 with a header, Lyrics3 (v1 and v2, found by searching
   for their end marker), an appended ID3v2 tag, and ID3v1 (128 bytes, or 355 with "TAG+"), which ends the file.

A stream needs **at least four frames** (the plan: "multiple consecutive valid frames"); fewer is `Broken` at 0
bytes, which the carver rejects. An ID3v2 tag without frames of the format after it is `Broken` at 0 as well, so
an MP3's tag does not make an AAC file and the other way round. If the data ends inside a stream's first frame and
no ID3v2 tag came before it, nothing confirms that lone header: it is rejected too.

Where the data ends at a frame boundary and nothing records the frame count, the stream is complete: an MP3
without an info tag or an ADTS file cut at a frame boundary is a shorter, valid file (L68). Where the data ends
inside a frame or a tag, or inside a marker ("TA" of "TAG"), the file is `Truncated`.

### MP3

**Header check** (4 KiB): an ID3v2 header ("ID3", major version 2-4, a revision below 0xFF, only the flags the
version defines, a syncsafe size), or a Layer III frame header (no reserved version, layer, sample rate or
emphasis, no free-format bitrate) whose side information is plausible and whose main data fits in what the frame
and the bit reservoir hold, followed by frames that agree with it as far as the 4 KiB hold them (at least two,
since no Layer III frame is longer than 1441 bytes). A frame in the middle of a stream passes: after a break the
rest of a stream starts there.

**Info tags.** The first frame may carry a Xing or Info tag at `4 + side information` (FFmpeg and LAME put it
there with or without a CRC, overlapping the CRC-protected side information) or a VBRI tag at 36. A Xing/Info tag
records the audio frames after its own frame and the stream's bytes including it, as LAME and FFmpeg were measured
to write them. The walk stops after that many frames; fewer frames before something else is `Broken`, fewer
before the data ends is `Truncated`. Another file's info tag in the middle of a stream ends it: that is where the
next file starts. VBRI tags mark a start but their counts are not used (L73).

**Validation** also checks, for every frame:

| Check | Catches |
| --- | --- |
| The frame CRC (ISO 11172-3, over the header's last two bytes and the side information) when the frame has one | Damaged side information |
| Side information: `big_values` at most 288, no window switch to a normal block | Garbage that looks like a frame |
| The **bit reservoir**: a frame's main data starts `main_data_begin` bytes back, in the main data of earlier frames (headers and side information not counted); it may not overlap the previous frame's main data or run beyond its own frame | Frames of another stream, damaged side information |
| The first audio frame's main data does not begin before the stream | A stream whose start is missing (L80) |

and for the stream: the Xing/Info byte count, and a **LAME extension**'s CRC-16 of itself (over the frame up to the
CRC), its music length, and its CRC-16 of all the audio frames. The LAME extension is recognised by its own CRC,
so the music CRC is checked for LAME and FFmpeg files alike. It catches any changed byte of audio.

### Raw AAC (ADTS)

**Header check** (4 KiB): an ID3v2 header, or an ADTS header (sync, layer 0, a sampling frequency index below 13,
a frame no longer than the decoder's input buffer allows: 768 bytes per channel and raw data block, ISO 14496-3
4.5.3.1) followed by agreeing frames as far as the header holds them. Frames of 6 or 8 channels, or with a program
config element, can be longer than 4 KiB; their next header is then only checked by end detection.

**Validation**: the frame headers and the tags. The raw data blocks are Huffman-coded and not decoded, and the
header CRC (which none of the encoders sampled writes) is not checked (L78). Two ADTS files with the same
parameters, stored back to back, are one stream (L70).

## WAV (RIFF WAVE)

**Header check** (4 KiB): "RIFF" … "WAVE"; a RIFF size with room for a fmt and a data chunk and not the placeholder
0xFFFFFFFF of a recording that was never finished (L75); the chunks as far as the header holds them, with printable
ids and sizes within the RIFF data; a fmt chunk among them without zero format tag, channels, sample rate or block
alignment. fmt does not have to come first (broadcast WAV puts JUNK and bext before it).

**Walk**: as for WebP, the RIFF size says where the file ends and the chunks must fill it; a chunk whose id is not
printable, or that runs beyond the RIFF data, breaks the structure. Unlike WebP, a last chunk whose pad byte is
missing is accepted: the RIFF size decides.

**Problems**: no fmt chunk, a second one, or fmt after data; no data chunk or a second one; fmt fields that
disagree for PCM, IEEE float, A-law and mu-law (block alignment must be channels × bytes per sample, the byte rate
sample rate × block alignment, bits per sample 1-64 for PCM, 32 or 64 for float, 8 for A-law and mu-law);
WAVE_FORMAT_EXTENSIBLE that is too short, has more valid bits than bits per sample, or whose subtype is not a
wrapped format tag; data that is not a whole number of blocks; a fact chunk too short for its sample count; a LIST
chunk that its sub-chunks do not fill. Other formats (ADPCM, GSM, MP3 in WAV) are checked for non-zero fields only.
Other chunks (cue, bext, iXML, smpl, id3) are skipped.

The samples have no structure (L71): a WAV holding another file's data between its fragments validates.

## M4A (ISO base media file format)

**Header check** (4 KiB): an ftyp box of 16 to 1024 bytes that is a whole number of brands, a printable major
brand that is not a video or image brand (M4V, qt, heic, avif, ...), and, when the header holds it, a plausible box
header after it. Compatible brands are not checked: mp4v2 (faac, fdkaac) writes a zero one.

**Audio or video.** Since P12, M4A and the MP4 video format ([mp4.md](mp4.md)) divide the ftyp files by one rule
(`mp4::classify`, in `formats/mp4_analysis.hpp`), so every such file goes to exactly one of them. The major brand
decides when it is an audio brand (M4A, M4B, M4P, F4A, F4B). A generic brand (isom, mp41, mp42, 3gp4, ... as
Android, Windows' Media Foundation and many recorders write) makes the file M4A only when its moov holds a sound
track and no video track (each trak's mdia/hdlr). Anything else is not an audio file: end detection reports
`Broken` at 0 bytes, which the carver rejects, and MP4 takes it, including a generic brand without moov (L76). An
audio brand with a video track is accepted (audiobooks carry chapter pictures).

**Walk**: the top-level boxes, `[size BE32][type]` or `[1][type][size BE64]`, in any order: moov before or after
mdat, free, skip, wide, udta, uuid and the boxes of fragmented files (moof, mfra, sidx, ...). The walk never assumes
that moov precedes mdat or that another ftyp is merely a box: the next ftyp ends the file. Before both moov and
mdat have been seen any box with a printable type and a plausible size continues the file (Media Foundation writes
a uuid box first); after that only the known top-level types do. M4A and MP4 share this walk
(`src/formats/iso_walk.hpp`). So:

| The walk meets | Result |
| --- | --- |
| The end of the data once moov and mdat were seen | `Found` there |
| Something that is not a box (or not a top-level one) once moov and mdat were seen | `Found` before it |
| A box that runs beyond the data, or the data ending inside a plausible box header | `Truncated` |
| A box of size 0 ("to the end of the file", as unfinished recordings leave mdat) | `Broken` at the box |
| Something that is not a box before moov or mdat | `Broken` there |
| The data ending right after a moof box, before its mdat | `Truncated`; something else after it: `Broken` at the moof |

**Validation** is the MP4 parser's `parseFile` (P11, with the movie fragments of P12): the boxes, the movie, every
sample table and the chunks it derives from stsc, stsz and stco (every chunk inside an mdat, no two overlapping),
and every movie fragment. On top of it: the file must classify as audio and hold a sound track, and a file that
parses but has no mdat box (a fragmented file cut after moov) is `Truncated`. A chunk that lies beyond the end of
a file whose boxes are complete makes it `Invalid` (the chunk offset is wrong, as P10 decided), where MP4 reports
the same file `Truncated` (L93). What the samples hold is not checked: that is media validation (P14).

## Checked against

The rules above were checked against files written by LAME 3.100, FFmpeg 6.1.1, faac 1.30, fdkaac 1.0.0, SoX
14.4.2, Python's wave module, and Windows' speech synthesizer and Media Foundation (36 embedded samples, the same
tools' 20-second files, and the 70 WAV files in `C:\Windows\Media`). What they showed:

- LAME and FFmpeg count Xing frames without the tag's own frame, and bytes with it; the LAME music CRC covers the
  audio frames after the tag's frame.
- LAME omits the Info tag when the frames are too small to hold it, and Media Foundation never writes one: bare
  streams are common, so the format carves them.
- In a stream using the bit reservoir only the first frames have `main_data_begin` 0, so it cannot be required of
  a stream's start when the start is lost.
- Media Foundation's M4A has the generic brand mp42 and a uuid box before mdat; FFmpeg can write any brand.
- mp4v2 writes a compatible brand of four zero bytes.

## Tests

Label `formats`, executable `recovery_formats_tests` (see [../testing/testing.md](../testing/testing.md)):

| File | Covers |
| --- | --- |
| `tests/formats/mp3_format_test.cpp` | MPEG-1, 2 and 2.5 at several rates and bitrates (CBR and VBR), every channel mode, CRCs, info tags with and without LAME, the reservoir on and off, every tag; prefixes; header rejection; the info tag's count; CRC, LAME tag and music CRC damage; reservoir and side information damage; tag layouts; false streams and a flood of frame syncs; fragments; fuzzing |
| `tests/formats/aac_format_test.cpp` | Rates, channel configurations, profiles, MPEG-2 and MPEG-4 headers, tags; prefixes; header rejection; streams ending where their headers change; false streams; fragments; fuzzing |
| `tests/formats/wav_format_test.cpp` | Every encoding, depth and channel count, WAVE_FORMAT_EXTENSIBLE, every chunk; prefixes; header rejection; RIFF sizes that disagree with the chunks; fmt fields that disagree; chunk order and counts; fragments; fuzzing |
| `tests/formats/m4a_format_test.cpp` | moov first and last, co64, 64-bit mdat, metadata, audio and generic brands; prefixes; header rejection; audio and video; the top-level walk; missing and repeated boxes; damaged sample tables; fragments; fuzzing |
| `tests/formats/audio_carving_test.cpp` | Registration; all four formats carved back byte for byte from a noisy disk (with the image formats); a cut-off and a damaged stream carved once; the tail of a stream whose start was lost; files cut off by the end of the source; an M4A whose moov was overwritten; a flood of false signatures; a cover picture inside an MP3; a FAT32 volume with active, deleted and fragmented files; a disk image file |
| `tests/formats/audio_reference_test.cpp` | The embedded samples, an optional corpus from elsewhere, and exporting the builders' files for other decoders |
| `tests/carving/file_carver_test.cpp` | `OwnHitsInsideASelfSynchronizingCandidateAreSkippedWhateverItsVerdict` |

## Known limitations (P10)

All known limitations, with IDs for discussion, are listed in [../limitations.md](../limitations.md): L68-L83 for
the audio formats, and L51, L55 and L50 updated. P12 resolved L76 and L77 (M4A on the P11 parser, and generic
brands without moov left to MP4) and added L92-L93, which concern M4A too.
