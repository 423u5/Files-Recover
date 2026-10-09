# File carving framework (P8)

Carving finds files by their content, without filesystem metadata: a signature says where a file may begin, and
the file's format decides whether it really is one, where it ends, and whether its bytes are consistent. P8 builds
the format-independent part, which the tests exercise with formats that exist only in them. The first real formats
are the images of P9 (JPEG, PNG, WEBP, GIF, BMP) in `recovery_formats`, added without a change here; see
[../formats/images.md](../formats/images.md). The audio of P10 (MP3, WAV, M4A, raw AAC) needed one addition, the
self-synchronizing flag below; see [../formats/audio.md](../formats/audio.md). The MP4 video format of P12 needed
none ([../formats/mp4.md](../formats/mp4.md)); MP4 recovery ([mp4_recovery.md](mp4_recovery.md)) drives the carver
hit by hit and joins its candidates with filesystem candidates, which gives carved MP4 files a `RecoveryCandidate`
that `RecoveryWriter` can write (L48 for MP4).

Library `recovery_carving` (namespace `recovery::carving`, headers in `include/carving/`). It depends only on
`recovery_storage`: carving reads any `IStorageSource` (disk, image, partition) and knows nothing about
filesystems.

| Header | Contents |
| --- | --- |
| `file_signature.hpp` | `FileSignature` (pattern, mask, offset), `validateSignature`, `byteSignature`, `textSignature` |
| `content_reader.hpp` | `IContentReader`, `MemoryContentReader`, `SourceContentReader`, `SourceReadOptions`, `findPattern` |
| `format_validator.hpp` | `FormatValidator`, `ValidationResult`, `ValidationStatus` |
| `file_format.hpp` | `IFileFormat`, `FormatDescriptor`, `HeaderCheck`, `EndDetection`, `EndStatus`, `EndDetectionMethod`, `ExtractionStrategy`, `validateDescriptor` |
| `format_registry.hpp` | `FormatRegistry` |
| `signature_scanner.hpp` | `SignatureScanner`, `ScanOptions`, `SignatureHit`, `ScanReport`, `ScanProgress` |
| `file_candidate.hpp` | `FileCandidate`, `CarvedExtent`, `SignatureEvidence`, `EndEvidence`, `CarveWarning`, `validateFileCandidate` |
| `file_carver.hpp` | `FileCarver`, `CarveOptions`, `CarveOutcome`, `CarveRejection`, `RejectionReason`, `CarveReport` |

## Flow

```
FormatRegistry ──snapshot──► SignatureScanner ──hits──► FileCarver ──candidates──► sink
                              (one sequential pass)      per hit:
                                                         1. signature and header check
                                                         2. end detection
                                                         3. extraction (contiguous)
                                                         4. structural validation
```

A carve is never "find a header, copy until the next header". Each format decides from its own structure where
its files end and whether their bytes are consistent, and a candidate records what it was found with.

## Formats

A format implements `IFileFormat`:

| Member | What it supplies |
| --- | --- |
| `descriptor()` | id, name, extension, signatures, minimum size, maximum reasonable size, header size, end detection method, extraction strategy, whether the format is self-synchronizing |
| `checkHeader(header)` | Header detection: whether a file plausibly starts here. Gets the first `headerSize` bytes. Called for every hit, so it should be cheap |
| `findEnd(content)` | End detection: where the file ends (`EndDetection`) |
| `validator()` | Structural validation (`FormatValidator`) |

`validateDescriptor` enforces the descriptor's rules. The registry refuses a format that breaks one:

| Field | Rule |
| --- | --- |
| `id` | 1-32 characters from `a-z 0-9 - _`, unique in the registry |
| `extension` | 1-16 characters from `a-z 0-9`, no dot |
| `signatures` | 1-16, each valid (see below) |
| `minimumSize` | At least the reach (offset + length) of every signature |
| `maximumSize` | Between the minimum size and 1 TiB |
| `headerSize` | Between the longest signature reach and 64 KiB |

A **signature** is a pattern of 2-64 bytes at an offset of at most 4096 bytes from the file's start (4 for an ISO
BMFF `ftyp` type, 8 for `WEBP`). An optional mask selects the bits compared (MPEG frame sync: `FF E0` under `FF
E0`); its first byte must be `FF`, because the scanner indexes signatures by their first byte.

**End detection** reports one of:

| `EndStatus` | Meaning | `length` | Candidate warning |
| --- | --- | --- | --- |
| `Found` | The structure says where the file ends | The file's length | |
| `Truncated` | The data available ends before the structure does | The bytes available that belong to the file | `TruncatedBySourceEnd` or `TruncatedByMaximumSize` |
| `Broken` | The structure breaks at `length` (overwritten, fragmented, or not this format after all) | The consistent bytes before the break | `StructureBroken` |
| `Unknown` | The format cannot tell | The format's estimate | `EndUnknown` |

