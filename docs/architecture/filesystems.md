# Filesystem layer (P4: FAT32, P5: exFAT, P6: NTFS)

## Filesystem-independent model (`include/filesystem/filesystem.hpp`)

Recovery code only uses these types. It never touches on-disk structures of a particular filesystem.

| Type | Meaning |
| --- | --- |
| `FilesystemInfo` | Type, label, serial number, sector and cluster size, cluster count and first cluster number (2 on FAT, 0 on NTFS), data offset, warnings |
| `DirectoryEntry` | One listing item: name, 8.3 name (FAT32, NTFS), active/deleted, size, valid data length (exFAT, NTFS), first cluster (`ClusterNumber`), whether the data is recorded as one run (`contiguousData`), timestamps, attributes, where its metadata is, issues |
| `FileAllocation` | Where the data is: extents (volume byte ranges, in file order), how they were found (`ClusterChain` / `Contiguous` / `ContiguousGuess` / `RunList` / `Resident` / `None`), the data itself for `Resident`, and allocation issues |
| `FileRecord` | A `DirectoryEntry` with its path, its allocation, and whether its parent directory was deleted |
| `FileScan` | Every record below the root, scan issues, referenced and cross-linked cluster counts, and whether the scan finished |
| `ClusterUsage` | Free, allocated, bad, invalid and unreadable clusters; FAT copy mismatches; the free count the volume itself records |
| `IFilesystem` | `info`, `rootDirectory`, `readDirectory`, `resolveAllocation`, `scan`, `analyzeClusters`, `clusterState` |

P6 extended the model for NTFS without changing what FAT32 and exFAT report: the allocation methods `RunList`
and `Resident` with `FileAllocation::residentData`, NTFS allocation issues (`InvalidRunList`, `SparseRuns`,
`CompressedData`, `EncryptedData`, `AttributeListNotFollowed`, `DataAttributeMissing`), entry issues
(`ParentMissing`, `DamagedRecord`) and scan issue kinds (`RecordInvalid`, `RecordUnreadable`).

## FAT32 (`filesystem::fat32::Fat32Filesystem`)

### Opening

- The boot sector is validated field by field (`parseBootSector`). The result is
  `UnsupportedFilesystem` for anything that isn't FAT32 (including FAT12/16, exFAT and NTFS), and
  `CorruptedFilesystem` for impossible FAT32 values.
- If the primary boot sector is invalid, the backup at sector 6 is used, with a warning.
- The layout (FAT offset, data offset, cluster count) is computed with checked arithmetic. If the FAT is
  too small for the cluster count, the count is limited to what the FAT covers.
- Volumes with fewer than 65,525 clusters are accepted with a warning. They are non-standard but common
  on media formatted by non-Windows tools.
- FSInfo is read for the free-cluster count the volume records about itself.
- The label comes from the root directory's label entry if there is one, otherwise from the BPB.

### Allocation table access

- The FAT is read through a bounded LRU cache of 64 KiB pages (4 MiB by default), so memory use doesn't
  grow with volume size.
- If a page can't be read from the active FAT, it is re-read sector by sector, each sector from any FAT
  copy that can supply it. Only entries unreadable in every copy are reported as `Unreadable`.
- Entries stored beyond the end of the source (a truncated image) are counted as unreadable without any
  read attempts.

### Cluster chains

- Every cluster number is range-checked before use.
- A file's chain is followed for at most `ceil(size / clusterSize)` clusters. A directory's chain is
  followed for at most the FAT maximum of 65,536 entries (2 MiB).
- A cluster that repeats is reported as `ChainLoop`, and the walk stops.
- A chain that ends early (end-of-chain, free, bad or out-of-range value, or unreadable FAT) gets both
  that specific issue and `ChainShorterThanSize`.
- A chain that continues past the file size gets `ChainLongerThanSize`, and the extra clusters are not
  followed.
- **Cross-links:** a one-bit-per-cluster map records which clusters active entries own. Clusters claimed
  twice are reported, and every active entry touching them is flagged `CrossLinked`.

