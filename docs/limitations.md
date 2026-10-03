# Known limitations

Every known limitation of the engine and of its testing, in one list. Items are added when they are
found and are kept here for discussion. The architecture documents explain the design around them.

Each item has a fixed ID so it can be referred to in discussion. **Status** is one of:

- **Open**: not decided yet.
- **Planned (Pn)**: expected to be solved by a later phase of the plan.
- **Accepted**: decided to keep as is (record why).
- **Resolved**: fixed (record where).

## Storage and imaging (P1–P2)

### L1. Imaging computes no hash
**Status:** Open  
No image hash (for example SHA-256) is computed while imaging. Integrity is only checked by the tests,
which compare bytes. A user has no way to prove that an image matches the source.

### L2. Imaging is single-threaded
**Status:** Open  
Imaging uses one thread, and progress is reported at most every `progressInterval`.

### L3. The metadata replace is only atomic on NTFS and ReFS
**Status:** Open  
Image metadata is replaced with `MoveFileExW`. On FAT or exFAT destinations the replace is not atomic,
so a crash at the wrong moment can leave damaged metadata.

### L4. Only raw images
**Status:** Open  
There is no E01 or other forensic container format.

### L5. Some destinations are refused for physical-disk sources
**Status:** Accepted (safety)  
Network shares and SUBST drives can't be mapped to a physical disk, so the engine can't prove they are
not on the source disk. It refuses to write to them when the source is a physical disk.

## FAT32 (P4)

### L6. Deleted fragmented files are only a contiguous guess
**Status:** Resolved (P13) as far as the evidence allows; see L104-L111  
Windows frees the cluster chain on delete, so only the start cluster and size remain. The allocation is
reported as `ContiguousGuess`, which is wrong for fragmented files. Since P7, such recovery candidates are
labelled `LayoutGuessed`, and guessed clusters that now belong to other files are marked reallocated.
P13: fragment reconstruction ([recovery/fragment_recovery.md](recovery/fragment_recovery.md)) takes every such file
as a seed and tries other layouts (skipping clusters in use or claimed by other files, continuations where the
structure breaks, MP4 samples), each validated by its format; the result is COMPLETE, PARTIAL, CORRUPTED, AMBIGUOUS
or UNRECOVERABLE. P7 still reports the guess; the filesystem module is unchanged.

### L7. Cleared high 16 bits of a deleted entry's start cluster
**Status:** Open  
Some Windows versions clear the high 16 bits of the start cluster when deleting. On volumes with more
than 65,535 clusters, such entries point to the wrong cluster. This is not detected or corrected.

### L8. Only the first cluster of a deleted directory is read
**Status:** Open  
Entries in later clusters of a deleted directory are not found.

### L9. Orphaned clusters are counted, not listed
**Status:** Open  
`ClusterUsage::allocated - FileScan::referencedClusters` gives the number of orphaned clusters (allocated
but referenced by no entry). The orphaned chains themselves are not listed. The same applies to exFAT.

### L10. Paths are built from raw names
**Status:** Open (recovery output is safe since P7; the FAT32 part remains)  
FAT32 does not check long names for characters that FAT forbids. A damaged long name containing `/`
therefore gives an ambiguous `FileRecord::path`. exFAT and NTFS flag such names (`InvalidName`) but also
keep them unchanged in the path (NTFS: `NtfsRecordCorruptionTest.InvalidNames`). P4 code was not changed.
P7: recovery output passes every name through `safeFileName`, and the writer removes a candidate's file name
from its path as a whole, so such names cannot leave the destination
(`RecoveryWriterTest.UnsafeNamesStayInsideTheDestination`). A directory name containing `/` still adds a level
to the recovered tree, and FAT32 still does not flag the name.

## exFAT (P5)

### L11. Deleted fragmented files whose chain was cleared are only a contiguous guess
**Status:** Resolved (P13) as far as the evidence allows, as L6  
A deleted FAT-chained file is recovered through its old chain only if the chain is still complete.
Otherwise it is a `ContiguousGuess`, as on FAT32 (L6), and its recovery candidate is labelled `LayoutGuessed`.
P13 reconstructs such files like deleted FAT32 files
(`FragmentRecoveryTest.ExFatFilesWhoseChainWasClearedAreReconstructed`).

### L12. Is the chain of a deleted file kept or cleared?
**Status:** Open (needs a Windows-written image)  
Deletion only has to free clusters in the allocation bitmap. It has not been verified whether Windows
also clears the FAT chain of deleted fragmented files. The engine handles both cases, but how often
L11 applies in practice depends on this.

### L13. Orphaned entries lose the directory flag
**Status:** Open  
When a deleted File entry was overwritten, the surviving Stream Extension and File Name entries are
listed (`MetadataIncomplete`), but without attributes or timestamps. A deleted directory found this way
is reported as a file and not searched.

### L14. Vendor allocation entries are skipped
**Status:** Open  
Benign secondary entries that own clusters are skipped, so their clusters count as orphaned.

### L15. TexFAT is barely supported
**Status:** Open  
With two FATs, only the active FAT and allocation bitmap are chosen. Transactions are not examined, and
`ClusterUsage::mirrorMismatches` is always 0 because the second FAT is not a mirror.

### L16. Volume GUID and OEM parameters are not reported
**Status:** Open

### L17. Name hashes need the volume's up-case table
**Status:** Accepted  
If the up-case table is missing, unreadable or fails its checksum, a built-in table covering ASCII and
Latin-1 is used. Names with other letters then can't be verified (`LongNameUnverified`).

### L18. Hostile boot sectors still cost some time
**Status:** Open  
A boot sector can claim about 2^32 clusters on a tiny image. Every operation stays bounded, and nothing
beyond the end of the image is read. Cluster analysis still steps through every FAT page (about
262,000), which takes about 2 seconds in a debug build.

## Cross-checking and test data

### L19. No exFAT files written by another implementation
**Status:** Open  
The exFAT parser has only been checked against empty volumes made by `mkfs.exfat` (exfatprogs), and
`fsck.exfat` has checked the test builder's images. No image with files written by Windows or Linux has
been read. `mkfs.exfat` can only format, and writing files needs root (a FUSE or kernel mount), which was
not available. The builder's up-case table covers ASCII, Latin-1, Greek and Cyrillic, not Windows' full
table.

### L20. fsck.exfat rejects vendor extension entries
**Status:** Accepted  
exfatprogs 1.2.2 reports vendor extension entries, which the specification allows, as missing name
entries. The images exported for `fsck.exfat` therefore leave them out. The engine's own tests cover
them.

### L21. No Windows-written reference images
**Status:** Open  
The reference tests (`Fat32ReferenceImageTest`, `ExFatReferenceImageTest`) only run when pointed at a
directory of images. No image formatted and populated by Windows is in the reference set yet. The FAT32
cross-check with pyfatfs cannot cover non-ASCII names or deleted files (see
[testing/testing.md](testing/testing.md)).

### L22. exFAT tests borrow helpers from the FAT32 builder
**Status:** Open (tidy-up)  
`readExtents` and `utf8ToUtf16` live in `tests/support/fat32_builder.hpp`, and the exFAT tests include
that header for them. Moving them to a shared helper would change P4 test support, so it was not done.
The NTFS tests and builder (P6) include it too.

## NTFS (P6)

