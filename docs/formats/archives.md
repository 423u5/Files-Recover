# Archive formats (after P20): ZIP

Added after P20 at the user's decision (2026-10-12), when a test on a real USB stick showed that the engine
recovered archives only by name (L213): two intact ZIP archives of Android projects whose entries were gone
were not recovered at all. ZIP is also the container of DOCX, XLSX, PPTX, ODT, JAR, APK and EPUB files,
which are carved as ZIP archives. Text files are not carved: they have no structure to find them by (L213).

Library `recovery_formats`, like the image, audio and video formats; header `include/formats/zip_format.hpp`.

```cpp
carving::FormatRegistry registry;
Status added = formats::registerArchiveFormats(registry, validation::deflateDecoder());
```

| Header | Format | Signature | Minimum | Maximum | End detection |
| --- | --- | --- | --- | --- | --- |
| `zip_format.hpp` | `ZipFormat` | `50 4B 03 04` (a local header) | 100 | 64 GiB | StructureWalk: the end of the end of central directory record's comment |

The format checks each member's data by decoding it, and the engine's DEFLATE decoder is the validation
library's (`src/validation/inflate.cpp`, which decodes the image data of PNG files), which depends on the
formats library. So `ZipFormat` is handed a decoder (`formats::IDeflateDecoder`) when it is made
(`ZipFormat::make`, `registerArchiveFormats`), and `validation::deflateDecoder()` is the engine's: the same
inflater on raw DEFLATE data (no zlib header or Adler-32). The applications register the archive formats
after the video format: the API, the CLI's scan, the fuzz targets and the corpus.

## The walk

One walk (`src/formats/zip_format.cpp`) serves end detection and validation, as for every format
([images.md](images.md)), from the first local header:

1. **Members**, one after the other: the local header (30 bytes, the name, the extra fields), the data, and
   a data descriptor when the header's flag 3 says the sizes follow the data. A local header's sizes set to
   `0xFFFFFFFF` are in its ZIP64 extended information field. The data's length is the recorded compressed
   size; with a data descriptor it is found by decoding the DEFLATE data (which ends itself), or, for other
   data, by the first descriptor signature `PK 07 08` whose recorded size is its distance from the data.
2. An **APK signing block** after the last member (APK Signature Scheme v2 and later), checked by its two
   size fields and its magic.
3. The **central directory**: its entries, a digital signature record, the ZIP64 end of central directory
   record and its locator, and the **end of central directory record**, whose comment ends the file.