### Directories

- 8.3 names are decoded from code page 437, honouring the Windows lowercase flags (byte 12) and the 0x05
  escape.
- Active long names must have consecutive ordinals, a consistent checksum, and a checksum that matches
  the 8.3 name. Otherwise they are discarded (`LongNameChecksumMismatch`).
- **Deleted entries:** the first byte of every slot is 0xE5, so the long name's ordinals and the 8.3
  name's first character are lost. The long name is rebuilt from the adjacent deleted slots that share a
  checksum. The lost first character is taken from the long name, or `_`, and must reproduce the
  checksum. If it can't be verified, `LongNameUnverified` is set. If there is no long name, the first
  character becomes `_` (`NameReconstructed`).
- Deleted slots that can't be former entries (invalid name bytes, reserved attribute bits, out-of-range
  cluster) are ignored.
- Timestamps are decoded as local time with no zone (`Timestamp::local`). Out-of-range fields set
  `InvalidTimestamp`.

### Deleted files and directories

When Windows deletes a file it frees the cluster chain, so only the start cluster and size remain.
Deleted files therefore get `ContiguousGuess` allocations, which are flagged:

- `ClustersInUse` when any guessed cluster is allocated again;
- `BeyondVolume` when the guess would run past the last cluster.

A deleted directory is listed only if its first cluster is still free and still starts with a `.` entry
that points to itself. The files found inside it have `parentDeleted = true`.

### Traversal limits

The traversal is depth-first and limited by `ScanLimits`: maximum depth (64), maximum number of entries
(10 million), whether to include deleted entries, and whether to go into deleted directories. Directory
cycles (entries pointing to an ancestor) are reported as `DirectoryLoop` and not followed. Cancellation is
checked for every directory.

## Known limitations (P4)

All known limitations, with IDs for discussion, are listed in [../limitations.md](../limitations.md).

- Deleted fragmented files come back as a single contiguous guess, which is wrong for them. Fragment
  reconstruction (P13, [../recovery/fragment_recovery.md](../recovery/fragment_recovery.md)) resolves them.
- Some Windows versions clear the high 16 bits of a deleted entry's start cluster. On volumes with more
  than 65,535 clusters, such entries point to the wrong cluster. This is not corrected yet.
- Only the first cluster of a deleted directory is read.
- Orphaned clusters (allocated, but not referenced by any entry) can be counted as
  `ClusterUsage::allocated - FileScan::referencedClusters`, but the orphaned chains are not listed yet.
- The engine's own tests use volumes built by `tests/support/fat32_builder`. Independent checks are
  described in [../testing/testing.md](../testing/testing.md).

## exFAT (`filesystem::exfat::ExFatFilesystem`)

### Opening

- The main boot region (sectors 0-11) is validated field by field (`parseBootSector`) and by its checksum:
  sector 11 repeats a checksum of sectors 0-10. If either check fails, the backup boot region (sectors 12-23) is used
  when it is valid, with a warning. If no checksum matches but a boot sector parses, it is used with a warning.
- The result is `UnsupportedFilesystem` for anything without the `EXFAT` name (FAT, NTFS) and for revisions other than
  1.x. It is `CorruptedFilesystem` for impossible values: sector or cluster shift out of range, FAT inside the boot
  regions or overlapping the cluster heap, heap beyond the volume, no clusters, root cluster out of range.
- The cluster count is limited to what fits in the heap, in the FAT, and under the 2^32 − 11 maximum, with a warning.
- A dirty volume, or one that records media failures, opens with a warning. With two FATs (TexFAT), the active-FAT
  flag selects the FAT and the allocation bitmap.