### L23. Attribute lists are not followed
**Status:** Open  
A record whose attributes do not fit in one MFT record keeps some of them in extension records, listed
in its `$ATTRIBUTE_LIST`. These are not read. A file whose data runs continue there gets only the runs
in its base record (`AttributeListNotFollowed` with `ChainShorterThanSize`); a file whose `$DATA` is
entirely there has no allocation. A `$MFT` with an attribute list (a large, very fragmented MFT) is read
only as far as record 0 describes it, with a warning. This affects heavily fragmented files and files
with many names.

### L24. Sparse, compressed and encrypted data are only flagged
**Status:** Open (sparse files are recovered since P7)  
Holes in sparse files are skipped, so the extents cannot simply be concatenated (`SparseRuns`).
Compressed data (LZNT1) is not decompressed (`CompressedData`), and EFS-encrypted data is ciphertext
(`EncryptedData`). The run lists themselves are complete. P7: `NtfsRecovery` lays sparse files out from their
run list, with the holes as zeros; compressed and encrypted data remains (L36).

### L25. Named data streams are ignored
**Status:** Open  
Only the unnamed `$DATA` stream is reported as a file's content. Alternate data streams (for example
`Zone.Identifier`) are not listed; their clusters still count as referenced.

### L26. Directory indexes are not read
**Status:** Open  
The tree is rebuilt from `$FILE_NAME` parent references. Directory indexes are not used or checked
against the records, and entries of deleted files left in index slack (useful when the file's record was
reused) are not recovered.

### L27. $LogFile and $UsnJrnl are not read
**Status:** Open  
The journals could tell more about recent deletions and renames. "Complex journaling recovery" is
outside P6.

### L28. Only record 0 falls back to $MFTMirr
**Status:** Open  
If record 0 is damaged, its copy in `$MFTMirr` locates the MFT. Damaged records 1-3 (`$MFTMirr`,
`$LogFile`, `$Volume`) are not taken from the mirror, and the mirror is not compared with the MFT.

### L29. The whole MFT is read before the first listing, and kept in memory
**Status:** Open  
`readDirectory` and `scan` first build a catalog of every named record. Its memory grows with the number
of records, bounded by `NtfsOptions::maxRecords` (16,777,216 by default; later records are ignored with a
warning). The data of each resident file (at most one MFT record) is copied into the catalog and into
the scan results.

### L30. Orphans are listed under a virtual "/$OrphanFiles"
**Status:** Open  
Entries whose parent directory is gone get the path `/$OrphanFiles/<name>`. A real root entry with that
name would share the prefix; the two are told apart by `ParentMissing`. Recovery output must not rely on
the path alone (see also L10). P7: their candidates carry the warning `LocationUnknown`, and the recovery
writer recreates `$OrphanFiles` as an ordinary directory in the output.

### L31. Some NTFS metadata is not reported
**Status:** Open  
Times come from `$STANDARD_INFORMATION` only. The copies in `$FILE_NAME` (useful to spot altered times),
the MFT-change time, reparse points (symbolic links, junctions) and security descriptors are not
reported. Reparse points are listed as ordinary files and directories.

### L32. NTFS borrows exFAT's page cache
**Status:** Open (tidy-up)  
The MFT and `$Bitmap` are read through `exfat::PagedRegion` (`src/filesystem/exfat/paged_region.hpp`).
Moving it to a shared private helper would change P5 code, so it was not done.

### L33. The NTFS builder writes empty directory indexes
**Status:** Open (testing)  
`tests/support/ntfs_builder` writes empty `$I30` indexes, except that the root lists `$Secure`, which
ntfs-3g needs to mount a volume. Its images cannot be listed by path with ntfs-3g or Windows, so the
cross-check reads them by record number (`ntfscat`, `ntfsundelete`). `$AttrDef` holds no definitions.
The P6 tests do not need more, but a phase that reads indexes will.

### L34. No Windows-written NTFS images
**Status:** Open  
The NTFS parser has been cross-checked against ntfs-3g only: mkntfs formats, and the ntfs-3g FUSE driver
writes and deletes the files (see [testing/testing.md](testing/testing.md)). No image written by Windows
has been read; its allocator, short names and attribute layout may differ. As for FAT32 and exFAT (L21),
the reference images are generated locally and not committed.

## Filesystem-based recovery (P7)

### L35. "Reallocated" is evidence, not proof
**Status:** Open  
A cluster of a deleted file that is allocated again may still hold the old data (allocated but not yet
written). A cluster that is free may have been overwritten by a file that was deleted in turn. Recovery reads
both kinds as they are. Nothing in P7 checks that the recovered content is the original; format validation
comes with P9-P14.

### L36. Compressed and encrypted NTFS data is not recovered
**Status:** Open  
Such candidates have every byte `Missing` (`DataNotDecoded`), and the writer refuses them. Compressed files need
LZNT1 decompression; EFS files cannot be decrypted without the user's keys (see L24).

### L37. Recovered files do not get their timestamps or attributes back
**Status:** Open  
The candidates carry the original times and attributes, but the written files get the time of recovery. FAT
times are local time with no zone (`Timestamp::local`), so restoring them needs a decision about the time zone.

### L38. Directories whose names differ only in case share one output directory
**Status:** Open  
Windows destinations are case-insensitive, and `safeFileName` can map different names to the same one (`a:b`
and `a_b`). Such original directories are merged in the output. Their files never overwrite each other (they
get numbered names), but the tree no longer shows that they were separate.

### L39. Hard links are recovered once per name
**Status:** Open  
Each name of an NTFS record is its own candidate with the same regions, so the data is written once per name.
Content-based duplicate detection is planned for P17.

### L40. Candidates are kept in memory
**Status:** Open  
`CandidateScan` holds every candidate of a volume, each with its regions and a copy of any resident data, on top
of the filesystem's own catalog (L29). Memory grows with the number of files, bounded by `ScanLimits::maxEntries`.

### L41. Reading around bad sectors is slow
**Status:** Open  
After a failed chunk, reconstruction reads the chunk sector by sector with retries. There is no intermediate
block size as in imaging, so a region with many bad sectors takes long. Imaging the source first is preferable.

### L42. The recovered file does not record that it is incomplete
**Status:** Open  
Missing data inside a file is zero-filled and missing data at its end is left out, so the file's size can
differ from the original. The `ReconstructionReport` says so, but it is only returned to the caller; nothing next
to the file records it. Sessions (P16) and the CLI report (P18) are expected to persist it.

### L43. Sizes larger than the volume are cut to the volume
**Status:** Accepted (safety)  
A size larger than the volume is treated as damage: only the first cluster-area bytes of the file are kept, and
sparse holes become `Missing`. This bounds every reconstruction, but a genuine sparse NTFS file larger than its
volume (a sparse virtual disk, for example) loses its tail.

