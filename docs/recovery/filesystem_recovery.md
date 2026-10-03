# Filesystem-based recovery (P7)

Filesystem metadata is used before raw carving, because it gives names, sizes and data locations. P7 turns the
files that FAT32, exFAT and NTFS metadata know about, active and deleted, into **recovery candidates**. It rebuilds
their data from the source and writes it to a separate destination.

Library `recovery_candidates` (namespace `recovery`, headers in `include/recovery/`):

| Header | Contents |
| --- | --- |
| `recovery_candidate.hpp` | `RecoveryCandidate`, `SourceRegion`, `LayoutEvidence`, `AllocationInfo`, `FragmentationInfo`, `FilesystemEvidence`, `CandidateWarning`, `validateCandidate` |
| `filesystem_recovery.hpp` | `FilesystemRecovery` interface, `Fat32Recovery`, `ExFatRecovery`, `NtfsRecovery`, `openFilesystemRecovery`, `CandidateOptions`, `CandidateScan` |
| `candidate_reader.hpp` | `reconstructCandidate`: reads a candidate's data from the source |
| `recovery_writer.hpp` | `RecoveryWriter`: writes reconstructed candidates below a destination directory |
| `output_names.hpp` | `safeFileName`, `numberedName`: Windows-safe, collision-safe names |

Recovery code depends only on the filesystem-independent model (`filesystem.hpp`). The only exception is
`NtfsRecovery`, which re-reads a record's run list to place the holes of sparse files.

## Candidates

A `RecoveryCandidate` holds everything needed to rebuild one file from the source alone, without the filesystem:

| Field | Meaning |
| --- | --- |
| `id` | Unique within a scan, in scan order from `CandidateOptions::firstId` |
| `method` | `FILESYSTEM` here; MP4 recovery (P12, [mp4_recovery.md](mp4_recovery.md)) adds `CARVING` and `HYBRID` |
| `filename`, `extension` | The original name exactly as recorded (UTF-8; may contain characters Windows forbids), and its lower-case extension |
| `expectedSize` | The size the metadata records |
| `sourceRegions` | Every byte of the file, in order (see below) |
| `embeddedData` | NTFS resident data |
| `fragmentation` | Number of physically separate pieces, and whether that number is known |
| `filesystemEvidence` | Filesystem, volume offset, source offset of the metadata record, original path, 8.3 name, active or deleted, parent deleted, valid data length, timestamps, attributes, entry issues, and `AllocationInfo` (method, `LayoutEvidence`, first cluster, cluster count and size, allocation issues) |
| `warnings` | What the evidence means for the data (see below) |

Every offset is a **source** offset: the volume's offset on the disk plus the offset inside the volume. A candidate
from a partition can therefore be rebuilt from the whole disk or image.

There is no numeric confidence score. The plan forbids one until a validated scoring model exists. The evidence
is kept as it is, and P14 will unify it with carving evidence.

### Source regions

The regions cover `[0, expectedSize)` without gaps or overlap:

| Kind | Meaning | Reconstruction |
| --- | --- | --- |
| `Stored` | On the source at `sourceOffset` | Read |
| `Embedded` | Inside the metadata record (NTFS resident data) | Copied from `embeddedData` |
| `Zeros` | A sparse hole, or bytes at or beyond the valid data length (their clusters hold stale data) | Zeros |
| `Missing` | The metadata doesn't say where these bytes are: a chain or run list cut short, an invalid start cluster, data beyond the volume, or compressed/encrypted data | Zero-filled inside the file, left out at its end |

Files are **never assumed to be contiguous**. The regions follow the recorded chain, runs or contiguous-data flag.
When only a start cluster survives (a deleted FAT32 entry, or a deleted exFAT entry whose chain was cleared), the
contiguous run from it is a guess, and it is labelled as one:

| `LayoutEvidence` | When |
| --- | --- |
| `Recorded` | Active chains, exFAT contiguous runs (also when deleted), exFAT chains that survived deletion, NTFS run lists (also of deleted records), resident data, and a guessed file of one cluster (it cannot be fragmented) |
| `Guessed` | A contiguous guess of more than one cluster. `fragmentation.known` is false and the warning `LayoutGuessed` is set |
| `None` | Nothing located (an empty file, no usable allocation) |

### Partly overwritten files

When the filesystem reports that clusters of a deleted entry are in use again (`ClustersInUse`), each cluster of
the stored regions is checked with `IFilesystem::clusterState`. Clusters now allocated to other data (or marked
bad) are split into their own regions with `reallocated = true`, and the candidate gets `ClustersReallocated`. A
partly overwritten file therefore shows exactly which parts may have been overwritten. The rest is still expected
to be intact. The same regions of a guessed layout show where a guess ran into another file (see
`Fat32RecoveryTest.DeletedFragmentedFileIsNeverPresentedAsExact`).