- The root directory is read once to find the allocation bitmap, the up-case table and the volume label:
  - **Allocation bitmap:** the entry for the active FAT. If there is none, allocation is unknown: every cluster state
    is `Unreadable` and every allocation gets `UnreadableAllocationTable`. Clusters beyond a short bitmap are
    `Unreadable`.
  - **Up-case table:** read, checksum verified, then decoded (compressed or not). If it is missing, too large,
    unreadable or fails its checksum, a basic built-in table (ASCII and Latin-1) is used instead, with a warning.
    Hashes of names with other letters then can't be verified (`LongNameUnverified`).
  - Both are located through their FAT chains. If a chain is broken, the structure is assumed to be contiguous, with
    a warning.

### FAT and allocation bitmap

- Unlike the FAT32 FAT, the exFAT FAT does not record free space; the allocation bitmap does. `clusterState` and
  `analyzeClusters` use the bitmap. A FAT entry of 0xFFFFFFF7 marks a bad cluster.
- FAT entries: 2 to the last cluster = next cluster, 0xFFFFFFF7 = bad, higher = end of chain. 0, 1 and other
  out-of-range values are invalid. Entries of NoFatChain clusters are undefined and never read.
- Both are read through a bounded LRU cache of 64 KiB pages (4 MiB each by default). A page that can't be read in one
  piece is re-read sector by sector, and only the sectors that still fail are unreadable. Pages stored beyond the end
  of the source (a truncated image, or a boot sector claiming a huge volume) are unreadable without any read attempt.
- The second FAT of a TexFAT volume is a transaction copy, not a mirror. It is never used as a fallback, and
  `mirrorMismatches` is always 0.
- Bitmap range checks count 64-bit words at a time, skip unreadable pages whole, and stop at the first cluster in the
  state they look for.
- exFAT records no exact free count, so `ClusterUsage::recordedFree` is empty.

### Entry sets

Each file or directory is an entry set: a File entry (attributes, timestamps), a Stream Extension (NoFatChain flag,
valid data length, first cluster, size, name length, name hash), 1-17 File Name entries of 15 UTF-16 units each, and
optional benign secondaries such as vendor extensions, which are skipped.

- The set checksum covers every byte of the set except the checksum itself. Active sets with a wrong checksum are
  still listed, flagged `EntrySetChecksumMismatch`. So are active sets cut short (secondary count larger than the
  entries present), as long as the Stream Extension and every name entry are there.
- Sets without a Stream Extension, with a name length of zero, with too few name entries or with an unknown critical
  secondary entry are not listed. Each is reported as a `DirectoryInvalid` scan issue, as are unknown critical primary
  entries and secondary entries without a primary.
- The name must match the name hash computed with the volume's up-case table (`NameHashMismatch`). Names with
  characters exFAT forbids, and "." and "..", are flagged `InvalidName`.
- Timestamps have a 10 ms increment and a UTC offset. With a valid offset the time is converted to UTC
  (`Timestamp::local = false`). Without one it is local time. Out-of-range fields set `InvalidTimestamp`.
- `validDataLength` is reported. Data between it and the size reads as zeros on a live system.
  `ValidDataLengthExceedsSize` flags a valid data length larger than the size.
- An entry set cannot continue across an unreadable sector. The part before the gap is treated as a set cut short.
- Directories are read in 64 KiB chunks, up to the specification's limit of 256 MiB. `readDirectory` returns at most
  `ExFatOptions::maxListingEntries` entries (1 million by default), then an `EntryLimit` issue.

### Allocation

| Entry | Method | Extents | Bitmap check |
| --- | --- | --- | --- |
| Active, NoFatChain | `Contiguous` | One run from the first cluster, clipped to the heap (`BeyondVolume`) | `ClustersMarkedFree` if a cluster is free |
| Active, FAT-chained | `ClusterChain` | The chain, walked as on FAT32: loops, invalid, bad and unreadable entries, chain shorter or longer than the size | `ClustersMarkedFree` |
| Deleted, NoFatChain | `Contiguous` | The exact run, which deletion does not change | `ClustersInUse` if a cluster was reused |
| Deleted, FAT-chained, chain intact | `ClusterChain` | The old chain. Deletion only has to update the bitmap, so the chain may survive. It is used only if it has exactly the right length and ends with end-of-chain | `ClustersInUse` |
| Deleted, FAT-chained, chain gone | `ContiguousGuess` | A run from the first cluster, as on FAT32 | `ClustersInUse` |

