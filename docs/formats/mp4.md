# MP4 parser (P11) and MP4 format (P12): the ISO base media file format

A dedicated parser for the ISO base media file format (ISO/IEC 14496-12): MP4, MOV (QuickTime), M4V, M4A and 3GP
files. It lives in `recovery_formats` (namespace `recovery::formats::mp4`) next to the image and audio formats
([images.md](images.md), [audio.md](audio.md)) and, like them, sees the bytes only through `IContentReader`.

P12 added movie fragments to the parser, an analysis layer on top of it (audio or video, where the media data
lies, moov discovery, sample framing), and the MP4 carving format, `Mp4Format`, registered by
`registerVideoFormats` (`formats/video_formats.hpp`). M4A (P10) now validates with the parser too, and the two
formats share the top-level walk and one rule for telling audio from video. MP4 recovery, which puts this together
with the filesystem evidence, is in [../recovery/mp4_recovery.md](../recovery/mp4_recovery.md). P13 uses the chunk
map for fragment reconstruction.

```cpp
carving::MemoryContentReader content(bytes);          // or a SourceContentReader over a carve window
Result<mp4::Mp4File> file = mp4::parseFile(content);   // fails only for bad limits or a failing read
if (file.ok() && file->status == mp4::FileStatus::Valid) {
    for (const mp4::Track& track : file->movie->tracks) {
        mp4::forEachSample(track, [](std::uint32_t index, std::uint64_t offset, std::uint32_t size) { ... });
    }
}
```

## Layers

| Header | What it offers |
| --- | --- |
| `include/formats/mp4_box.hpp` | `FourCc`; `BoxHeader` with 32-bit, 64-bit and "to the end" sizes and uuid extended types; `parseBoxHeader` (bytes) and `readBoxHeader` (content), each checked against its bounds; `BoxSequence`, the boxes that follow each other in a range; `scanTopLevel`, the top-level boxes of a file |
| `include/formats/mp4_parser.hpp` | `parseMovie` (a moov box and everything in it), `parseFileType`, `parseFile` (a whole file, with its movie fragments); the model (`Movie`, `Track`, `SampleTable`, `Chunk`, `TrackExtends`, `MovieFragment`); `IssueList`; `ParseLimits`; `forEachSample` |
| `include/formats/mp4_analysis.hpp` (P12) | `classifyBrand` and `classify` (audio, video or neither); `mediaExtent`; `findMovie` (moov discovery); `checkSampleFraming` |
| `include/formats/mp4_format.hpp` (P12) | `Mp4Format`, the carving format for MP4, MOV, M4V and 3GP video; `mp4Verdict` |

### Box headers

`[size BE32][type]`, `[1][type][size BE64]` or `[0][type]` (the box runs to the end of its bounds), then 16 bytes
of extended type for `uuid`. The bounds are the parent's payload, or the content at the top level. A header is one
of:

| Status | Meaning |
| --- | --- |
| `Valid` | The box lies inside its bounds (`ToEnd` boxes get the rest of the bounds as their size) |
| `HeaderCut` | The bounds end inside the header (8, 16, 24 or 32 bytes) |
| `TooLong` | The size runs past the bounds; no arithmetic overflows, even for a 64-bit size near 2^64 |
| `BadSize` | The size is smaller than the box's own header: 2 to 7, a 64-bit size below 16, a uuid box without room for its type |

A `BoxSequence` ends at the first box that is not `Valid` or is `ToEnd`, and never reads outside its range.

### The top level

`scanTopLevel` lists every box whose type is printable and whose size fits, in content order and in any order of
types: moov before or after mdat, any number of mdat, free, skip, wide, uuid, moof and unknown types. It stops
when:

| `LayoutEnd` | When |
| --- | --- |
| `EndOfData` | The last box ends at the end of the content (or runs to it: size 0) |
| `NotABox` | The next bytes have an unprintable type or a size below their header (zero padding, another kind of data) |
| `Truncated` | The content ends inside a box (`cutBox` holds its header: an mdat cut short, say) or inside a header that could be one |
| `BoxLimit` | `maxBoxes` boxes were read and more follow |

A later ftyp is one more box. What it means (another file starting there) is for the caller to decide.

## What the parser reads