### Warnings

| Warning | Cause |
| --- | --- |
| `LayoutGuessed` | Layout `Guessed` |
| `DataMissing` | Some region is `Missing` |
| `ClustersReallocated` | Some stored region is reallocated |
| `DataNotDecoded` | NTFS compressed or encrypted data |
| `CrossLinked` | Active entry sharing clusters with another |
| `AllocationDamaged` | Loops, invalid, free or bad clusters in a chain, a malformed run list, an invalid start cluster, a size larger than the volume, a chain longer than the size, an active entry whose clusters are free, a missing data attribute, or more stored bytes than the volume holds |
| `AllocationUnknown` | The allocation table or bitmap is unreadable where the data is |
| `NameUncertain` | Name rebuilt (FAT32 deleted 8.3 names lose their first character), unverified, not matching its hash or checksum, or containing forbidden characters |
| `LocationUnknown` | NTFS orphan: the parent directory is gone, the path is under `/$OrphanFiles` |
| `MetadataDamaged` | Checksum mismatch, damaged or incomplete record, invalid timestamps or attribute bits, valid data length larger than the size |

The allocation and entry issues from the filesystem are kept alongside the warnings, as evidence.

## Per-filesystem recovery

`FilesystemRecovery` does the work that is the same for all three filesystems: scanning, building regions,
applying the valid data length, checking for reuse, and working out warnings. Each implementation opens and owns
its filesystem, and supplies what is specific to it:

| | FAT32 (`Fat32Recovery`) | exFAT (`ExFatRecovery`) | NTFS (`NtfsRecovery`) |
| --- | --- | --- | --- |
| Deleted file layout | `Guessed` beyond one cluster (Windows frees the chain) | `Recorded` for contiguous runs and surviving chains, `Guessed` if the chain was cleared | `Recorded`: the record keeps its runs |
| Valid data length | None | Bytes beyond it are `Zeros` | Initialized size: bytes beyond it are `Zeros` |
| Special data | | | Resident data is `Embedded`. Sparse files are laid out from the record's run list, holes as `Zeros`. Compressed and encrypted data is `Missing` (`DataNotDecoded`) |
| Not candidates | Directories | Directories | Directories, the metadata files (records 0-15, everything under `/$Extend`) |

`openFilesystemRecovery(volume, volumeOffset)` reads the boot record's signature and tries the matching module
first. If that fails, or the signature is unrecognisable (for example a damaged primary boot sector), it tries the
others, because each can fall back to its own backup boot record. It fails with `UnsupportedFilesystem` when no
module recognises the volume. A module that recognises the volume but can't use it gives `CorruptedFilesystem`,
which takes precedence.

`findCandidates(options, cancel)` scans with `options.limits`. It includes active and deleted files by default
(`includeActive`, `includeDeleted`) and returns the candidates in scan order, the scan issues, the number of
directories and skipped metadata files, and whether the scan completed. Cancellation is checked for every entry.

### Bounds

Metadata is untrusted, so a candidate can never describe more data than the volume holds:

- Stored regions are clipped to the volume by the filesystem modules. A file's stored bytes are also capped at the
  cluster area; more than that means runs that repeat clusters, and the excess becomes `Missing`
  (`AllocationDamaged`).
- A size larger than the volume (`SizeExceedsVolume`) is untrustworthy. Its `Zeros` become `Missing`, and nothing
  past the first cluster-area bytes of the file is kept.
- So every reconstruction is at most the size of the volume's cluster area (or the resident data). The random
  corruption tests check this on every candidate.

## Reconstruction (`reconstructCandidate`)

`reconstructCandidate(source, candidate, sink, options)` validates the candidate first (`validateCandidate`).
Candidates may later come from a saved session, so they are checked before use. It then delivers the data to
`sink` in file order.

- Stored regions are read in chunks of at most `chunkSize` (1 MiB; between 4 KiB and 64 MiB).
- If a chunk fails with an I/O error, it is read again **sector by sector**. Each sector gets `1 + sectorRetryCount`
  attempts. Sectors that still fail are zero-filled and reported, so one bad sector costs one sector of the file.
  Other read failures (source closed, invalid request) stop the reconstruction.
