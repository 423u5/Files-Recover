# Metadata for a user interface (P17)

P17 gives a future user interface what it shows about the files a scan found, without the engine depending on
any user interface library. The library `recovery_metadata` (namespace `recovery::metadata`, headers in
`include/metadata/`) has three parts:

```
EvaluatedCandidate (P14) ──► describeCandidate()        name, path, kind, size, validation, condition, SHA-256
                         ──► DuplicateGroups::build()    candidates with the same content, original first
                         ──► readMediaMetadata(source)   what the content says: image, audio, video, tags, previews
RecoverySession (P16)    ──► RecoveryJobIndex            what recovery jobs did with each candidate
```

`describeCandidate` and `DuplicateGroups` use the evaluated candidate alone. `readMediaMetadata` reads the
candidate's bytes from the source when it is asked. The user's P17 decision was on demand: nothing is added to a
scan or to a session's journal, and the source must be attached. `RecoveryJobIndex` reads a session's jobs.

## The plan's items

| The plan's item | Where |
| --- | --- |
| SHA-256 duplicate identity | `CandidateMetadata::sha256` and `duplicateOf` (P14's identity); `DuplicateGroups`: the original and its duplicates, whatever their names |
| image dimensions | `ImageMetadata`: width, height, colour model, channels, bits per channel and per pixel, alpha, frames, loop count, progressive; Exif orientation, date taken, camera make and model |
| audio metadata | `AudioStreamMetadata`: codec, profile, sample rate, channels, bits per sample, bitrate; `MediaMetadata::duration`; `MediaTags` |
| video metadata | `MovieMetadata`: brands, creation and modification time, fragmented; `TrackMetadata` per track (codec, size, frame rate, rotation, channels, language, duration); `VideoStreamMetadata` (codec, profile, level, sizes, frame rate, rotation, bitrate); tags |
| file size | `CandidateMetadata::size` (the bytes recovery writes) and `expectedSize` |
| validation status | `CandidateMetadata`: the overall status, each level's status, the deepest level passed |
| recovery status | `CandidateMetadata::condition` and `reasons` (the condition a file would be recovered in); `RecoveryJobIndex`: whether a job wrote it, where, and whether every byte was read |
| thumbnail/preview metadata interface | `PreviewSource` (the content, Exif thumbnails, cover art: format, media type, place, size, orientation), `readPreview` and `openPreview` (their bytes) |

## Reading a file's metadata

```cpp
Result<MediaMetadata> readMediaMetadata(source, candidate, MetadataOptions{}, SourceReadOptions{});
Result<MediaMetadata> extractMetadata(IContentReader& content, std::string_view formatId, MetadataOptions{});
```

`readMediaMetadata` opens a `CandidateContentReader` on the candidate, so it reads the bytes recovery would write,
and calls `extractMetadata` with the format the evaluation found (`EvaluatedCandidate::formatId`). An unknown or
empty format gives `MediaKind::Unknown` and nothing else.

Only headers, tags and frame headers are read, never the coded pixels or samples. `MetadataOptions` bounds the work:

| Option | Default | Bounds |
| --- | --- | --- |
| `maxScanBytes` | 64 MiB | Frame streams (MP3 without an info tag, ADTS) and animations (GIF blocks, PNG and WebP chunks) walked to count their frames. A longer file's duration is extrapolated from what was walked (`durationEstimated`), and an issue says so. |
| `maxTagBytes` | 16 MiB | Blocks read into memory whole: an Exif block, a RIFF INFO list, an unsynchronised ID3v2 tag. A larger one is skipped with an issue. Pictures inside tags are never read, only located. |
| `maxTextLength` | 1024 | The longest text kept, in UTF-8 bytes, cut at a character boundary. |
| `mp4` | P11's `ParseLimits` | The MP4 parse (boxes, tracks, sample table entries), and the samples of movie fragments whose durations are summed. |

The content is untrusted. Every count, size and offset is checked against the bytes that hold it before anything
is read or allocated. IFDs are followed one hop from IFD0, so a loop of IFD pointers cannot make the walk go on.
Problems of the content are `issues`, never errors. Each issue has an offset and a description; the first 32 are
kept and all are counted. `extractMetadata` fails only with the reader's errors (`Cancelled`, a failing source)
and with `InvalidInput` for options that are 0.

Tag and Exif values are file content. They are returned to the caller and never logged; the library does not log.

Text is UTF-8. The decoding rules:
- Control characters become spaces, and spaces at both ends are trimmed.
- Invalid sequences become U+FFFD.
- ID3 text is read in the encoding its frame names. UTF-16 without a byte order mark is read as little-endian, as
  the writers that leave the mark out write it.
- Exif ASCII, RIFF INFO and ID3v1 should be ASCII and are not always. They are read as UTF-8 when they are valid
  UTF-8, and as ISO 8859-1 otherwise.

### Images

| Format | What is read |
| --- | --- |
| JPEG | The marker segments up to the first scan: the first frame header (SOF0-SOF15: size, precision, components, progressive), JFIF, the Adobe APP14 transform, the first Exif APP1. Colour: 1 component grayscale; 3 YCbCr, or RGB with Adobe transform 0 or component ids R, G, B without JFIF; 4 CMYK, or YCCK with Adobe transform 2. A height of 0 (DNL) is an issue. |
| PNG | IHDR (size, colour type and bit depth, which must be a valid pair, interlace), then every chunk header up to IEND: tRNS (transparency), acTL (frames, plays), fcTL (frame delays: the duration), eXIf. |
| GIF | The screen (size, colour table bits), then every block up to the trailer: image descriptors (frames; the first frame's interlace), graphic control (transparency, delays: the duration of an animation), NETSCAPE2.0 or ANIMEXTS1.0 (loop count). |
| BMP | The file and DIB headers of every size (12, 16, 40, 52, 56, 64, 108, 124 bytes): size, bits per pixel, compression, bit-field masks (channel depth, alpha mask). OS/2 headers give compressions 3 and 4 other meanings and have no masks. |
| WebP | The first chunk (VP8 key frame header, VP8L header, or VP8X canvas and flags), then every chunk: ALPH, ANIM (loop count), ANMF (frames, durations, the first frame's coding), EXIF. |

Exif (PNG eXIf and WebP EXIF chunks may or may not start with "Exif\0\0"; both are read) gives:
- IFD0: Make, Model, Orientation (SHORT or LONG, 1 to 8; anything else is an issue).
- The date taken: the Exif IFD's DateTimeOriginal with OffsetTimeOriginal. Without it, DateTimeDigitized with
  OffsetTimeDigitized. Without that, IFD0's DateTime with OffsetTime. A date of zeros or blanks means not set; other
  text that is not a date is an issue.
- IFD1's JPEG thumbnail (JPEGInterchangeFormat and its length), with IFD1's own orientation if it has one.

### Audio

| Format | What is read |
| --- | --- |
| MP3 | The ID3v2 tags at the start (any number) and zero padding up to 64 KiB, then the first Layer III frame (at most 64 KiB further, confirmed by the next frame's header or the end). A Xing, Info or VBRI tag in it gives the frame count and the bytes: the duration is frames x samples per frame / rate. Xing and VBRI mean a variable bitrate, Info a constant one. Without a tag the frames are walked. ID3v1 at the end fills what ID3v2 leaves empty. |
| ADTS (AAC) | ID3v2 at the start and ID3v1 at the end as for MP3; the frames are walked: 1024 samples per raw data block. The profile is the header's (AAC Main, LC, SSR, LTP). |
| WAV | Every chunk: fmt (format tag, channels, rate, byte rate, block align, bits; WAVE_FORMAT_EXTENSIBLE's valid bits and sub-format), fact, data (the bytes there), LIST INFO (INAM title, IART artist, IPRD album, ICRD date, IGNR genre, IPRT and ITRK track), "id3 " and "ID3 " chunks (ID3v2 tags; INFO fills what they leave empty). PCM, float, A-law and mu-law last data bytes / block align / rate; others the fact chunk's frames, or the data over the byte rate (estimated). |

ID3v2 (2.2, 2.3 and 2.4):
- Tags are walked frame by frame through the reader. Text frames are read up to what the text limit needs; a picture
  is only located, and its first bytes tell its format.
- Version 2.4 sizes are syncsafe. Some writers (old iTunes) wrote plain ones, so the plain size is taken when only
  it leads to the next frame.
- Frames that are compressed or encrypted are skipped. A 2.4 frame that is unsynchronised has its text restored, and
  its picture is not offered.
- A tag unsynchronised as a whole (2.2, 2.3) is read into memory and restored; its pictures are not offered (L154).
- The frames read: TIT2, TPE1, TALB, TCON, TRCK, TDRC, TYER with TDAT and TIME, and APIC (with 2.2's three-letter ids).
- Genres: "13", "(13)" and ID3v1 numbers are names from the 192-entry Winamp list; "(4)Eurodisco" gives the
  refinement; "(RX)" and "(CR)" are Remix and Cover.

### MP4 and M4A

P11's `parseFile` reads the movie: the brands, the tracks, the sample tables and the movie fragments. Its audio or
video classification (P12's `classify`) gives the kind; content with neither an ftyp nor a moov keeps its format's
kind. What the parser leaves out is read here:
- **Times.** The movie header's creation and modification time: seconds since 1904, UTC, where 0 means not set.
- **Rotation.** The track header's matrix gives degrees clockwise to show the picture: (0, 1, -1, 0) is 90, as
  exiftool's `Rotation` says. FFmpeg's `-display_rotation` and ffprobe's `rotation` count counter-clockwise. A matrix
  that is not a quarter turn (a mirror) is an issue, and the rotation is 0.
- **Codecs.** From the first sample description:
  - avcC gives the AVC profile and level ("Constrained Baseline", "1.0", or "1b").
  - hvcC gives the HEVC profile, level and tier.
  - esds (also inside QuickTime's `wave`) gives the object type: MPEG-4 audio with its AudioSpecificConfig (object
    type, rate, channels, explicit SBR and PS), MPEG-2 AAC, or MP3.
  - Sample entries are named like FFmpeg's codecs ("h264", "hevc", "aac", ...); others keep their four characters.
- **Durations.** A track's duration is its media header's, or the sum of its sample durations (stts and movie
  fragments) when the header has none or the movie is fragmented. Fragment durations come from each trun's
  per-sample durations, or tfhd's and trex's defaults. The movie's duration is its header's, or the longest track's
  for fragmented movies.
- **Frame rate.** Samples x time scale over the summed sample durations, reduced: 30000/1001.
- **Tags.**
  - moov/udta/meta/ilst (meta as a full box or, as QuickTime writes it, a plain one): ©nam, ©ART, ©alb, ©day, ©gen,
    gnre, trkn, and covr, whose every picture is offered.
  - QuickTime's udta text atoms: ©nam, ©ART, ©aut, ©alb, ©day, ©gen.
- **Media type.** From the major brand: video/quicktime (qt), video/3gpp, video/3gpp2, video/mp4; audio/mp4 or
  audio/3gpp.

A structure the parser does not find valid gives one issue with its detail; the metadata is what could be read.

## Previews

`MediaMetadata::previews` lists Exif thumbnails and cover art in content order, then the content itself. Each
`PreviewSource` gives:
- the kind;
- the carving format and media type of the bytes;
- their offset and length in the content;
- the picture's size, from the picture's own header;
- the orientation: Exif, with a thumbnail sharing its image's, or for video the track's rotation as an orientation;
- for ID3 cover art, the picture type (3: front cover).

A picture's format comes from its first bytes (JPEG, PNG, GIF, BMP, WebP), never from the MIME type or the type
code of its tag. A picture is offered only when the engine reads its header; otherwise an issue says why. The
content itself is offered once its header was read: images, audio, and MP4 with a video track (or an audio track for
M4A).

`readPreview(content, preview, maxBytes)` returns the bytes; a preview larger than `maxBytes` (64 MiB) is
refused. `openPreview(content, preview)` returns a reader of them, which a platform decoder can read through
(`WindowReader`). Nothing here decodes or renders.

## Dates, orientation

`MediaDateTime` holds what a file records:
- the date and time to its precision (year to second);
- its zone: none (Exif without an offset, ID3, RIFF INFO), UTC (MP4 headers; text ending in "Z"), or a fixed
  offset.

`iso8601()` prints it to its precision, and `utc()` gives the instant when the zone is known. `Orientation` is
Exif's value (1-8); `rotationOf` and `mirrored` say what it does, and `orientationForRotation` gives the value for
a quarter turn.

## Candidate metadata

`describeCandidate(candidate)` gives what a list shows before anything is read:
- the id, the name (as the metadata records it, or the made-up name of a carved file), the path on its volume, and
  the extension;
- whether the filesystem says the file is deleted (false without filesystem evidence: carving cannot tell);
- the method, and the kind, format and media type of the format the evaluation validated the content as;
- the size recovery writes and the expected size, the first source offset, and the number of fragments;
- the filesystem times;
- the overall validation status, each level's status, and the deepest level passed;
- the condition and its reasons;
- the SHA-256, the original it duplicates, and the active file a carved file lies inside.

The condition is one word for a list. The first rule that applies decides it:

| Condition | When | Reasons |
| --- | --- | --- |
| AMBIGUOUS | one of several layouts the evidence supports equally | `AlternativeLayout` |
| UNRECOVERABLE | the file has bytes and none is located, or reconstruction found no layout that validates | `NothingLocated`, `ReconstructionFailed` |
| PARTIAL | bytes the metadata does not locate, content cut short (a level is Truncated), a partial reconstruction | `DataMissing`, `ContentTruncated`, `ReconstructionPartial` |
| CORRUPTED | a level failed, bytes unreadable (zeros), bytes in clusters allocated to other data now, a corrupted reconstruction | `ValidationFailed`, `DataUnreadable`, `ClustersReallocated`, `ReconstructionCorrupted` |
| UNVERIFIED | nothing validated the content (no format, validation off) | `NotValidated` |
| COMPLETE | none of these | |

Every fact that applies is listed in `reasons`, so a PARTIAL file can also say it is damaged. An empty file is
COMPLETE when its format validates it, and UNVERIFIED otherwise.

## Duplicates

P14 identifies duplicates: equal SHA-256 of non-empty content. The first candidate delivered is the original, and
each later one's `duplicateOf` names it. `DuplicateGroups::build(candidates)` turns those links into groups:
- Each group lists the original first, then its duplicates in id order, with the digest and size.
- `groupOf(id)` finds the group of any member, and `duplicateCount()` counts the duplicates.
- The original is listed even when the candidates given are a page that does not hold it.

On the scan tests' card, PHOTO.JPG and COPY.JPG form a group, as do CLIP.MP4 and the copy of it that only carving
finds.

## Recovery status

`RecoveryJobIndex::fromSession(session)` reads a session's jobs: each job's candidates (`info().jobs`) and the
items it is done with (`recoveredItems(job)`). `addJob` takes any job's records. For each candidate it gives one
`JobRecovery` per job:
- **Recovered:** the path, the reconstruction report, and `complete` when every byte was located and read.
- **Failed:** the error.
- **Pending:** the job has not reached the candidate; it runs, is paused, was cancelled or interrupted, or has not
  run.

The summary state is the first that applies: Recovered, Pending, Failed, NotRecovered. The index is a snapshot of
the session when it was built.

## Threads

`extractMetadata` and `readMediaMetadata` are free functions without shared state: concurrent calls are safe on
different readers (a reader has one owner at a time), and `readMediaMetadata` opens its own on a source that allows
concurrent reads. A reader from `openPreview` shares the content reader it views: one owner for both.
`DuplicateGroups` and `RecoveryJobIndex` are immutable once built (`addJob` has one owner); their const members are
safe from any thread.

## Tests

`recovery_metadata_tests` (label `metadata`):

- **`media_metadata_test.cpp`**, metadata extraction on the builders' files:
  - every JPEG sampling and progression;
  - Exif in both byte orders, with a thumbnail, date, offset and camera (the JPEG builder gained Exif fields, off by
    default);
  - every PNG colour type and depth, interlaced and animated;
  - GIF frames, loops and delays; every BMP header and depth;
  - WebP simple, extended, with alpha and animated;
  - MP3 info tags or frames counted, every MPEG version, ID3v2.2-2.4 and ID3v1, cover art and its bytes, text in
    every ID3 encoding, dates, tracks and genres, unsynchronised tags, long text and control characters;
  - ADTS; every WAV encoding, fact, INFO and ID3 chunks, cut data;
  - M4A, and MP4 tracks, codecs, frame rate, fragments, rotations from patched matrices, header times, iTunes items
    with cover art, QuickTime text atoms, brands.
  
  It also covers missing metadata (no Exif, no tags, no header times: empty fields), options and limits.
- **`metadata_robustness_test.cpp`**, invalid media:
  - garbage and empty content of every format;
  - every builder file truncated at every position (or 3000 positions), and 300 mutations of each (bytes, bits,
    runs of 0xFF and 0x00);
  - damaged Exif: values outside the block, orientation 9, an Exif IFD outside, a thumbnail past the block, IFD1
    pointing back at IFD0, counts that do not fit, not TIFF, over the tag limit;
  - damaged ID3: a frame past the tag, an invalid frame id, an unknown encoding, a tag past the content, over the
    limit, a picture that is not one;
  - the issue cap, WAV without fmt, and reader errors.
  
  Extraction never fails because of the content, previews always lie inside it, and issues stay bounded.
- **`metadata_reference_test.cpp`**, files of independent writers:
  - New samples from `tests/reference/make_metadata_samples.sh`: FFmpeg, ExifTool on FFmpeg and cwebp output,
    mutagen. They cover Exif in JPEG (both byte orders, a thumbnail), PNG and WebP; GIF and APNG animations; ID3v2.3
    and 2.4 with covers; ADTS with ID3; WAV with INFO and with an ID3 chunk; M4A, MP4 and MOV tags; creation times;
    rotations of 90 and 180 degrees.
  - Every image, audio and MP4 sample already embedded in `tests/support`.
  - `embed_metadata_samples.py` records what exiftool and ffprobe read in each file (102 files, 774 values), and the
    engine must agree. Durations may differ by 70 ms or 3%, because ffprobe counts encoder delay and edit lists.
    Rotations are checked against exiftool and ffprobe both.
  - Every thumbnail and cover, read on its own, is the picture its preview describes.
- **`candidate_metadata_test.cpp`**:
  - every condition rule and reason, and descriptions of filesystem and carved candidates;
  - duplicate groups from links and from a page without the original;
  - job states;
  - on the scan tests' card: duplicate content under different names (PHOTO.JPG and COPY.JPG; CLIP.MP4 and its
    carved copy), every candidate's media metadata read from the source (the reconstructed fragmented JPEG among
    them), and the recovery status of a real session's jobs, one run and one pending.

## Known limitations

L153-L166 in [../limitations.md](../limitations.md). L39 changed with P17.