Directories follow the same rules in whole clusters. Unlike FAT32 directories they have a size, which their chains
must match. The root has no Stream Extension, so its chain is followed like a FAT32 directory's. An unreadable bitmap
adds `UnreadableAllocationTable`.

### Deleted entries

Deleting a file clears bit 7 (InUse) of every entry type in its set (0x85 → 0x05, 0xC0 → 0x40, 0xC1 → 0x41) and frees
its clusters in the bitmap. Nothing else in the set changes.

- Name, size, first cluster, flags and timestamps all survive. A deleted set is verified with its checksum, computed
  with the InUse bits set again. If the checksum fails, the set is dropped, since its fields may belong to different
  files.
- **Orphaned entries:** a new entry set can overwrite the File entry of a deleted set and leave its Stream Extension
  and File Name entries behind. These are listed when the name matches the name hash, flagged `MetadataIncomplete`:
  attributes and timestamps are unknown, and the entry is reported as a file. Deleted sets that fail their checksum
  are tried the same way.
- Deleted-looking slots that pass neither check are ignored.
- A deleted directory is read only if none of its clusters is allocated again and the bitmap is readable. Files found
  in it have `parentDeleted = true`. Entries inside it that are still marked in use are reported as deleted.

### Traversal and cross-links

Traversal, limits, loop detection and cancellation work as on FAT32. Cross-links are found from cluster runs rather
than from a per-cluster map, because an exFAT volume can have 2^32 clusters: the runs of the system structures, the
root and every active entry are sorted and merged, so memory grows with the number of runs. `referencedClusters`
includes the allocation bitmap and up-case table, so `ClusterUsage::allocated − FileScan::referencedClusters` is the
number of orphaned clusters.

## Known limitations (P5)

All known limitations, with IDs for discussion, are listed in [../limitations.md](../limitations.md).

- Deleted FAT-chained files whose chain was cleared come back as a contiguous guess, as on FAT32. Fragment
  reconstruction (P13) resolves them.
- Orphaned entries (`MetadataIncomplete`) don't say whether they were directories. A deleted directory found this way
  is reported as a file and not searched.
- Vendor allocation entries (benign secondaries that own clusters) are skipped, so their clusters count as orphaned.
- TexFAT support stops at choosing the active FAT and bitmap. Transactions are not examined.
- The volume GUID and the OEM parameters sector are not reported.
- The only independent exFAT implementation used for cross-checking is exfatprogs (see
  [../testing/testing.md](../testing/testing.md)). No image with files written by Windows has been tested yet.

## NTFS (`filesystem::ntfs::NtfsFilesystem`)

P6 is the NTFS foundation: boot sector, MFT discovery, FILE records, `$FILE_NAME` and `$DATA` attributes, resident
and non-resident data, data runs and deleted files. What is not supported yet is flagged, never silently wrong (see
the limitations below).

### Opening

- The boot sector is validated field by field (`parseBootSector`). Bytes per sector must be 512-4096. Sectors per
  cluster is a power of two up to 128, or 256 − log2 for clusters above 64 KiB (at most 2 MiB). The MFT record size
  uses the "clusters per record" encoding (a negative value is −log2 of the size in bytes) and must be a power of two
  from 512 bytes to 64 KiB. The MFT cluster must lie inside the volume. The result is `UnsupportedFilesystem` for
  anything without the `NTFS` OEM name and `CorruptedFilesystem` for impossible values. Non-zero FAT fields, an
  unusual jump instruction, a mirror outside the volume and a volume larger than its source are warnings.
- The backup boot sector is the last sector of the volume, just after the sectors the boot sector counts. It is used
  when the primary is invalid, and when the primary describes an MFT that cannot be used while the backup describes a
  different layout.