| Box | Where | Read | Checked |
| --- | --- | --- | --- |
| ftyp | top level | major brand, minor version, compatible brands | a whole number of brands; the first box; only one |
| moov | top level | mvhd, trak, mvex (fragmented) | only one; mvhd and at least one track |
| mdat | top level | where it is | every chunk lies in one mdat |
| free, skip, wide, uuid, ... | anywhere | skipped | they fit their parent |
| mvhd | moov | time scale, duration, next track id | version 0 or 1, exactly its version's size, time scale not 0 |
| trak | moov | tkhd, mdia | both present, once; at most `maxTracks` |
| tkhd | trak | track id, flags, duration, display size | as mvhd; track id not 0 and not another track's |
| mdia | trak | mdhd, hdlr, minf | all present, once |
| mdhd | mdia | time scale, duration, language | as mvhd; time scale not 0 |
| hdlr | mdia | handler type: `vide` (video), `soun` (audio), anything else (other) | at least 24 bytes |
| minf | mdia | stbl | present, once |
| stbl | minf | stsd, stts, stsc, stsz or stz2, stco or co64 | all present, once each |
| stsd | stbl | sample descriptions: format, data reference index; video width and height, and the NAL unit length size of avcC or hvcC (P12); audio channels, sample size and rate (QuickTime version 2 too) | at least one; as many as the count; they fill stsd; long enough for their kind; an avcC or hvcC that is not version 1, too short, or with a length size of 3 gives no length size (its samples are then not framed) |
| stts | stbl | (sample count, delta) entries | the entries fit; they time every sample |
| stsc | stbl | (first chunk, samples per chunk, description) entries | the entries fit; first chunk 1, increasing, within the chunks; no empty chunks; valid description index |
| stsz, stz2 | stbl | one size for all, or a size per sample (stz2: 4, 8 or 16 bits) | the entries fit; field size 4, 8 or 16 |
| stco, co64 | stbl | 32-bit or 64-bit chunk offsets | the entries fit |
| mvex, trex | moov | each track's defaults for its fragments: sample description, duration, size, flags (P12) | trex exactly 24 bytes, once per track, for a track of moov |
| moof, mfhd | top level | a movie fragment and its sequence number (P12, `parseFile` only) | mfhd present; sequence numbers increase; moov has mvex |
| traf, tfhd, tfdt, trun | moof | a track fragment: its track, base data offset and defaults; its runs of samples (P12) | tfhd present, for a track with a trex; description index in range; the runs' entries fit; a run's data lies in the file |

In every container the parser walks (moov, trak, mdia, minf, stbl, stsd), the children must fill the parent (a
QuickTime 32-bit zero terminator is accepted), none may have size 0, and a box of the list above found in the wrong
container (tkhd in moov, stco in minf, ...) is malformed: that is what a box whose size grew over its neighbour's
header leaves behind. Children may come in any order: the handler is read before the sample descriptions whatever
their order. Other boxes (edts, udta, meta, dinf, vmhd, smhd, stss, ctts, sgpd, ...) are skipped.

### Chunks

When a track's five tables are present, well formed and agree, the parser derives its chunks from stsc, stsz and
stco/co64: each chunk's offset, size (the sum of its samples' sizes), first sample, sample count and sample
description, and sets `sampleTableValid`. They agree when stsc's runs cover exactly the chunks of stco and hold
exactly the samples of stsz, and stts times exactly those samples. `forEachSample` then gives every sample's offset
and size.

`parseFile` places every chunk:

| `ChunkPlacement` | When |
| --- | --- |
| `MediaData` | Inside the payload of one mdat (one cut short by the end of the data counts) |
| `OutsideMediaData` | Inside the content but not inside one mdat payload: malformed |
| `BeyondData` | It ends beyond the content: the data it needs is missing (truncation) |

and checks that no two chunks overlap, of the same track or of different tracks. `parseMovie` leaves the placement
`Unchecked`.

### Movie fragments (P12)

A fragmented file (live recorders, DASH, CMAF, some cameras) has a moov with mvex and usually empty sample tables,
then moof and mdat pairs. `parseFile` reads every moof in content order: its mfhd, and in each traf the tfhd (the
track, flags, defaults) and every trun (a run of samples: how many, a data offset, and a duration, size, flags and
composition offset per sample, or the defaults of tfhd and trex). Each run becomes a `Chunk` in the track's
`fragmentRuns`, with its samples' sizes in `fragmentSampleSizes`; `Movie::trackExtends` holds the trex defaults and
`Mp4File::fragments` the fragments themselves.