- `knownBadRegions` (for example from an image's metadata, where they were zero-filled) are not read, and count as
  unreadable.
- Bytes beyond the end of the source (a truncated image) count as unreadable (`outsideSourceBytes`).
- The `ReconstructionReport` gives the output size, the bytes stored, embedded, zero, missing, unreadable and
  reallocated, and the merged unreadable source ranges. The plan requires "Recovery results must indicate when
  unreadable regions affected a recovered file".
- The source is only read. Cancellation is checked for every region, chunk and sector.

The output size is the end of the last region that isn't `Missing`. Missing data inside the file is zero-filled,
so later data keeps its offsets (media parsers depend on them). Missing data at the end is left out rather than
written as zeros.

## Recovery output (`RecoveryWriter`)

`RecoveryWriter::create(source, destination, options)` normalises the destination to an absolute path. It checks
it with `checkDestinationSafety`: not a device path, not the source image, and for a physical disk not a volume on
that disk (fail-closed). Then it creates the directory and checks it again. A destination that is a file or a link
is refused.

`recover(candidate)`:

1. Validates the candidate. A candidate with data but not one located byte is refused (`InvalidInput`), instead of
   being written as an empty file.
2. Recreates the original directories below the destination (`preserveDirectories`, on by default). Every
   component goes through `safeFileName`. A name held by a file or a link gets a number (`DCIM (1)`), and links and
   junctions are never followed. Each new directory is checked with `checkDestinationSafety` again. An existing
   plain directory is shared.
3. Creates the file with `DestinationFile::CreateNew`. If the name is taken, it tries `name (1).ext`,
   `name (2).ext`, ... up to 10,000 times, so existing files are never overwritten. The check and the creation
   are atomic.
4. Writes the reconstruction, extends the file over trailing zeros, flushes it and checks its size. If any of
   this fails, or the operation is cancelled, the new file is deleted, so no half-written file is left behind.
5. Returns the path and the `ReconstructionReport`, and logs the file (candidate id, path, sizes, missing,
   unreadable and reallocated bytes; never content).

`safeFileName` makes an untrusted name one ordinary Windows path component:

- Control characters and `< > : " / \ | ? *` become `_`.
- Trailing dots and spaces are removed. `.`, `..` and names left empty become `_`, and an empty name becomes
  `unnamed`.
- Reserved device names (`CON`, `PRN`, `AUX`, `NUL`, `COM0-9`, `LPT0-9`, including the superscript digits, and
  `CONIN$`, `CONOUT$`, with or without an extension) get a leading `_`.
- Invalid UTF-8 becomes U+FFFD.
- Names longer than 255 UTF-16 units are shortened, keeping the extension and never splitting a surrogate pair.

Paths use the `\\?\` prefix, so deep original trees are not limited to `MAX_PATH`. This is safe because every
component is sanitised: Windows does not normalise `\\?\` paths.

Thread safety: `FilesystemRecovery` and `RecoveryWriter` are not thread-safe (one owner each). `reconstructCandidate`
only reads the source and may run concurrently on sources that allow concurrent reads.

## Tests

| File | Covers |
| --- | --- |
| `tests/recovery/filesystem_recovery_test.cpp` | FAT32, exFAT and NTFS candidates compared with the original files: contiguous, fragmented, deleted, partly overwritten, valid data length, resident, sparse, compressed and encrypted files; Unicode and duplicate names, orphans and hard links; detection, backup boot sectors, partitioned disks, cancellation, logging |
| `tests/recovery/recovery_writer_test.cpp` | Output tree, numbered duplicates, never overwriting, unsafe names, names held by files and links, missing data and zeros, failures leaving no file, bad sectors, paths beyond `MAX_PATH`, destination safety for image and physical-disk sources |
| `tests/recovery/recovery_reference_image_test.cpp` | End-to-end recovery of files written and deleted by ntfs-3g and written by pyfatfs (see [../testing/testing.md](../testing/testing.md)) |
| `tests/unit/recovery/*` | Reconstruction of hand-made candidates (every region kind, chunking, bad and known-bad sectors, retries, truncated sources, malformed candidates, cancellation, sink errors) and output names |
| `tests/corruption/filesystem_recovery_corruption_test.cpp` | Chain loops, cross-links, hostile sizes, invalid and repeating run lists, checksum mismatches, unreadable bitmaps and data sectors, and random mutation of all three filesystems with bounds checked on every candidate and reconstruction |

## Known limitations (P7)

All known limitations, with IDs for discussion, are listed in [../limitations.md](../limitations.md).

- Deleted FAT32 files, and deleted exFAT files whose chain was cleared, get a contiguous guess. P7 labels the guess
  and shows where it runs into other files, but tries no other layouts: fragment reconstruction (P13,
  [fragment_recovery.md](fragment_recovery.md)) does.
- "Reallocated" is evidence, not proof. An allocated cluster may still hold the old data. A free cluster may have
  been overwritten and freed again. Nothing in P7 validates content (P9-P14).
- Compressed and encrypted NTFS data is not recovered.
- Recovered files do not get their original timestamps or attributes back.
- Original directories whose names differ only in case, or that map to the same safe name, share one output
  directory. Their files still never overwrite each other.
- Each name of an NTFS hard link is its own candidate, so the data is recovered once per name.
- All candidates of a scan are kept in memory, and resident data is copied into them.
- After a failed chunk, reconstruction reads sector by sector. A region with many bad sectors is slow, so
  imaging the source first is preferable.