`EndDetectionMethod` (`SizeField`, `StructureWalk`, `EndMarker`, `None`) states how a format finds the end and is
recorded as evidence. `ExtractionStrategy` states how the bytes are taken once the end is known. The only strategy
is `Contiguous`, one piece from the start to the end. Fragmented files are reconstructed by a subsystem of their own
(P13, [fragment_recovery.md](fragment_recovery.md)), which takes carves that break as seeds.

**Validation** (`FormatValidator::validate(content)`) checks the carved bytes and gives `Valid`, `Truncated`
(consistent but cut short) or `Invalid`, the number of consistent bytes from the start, and a detail. Validators see
only an `IContentReader`, not the source, so the same validator can later check files found through filesystem
metadata (P14). A candidate is never considered recovered because of its header alone; that is what the
validation status is for.

Structural problems are results, never errors. Formats return errors only when their reader fails (cancellation,
a source failure).

### Adding a format

1. Implement `IFileFormat` (and `FormatValidator`, often in the same class).
2. Add the format to a `FormatRegistry` with `add(std::make_shared<MyFormat>())`.

Nothing in the scanner or the carver changes. `FileCarverTest.ANewFormatNeedsNoChangeToTheScannerOrTheCarver`
defines a format inside the test file and carves it next to the other test formats.

Formats are added explicitly. Each format module provides a function that adds its formats to a registry
(`formats::registerImageFormats` for the images of P9, `formats::registerAudioFormats` for the audio of P10,
`formats::registerVideoFormats` for MP4 in P12). There is deliberately no registration through static
initialisers: the engine is built
from static libraries, and the linker drops object files that nothing references, so such formats would silently
go missing.

Formats are immutable once registered and every member is `const`, so a format can be used from several threads
(P15).

## Reading content (`IContentReader`)