A run's data starts at its track fragment's base plus the run's data offset. The base is given in one of the three
ways ISO/IEC 14496-12 allows:

| tfhd | Base |
| --- | --- |
| base-data-offset (0x01) | That absolute file offset; runs without a data offset follow the previous run |
| default-base-is-moof (0x020000) | The moof box (CMAF, FFmpeg's `default_base_moof`) |
| neither | The moof for the first track fragment of a moof; each later one starts where the previous one's data ended |

Runs are placed like chunks (inside one mdat, beyond the data, or outside the media data) and must not overlap a
chunk or another run. `forEachSample` visits a track's chunk samples, then its run samples, numbered on from the
sample tables' count. The rules, and what is not read, are L102.

## Audio or video (P12)

M4A (audio) and MP4 (video) both start with ftyp, so one rule, `mp4::classify`, divides the files between them:

| Major brand | Examples | The file is |
| --- | --- | --- |
| Audio | M4A, M4B, M4P, F4A, F4B | audio (M4A), whatever its tracks |
| Video | M4V, M4VH, M4VP, F4V, F4P, qt | video (MP4) |
| Image | heic, heix, heim, heis, hevc, hevx, mif1, msf1, avif, avis, crx, jp2, mjp2 | neither (HEIF, AVIF, CR3, JPEG 2000) |
| Generic (any other) | isom, iso2, mp41, mp42, avc1, 3gp4-3gp9, 3g2a, dash, ... | audio when moov has a sound track and no video track; video otherwise, also when there is no moov to tell by |

A file without ftyp (old QuickTime) is classified by its tracks in the same way.

## Analysis (P12)

`formats/mp4_analysis.hpp` builds on the parse:

- **Media extent**: `mediaExtent` spans every chunk and run of every track: where the media data lies according to
  the sample tables, whatever the mdat boxes say (mdat discovery). An mdat of size 0 after moov ends there.
- **Moov discovery**: `findMovie` searches content for "moov", checks the box header around each occurrence, and
  parses it with `parseMovie`; it accepts the first whose movie has a track with a valid sample table, after at most
  64 attempts. MP4 recovery uses it when a file's top-level boxes do not lead to a moov (its start is overwritten).
- **Sample framing**: `checkSampleFraming` walks every AVC and HEVC sample of a video track whose sample description
  gives a NAL unit length size (1, 2 or 4 bytes, from avcC or hvcC): a sequence of length fields, each followed by a
  NAL unit whose forbidden zero bit is clear, filling the sample exactly (ISO/IEC 14496-15). Another file's data in
  place of a sample, or a sample that is not where the tables say, almost never passes; the builder's samples and
  the tens of thousands of samples of the corpus all do. It reads each NAL unit's first bytes, not the coded video.
  Only samples placed in an mdat (or, for a moov on its own, inside the content) are walked.

## The MP4 carving format (P12)

`Mp4Format` (id `mp4`, extension `mp4`, signature `ftyp` at offset 4, 32 bytes to 256 GiB):

**Header check** (4 KiB): an ftyp box of 16 to 1024 bytes that is a whole number of brands, a printable major
brand that is neither an audio nor an image brand, and, when the header holds it, a plausible box header after it.

**End detection** (ftyp detection, then moov and mdat discovery): the top-level walk it shares with M4A
(`src/formats/iso_walk.hpp`, described in [audio.md](audio.md)): boxes in any order, movie fragments after moov, the
next ftyp ends the file. Then:

| The walk found | End |
| --- | --- |
| A generic brand whose moov holds sound tracks only | `Broken` at 0: an audio file, left to M4A |
| An mdat of size 0 after moov | where the sample tables' media data ends (`Truncated` if that is beyond the data) |
| Complete boxes, but chunks of moov that need data after the last box | `Truncated` there when the data ends; `Broken` there when something else follows (L93) |
| A moof as the last box | `Truncated` when the data ends; `Broken` at the moof otherwise |
| Otherwise | as M4A's walk: `Found` after the last box, `Truncated`, or `Broken` |

**Validation** (`mp4Verdict`): `parseFile`, the file must classify as video (`Invalid` otherwise), a file that
parses without an mdat box is `Truncated`, and then the framing of every AVC and HEVC sample: one misframed sample
makes the file `Invalid` ("the NAL units of N samples of M do not fill them; the first: ..."), with `validBytes` at
that sample. `Mp4FormatOptions::checkSampleFraming = false` leaves the framing out.

## Never assumed

The plan names four assumptions the parser must not make:

| Assumption | How the parser avoids it |
| --- | --- |
| moov precedes mdat | The top-level scan takes boxes in any order; moov may be first, last or between mdat boxes |
| mdat is contiguous | Any number of mdat boxes, with other boxes between them; every chunk is placed on its own, in whichever mdat holds it; gaps are allowed |
| the file is contiguous | Everything is read through `IContentReader`, so a file assembled from scattered clusters parses the same (tested with a scattered reader); `parseMovie` works on a moov found on its own, without ftyp or mdat, and reports the chunk offsets as recorded |
| the next ftyp marks the end | A later ftyp is listed as a box and reported as an issue; the scan goes on past it |

## Results

A malformed structure is not an error of the `Result`: the parser returns what it could read and lists the
**issues**, each with its kind, the offset of the box at fault, its path (`moov/trak[2]/mdia/minf/stbl/stsc`) and
what is wrong. The first 32 are kept, all are counted. `Malformed` issues break the format's rules or show parts
that disagree; `LimitExceeded` issues mark structures larger than the limits allow to be parsed. A layout fault
stops the walk of the container it is in; everything else is noted and the parse goes on, so one damaged track does
not hide the others.

`parseFile` sums it up:

| `FileStatus` | When |
| --- | --- |
| `Valid` | No issue: the boxes fill the content; ftyp, if any, comes first; one moov with at least one track; every sample table valid; every chunk in an mdat; no overlaps |
| `Truncated` | No issue, but the data ends inside a box, before a moov box, or before the media data that some chunk needs |
| `Invalid` | Any issue (the first one is in `detail`) |

A `Result` fails only for invalid arguments (a header that is not a moov inside the content, limits of 0) and for a
read that fails (the reader's error: I/O, `Cancelled`).

## Safety

- Every box is checked against its parent, and every parent against the content, before its payload is read.
- Every entry count is checked against the bytes that hold its entries, then against `maxTableEntries`, before
  anything is allocated or read: a table that claims 2^30 entries in a box of 4 GiB costs one header read.
- Offsets and sums are checked for overflow (a chunk that would end beyond 2^64 is malformed).
- `ParseLimits` bound the work and memory of one parse: `maxBoxes` (1,000,000 box headers at every level, the top
  level included), `maxTracks` (256), `maxSampleDescriptions` (64 per track) and `maxTableEntries` (2^22 entries
  of all tables and derived chunks together; at most 32 bytes each, so 128 MiB). Tables are read in 64 KiB
  batches.
- There is no recursion: the parser follows the fixed tree of the boxes it knows.
- Formats never write; the parser only reads through the reader it is given.

## Checked against

The rules were checked against files from FFmpeg 6.1.1 (its MP4, MOV and 3GP muxers: moov last and first, video
only, audio only, two audio tracks, B-frames with ctts and edit lists, a mov_text subtitle track, fragments, H.265,
MPEG-4 Part 2), GPAC's MP4Box 2.2.1 (interleaved, flat, co64, stz2, moov padding, fragments) and Windows' Media
Foundation (MediaComposition and MediaTranscoder, H.264 and AAC). 21 of them are embedded with what FFmpeg's
demuxer finds in them: every track's codec, size, channels and rate, sample count, and a hash of every sample's
offset and size, which the parser must reproduce. What they showed:

- GPAC writes moov first and ends its files with a free box; FFmpeg writes a free box before mdat and, for
  fragmented files, an mfra box at the end; FFmpeg's QuickTime files have a wide box before mdat.
- Media Foundation writes the brand `mp42`, a uuid box after ftyp, moov last, and ctts for its B-frames.
- The fragmented files of FFmpeg and GPAC keep empty sample tables in moov (their samples are in moof boxes).
- FFmpeg's 3GP files have the brand `3gp6`. Its AAC tracks start with a priming frame that the edit list skips
  (media time 1024) but the sample tables hold, so the samples are compared with edit lists ignored.

A corpus of 48 larger files (the same writers' 20-second files, and 27 MP4 videos shipped with Visual Studio,
made by screen recorders and an online GIF converter) all parse as `Valid`. In the other direction, FFmpeg lists
exactly the samples the builder wrote in each of its layouts and remuxes the files, and MP4Box reads them; a copy
with one chunk offset moved is caught. See [../testing/testing.md](../testing/testing.md).

P12 regenerated the embedded samples with three more fragmented FFmpeg files (CMAF with `default_base_moof`, a
fragment per keyframe after a moov with samples, and `separate_moof`), 24 in all. For the 5 fragmented ones the
parser's samples of the movie fragments reproduce FFmpeg's offsets and sizes too. What they showed: with
`frag_keyframe`, FFmpeg keeps the samples before the second keyframe in moov's tables and an mdat after moov, and
fragments only what follows (a file with one keyframe has mvex but no moof at all, hence `-g 2`). The corpus
grew to 51 files (3 larger Media Foundation files), and every one of them is carved whole by the right format (MP4
or M4A) and validates, with the NAL units of all its AVC and HEVC samples (tens of thousands) filling them.

## Tests

Label `formats`, executable `recovery_formats_tests`:

| File | Covers |
| --- | --- |
| `tests/formats/mp4_box_test.cpp` | FourCC codes; box headers of every size form at every bound (no combination escapes its bounds); box sequences; the top-level scan: any order, a second ftyp, boxes inside mdat, unknown types, what is not a box, truncated boxes and headers, size 0, the box limit |
| `tests/formats/mp4_parser_test.cpp` | A normal MP4; moov first, last and between; media data in several mdat boxes, interleaved or not; children in any order; QuickTime layouts and no ftyp; 64-bit sizes at every level; version 1 headers; an mdat to the end; stsz, stz2 (4, 8, 16 bits) and one size for all; offsets and a moov beyond 4 GiB; corrupt sizes at the top level and inside moov; boxes in the wrong container; missing and repeated boxes; damaged headers, sample descriptions and tables; chunks outside mdat, overlapping or beyond 2^64; every prefix; multiple tracks; audio only and video only; an empty track; limits; a file scattered over a source; a moov on its own; two files back to back; failing reads and cancellation; fuzzing |
| `tests/formats/mp4_reference_test.cpp` | The embedded samples against FFmpeg's reading of them (movie fragments included), their prefixes, fuzzing; an optional corpus (`RECOVERY_MP4_REFERENCE_DIR`), each file also classified, carved whole by MP4 or M4A and validated; exporting the builder's files (`RECOVERY_MP4_EXPORT_DIR`) |
| `tests/formats/mp4_fragment_test.cpp` (P12) | Fragmented files with each kind of base data offset, samples in moov and in fragments, every trun and tfhd field; mvex and trex required; damaged fragment boxes; prefixes; limits; fuzzing; the NAL unit length size from avcC and hvcC |
| `tests/formats/mp4_analysis_test.cpp` (P12) | Brand classes; audio or video by brand and tracks, for every embedded sample; the media extent; moov discovery; the framing of the builder's samples, and samples that are not their own |
| `tests/formats/mp4_format_test.cpp` (P12) | The descriptor; builder files of every layout and other writers' files intact; prefixes; header rejection (false signatures, audio and image brands); MP4 and M4A dividing the ftyp files; where the walk ends; corrupt metadata; overwritten samples; fuzzing |
| `tests/formats/video_carving_test.cpp` (P12) | Registration; every MP4 and M4A carved back byte for byte, each by exactly one format; a file larger than 4 GiB beyond 4 GiB; files cut off by the end of the source; an overwritten moov; two files back to back; a motion photo; a FAT32 volume with deleted and fragmented files; a disk image file |

The builder (`tests/support/mp4_builders.hpp`) writes every layout above, movie fragments too, and records where
each sample, chunk and run went; `tests/formats/mp4_test_helpers.hpp` compares the parser's findings with that
record. Its video samples are NAL units that fill them, as an encoder's would; `spreadMp4` moves a file's media
data gigabytes further on, for sources larger than memory.

## Known limitations

All known limitations, with IDs for discussion, are in [../limitations.md](../limitations.md): L84-L91 for the MP4
parser (P11; L84 and L91 resolved in P12) and L92-L103 for P12 (the formats, and MP4 recovery), with L77 and L86
updated.
