# Validation levels and media validation (P14)

A file is never taken as recovered because its header, its extension or its name looks right. P14 checks a file's
content at three levels, one after the other, each with a status and a detail that says what was checked or what
failed and where (`include/validation/validation.hpp`):

| Level | What checks it | When it runs |
| --- | --- | --- |
| structural | the format's own validator (P8–P12): lengths, checksums, element order, sample tables | always, when a format is known |
| media | the engine's own deterministic decoders (`media_validator.hpp`): the coded data inside the structure | on by default; not when the structure failed |
| playability | a platform decoder decodes the whole file (`playability.hpp`; on Windows WIC and Media Foundation, `windows_playability.hpp`) | off by default; only when the structure passed and the media neither failed nor were cut short |

A level ends in `Passed`, `Truncated`, `Failed`, `NotApplicable` (nothing is coded at this level: uncompressed
pixels, PCM), `Unsupported` (no checker for this content, a coding feature not decoded, a limit, no platform decoder)
or `NotRun` (not asked for, or an earlier level failed). The media level also says whether every coded element was
checked (`Coverage::Full`) or only part of it (`Partial`: the headers of lossy codecs). Nothing is a score: the
overall status follows from the levels, first match wins: `Invalid` when a level failed, `Truncated` when one found
the content cut short, `Valid` when the structure passed, `NotValidated` otherwise.

`validateContent(content, format, media, options, knownStructure)` runs the levels on a file's bytes. A caller that
holds the format's verdict on exactly those bytes passes it as `knownStructure`, and the structural validator is not
run again.

## Media decoders

They decode and discard: no pixel or sample is kept beyond what the next one depends on. Where a rule is a matter of
judgement, the decoder follows a reference implementation, named below; what that implementation rejects fails, what
it accepts passes.

| Format | What is decoded | Coverage | Reference |
| --- | --- | --- | --- |
| JPEG | baseline, extended and progressive Huffman scans: every code, coefficient and MCU, restart intervals, the end of each scan; Annex K tables when a file has none; arithmetic coding and more than 4 components are `Unsupported` | full | libjpeg-turbo |
| PNG | the zlib stream of every image and APNG frame: inflate, the exact size of the scanlines, filter types, Adler-32; data after the scanlines or the stream fails | full | libpng |
| GIF | the LZW data of every image: every code, the exact pixel count (a missing end code is accepted, extra pixels fail) | full | giflib |
| BMP | RLE8 and RLE4: every run stays inside its row and the bitmap; uncompressed pixels are `NotApplicable`; JPEG, PNG, Huffman 1D and RLE24 inside a BMP are `Unsupported` | full | Windows GDI semantics |
| WebP | VP8L (lossless) images and alpha in full (prefix codes, color cache, transforms, sub-images); VP8 (lossy) frame headers, partitions and token probabilities | full (VP8L), partial (VP8) | libwebp |
| WAV | IMA and Microsoft ADPCM block headers (step index, reserved byte, predictor, delta); PCM, float, A-law and µ-law are `NotApplicable`; other codings `Unsupported` | partial | FFmpeg, sox |
| MP3 | not decoded: `Unsupported` (the structural validator already checks side info and the bit reservoir) | — | — |
| AAC (ADTS) | every frame's raw data block as far as it can be read without the Huffman tables (section data, and whole channel streams of the zero codebook), the CRC of protected frames | partial | libfdk-aac, FFmpeg |
| M4A, MP4 | every sample of every audio and video track, with its sample description's configuration: AAC samples as raw data blocks; AVC (`avcC`) and HEVC (`hvcC`) parameter sets and every slice header; every NAL unit scanned for the byte sequences it may not hold | partial | FFmpeg 6.1 |

### AAC

The raw data block reader (`src/validation/aac_syntax.cpp`) reads the elements in order: data stream, program config
and fill elements completely; a channel element's streams up to their coded scale factors (global gain, `ics_info`
with the reserved bit clear and `max_sfb` within the window's bands, prediction data only in AAC Main, LTP only in
AAC LTP, `ms_mask_present` not 3, sections without the reserved codebook 12 that end exactly at `max_sfb`). A stream
whose sections all use the zero codebook has no scale factors or spectral data, so it is read to its end (pulse data
only with long windows, TNS orders within the profile's limit, no gain control outside AAC SSR) and the block goes
on. A complete block's `ID_END` must end the frame (or the MP4 sample). The channel elements may not hold more
channels than the configuration (a lone CPE in a mono configuration is accepted, as FFmpeg plays it as stereo), and
an LFE needs a configuration that has one.

The ADTS CRC covers, as libfdk-aac computes it and fdkaac's files bear out (found by experiment, 2026-10-01): CRC-16
(0x8005, initial 0xFFFF) over the 56 header bits, then per element of the block the first 192 bits of each SCE and
LFE and of each CPE, the first 128 bits of a CPE's second channel stream (zero-padded when an element is shorter),
and every bit of a data stream element; fill elements are not covered. Where the regions lie is known once the
elements before them were read to their ends; for the last element read only in part (mono and stereo frames) every
possible end and second-stream start is tried, rolling the CRC bit by bit, and a match counts only where a second
stream's header reads. A frame fails only when no placement gives the stored CRC. In 1,424 deliberately wrong CRCs of
the test files (every CRC bit of every mono and stereo frame) no placement ever matched by chance. Frames with a
program config element, frames of several blocks and frames of more channels whose elements were read only in part
are not checked (L119).

### AVC and HEVC

`src/validation/avc_syntax.cpp` and `hevc_syntax.cpp` read the decoder configuration records and every parameter set
(sequence, picture and, for HEVC, video parameter sets, with VUI, HRD, scaling lists, short-term reference picture
sets including inter-set prediction, and the range, multilayer, 3D and screen content extensions), and the header of
every slice: reference list modification, prediction weights, reference picture marking, and with CABAC the
`cabac_alignment_one_bit`s; for HEVC the slice's own reference picture sets, long-term pictures, entry points (which
must lie inside the slice segment data) and `byte_alignment()`. Slice data is not decoded.