### L44. Returned paths may exceed MAX_PATH
**Status:** Open  
The writer creates files through `\?\` paths, so deep trees work, but `RecoveredFile::path` is an ordinary path.
Opening one longer than 260 characters needs the `\?\` prefix or Windows long-path support.

### L45. FAT12 and FAT16 are not supported
**Status:** Open (found during P7; the plan only names FAT32)  
The FAT32 module rejects FAT12 and FAT16 volumes (`UnsupportedFilesystem`), and so does `openFilesystemRecovery`.
SD cards up to 2 GB and many older USB sticks are formatted FAT16, so their files can only be carved.

## Carving framework (P8)

### L46. Carving extracts contiguous files only
**Status:** Partly resolved (P13): carves that break in a volume's free space are reconstructed by fragment
reconstruction (a subsystem of its own, not an extraction strategy: the carver still extracts one piece); see L110  
The only extraction strategy is `Contiguous`: the bytes from a file's start to its detected end, in one piece. A
fragmented file is carved up to the point where its structure breaks (`StructureBroken`, or `Invalid` validation),
and the rest of it is not searched for.

### L47. Allocated space is carved too
**Status:** Resolved for every format (P14); the scan still covers allocated space (L97, L114)  
A scan covers the whole range, whether a filesystem allocates it or not. Files that filesystem-based recovery
already found (active and deleted) are carved again as separate candidates. Unifying the two kinds of evidence is
P14's job, and content-based duplicate detection P17's. MP4 recovery (P12,
[recovery/mp4_recovery.md](recovery/mp4_recovery.md)) attaches the carve of a file that a filesystem candidate names
(same first byte) to that candidate instead of listing it again, and gives every other carve its place in the
volume's allocation (free or allocated clusters, inside an active file). The scan itself still covers allocated
space (L97).

P14's candidate evaluation ([recovery/candidate_evaluation.md](recovery/candidate_evaluation.md)) does the same for
every format: a carve that starts where a filesystem candidate starts joins it (its layout stands, or the carve gives
it: HYBRID), and every other carve gets its place in the allocation.

### L48. Carved candidates cannot be written yet
**Status:** Resolved (P14)  
`FileCandidate` is carving's own type, not a `RecoveryCandidate`, so `RecoveryWriter` cannot write it, and carved
files do not get the plan's `recovered_000001.<ext>` names yet. Their extents are exact, so writing them needs no new
reading code, only the unified candidate model. P12's `Mp4Candidate` holds a `RecoveryCandidate` (method `Carving`
or `Hybrid`, one stored run from the carve), which `RecoveryWriter` writes as it is; carved MP4 files are named
`recovered_000001.mp4` (`.mov`, `.m4v`, `.3gp`, `.3g2` after the brand).

P14: every carve becomes an `EvaluatedCandidate` whose `data` is a `RecoveryCandidate` (method `Carving`, the carve's
extents as stored runs), which `RecoveryWriter` writes as it is, named `recovered_<id>.<ext>` after the evaluation's
id.

### L49. A carved file's bytes are read at least twice
**Status:** Open (P15)  
End detection and validation read a file through its own reader, and the scanner later reads the same bytes again as
part of its pass. The carve's reads also jump ahead of the scan, so a carving run is not strictly sequential. This is
cheap on flash media but costs seeks on hard disks.

### L50. Hostile images can still cost much work
**Status:** Open (P20)  
Every hit that passes its format's header check may read up to the format's maximum size. An image crafted with
many such headers therefore costs about hits × maximum size in reads. Memory stays bounded, a rejected header costs
one 4 KiB read, and `ScanOptions::maxHits` (10 million by default) bounds the number of hits, but there is no budget
per scan in bytes or time. The audio formats' short frame-sync signatures make rejected hits much more frequent
(L72).

### L51. Only hits strictly inside a validated candidate are skipped
**Status:** Open for overlapping candidates; competing carves at one start resolved (P14)  
Hits inside a candidate whose end was not `Found` or that did not validate as `Valid` are carved, which gives
overlapping candidates (for example a truncated file and a smaller file inside it). Hits of different formats at the
same offset are all carved. Deciding between overlapping or competing candidates is left to P13/P14.
P10 added one exception, decided with the user: a self-synchronizing format (MP3, ADTS) skips its own hits inside
any earlier candidate of the same format, whatever its verdict, because every frame of a stream is a hit
(`FormatDescriptor::selfSynchronizing`). Hits of other formats inside such a candidate are still carved.

P14: carves that start at the same byte are one file: the best (Valid, Truncated, not validated, Invalid; then the
longest) speaks for it, the others are kept as its evidence (`otherCarves`). Overlapping carves that start at
different bytes are still separate candidates (L126).

### L52. The sector-by-sector fallback exists twice
**Status:** Open (tidy-up)  
`src/carving/source_reads.cpp` repeats the read fallback of `src/recovery/candidate_reader.cpp` (P7): a failed read
is retried sector by sector, and failing sectors become zeros. Neither has the intermediate block size of imaging
(L41). Moving it to a shared helper would change P7 code, so it was not done.

### L53. Signature constraints
**Status:** Accepted (design)  
A signature is 2 to 64 bytes at most 4096 bytes into the file, and its first byte is compared in full (the scanner
indexes signatures by it). A format whose files start with variable bytes anchors its signature at a later, fixed
position instead.

### L54. Scanning and carving are single-threaded
**Status:** Planned (P15)  
One scan runs on one thread. Scans of different ranges can run concurrently (adjacent ranges report every hit
once), but nothing splits a scan into ranges yet, and `FileCarver` is not thread-safe.

### L55. No real format has used the framework yet
**Status:** Resolved (images P9, audio P10, MP4 P12)  
The framework was only exercised with the test formats. P9 added JPEG, PNG, WEBP, GIF and BMP without changing the
scanner or the carver; the one thing they needed that the core lacked, big-endian field readers, was added to
`include/recovery/byte_order.hpp`. The 1 MiB limit on a content read was never in the way (the image formats read
small structures and stream the rest). MP4 files whose `moov` and `mdat` are far apart may still need a
non-contiguous extraction strategy (P11/P12). The audio formats of P10 needed 64-bit big-endian readers (ISO BMFF
sizes, `co64`) and one change to the carver: the self-synchronizing rule (L51). The MP4 format of P12 needed no
change to the framework either: a carved MP4 is one contiguous run, and one whose moov and mdat are far apart on
the disk is only found whole through its filesystem candidate. MP4 recovery drives the carver hit by hit
(`FileCarver::carve` without validation, then its own analysis), which the framework already allowed.

### L56. Carving progress reports only the scan
**Status:** Open  
`FileCarver::run` passes `ScanOptions::onProgress` through to the scanner, so progress shows the scan position, bytes
read and hits, but not the candidates carved or rejected so far. A long carve of one file (up to its maximum size)
reports no progress while it runs.

### L57. Format details are trusted not to contain content
**Status:** Open  
Candidates and log records carry the `detail` and `reason` strings that format modules return. The rule that they
never contain file content is a convention for format modules, not something the carver can check.

### L58. Bounded-memory checks do not run under AddressSanitizer
**Status:** Accepted (testing)  
The carving tests measure heap growth by replacing the global `operator new` of their executable
(`tests/carving/heap_tracking.cpp`). AddressSanitizer brings its own allocator, so the ASan build runs the same tests
without the memory assertions.

## Image formats (P9)

### L59. Validation is structural: no image is decoded
**Status:** Resolved (P14): the media level decodes Huffman, deflate, LZW, RLE and VP8L data, and VP8 headers
([formats/media_validation.md](formats/media_validation.md))  
The image formats check what their own structures say about themselves (lengths, chunk order, checksums, field
values), not the image inside. Huffman, LZW, deflate and VP8 data is walked, never decoded, so a file whose pixels
are damaged but whose structure is consistent validates as `Valid`. The plan separates structural, media and
playability validation; decoding belongs to P14. It is also what L62, L64 and L65 come down to.

### L60. Images appended after a JPEG's EOI become files of their own
**Status:** Open  
A JPEG ends at its first EOI. Cameras and phones that append a second image (Multi-Picture Format, used for
previews and depth maps) or a vendor trailer after it therefore lose that part: the appended image is carved as a
separate JPEG (which is usually what a user wants to see anyway), and a trailer that is not a JPEG is lost. The MPF
index in APP2 is not read.

### L61. A JPEG whose scan runs into zeros is read to the maximum size
**Status:** Open (P20)  
Zero bytes are valid entropy-coded data, so a JPEG header followed by zeroed or unallocated space has no structural
end: end detection reads to the format's maximum size (256 MiB) before reporting `Truncated`. Erased flash (0xFF)
is bounded after 4 KiB of fill bytes, and other data usually breaks within a few kilobytes, but this one case costs
one long read per hit. A budget per scan (L50) would bound it.

### L62. Marker-free data between JPEG fragments is not noticed
**Status:** Resolved at the media level (P14): `JpegMediaTest.ForeignDataInsideAScanIsCaught`  
When a JPEG is fragmented and the gap holds data without 0xFF bytes (zeros, text), the walk reads it as
entropy-coded data and the file still validates. Restart markers catch the case where data was overwritten in
place, because the number of restart markers no longer fits the frame, but not the case where foreign data lies
between two fragments. Decoding the scan (L59) or fragment reconstruction (P13) is what would catch it.

### L63. A GIF sub-block chain of 0xFF reads to the end of the data
**Status:** Open  
0xFF is a valid sub-block size, so a GIF header followed by erased flash chains through it until the data or the
maximum size (256 MiB) ends. As with L61, the cost is bounded but large.

### L64. Damaged GIF data can resynchronise and still validate
**Status:** Resolved at the media level (P14): `GifMediaTest.ForeignDataTheSubBlockWalkAcceptsIsCaught`  
A damaged chain of sub-blocks can find its way back to a terminator and then to a trailer, so a GIF holding another
file's data can validate as `Valid` with a plausible length.
`GifFormatTest.FragmentedImageDataIsNotValid` pins this behaviour. Zeros in the data are caught (a chain ends and
the byte after it is not a block introducer); other data is not. LZW decoding would settle it.

### L65. Fragmentation inside uncompressed pixels cannot be detected
**Status:** Accepted (design); P13 places such files by the allocation evidence alone (L107)  
Uncompressed BMP pixel data and a simple WebP's VP8 bitstream have no internal structure, so another file's data in
the middle of one changes nothing a walk can check, and the file validates. Only data *inserted* between fragments
is noticed, because the file is then longer than its headers say. Choosing between fragment reconstructions (P13)
needs image data, not structure.

### L66. The maximum size per format is a judgement, not a measurement
**Status:** Open  
JPEG 256 MiB, PNG 1 GiB, WEBP 1 GiB, GIF 256 MiB, BMP 2 GiB. They bound the work one hit can cost and are far above
any ordinary photo, but a larger file is carved as `Truncated` and cannot be recovered whole. No corpus was
measured to pick them.

### L67. PNG files with a CgBI chunk are refused
**Status:** Accepted (design)  
Apple's iOS build tools write PNGs whose first chunk is `CgBI` and whose image data is not a standard zlib stream.
The header check requires IHDR as the first chunk, so such files are not carved as PNG. They only occur inside iOS
application bundles, not on the media this engine targets, and standard decoders refuse them too.

## Audio formats (P10)

### L68. A stream without length information is complete at every frame boundary
**Status:** Accepted (format)
An MP3 without a Xing or Info tag and a raw AAC (ADTS) file record nowhere how long they are: the walk follows the
frames while their headers agree, and wherever it stops, the frames before are a complete, playable stream. So a
file cut at a frame boundary (by fragmentation or overwriting) validates as `Valid`, only shorter. With a Xing or
Info tag (LAME, FFmpeg and most encoders write one) the recorded frame count makes a shorter stream `Broken` or
`Truncated`. Tests: `Mp3FormatTest.EveryPrefixIsTruncatedUnlessItIsAFileItself`,
`AacFormatTest.FragmentsEndTheStreamAtTheGap`.

### L69. Frames of another stream inside an MP3 are only noticed by its checksums
**Status:** Open: MP3 audio is not decoded in-house (the user's P14 scope); P13 relies on the frame chain (L106)  
Frames from another file with the same parameters (a fragment of another song by the same encoder) chain on like the
file's own. The LAME tag's music CRC catches them, and so can the frame CRCs and the bit reservoir (main data that
overlaps the previous frame's), but a stream without a LAME tag and without CRCs validates as `Valid`. Decoding the
audio (P14) or fragment reconstruction (P13) would be needed. Test:
`Mp3FormatTest.FragmentsAreNoticedWhereTheStructureAllows`.

### L70. Adjacent ADTS streams with the same parameters are carved as one
**Status:** Open
Two raw AAC files with the same profile, sampling frequency and channels, stored one after the other, form one
chain of frames, so they are carved as one file. MP3 streams are separated by a following file's ID3v2 tag or info
tag, and by the first one's frame count; ADTS has neither. Only the headers of ADTS frames are checked: the raw data
blocks are Huffman-coded and not decoded, and the header CRC is not checked (L78). Test:
`AacFormatTest.AStreamEndsWhereItsHeadersChange`.

### L71. WAV samples have no structure to check
**Status:** Accepted (design); P13 places such files by the allocation evidence alone (L107)  
As for uncompressed images (L65), PCM, float and G.711 samples are plain numbers: another file's data over them, or
between two fragments, changes nothing a walk can check. The RIFF size still ends the file, so a fragmented WAV is
carved with the right length, validates, and holds the wrong bytes. Test: `AudioCarvingTest.FragmentedFilesInAVolume`.

### L72. Frame-sync signatures make rejected hits frequent
**Status:** Planned (P15)
MP3 and ADTS frames start with 11 or 12 set bits, so random and compressed data (JPEG, MP4, ZIP) holds about 100
MP3 and 60 ADTS signature matches per MiB. Each costs a 4 KiB header read before it is rejected, which adds about
a third to the reads of a scan over such data. An ADTS header that claims a frame longer than the header check can
see (6 or 8 channels, or a program config element) reaches end detection before it is rejected, and a "LYRICSBEGIN"
after a stream is searched for its end marker up to 1 MB further. Reusing the scanner's block for header checks
(P15) would remove most of the cost.

### L73. VBRI info tags are recognised but their counts are not used
**Status:** Open
Fraunhofer's VBRI tag marks the start of a stream (and ends the stream before it), but its frame and byte counts
are not used to end the stream or to validate it: no sample file with a VBRI tag was available to confirm how they
count. Xing and Info tags are used in full.

### L74. Only MPEG audio Layer III is carved as MP3
**Status:** Accepted (decision to review)
Layer I and II files (.mp1, .mp2, used by DVB and some old recorders) and free-format bitrates (bitrate index 0,
whose frame length is not in the header) are not accepted. Both are rare on removable media.

### L75. Unfinished and very large WAV files are not carved
**Status:** Open
A recording that was never finished (power lost, card pulled) leaves the RIFF size as 0 or as the placeholder
0xFFFFFFFF, and then nothing says where the file ends: the header is refused. RF64 and BW64 (WAV beyond 4 GiB, from
field recorders) and big-endian RIFX files are not supported.

### L76. M4A files with a generic brand and no moov are not carved
**Status:** Resolved (P12): MP4 carves them. The shared rule (`mp4::classify`) takes a generic brand without moov as
video, so such a file is an MP4 candidate (`Truncated` or `Broken`) whether it held audio or video.  
A file whose major brand is generic (isom, mp42, 3gp4: Android, Windows and many recorders) is audio only if its
moov holds a sound track and no video track. Without moov (lost, overwritten, or beyond a truncation) audio cannot
be told from video, so end detection refuses the file. With an audio brand (M4A, M4B, ...) such a file is carved
as `Truncated` or `Broken`. P12's MP4 carver will see these files as MP4.

### L77. M4A sample data is not checked against the sample tables
**Status:** Resolved (P12): M4A validates with `parseFile`, so every chunk's size (stsc and stsz), its place inside
an mdat and overlaps are checked, and the movie fragments too. What the audio samples hold is still not checked
(L95).  
Validation checks the box structure, the presence of the sample tables and that every chunk offset points into
mdat, but not the chunks' sizes (stsc and stsz) or what the samples hold. A gap inserted inside mdat of a file with
moov first, or samples overwritten in place, go unnoticed. P11's parser ([formats/mp4.md](formats/mp4.md)) now
derives every chunk's size and checks that the chunks lie in mdat and do not overlap. M4A was not moved onto it in
P11, because completed phases are only changed to fix a verified defect: that is left for P12, which builds the
MP4 carver on the same parser and has to divide ftyp files between the two formats anyway. What the samples hold
stays unchecked either way (media validation, P14).

### L78. The ADTS header CRC and the AAC raw data are not checked
**Status:** Resolved (P14) as far as the codec's tables allow: the CRC of protected frames and the raw data blocks up
to their coded data are checked (L116, L119)  
A protected ADTS frame's CRC is skipped (none of the encoders used for the samples writes one), and raw data blocks
are not parsed: that needs Huffman decoding (media validation).

### L79. FLAC, Ogg (Vorbis, Opus), WMA and AMR are not carved
**Status:** Open
The plan lists "FLAC where practical" among the audio targets, but P10's priority list is MP3, WAV and M4A/AAC. Ogg,
WMA and AMR files (voice notes, older players) are not carved either. AMR in a 3GP or M4A container is carved as M4A.

### L80. What is carved of a stream whose start is lost
**Status:** Open
The frames that survive after a lost start are carved as one file from the first whole frame, and validation
reports the stream as `Invalid` ("its start is missing") when that frame's main data begins in an earlier (lost)
frame. An ID3v2 tag with no frames after it is not carved at all (its cover picture is found as an image), and a
lone frame header cut off by the end of the data is not a file.

### L81. The maximum size per audio format is a judgement
**Status:** Open
MP3 1 GiB, AAC 1 GiB, M4A 4 GiB (audiobooks), WAV 4 GiB + 8 (the RIFF limit). As for the images (L66), they bound
the work of one hit and were not measured on a corpus.

### L82. A stale info tag frame count splits a stream
**Status:** Open
With a Xing or Info tag the stream ends after the number of frames the tag records. If a tool changed the file
without updating the count, the frames after it are carved as a separate stream.

### L83. Some trailing tags cannot be found from the front
**Status:** Open
APEv1 tags and APEv2 tags without a header are recognised only by their footer, at the end of a file; a walk from
the front does not find them, so the file ends before them and the tag is lost. ID3v1, APEv2 with a header,
Lyrics3 and appended ID3v2 tags are kept.

## MP4 parser (P11)

### L84. Movie fragments are not parsed
**Status:** Resolved (P12): `parseFile` reads mvex/trex and every moof (mfhd, traf, tfhd, tfdt, trun), locates each
run's samples with the three ways of giving a base data offset, and checks them like chunks. Its rules are L102.  
The samples of a fragmented file (moov with mvex, then moof and mdat pairs, as live recorders, DASH and some
cameras write) are described by moof/traf/tfhd/trun boxes, which the parser lists at the top level but does not
read. Such a file is `Valid` when its moov is consistent (its sample tables are usually empty) and `Movie::fragmented`
is set, but none of its samples is located or checked. P12's test list names "fragmented MP4": if that means movie
fragments, P12 needs them parsed.

### L85. Edit lists, composition offsets, sync samples and data references are not read
**Status:** Open
edts/elst, ctts, stss, stps, sdtp, sgpd/sbgp, subs and saiz/saio are skipped: they do not say where the samples
are. dinf/dref is skipped too, so a track whose samples are in another file (a data reference that is not
self-contained, which ISO allows and hardly any camera writes) has its chunk offsets checked against this file's
mdat boxes and is reported `Invalid`.

### L86. Sample descriptions are read only for their common fields
**Status:** Resolved (P14): the media level reads avcC, hvcC and esds and checks every sample they describe (L120 for
the codecs it does not decode)  
From each stsd entry the parser takes the format, the data reference index, a video entry's width and height and
an audio entry's channels, sample size and rate (QuickTime's version 2 sound description included; its version 1
fields and Macintosh language codes are not interpreted). Codec configurations (avcC, hvcC, esds, dOps, ...) and
the samples themselves are not checked: that is media validation. P12 reads one field of avcC and hvcC, the size of
the NAL unit length fields, for the sample framing check (L95).

### L87. Every chunk must lie inside an mdat box
**Status:** Open (decision to review)
A chunk outside the media data boxes (in moov, in a free box, in an ISO 2020 `imda` box, or across the boundary
between two mdat boxes) makes the file `Invalid`. The standard allows sample data anywhere in the file; no writer
sampled put it anywhere but mdat, and a chunk elsewhere is far more often damage than design.

### L88. How strict the parser is
**Status:** Accepted (decision to review)
Calls made without a sample that decided them either way:
- mvhd, tkhd and mdhd must be exactly the size of their version's fields; bytes after them are malformed (the box
  then grew over its neighbour's header). Table boxes (stts, stsc, stsz, stz2, stco, co64) may be longer than their
  entries, and their version fields are not checked.
- A known box (the plan's list) in the wrong container is malformed; unknown boxes may be anywhere.
- The stsd entries must fill stsd, apart from a 32-bit zero terminator; hdlr needs its 24 fixed bytes, the name
  may be empty.
- stsc's first entry must start at chunk 1 and no entry may start beyond the last chunk (FFmpeg tolerates some of
  both with a warning).
- A moov without tracks, a track id of 0 or used twice, and a time scale of 0 are malformed.
All 69 files from FFmpeg, GPAC, Media Foundation and the Visual Studio videos pass these rules.

### L89. Boxes the parser does not know are not checked
**Status:** Accepted
Unknown boxes (vmhd, smhd, dinf, udta, meta, edts, ...) are only checked for fitting their parent. Damage confined
to them is invisible: a vmhd whose size grew over the header of the dinf after it leaves minf consistent. Damage that
reaches a known box (a missing, misplaced or misaligned tkhd, stbl, stco, ...) is found.

### L90. The parse limits are a judgement
**Status:** Open
maxBoxes 1,000,000, maxTracks 256, maxSampleDescriptions 64 and maxTableEntries 2^22 (at most 128 MiB) bound the
work and memory of one parse. They were not measured on a corpus of long recordings: a movie of more than about ten
hours at 30 frames per second, or with a table entry per sample in every table, can pass maxTableEntries and is
then reported `Invalid` with a `LimitExceeded` issue. Callers can raise the limits.

### L91. The parser does not decide where a file ends
**Status:** Resolved (P12): the carving formats' top-level walk (`src/formats/iso_walk.hpp`) decides it. The next
ftyp, a box that does not belong at the top level after moov and mdat, or bytes that are no box end the file; a
size-0 mdat after moov ends where its sample tables' media data ends. The parser is unchanged in this.  
`scanTopLevel` lists every box whose type is printable and whose size fits, so bytes after a file that happen to
look like boxes (another file, a second ftyp) are listed as more boxes, and a top-level box of size 0 runs to the
end of the content. `parseFile` parses only the first moov and reports a second ftyp or moov as an issue. Chunk
offsets are taken as offsets from content offset 0; `parseMovie` on a moov found on its own reports them as
recorded. Deciding where a carved file starts and ends, and which moov belongs to which media data, is the MP4
carver's job (P12).

## MP4 recovery (P12)

### L92. A video track damaged before its handler makes the file look like sound only
**Status:** Open  
Audio is told from video by the handler (mdia/hdlr) of each track. When a generic-brand file's video trak is
damaged before its hdlr can be read (a tkhd whose size broke the trak, say), the parser keeps only the sound track,
the file classifies as audio, and M4A carves it (`Invalid`) while MP4 rejects it. The file is still carved once, by
the other format. Test: `Mp4FormatTest.CorruptMetadataIsCaught`.

### L93. A chunk beyond the end of a complete file: M4A says `Invalid`, MP4 says `Truncated`
**Status:** Open (decision to review)  
When a file's top-level boxes are complete but a chunk of its sample tables lies after the last of them, either the
chunk offset is wrong or media data that followed is missing (a later mdat lost, or stored elsewhere on the disk).
P10's M4A tests pinned the first reading (`Invalid`), and M4A keeps it. MP4 takes the second: its walk ends the file
there (`Truncated` when the data ends, `Broken` when something else follows) and validation reports `Truncated`.
The two formats therefore call the same situation differently. Test: `Mp4FormatTest.CorruptMetadataIsCaught`.

### L94. A movie-fragmented file cut at a fragment boundary is a valid, shorter file
**Status:** Accepted (format)  
Every movie fragment (moof and its mdat) stands on its own, and nothing requires a certain number of them (mehd,
which some writers use to record the whole duration, is not read). So when carving reads a fragmented file whose
data breaks off after a fragment (the rest is elsewhere on the disk, or overwritten), the fragments before are a
`Valid` file, only shorter, as for streams without length information (L68). Only the filesystem's record of the
layout gives the rest. A trailing moof without its mdat is `Truncated` (the data ends) or ends the file before the
moof (something else follows). Test:
`Mp4RecoveryTest.DeletedFilesWhoseLayoutExFatAndNtfsRecordedAreFilesystemCandidates`.

### L95. Only AVC and HEVC samples are checked by content
**Status:** Resolved for AAC, AVC and HEVC (P14): AAC samples are read as raw data blocks, AVC and HEVC slice headers
and every NAL unit's bytes are checked; other codecs are not decoded (L120)  
The sample framing check walks the NAL units of every AVC and HEVC sample (their length fields, of the size avcC or
hvcC gives), which catches a sample that other data replaced or that is not where the tables say. Samples of other
tracks are not looked at: audio (AAC, AMR, ...), text, and video in other codecs (MPEG-4 Part 2, MJPEG, ProRes,
AV1) or without an avcC or hvcC box. So a file whose audio samples lie in another file's data still validates; a
carve can even run past its data into foreign bytes that only audio samples cover (the case of L94's test). Samples
written as an Annex B byte stream (start codes instead of length fields, which some broken muxers produce) would be
reported misframed. The check reads each NAL unit's header with the cache block around it, so about all of a
file's video data. `Mp4FormatOptions::checkSampleFraming` turns it off.

### L96. A moov whose file start is lost is not carved
**Status:** Open (left out of P13 by decision, 2026-09-30: the recovered file would lack its ftyp and mdat header)  
Carving starts at an ftyp. When a file's first clusters are overwritten, its moov and media data are found only if a
filesystem candidate still locates the rest of its data: moov discovery (`mp4::findMovie`) searches the data of that
candidate, trying at most 64 occurrences of "moov". A moov with no filesystem candidate around it, and mdat data
whose moov is lost, are not carved: pairing orphan moov and mdat pieces is fragment reconstruction (P13).

### L97. MP4 recovery reads each file several times, and carves allocated space
**Status:** Open (P15)  
A filesystem candidate's data is read to analyse it (the parse, then the framing check, L95). The scan then reads
the whole source, active files included, and each hit's carve reads the file's boxes; unless the carve matches a
filesystem candidate stored in one run with an intact structure, the carve is analysed again. So the media data of a
file is read two to four times (L49). The scan cannot simply leave out allocated clusters either, because data
inside active files (a motion photo's video) is recovered too.

### L98. Unreadable samples are counted only where the analysis read
**Status:** Accepted (cost)  
`Mp4TrackEvidence::samplesUnreadable` counts the samples with bytes in sectors that failed while the analysis read
them. The analysis reads the boxes and the video samples' NAL unit headers (with the cache block around each), not
every byte of every sample, so a bad sector inside a large NAL unit or among audio samples can go uncounted.
Reading every sample as well would add a full read of every file. `RecoveryWriter`'s report counts every unreadable
byte of a file it writes. Test: `Mp4RecoveryTest.BadSectorsMakeSamplesUnreadable`.

### L99. MP4 candidates are kept in memory until the end of the run
**Status:** Open (P15); P14's evaluation keeps its inputs too (L129)  
Filesystem candidates are examined before carving, so that a carve can be attached to the candidate that starts
where it does, and every MP4 candidate (with its structure and sample evidence, and the first 32 parser issues) is
kept until the run delivers them. Memory grows with the number of MP4 files on the source (a few kilobytes each),
as for the filesystem candidates themselves (L40).

### L100. How HYBRID and SizeMismatch are decided
**Status:** Accepted (decision to review)  
Calls made in P12 without a sample that decided them:
- A filesystem candidate with a guessed layout (a deleted FAT32 or exFAT file) becomes HYBRID when its structure is
  `Valid` and no AVC or HEVC sample is misframed. Damaged samples (reallocated, unreadable) do not stop it.
- A carve that starts at a filesystem candidate's first byte is attached to it when the candidate's own structure
  is valid, or when the carve is not; otherwise the candidate becomes HYBRID with the carve's layout and length (one
  run from its start). An MP4 name that locates no data (size 0, nothing stored) takes any carve that starts at its
  first cluster, whatever its verdict.
- Such a layout is a guess too: `LayoutGuessed` stays when the metadata's layout was guessed, and
  `fragmentation.known` is false. For a deleted entry, the run's clusters that are allocated now are marked
  reallocated.
- A deleted entry whose first cluster is allocated to other data now is never confirmed, neither by its guessed
  layout nor by a carve: the structure that starts there belongs to the new owner. The entry stays FILESYSTEM (its
  samples counted as reallocated), and one that locates no data is not delivered.
- `SizeMismatch` compares where the structure ends with the size the metadata records. It is not raised for an
  `Invalid` structure (it may end anywhere), except when its only issue is data after its last box.

### L101. The maximum MP4 size (256 GiB) is a judgement
**Status:** Open  
It bounds the work of one hit, as for the other formats (L66, L81). Cameras split recordings at 4 GiB on FAT32 but
not on exFAT or NTFS; no corpus of long recordings was measured to pick the bound.

### L102. How strict the movie fragment rules are
**Status:** Accepted (decision to review)  
Calls made without a sample that decided them either way, all as ISO/IEC 14496-12 states them:
- A moof in a file whose moov has no mvex, a track fragment of a track that moov does not have or that has no trex,
  and a sample description index beyond the track's are malformed.
- mfhd's sequence numbers must increase; trex must be exactly 24 bytes; a track's samples (tables and fragments
  together) must stay below 2^32.
- A run's data must lie inside an mdat and not overlap any chunk or other run; a data offset before the start of the
  file is malformed.
- tfdt's decode time, sample flags and composition offsets are not used; sidx, mfra (with its tfra), styp and
  emsg are skipped like other top-level boxes.

All 24 embedded samples (5 of them fragmented by FFmpeg and GPAC) and the 51 corpus files pass these rules.

### L103. A file inside an active file is a candidate of its own
**Status:** Resolved (P14): the evaluation links a carve inside an active file to it (`container`,
`INSIDE_ACTIVE_FILE`) and does not mark its clusters reallocated  
An MP4 carved inside an active file's data (the video of a motion photo, a video inside an uncompressed archive or a
disk image) is a CARVING candidate with the warning `InsideActiveFile`, not linked to the file that holds it. Only a
carve lying entirely inside one active file gets that warning; one across several files gets `AllocatedClusters`.
A carve's clusters are checked one by one against the allocation, up to `Mp4RecoveryOptions::maxClusterChecks`
(4,194,304) per file; beyond that the evidence says it is incomplete.

## Fragment reconstruction (P13)

### L104. The fragment search is bounded
**Status:** Accepted (decision to review)  
Per seed: at most 2048 layouts validated, 4 GiB read, 16 fragments; per break, the 4 cluster boundaries nearest it
and 64 candidate clusters for each, 8 partial layouts carried from one fragment to the next, 1,048,576 clusters
looked at; MP4: 8 moov anchors, windows of 16 samples, 1,048,576 windows. A gap between fragments that no evidence
explains (free clusters no file claims) is crossed only if the next fragment is among the first 64 candidates after
it. Every limit is a field of `FragmentSearchLimits`; a search that one stopped reports it (`SearchStats::limit`), and
when it ended without a valid layout, so does a list of candidates that was cut. None was measured on a corpus.

### L105. Every layout is validated from the start of the file
**Status:** Open (P15)  
The formats validate whole files, not a part of one, so the generic search validates each layout it tries from the
file's first byte, reading and parsing the part before the break again each time: the cost grows with the file's
size times the layouts tried (hundreds for a fragment across data no evidence explains). The reads are served from
the source each time; there is no cache across layouts. MP4 walks its samples instead, a window at a time.

### L106. What a structure confirms
**Status:** Accepted (decision to review)  
Foreign data can pass for a while as the structure of a file: JPEG takes zeros and marker-free bytes as entropy-coded
data (L62), and a JPEG segment or an ISO box whose length foreign bytes give makes the walk jump over them unchecked.
So a continuation counts as found only once its structure holds for `minimumContinuation` bytes (4 KiB) past its
start, or the file validates; a layout that does not validate delivers only what is confirmed: a base layout, the
clusters before the one where its structure broke; a continuation, the file up to the boundary it starts at. A
PARTIAL result therefore leaves out a middle fragment found with weak evidence, and a middle fragment shorter than
4 KiB (8 clusters of 512 bytes) is found only if the file validates. A layout whose structure ends before the recorded
size is not continued (ranked below the others). Tests: `AMissingFragmentLeavesAPartialFile`,
`OverwrittenFragmentsAreReportedForWhatTheyCost`.

### L107. Files whose data has no structure are placed by the allocation alone
**Status:** Accepted (the user's tie rule, 2026-09-30); P14's media level checks the delivered reconstruction where
something is coded (ADPCM, AAC, VP8L), not the layouts that lost  
BMP pixels, WAV samples, the VP8 data of a simple WebP and the audio of an MP4 or M4A validate whatever they hold
(L65, L71, L95). Such a file's layouts differ only in the allocation evidence, which decides as the user chose:
a layout that reads no clusters in use now wins. So a file overwritten in place can come out COMPLETE through the
layout that skips the new file's clusters (its tail is then the data after them), and a file fragmented around free
clusters that no file claims comes out COMPLETE as one piece (nothing shows it was fragmented).
`HypothesisEvidence::dataChecked` is false for them: the validator does not see the data where the pieces join.
Tests: `AllocationEvidenceDecidesWhereTheStructureCannot`, `AudioStreamsAreReconstructed`.

### L108. MP4 joints that the samples cannot check are ambiguous
**Status:** Accepted (format)  
A fragment boundary inside audio data, or inside a NAL unit's payload, is not checked by any sample: every cluster
boundary the samples cannot tell apart gives a layout, and they end AMBIGUOUS unless the allocation decides (L107).
With 512-byte clusters (the tests) this is frequent; with the 32 KiB and larger clusters of cards it is rare, since
nearly every cluster holds a NAL unit's start. When the moov's fragment starts somewhere in audio data (an M4A), every
boundary up to the moov is a layout: the reason gives how many, only `maxAlternatives` are delivered. Test:
`AudioInMp4IsPlacedByTheAllocationOrReportedAmbiguous`.

### L109. What MP4 placement assumes
**Status:** Open  
The movie is taken from the first fragment (moov before the media data, parsed without issues), or found where the
first mdat's size puts it (moov right after the first mdat, in one piece in free space, its media data in that mdat).
Files with several mdat boxes before the moov, a moov split across fragments, movie fragments (moof) spread over
fragments of the disk, or no moov at all (L96) are left to the generic search, which validates whole layouts (L105).
Only AVC and HEVC samples are checked (L95).

### L110. Which files are seeds
**Status:** Open  
Deleted files whose layout P7 guessed (FAT32, exFAT with a cleared chain) and carves that break at the start of a free
cluster of an added volume. Not: files whose chain is cut short or damaged (`ChainShorterThanSize` and the like), NTFS
files whose runs continue in an attribute list (L23), carves outside a volume's cluster area (a source without a
filesystem, unpartitioned space), carves in clusters in use (inside an active file) or not at a cluster start. moov
anchors are found only at the scan's alignment (`ScanOptions::alignment`).

### L111. Where a next fragment may start
**Status:** Accepted (design)  
Continuations are looked for in the order a writer that takes the next free cluster would meet them, wrapping around
at the end of the volume; other allocators are found only within the bounds (L104). A continuation never starts in a
cluster allocated now (its data would be another file's) or claimed strongly by another file (its recorded layout or
first cluster, a carve that validated, an earlier reconstruction): a fragment whose clusters were reallocated but not
yet overwritten, or that stale evidence claims, is not found. The layouts skipping clusters (`SkipAllocated`,
`SkipClaimed`) still read through nothing but the file's own.

### L112. Reconstructions claim their clusters in delivery order
**Status:** Open  
A reconstruction that places data claims its clusters for the rest of the run, so a later file cannot use them; an
earlier wrong reconstruction can therefore keep a later file from its own clusters. Carves of two formats that start
at the same cluster share their claims, and both are reconstructed.

### L113. Some format knowledge lives in the fragment module
**Status:** Accepted (tidy-up)  
A deleted file is identified by its content first; when that fails, by its extension, through the descriptors'
extensions and a small table of other names (jpeg, jpe, jfif, mov, qt, m4v, 3gp, 3g2, 3gpp, 3gp2, m4b, m4p, adts).
A new format with other extensions needs an entry there, or is found by content only. MP4 and M4A are recognised by
their format classes for the sample-table placement. The `dataChecked` control replaces a cluster with a fixed pattern
(pseudo-random, `FF 01` every 64 bytes) and asks the validator: it tells whether the validator notices foreign data
there, not whether the layout is right.

### L114. The carving pass carves every format again
**Status:** Open (P15)  
Fragment reconstruction scans the whole source with every registered format (and a moov probe), after whatever other
passes a caller runs (L49, L97), and carves every hit at the start of a free cluster of an added volume: the costs of
L50 and L61 apply (a JPEG header followed by zeros reads to the maximum size).

### L115. What is kept in memory
**Status:** Open (P15)  
The claims of every file (interval maps over clusters), the carve seeds and the moov anchors are kept for the whole
run, on top of the volumes' candidates (L40); cluster states are cached per volume (at most 16 MiB); each seed's
layouts until it is delivered.

## Validation and candidate evaluation (P14)

### L116. Lossy codecs are checked only in part
**Status:** Accepted (the user's P14 scope: no codec tables in the engine)  
Lossless data is decoded completely (JPEG Huffman scans, PNG deflate, GIF LZW, BMP RLE, VP8L). Lossy codecs are read
as far as their structure goes without the codecs' tables: VP8 frame headers, partitions and token probabilities;
AAC raw data blocks up to their coded scale factors (whole only for streams of the zero codebook); AVC and HEVC
parameter sets and slice headers, not the slice data; ADPCM block headers. MP3 audio is not decoded at all
(`Unsupported`). Damage inside the coded data of a lossy frame is not seen unless it breaks one of those structures.
The media level says `Partial` for them.

### L117. AVC and HEVC headers are checked as FFmpeg checks them
**Status:** Open  
A header fails where the specification forbids what it holds and FFmpeg 6.1 rejects it too, or where the syntax
cannot be followed, plus a few rules FFmpeg does not enforce (forbidden NAL byte sequences, CABAC alignment bits,
`nuh_temporal_id_plus1` 0, memory management and long-term reference errors, HEVC alignment zero bits). What FFmpeg
accepts with a warning passes: a VUI that does not read cleanly (then, for HEVC, the SPS extensions after it are
taken as absent), an HEVC PPS that ends early, AVC weight denominators above 7, data after a parameter set's fields.
Spec ranges FFmpeg does not check are mostly not checked either. About half of the first 32 bits of a slice header
are fields any value of which is valid, so a flipped bit there passes (157 of 320 flips caught in an AVC file, 151
of 320 in an HEVC one). NAL units of layers above the base layer, data partitions, SEI payloads and access unit
delimiters are not read.

### L118. Zeros at the end of a NAL unit are taken as padding
**Status:** Accepted  
FFmpeg drops zero bytes at the end of a NAL unit, and muxers that copied a byte stream may keep its
`trailing_zero_8bits`, so the scan accepts them. A zero-filled cluster at the end of a video sample therefore
passes; one in the middle (zeros with data after them) fails.

### L119. Some ADTS CRCs are not checked
**Status:** Open  
The CRC rule (the header, then the first 192 bits of each channel element and 128 of a CPE's second stream, and every
data stream element) was found by experiment on fdkaac's files; libfdk-aac's source agrees, but no other encoder that
writes CRCs was at hand. The coverage of a program config element is not known, so frames with one are not checked;
neither are frames of several raw data blocks, nor frames of more than one channel element whose elements were read
only in part (5.1). The data stream elements are assumed to be covered whole (libfdk-aac), not verified on a file.

### L120. Some audio and video in MP4 is not decoded
**Status:** Accepted  
AAC (MPEG-4 audio, and MPEG-2 AAC) is read; MP3 in MP4, ALAC, AC-3, E-AC-3, Opus, FLAC, AMR, MPEG-4 Part 2, MJPEG,
ProRes, AV1, VP9 and encrypted tracks are `Unsupported`; PCM is `NotApplicable`. An AAC sample must end where its
`ID_END` ends (FFmpeg also accepts zero bytes after it). An `mp4a` entry without a decoder configuration is read as
AAC LC of its channels and rate, as FFmpeg plays it.

### L121. Validation and hashing read every candidate's data
**Status:** Open (P15)  
The media level reads all of a video track's media data (the NAL byte scan), and the evaluation then reads every
candidate's data again for its SHA-256: two passes over every recovered byte, more for HYBRID candidates (their own
layout is validated first). The NAL scan is linear in the file's size and not bounded by `MediaLimits`.

### L122. Platform decoders conceal damage
**Status:** Accepted  
Playability passes when WIC or Media Foundation decode every frame or sample without reporting an error. They
conceal much damage: WIC decoded every test PNG whose deflate data is broken (the media level fails them), and Media
Foundation decoded the MP4 builder's pattern-byte H.264. Playability is not an integrity check; the media level is
the engine's.

### L123. The playability gaps are what one Windows version showed
**Status:** Open  
Content Windows decodes wrongly or not at all although it is valid is excluded before decoding, from lists observed
on Windows 10 22H2 (19045): JPEG other than Huffman-coded 8-bit baseline, extended and progressive; bitmaps of JPEG,
PNG, Huffman 1D or RLE24 data; MP4 with `stz2`; AVC beyond Baseline, Main and High. Other gaps, or other Windows
versions' decoders, can still make a valid file `Failed` at this level (and so `Invalid` overall when playability is
turned on).

### L124. A stalled platform decoder is abandoned, not cancelled
**Status:** Open  
Media Foundation has no way to cancel a decoder that neither delivers nor fails. After `sampleTimeout` (10 s) the
check returns `Unsupported`, and the source reader is never released, because releasing it waits for the stuck
decoder forever: its threads and memory stay until the process ends (the content stream is cut off from the reader,
so nothing reads the source any more). The timeout path was exercised by hand (2026-10-03, AVC High 4:2:2, 4:4:4 and
High 10, before those profiles were excluded); no test triggers it.

### L125. Playability runs platform code on untrusted data, with the codecs installed
**Status:** Accepted (optional, off by default)  
The decoders parse the recovered content in the engine's process. What they accept depends on the machine: WebP and
HEVC come from Microsoft Store extensions, ADTS sources may be missing. The tests skip the cases whose codec is not
installed (HEVC on the development machine).

### L126. One candidate per file means one per start
**Status:** Open  
Inputs are merged by the source offset of their first byte. Files that overlap but start at different bytes (a
thumbnail inside a carve that did not validate, two carves of a damaged stream) are separate candidates, and the
content they share is not a duplicate. A carve that starts where a filesystem candidate starts is taken as the same
file whatever the names say: same first byte, same content start.

### L127. Duplicates are exact copies by SHA-256
**Status:** Accepted (the user's P14 decision)  
Two candidates are duplicates only when every byte is equal: a copy that differs in one byte (another unreadable
sector, a different end) is not one. Duplicates are identified, not removed; the first delivered is the original.
Empty files are never duplicates, and without SHA-256 there is no duplicate detection (the preliminary hash only
tells files apart).

### L128. The format of a filesystem candidate is guessed from its content
**Status:** Open  
A filesystem candidate is validated as the registered format whose header check accepts its content (its
extension's when several do or none does). Files of formats the engine does not register (documents, archives,
executables) are `NotValidated` with `FORMAT_UNKNOWN`; a renamed file is validated as what it contains.

### L129. The evaluation keeps its inputs until the end of the run
**Status:** Open (P15)  
The volumes' scans, every carve, MP4 candidate and reconstruction are kept (and copied once) until the run has
delivered every candidate, on top of what the stages kept themselves (L40, L99, L115).

### L130. Carved files get new names
**Status:** Accepted  
Candidates without filesystem evidence are named `recovered_<id>.<ext>` after the evaluation's id, not the names the
MP4 recovery or fragment reconstruction runs gave them (their own numbering).

### L131. HYBRID needs the carve's verdict
**Status:** Open  
A carve gives a filesystem candidate its layout only when the candidate's own data does not pass the structure and
the carve validates (or the metadata locates no data). A carve that is longer than the metadata's own valid layout
(a file the metadata records too short but whose truncated form still validates, as for formats that validate any
prefix) does not replace it.
