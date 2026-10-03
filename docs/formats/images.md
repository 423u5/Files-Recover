# Image formats (P9): JPEG, PNG, WEBP, GIF, BMP

The first real formats for the carving framework of P8. Library `recovery_formats` (namespace
`recovery::formats`, headers in `include/formats/`), which depends only on `recovery_carving`: a format
sees the bytes of one would-be file through `IContentReader` and nothing else. The audio formats of P10 live
in the same library: [audio.md](audio.md).

```cpp
carving::FormatRegistry registry;
if (Status added = formats::registerImageFormats(registry); !added.ok()) { ... }
// carving as in ../recovery/carving.md: SignatureScanner::create(registry), FileCarver::run(...)
```

`registerImageFormats` adds JPEG, PNG, WEBP, GIF and BMP in the plan's priority order (hits at one offset
are reported in registration order). It adds nothing when one of the ids is already registered. The
scanner and the carver did not change for these formats; `imageFormats()` returns the same five objects
for anyone who wants to pick.

| Header | Format | Signature | Minimum | Maximum | End detection |
| --- | --- | --- | --- | --- | --- |
| `jpeg_format.hpp` | `JpegFormat` | `FF D8 FF` | 28 | 256 MiB | StructureWalk: the end of EOI |
| `png_format.hpp` | `PngFormat` | `89 50 4E 47 0D 0A 1A 0A` | 59 | 1 GiB | StructureWalk: the end of IEND |
| `webp_format.hpp` | `WebpFormat` | `"RIFF" ???? "WEBP"` (masked) | 26 | 1 GiB | SizeField: 8 + the RIFF size |
| `gif_format.hpp` | `GifFormat` | `"GIF89a"`, `"GIF87a"` | 28 | 256 MiB | StructureWalk: the end of the trailer |
| `bmp_format.hpp` | `BmpFormat` | `"BM"` | 30 | 2 GiB | SizeField: the file size field, or what the headers need |

The maximum sizes bound the work a single hit can cost (L50): end detection and validation never read
beyond them, and a file that is larger is carved as `Truncated` with the warning `TruncatedByMaximumSize`.
They are far above any ordinary photo and were chosen per format, not measured; see L66.

## One walk per format

Each format has exactly one walk of its structure (`src/formats/structure_walk.hpp`), and both end
detection and validation use it, so a file ends exactly where its validator stops looking. The walk
separates two things:

| | Meaning | `findEnd` | `validate` |
| --- | --- | --- | --- |
| **Layout** | Where the structure goes: segment lengths, chunk lengths, sub-block chains, size fields | `Found` at the end, `Truncated` when the data runs out, `Broken` where it stops making sense | Truncated / Invalid accordingly |
| **Problems** | Whether what it passes is consistent: checksums, field values, element order | Ignored | The first one makes the file `Invalid`, with its offset as `validBytes` |

A problem never stops the walk, because the layout still says where the file ends: a PNG with one damaged
chunk is carved whole and reported `Invalid`, which is more useful for recovery than cutting it short.
A break does stop it: the bytes before it are consistent, the bytes at it are not, and the carve keeps
the consistent part (`StructureBroken`).

Validation is **structural**. It checks what the format's own structures say about themselves; it does not
decode pixels. Huffman, LZW, deflate and VP8 decoding is media validation, which belongs to P14 (L59) —
which is also why some fragmentation goes unnoticed (L62, L64, L65).

## JPEG (ITU T.81, JFIF, Exif)

**Header check** (4 KiB): SOI, then a marker that can start a file an encoder wrote (APPn, DQT, DHT, a
frame header, COM, DRI, DAC), then the chain of segments as far as the header holds it. This refuses
`FF D8 FF 00`, `FF D8 FF D8`, EOI or SOS first, reserved markers, and segment lengths below 2.

**Walk**: marker by marker from SOI.

- Segments are skipped by their 16-bit lengths, so an Exif thumbnail inside APP1 never ends the file
  early. The thumbnail's own SOI is a hit of its own, which the carver skips because it lies inside a
  candidate that validated (`skipHitsInsideValidCandidates`).
- **Entropy-coded data** after each SOS is scanned for the marker that ends it: `FF 00` is a stuffed data
  byte, `FF FF` runs are fill bytes, and RSTn markers are restart markers. Anything else ends the scan.