A header fails where the specification forbids what it holds and FFmpeg's decoder (6.1) rejects it too, and where the
syntax cannot be followed (a field beyond the end of the NAL unit, a value that selects no branch). A few rules fail
although FFmpeg carries on: forbidden byte sequences inside a NAL unit, a CABAC alignment bit of 0, a
`nuh_temporal_id_plus1` of 0, memory management and long-term reference errors (FFmpeg logs them), HEVC alignment
zero bits. What FFmpeg logs and corrects is accepted: a VUI that does not read cleanly (FFmpeg retries the VUIs of
old encoders with another layout), an HEVC PPS that ends early, AVC weight denominators above 7, data after a
parameter set's fields. Coding features FFmpeg lacks but the specification has (separate colour planes, different
luma and chroma bit depths, slice groups) are read as the specification says. An SPS replaced by a different one
takes the PPSs that refer to it along, as in FFmpeg. NAL units of layers above the base layer are not read.

Every NAL unit is also scanned for 00 00 00, 00 00 01, 00 00 02 and 00 00 03 followed by a byte above 03
(`src/validation/nal_units.cpp`). Zero bytes at the very end of a unit are taken as padding (FFmpeg drops them, and a
muxer that copied a byte stream may have kept its trailing zeros); a run of zeros with data after it is how a
zero-filled cluster in the middle of a picture shows (L118). The scan reads all of a video track's media data.

Verification: every slice header of the 23 libx264 and libx265 vectors (CAVLC and CABAC, B pictures, weighted
prediction, interlacing, slices, 4:2:2, 4:4:4, 10 bits, lossless, wavefront entry points, open GOPs, temporal
sub-layers, in-band parameter sets) ends at exactly the bit where FFmpeg's `trace_headers` bitstream filter ends it
(280 of 280 slices, checked 2026-10-03). Flipping each of the first 32 bits after a slice's NAL unit header is caught
for about half of them (157 of 320 in an AVC file, 151 of 320 in an HEVC one); the rest are fields any value of which
is valid (L117).

### Limits

`MediaLimits` bounds each decoder: `maxMemory` (256 MiB: JPEG progressive coefficients, VP8L sub-images and codes,
PNG image data positions) and `maxDecodedBytes` (4 GiB of decoded output: scanlines, pixels, 64 bytes per JPEG block
a scan visits). A file whose headers ask for more is `Unsupported` before anything is decoded, since a small file can
claim a huge image. The video NAL scan is linear in the file's size and not bounded by them (L121).

## Playability on Windows

`WindowsPlayabilityChecker` (library `recovery_playability`, the only code in `validation/` that includes
`<windows.h>`, in `src/validation/windows/`) hands the content to the platform's decoders through a read-only
`IStream` over the content reader, cut off from the reader when the check returns:

- images (JPEG, PNG, GIF, BMP, WebP): the Windows Imaging Component opens the file and every frame's pixels are copied
  out through a format converter, in bands of rows;
- audio and video (MP3, WAV, ADTS, M4A, MP4): the Media Foundation source resolver opens the file (its extension is
  a hint, not a requirement) and the source reader decodes every sample of every audio and video stream to PCM or a
  YUV layout, asynchronously, waiting at most `sampleTimeout` (10 seconds) for each sample.

Content the Windows decoders cannot decode although it is valid, and on which they fail or stall rather than refuse,
is `Unsupported` before any decoder runs, so a failure means something: JPEG frames other than Huffman-coded
baseline, extended and progressive of 8-bit samples (WIC fails an arithmetic-coded JPEG as a bad image), bitmaps of
JPEG, PNG, Huffman 1D or RLE24 data, MP4 files with compact sample sizes (`stz2`; the MPEG-4 source fails them), and
AVC beyond Baseline, Main and High (Windows' H.264 decoder accepts High 10, 4:2:2 and 4:4:4 streams and then never
delivers a frame or an error). A decoder that stalls anyway is abandoned after the timeout: the result is
`Unsupported`, and the stalled source reader is never released, because releasing it waits for the stuck decoder
forever (L124). A missing codec (WebP and HEVC come from Store extensions) is `Unsupported`.

Decoders conceal much damage: WIC decodes every PNG of the test vectors whose deflate data is broken, and Media
Foundation decodes the MP4 builder's pattern-byte "H.264" without an error. `Passed` means they decoded every frame
or sample without reporting an error, not that the pictures and sound are intact (L122).

## Tests

`tests/validation` (executable `recovery_validation_tests`, label `validation`): every decoder on builder files, on
files of independent encoders (the samples of P9–P12), on vectors that exercise what the samples do not (PNG zlib
streams, libwebp coding tools, fdkaac CRC-protected ADTS, libx264 and libx265 video:
`tests/reference/make_*_media_vectors.*`, embedded by `embed_media_vectors.py`), on data written bit by bit that
breaks one rule each, on damage the structure cannot see (zero-filled runs, flipped header bits, foreign data inside a
scan), on truncation, limits and mutation fuzzing. `playability_test.cpp` runs the Windows decoders and skips the WebP
and HEVC cases when the codec is missing.

## Known limitations

L116–L125 in [../limitations.md](../limitations.md).