**Layout** (end detection): where each element starts follows from the recorded sizes. `Found` at the end of
the comment. `Broken` where no local header or central directory follows a member, where the end record
places the central directory elsewhere than where the walk found it (the walk started at a later member of
an archive whose first members are missing, or the directory is another archive's), and where a recorded
size reaches beyond the data. The last is `Broken`, not `Truncated`: a damaged size would otherwise make the
archive reach the end of the source, and since the format is self-synchronizing (below) every archive after
it would be skipped as its members. `Truncated` only where the data ends inside a header, the directory or
the comment.

**Problems** (validation): the central directory must describe exactly the members walked: every entry
names the offset of a member, once, with the same compression method, encryption flag, CRC-32, sizes and
name, and no member is left out; the end record counts the entries and records the directory's size. And
every member's **data** is decoded: stored data by its CRC-32; DEFLATE data by the decoder, which must end
exactly at the recorded compressed size and unpack to the recorded size with the recorded CRC-32. The detail
of a valid archive says what was checked: `6 members, 75700 bytes unpacked, every member's CRC-32 right`.

**Header check** (286 bytes): the signature, a version needed to extract up to 6.3, no flag that no writer
sets (bits 7 to 10, 14, 15), a known compression method, a name of at least one byte without NUL bytes,
and a stored member's two sizes equal.

## What cannot be checked

A member whose data the engine cannot decode leaves the archive **unchecked**: an encrypted one (flag 0 or 6,
or AES as method 99), or one compressed with a method other than stored and DEFLATE (Deflate64, bzip2, LZMA,
Zstandard, XZ, PPMd). So does a member beyond the limits (`ZipFormat::Limits`): more than 1,048,576 members
(the walk stops: `Broken`), or members that unpack to more than 4 GiB in all. Such an archive, consistent
otherwise, is `NotValidated` with what was not checked (`1 member was not checked (member 2 uses LZMA)`):
its structural level is `Unsupported` and its condition `UNVERIFIED`. The carver keeps it as a candidate but
never trusts it. This is the one verdict a validator gives as `NotValidated`; the carver used to reject that
verdict ("validation returned no verdict").

## Trust, and the other formats

The carver skips the hits inside a file whose end it found and that validated (L51): an embedded
thumbnail is part of its file. For an archive this means the images, audio and video **stored** in it (stored
members show their signatures; compressed ones do not) are part of the archive and are not carved as files
of their own. The user chose this on 2026-10-12, on condition that an archive is trusted only when every
member's data is checked: an archive is `Valid` only when every member's CRC-32 is right. A damaged archive,
or one the engine cannot check, is never trusted, and the other formats carve inside it as they did before.
On the test stick, 1,540 of 1,755 carved files lay inside verified project archives (1,499 of them PNG icons
stored by Android's build tools); they are now recovered with their names, inside the archives.

What is skipped is only what lies inside a verified archive's own bytes: the archive comes back exactly as it was on
the disk, nothing is added to it, and every image outside one (or inside a damaged one) is carved as a file of its
own. The user confirmed on 2026-10-14 that the images stored in an archive need no copy of their own, provided they
are really in it. Checked twice: the corpus scenario `zip-archives` marks the images stored in its intact archives,
and the archive and image stored in an archive stored in another, `inside-archive`: no candidate may hold their
bytes, and a recovered Complete archive must hold them byte for byte as a stored member (`CorpusTest`). On the test
stick (2026-10-14), each of the 647 files the 2026-10-12 scan carved and the archive-aware scan no longer lists lies
in one of the 9 recovered archives: 622 images are byte for byte members, one GIF a member of an SDK's archive
stored in a project's archive, and 24 were broken carves of compressed member data (their files are members too).
The archive's own records (names, extra fields, comments, an APK signing block) are not files of it: an image whose
signature lay in one of them would be skipped as well (L229). Archivers do not put images there, and none was on the
stick.

Two descriptor flags shape how carving and reconstruction see archives:

- `selfSynchronizing`: every member's local header is a hit of the format, inside the archive. The carver
  skips the hits of the format inside an earlier candidate of the same format, whatever its verdict: they are
  its members.
- `foundEndMeansContiguous` (added for ZIP): an archive whose end was found has every byte in place, since
  the central directory places every member from the archive's start, and a walk across a gap would not
  have found the end. So an archive that is `Found` but `Invalid` is damaged where it lies, not fragmented:
  fragment reconstruction (P13) does not take it as a seed, and it stays one `CORRUPTED` candidate with all
  its bytes. An archive whose walk breaks is a seed as for any format, and P13 puts an archive in two
  pieces back together by its members' CRC-32s and its central directory
  (`FragmentRecoveryTest.AFragmentedArchiveIsReconstructed`).

## Validation levels and kind

The structural level does all of it, data included, so that the carver's verdict can be trusted. The media
level is `NotApplicable` (`src/validation/zip_media.cpp`): an archive codes no media of its own. No Windows
decoder takes archives, so the playability level is `Unsupported` when it is asked for.

The kind of the format's files is `archive` (`metadata::MediaKind::Archive`, `api::MediaKind::Archive`; the
CLI's `recover --kind archive`), the media type `application/zip`. Archives have no media metadata and no
preview. A carved archive is named `recovered_NNNNNN.zip` whatever it holds: a carved DOCX or APK is not told
apart from other archives (L214).

## Tests

- `tests/formats/zip_format_test.cpp`: the samples (below) intact, with the details; data descriptors for every
  method; members that cannot be checked (LZMA, bzip2, encryption); no prefix complete; damaged data, central
  directory entries and end records; an archive seen from a later member; damaged sizes; header checks; the
  limits; the DEFLATE decoder; carving with the image formats (a PNG stored in a verified archive is skipped,
  in a damaged one carved); one candidate per archive; fuzzing.
- `tests/validation/zip_validation_test.cpp`: the validation levels of intact, unchecked and damaged archives.
- `tests/recovery/fragment_recovery_test.cpp`: a damaged archive in one piece is no seed; an archive in two
  pieces is reconstructed.
- `tests/support/zip_samples.cpp`: archives that Python's `zipfile` wrote (DEFLATE with Huffman codes, stored
  members, empty members and folders, data descriptors from a stream that cannot seek, ZIP64 extra fields, a
  comment, LZMA and bzip2 members) and reworked ones (ZIP64 end records, an APK signing block, a DOCX's first
  members), generated by `tests/reference/make_zip_samples.py`. `tests/support/zip_builders.cpp` builds
  archives of stored members.
- The corpus scenario `zip-archives`: named, deleted and unnamed archives on a FAT32 stick, images stored in
  intact and damaged archives.