- **MFT discovery:** record 0 (`$MFT`) at the MFT cluster must pass its fixups, be an in-use base record, and have a
  non-resident unnamed `$DATA` whose first run starts at the MFT cluster. Otherwise its copy in `$MFTMirr` is used
  (warning, `usedMftMirror()`). If neither is usable, the result is `CorruptedFilesystem`.
- The MFT is the run list of that `$DATA`, up to its initialized size. A hole, a run outside the volume or a damaged
  run list ends it, with a warning. If `$MFT` has an attribute list, only the runs stored in record 0 are followed
  (warning). At most `NtfsOptions::maxRecords` records (16,777,216) are examined.
- `$Volume` (record 3) gives the label, the version (a warning unless 3.x) and the dirty flag (warning). `$Bitmap`
  (record 6) is the cluster bitmap. The stored runs of `$BadClus:$Bad` (record 8) are the bad clusters. If `$Bitmap`
  is unusable, every cluster is `Unreadable` and every allocation gets `UnreadableAllocationTable`.

### FILE records

- **Fixups:** the last two bytes of every 512-byte block (whatever the sector size) must hold the update sequence
  number; the saved bytes are then put back. A mismatch (a torn write) or an invalid update sequence array rejects
  the record. Resident data is therefore returned as bytes (`residentData`), never as extents: on disk, two bytes of
  every block belong to the update sequence.
- **Header:** `FILE` signature (`BAAD` is rejected), allocated size equal to the record size, used size within it,
  first attribute after the update sequence array. NTFS 3.1 stores the record's own number, which must match its
  slot. A free record numbered 0 is accepted, because mkntfs formats its reserved records 16-23 that way (found by
  the cross-check with ntfs-3g).
- **Attributes:** every length, name, value and run list offset is bounds-checked. A break in the chain (a length
  below 16 or beyond the used size, or no end marker) keeps the attributes before it. A malformed attribute
  (unknown form code, name or value outside the attribute) is skipped. The entry is flagged `DamagedRecord`, and a
  `RecordInvalid` scan issue gives the detail.
- **Run lists** (`decodeRunList`): offsets are signed and relative to the previous stored run; a pair without an
  offset is a hole. Decoding stops at a truncated list, a zero length, a field wider than 8 bytes, a run before
  cluster 0, or a VCN overflow, and keeps the runs before it. Every pair takes at least two bytes, so a list cannot
  hold more runs than half its size.
- Invalid records are reported one by one up to 100, then counted in one further issue. Unreadable records (bad
  sectors, or records beyond the end of the source) are counted in one `RecordUnreadable` issue. Records beyond the
  end of the source are never read.

### Directories and deleted files

The MFT is read once, sequentially through 64 KiB cache pages, into a catalog of the named base records. Directories
are rebuilt from the parent reference (record number and sequence number) in each `$FILE_NAME`, not from the
directory indexes. Deleting a file removes it from its directory's index but leaves its record, so this finds
deleted files too.

- A reference belongs to a directory when it carries the directory's sequence number. Deleting a record increments
  its sequence number (skipping zero), so the entries of a deleted directory carry the previous number; both are
  accepted for deleted directories. ntfs-3g was checked to increment it. A reused record has a different sequence
  number and does not match.
- Each `$FILE_NAME` other than a DOS name is an entry in its parent, so a hard link gives several entries sharing one
  record (not a cross-link). A DOS name becomes the `shortName` of the long name in the same directory; a Win32 & DOS
  name is its own short name. Empty names, `.`, `..`, and names containing `/` or NUL are flagged `InvalidName`.
- Entries no directory holds any more (the parent record was reused, is not a directory, is missing, or the parents
  form a cycle) are listed under the virtual directory `/$OrphanFiles`, flagged `ParentMissing`, with their subtrees.
- **A deleted record keeps everything:** names, times, size and data runs. Its runs are exact, not a guess as on
  FAT32: the method is `RunList`, flagged `ClustersInUse` if `$Bitmap` shows that a cluster was allocated again.
  Resident data survives in the record itself.