- **Restart markers** must be in sequence (RST0…RST7, wrapping), and a scan without a restart interval
  must not have any. Both are breaks: they mean the data is somebody else's.
- After a scan, the number of restart markers must be what the frame needs
  (`ceil(MCUs / interval) - 1`, computed from the frame's dimensions and sampling factors). A mismatch is
  a problem, and it is what catches a cluster of a camera photo overwritten in place.
- A **second frame header** without DHP ends the walk: another image's data follows.
- A run of more than 4 KiB of `FF` fill bytes is a break: that is erased flash, not a file.

**Problems** (the file is `Invalid`, but it still ends where its structure does): frame headers with an
impossible precision, width 0, sampling factors outside 1–4, repeated component ids; scan headers naming
components the frame lacks, entropy tables beyond 3, or progressive spectral selection that does not
follow T.81; DQT and DHT tables that run beyond their segment, Huffman code lengths that cannot be a
canonical code, DC symbols beyond 16; a component whose quantization table was never defined; EOI before
any frame or scan; markers reserved for JPEG extensions.

DHT is **not** required: Motion-JPEG frames rely on the standard tables, and decoders supply them.
Arithmetic coding (SOF9–SOF11, DAC), lossless and hierarchical frames are walked, but only their markers
are checked.

## PNG (ISO/IEC 15948)

**Header check** (33 bytes): the 8-byte signature and a 13-byte IHDR whose fields are valid (dimensions
1…2^31-1, a bit depth the color type allows, compression 0, filter 0, interlace 0 or 1) and whose CRC-32
matches. That makes a false PNG signature practically impossible to follow.

**Walk**: chunk by chunk. A chunk's length must be at most 2^31-1 and its type four ASCII letters,
otherwise the structure breaks. Lengths lead to IEND.

**Problems**: a CRC-32 that does not match (checked in validation only, so end detection does not lose the
rest of a file over one damaged chunk); IHDR that is not the first chunk or has invalid fields; PLTE in a
grayscale image, after the image data, with a length that is not 3 to 768 bytes of RGB entries, or with
more entries than the bit depth can index; a palette image without PLTE; IDAT chunks that are not
consecutive; image data that does not start a zlib stream (CM 8, window at most 32 KiB, check value, no
preset dictionary); IEND with data or without any image data; an unknown **critical** chunk, which
decoders must refuse (this is also why CgBI PNGs from iOS are refused, L67).

Ancillary chunks (`tEXt`, `tIME`, APNG's `acTL`, `fcTL`, `fdAT`, unknown ones) are skipped.

## WEBP (RFC 9649)

**Header check** (30 bytes): "RIFF" … "WEBP", a RIFF size with room for a chunk, a first chunk of VP8,
VP8L or VP8X that fits inside it, and that chunk's own signature bytes (the VP8 key-frame bit and start
code `9D 01 2A`, the VP8L signature `0x2F` and version 0, a VP8X of at least 10 bytes).

**Walk**: the RIFF size says where the file ends, and the chunks must fill it exactly. Each chunk is
`[FourCC][size][payload][pad to even]`; a chunk whose id is not printable ASCII, or that runs beyond the
RIFF data, breaks the structure.

**Problems**: a last chunk without its padding byte; a first chunk that is not VP8, VP8L or VP8X; VP8X
anywhere else; a canvas of more than 2^32-1 pixels; VP8 data that is not a shown key frame of version 0–3,
has no start code, has a dimension of 0, or whose first partition runs beyond its chunk; VP8L data without
its signature or version 0; an image that does not have the canvas's size; more than one image; ALPH
outside an extended still image, with an unknown method, not followed by VP8 data, or uncompressed and
smaller than the canvas; ANIM in a still image; ANMF before ANIM, in a still image, reaching beyond the
canvas, or holding anything other than exactly one image; an animation without frames; a file without
image data.

ICCP, EXIF, XMP and unknown chunks are skipped.

## GIF (87a and 89a)

**Header check** (782 bytes): the 6-byte signature, and — when the header reaches that far — a first block
introducer of `0x2C`, `0x21` or `0x3B` after the global color table.

**Walk**: block by block to the trailer. Image descriptors and extensions are followed through their
chains of data sub-blocks; any other introducer breaks the structure.

**Problems**: an LZW minimum code size outside 1–11 (the specification says 2–8; browsers decode the wider
range, so it is a problem, not a break); an image without data; a graphic control, application or plain
text extension whose first sub-block is not 4, 11 or 12 bytes; a trailer before any image.

Unknown extension labels are skipped, as decoders do.

## BMP (Windows and OS/2)

"BM" is two bytes, so the header check carries the weight. It refuses anything whose DIB header size is
not one of 12, 16, 40, 52, 56, 64, 108 or 124; a bit depth the compression does not allow; a width of 0 or
negative, a height of 0; more than one plane; a pixel data offset pointing into the headers; a compressed
bitmap with no image size; a compressed top-down bitmap; and palettes larger than any bitmap has.

**End**: the pixel data's size follows from the dimensions, bit depth and compression (uncompressed:
`((width * bits + 31) / 32) * 4 * height`, with checked arithmetic; compressed: the image size field), and
a V5 color profile stored in the file counts too. Then:

| File size field | End | |
| --- | --- | --- |
| At least what the headers need | The file size field | Padding at the end of a file is normal |
| 0 | What the headers need | Some writers leave the field empty |
| Smaller, and not 0 | What the headers need | And a problem: the headers disagree with each other |

**Problems**: the file size field disagreeing as above; a pixel data offset that points into the palette or
bit masks; more than one plane; a color profile that overlaps the header; and, for RLE4 and RLE8, data
that does not end with an end-of-bitmap code, or that reaches beyond the last row. The RLE walk is the
only part that reads pixel data, and only during validation.

## What this does not check

Uncompressed BMP pixels, a simple WebP's VP8 bitstream and JPEG entropy-coded data without restart markers
have no structure that a walk can check, so another file's data in the middle of one is not always noticed
(L62, L64, L65). Decoding the image would be needed, and that is media validation (P14, L59). Every
limitation, with the IDs to discuss them by, is in [../limitations.md](../limitations.md) (L59–L67).

## Tests

Label `formats`, executable `recovery_formats_tests` (see [../testing/testing.md](../testing/testing.md)):

| File | Covers |
| --- | --- |
| `tests/formats/jpeg_format_test.cpp` | Every sampling, baseline and progressive, restart intervals, Exif thumbnails, comments, fill bytes, odd sizes; damaged segment lengths, reserved markers, frame, scan and table contents; entropy data with stray markers; restart order and count; fragments and erased media; fuzzing |
| `tests/formats/png_format_test.cpp` | Every color type and bit depth, interlacing, split image data, ancillary chunks, APNG; chunk headers, CRCs, order and required chunks; unknown critical chunks; the zlib header; fragments; fuzzing |
| `tests/formats/webp_format_test.cpp` | Simple and extended files, alpha, metadata, animations; RIFF sizes and chunk layouts that disagree; damaged bitstream headers; frame rules; fragments; fuzzing |
| `tests/formats/gif_format_test.cpp` | 87a and 89a, color tables, depths, frames, interlacing, extensions; introducers, sub-block chains, code sizes; files without images; fragments; fuzzing |
| `tests/formats/bmp_format_test.cpp` | Every header version and bit depth, bit fields, RLE4/RLE8, top-down, a color profile; header rejection; size fields; damaged RLE data; fragments; fuzzing |
| `tests/formats/image_carving_test.cpp` | Registration; all five formats carved back byte for byte from a noisy disk; embedded thumbnails; files cut off by the end of the source; an overwritten file; a flood of false signatures; a FAT32 volume with active, deleted and fragmented images; a disk image file; a JPEG whose scan never ends |
| `tests/formats/image_reference_test.cpp` | The embedded samples of other encoders, an optional corpus from elsewhere, and exporting the builders' files for other decoders |

Every intact file is checked the same way (`isIntact`): a signature matches, the header is accepted, the
end is found at the file's last byte (also with data after it), and validation says `Valid` for all of it.
Every **prefix** of an intact file must be `Truncated`, never `Valid` — that is what "do not mark a
candidate recovered based only on its header" comes down to in practice.