Formats read the would-be file only through `IContentReader::read(offset, length)`. It returns a view of exactly
`length` bytes (at most 1 MiB) at a file offset, and refuses any range outside `[0, size())`. `findPattern`
searches a range in 256 KiB chunks (overlapping by the pattern's length), so a marker search needs bounded memory
whatever the range.

`SourceContentReader` is the implementation over a source window `[start, start + size)`:

- The window is the smaller of the format's maximum size and the rest of the source. A format can read neither
  beyond its maximum size nor beyond the source, whatever its structures claim.
- Reads are served from a cache (256 KiB by default). The first load is only 4 KiB, because most carves end at the
  header check.
- A read that fails with an I/O error is repeated sector by sector (`sectorRetryCount`). Sectors that still fail,
  and bytes inside `knownBadRegions`, are delivered as zeros and recorded (`unreadable()`), as in imaging and
  reconstruction.
- `sourceFailure()` remembers a read that failed for another reason (source closed, device gone), so the carver can
  tell a source failure from an error of the format.

`MemoryContentReader` serves data held in memory (for testing a format's parser on byte vectors).

## Scanning (`SignatureScanner`)

`SignatureScanner::create(registry)` takes a snapshot of the registry's formats (sharing their ownership) and
indexes their signatures by offset and first byte. `scan(source, sink, options)` then makes one pass:

- **Range.** File starts in `[startOffset, endOffset)` are examined (default: the whole source). A pattern may
  extend beyond `endOffset`, up to the end of the source, so scans of adjacent ranges together report every hit
  exactly once (`SignatureScannerTest.AdjacentRangesTogetherFindEveryHitOnce`).
- **Streaming.** Reads are sequential blocks of `blockSize` (1 MiB; a multiple of the sector size), aligned to the
  sector. Before each read, only the bytes still needed (less than the longest signature reach) move to the front of
  the buffer. Memory is one block plus the reach, whatever the size of the source. Hits go to the sink as soon as
  their block has been read.
- **Order.** Hits come in increasing file offset. At one offset there is one hit per format (its first matching
  signature), in registration order.
- **Alignment.** `alignment` (a power of two up to 1 MiB) restricts file starts to multiples of it, for example 512
  for sector starts. Data that a large alignment makes irrelevant is not read.
- **Bad sectors.** A failed block is re-read sector by sector. Sectors that still fail (and known bad regions) are
  recorded in the report and never matched, even by a signature of zeros. Read failures other than I/O errors stop
  the scan with that error.
- **Stopping.** The report's `outcome` is `Completed`, `Cancelled` (the token, or a sink returning `Cancelled`) or
  `HitLimitReached` (`maxHits`, 10 million by default). `nextOffset` says where a later scan of the rest continues:
  every file start before it has been examined and its hits delivered. When the sink cancelled, hits at
  `nextOffset` itself may have been delivered already.
- **Progress** (`onProgress`, throttled by `progressInterval`) reports the position, bytes read, hits and
  unreadable bytes. Exceptions from the callback are swallowed.

The scan is only read. Nothing is written anywhere.

## Carving (`FileCarver`)

`carve(hit)` carves one hit, from a scanner or made by hand:

1. `TooLittleData` if fewer bytes than the format's minimum size (or the signature) are left before the end of the
   source.
2. The signature is checked again at the hit, then `checkHeader` runs (`HeaderRejected`).
3. `findEnd` runs on a `SourceContentReader` over the data window. A length below the minimum size is rejected
   (`TooSmall`, with the format's end detail in the rejection), and one beyond the window is a defect of the format
   (`InvalidEnd`). A format that finds after its header check that the file is not one of its own (an MP4 video
   for M4A, an ID3v2 tag in front of AAC frames for MP3) reports `Broken` at 0 bytes, which ends here.
4. Extraction: one `CarvedExtent` from the file's start (`Contiguous`).
5. `validate` runs on exactly the carved bytes (unless `validate` is off).
6. Unreadable ranges inside the file become `unreadableRegions` and the warning `UnreadableData`. Ranges the reader
   met beyond the file's end (its cache reads ahead) are not reported.

A rejection is an outcome, not an error. A format that fails with an error of its own is rejected too
(`FormatFailed`), so one broken format module does not stop a deep scan. Cancellation and source failures are
errors and stop the carve.

`run(scanner, sink)` scans with `options.scan` and carves every hit as it arrives. Each candidate goes to the sink
at once, so memory does not grow with the number of candidates. Hits strictly inside a candidate whose end was
`Found` and that validated as `Valid` are skipped (`skipHitsInsideValidCandidates`): an embedded thumbnail or
resource is part of that file. Hits inside any other candidate (truncated, broken, invalid, unknown end, not
validated) are carved, and so are hits of other formats at the same start. `options.scan.reads` (cancellation,
known bad regions, retries) and `options.scan.logger` apply to the carver's own reads and log records too.

**Self-synchronizing formats** (`FormatDescriptor::selfSynchronizing`, added in P10) are streams of frames that
each start with one of the format's signatures: MP3 and ADTS. Every frame of such a file is a hit, and a stream is
a file of its own from any frame on, so under the rule above a stream that is cut off or damaged (not `Valid`)
would be carved again from every one of its frames: thousands of overlapping candidates, with reads growing with
the square of the stream's length. For these formats `run` also skips the format's own hits strictly inside an
earlier candidate of the **same** format, whatever that candidate's end and verdict: they are its frames. Hits of
other formats inside it are carved as before, and so are the format's hits after the candidate's end (where the
rest of a broken stream may start). The carver keeps, per self-synchronizing format, the candidate reaching
furthest. `skipHitsInsideValidCandidates = false` turns both rules off. The flag is opt-in, so formats without it
(the images, WAV, M4A) behave exactly as before.

`CarveReport` holds the scan report, candidates per validation status, rejected hits per reason, skipped hits and
the bytes the carves read. In a completed run every hit is accounted for: `hits = candidates + rejected + skipped`.

The two skip rules are a value of their own since P15: `CarveSkipState` (`skips(hit)`, `record(hit, candidate)`).
`run()` keeps one for its run. A scan (`ScanCoordinator`, [scanning.md](scanning.md)) keeps one across its source
pass and in its checkpoints, carving each hit with `carve()` and committing them in source order, so that it skips
exactly what `run()` skips, also when it resumes.

## Candidates (`FileCandidate`)

| Field | Meaning |
| --- | --- |
| `id` | From `CarveOptions::firstId`, in carve order; rejected hits use no id |
| `formatId`, `extension` | From the format's descriptor |
| `sourceOffset`, `length` | Where the file starts on the source, and how long the carve is |
| `extraction`, `extents` | `Contiguous`: one extent `{fileOffset 0, sourceOffset, length}` |
| `signature` | Which signature matched, and the source offset of the pattern |
| `end` | End detection method and status, the format's detail, and the bytes available to it |
| `validation` | Status, consistent bytes, detail |
| `unreadableRegions` | Source ranges inside the file that could not be read (zeros) |
| `warnings` | `TruncatedBySourceEnd`, `TruncatedByMaximumSize`, `StructureBroken`, `EndUnknown`, `UnreadableData` |

There is no numeric confidence score, as for filesystem candidates. `validateFileCandidate` checks the structural
invariants (extents contiguous from 0 to the length, no overflow, one extent for contiguous extraction).
`FileCandidate` is carving's own result. P14 unifies it with `RecoveryCandidate`, and until then carved candidates
cannot be written by `RecoveryWriter` (L48), except MP4's: MP4 recovery (P12) turns each carve into an
`Mp4Candidate` whose data is a `RecoveryCandidate`.

## Bounds and hostile data

- Every read is inside the source and inside the format's window; offsets are checked before any addition that
  could overflow. Tests cover offsets beyond 4 GiB and a 1 TiB image.
- A format's own lengths never drive a read beyond its window, and a length beyond it is rejected.
- A flood of false signatures costs one 4 KiB read per hit (`CarvingStreamingTest.AFloodOfFalseSignaturesCostsBoundedWork`),
  and `maxHits` bounds the number of hits. A hit that passes its header check can still cost up to its format's
  maximum size in reads (L50).

## Logging

Component `carving`: scan start and end (range, block size, alignment, bytes, hits, unreadable bytes, elapsed
time), unreadable ranges (the first 100), carving start and end with its counts, each candidate (Debug), each
rejected hit (Debug), and each candidate that failed validation (Info). Offsets, lengths and ids only: never file
content.

## Thread safety

| Object | Rule |
| --- | --- |
| `FormatRegistry` | `add` must not overlap anything else; the rest is const |
| Formats, validators | Immutable; every member is const and safe to call concurrently |
| `SignatureScanner` | `scan` is const: concurrent scans (different ranges and sinks) are safe on sources that allow concurrent reads |
| `FileCarver`, `SourceContentReader` | Not thread-safe; one owner each |

## Tests

Label `carving`, executable `recovery_carving_tests` (see [../testing/testing.md](../testing/testing.md)):

| File | Covers |
| --- | --- |
| `tests/carving/file_signature_test.cpp` | Matching, masks, reach, signature limits |
| `tests/carving/format_registry_test.cpp` | Registration order and lookup, every descriptor rule, limits, duplicates, scanner snapshots |
| `tests/carving/content_reader_test.cpp` | Read bounds, the cache, bad, transient and known-bad sectors, fatal errors, cancellation, `findPattern` across chunk boundaries |
| `tests/carving/signature_scanner_test.cpp` | Planted signatures in order, offsets and masks, overlapping matches, one hit per format and position, every position relative to a block boundary, alignment, ranges and adjacent ranges, source edges, invalid options, bad and known-bad sectors, fatal and sink errors, cancellation and continuing from `nextOffset`, the hit limit, progress, logging, sequential bounded reads, a disk enforcing 4 KiB alignment |
| `tests/carving/file_carver_test.cpp` | Candidates of every test format compared with the originals, streaming delivery, every rejection reason, every end status, validation failures, skipping inside valid candidates, and inside any candidate of the same self-synchronizing format, unreadable and known-bad sectors, fatal errors, cancellation during a long carve, sink errors, ids, logging, a format defined in the test, disk image files, a disk enforcing alignment, `validateFileCandidate` |
| `tests/carving/carving_streaming_test.cpp` | The tail of a 1 TiB image, a full 128 MiB scan read exactly once, a carving run over 200+ files, offsets beyond 4 GiB, a flood of false signatures; heap growth measured in each |

The test formats (`tests/support/carving_formats.hpp`) cover each end detection method: `SizedFormat` (size field
and CRC), `MarkerFormat` (end marker), `BoxFormat` (structure walk, signature at offset 4), `SyncFormat` (masked
signature, no end information) and `ScriptedFormat` (behaviour supplied by the test, for broken and hostile
format modules). `tests/support/virtual_source.hpp` is a source of any size whose content is computed, with byte
strings planted at chosen offsets; it records the number, size and order of reads.

## Known limitations (P8)

All known limitations, with IDs for discussion, are listed in [../limitations.md](../limitations.md) (L46-L58).

- Only contiguous extraction: a fragmented file is carved up to its first break (P13 reconstructs such carves when
  they start in a volume's free space).
- The whole range is carved, allocated or not, so files the filesystem already knows are found again (P14, P17;
  for MP4, P12 attaches such carves to the filesystem candidate).
- Carved candidates are written as evaluated candidates since P14, named `recovered_<id>.<ext>` (MP4's since P12;
  L48 resolved); P18's `recovery recover` writes them.
- A carved file's bytes are read at least twice: by the carve, and again by the scan.
- The image formats of P9 met the interface without changing it (L55). The audio formats of P10 needed the
  self-synchronizing rule (L51); the MP4 format of P12 needed nothing.