- A record in use under a deleted directory keeps its own state (every NTFS record has its own in-use flag), with
  `parentDeleted` set.
- The metadata files (`$MFT`, `$Bitmap`, `$Extend`, ...) are listed in the root like any other entry.

### Allocation

| Data | Method | Extents | Issues |
| --- | --- | --- | --- |
| Resident `$DATA` | `Resident` | None; `residentData` holds the bytes | |
| Non-resident `$DATA` | `RunList` | The stored runs, in file order, up to the size; adjacent runs merged | `InvalidRunList`, `InvalidClusterInChain`, `BeyondVolume` (clipped), `ChainShorterThanSize`, `SizeExceedsVolume`; `ClustersMarkedFree` (active) or `ClustersInUse` (deleted); `UnreadableAllocationTable` |
| Sparse, compressed or encrypted `$DATA` | `RunList` | The stored runs; holes are skipped, compressed and encrypted bytes are as stored | `SparseRuns`, `CompressedData`, `EncryptedData` |
| Empty file | `None` | None | |
| No unnamed `$DATA` | `None` | None | `AttributeListNotFollowed` with an attribute list, otherwise `DataAttributeMissing` (not for view indexes such as `$Secure`) |
| Directory | `RunList` for `$INDEX_ALLOCATION:$I30` (whole clusters), `None` when the index fits in the record | | As for files |

Runs beyond the size (preallocated clusters) belong to the file but are not data. `DirectoryEntry::size` is the real
size, `validDataLength` the initialized size (`ValidDataLengthExceedsSize` if larger), `firstCluster` the first stored
cluster, `contiguousData` true for a single run. Times and attributes come from `$STANDARD_INFORMATION` and are UTC
(`Timestamp::local = false`); without it the entry is flagged `MetadataIncomplete`. `metadataOffset` is the volume
offset of the FILE record: `readDirectory` and `resolveAllocation` use it to find the record again, and
`NtfsFilesystem::recordNumberAt` (added in P7 for recovery) maps it back to the record number.

### Clusters and cross-links

- `clusterState` and `analyzeClusters` use `$Bitmap`; clusters listed in `$BadClus` are `Bad`. NTFS records no free
  count, so `recordedFree` is empty. Clusters whose bitmap bits lie beyond the end of the source are counted as
  unreadable without being visited, so a boot sector claiming a huge volume costs nothing.
- Every non-resident attribute of every in-use record (base and extension records) claims its clusters. The sorted
  runs are merged as on exFAT, giving `referencedClusters` and `crossLinkedClusters`, and active entries whose data
  overlaps a conflict are flagged `CrossLinked`. `ClusterUsage::allocated − FileScan::referencedClusters` is the
  number of orphaned clusters; it is 0 on every ntfs-3g reference image.

### Traversal

`ScanLimits` work as on FAT32 and exFAT: the root is listed first, depth-first in MFT order, then the orphans. A
directory reached through a second name is reported as `DirectoryLoop` and not entered again. The first call to
`readDirectory` or `scan` builds the catalog (the whole MFT); `readDirectory` returns at most
`NtfsOptions::maxListingEntries` entries. Cancellation is checked for every MFT page and every directory.

## Known limitations (P6)

All known limitations, with IDs for discussion, are listed in [../limitations.md](../limitations.md).

- Attribute lists are not followed. Data stored in extension records is missing (`AttributeListNotFollowed`), and a
  `$MFT` with an attribute list is only read as far as record 0 describes it.
- Sparse, compressed and encrypted data is flagged, not reconstructed.
- Named data streams, directory indexes (including entries left in index slack), `$LogFile` and `$UsnJrnl` are not
  read.
- Only record 0 falls back to `$MFTMirr`.
- The first directory listing reads the whole MFT, and the catalog stays in memory.
- The cross-check uses ntfs-3g only; no image written by Windows has been tested yet (see
  [../testing/testing.md](../testing/testing.md)).
