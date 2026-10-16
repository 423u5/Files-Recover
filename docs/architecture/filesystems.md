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

After P20 (L10): every module builds `FileRecord::path` from `pathComponent()` of each name, which puts `_` in place
of a `/` (only a damaged name holds one, and it is flagged `InvalidName`): every `/` of a path separates two names,
and a damaged folder name is one level of the path. The entry keeps its name. `kOrphanDirectory` (`$OrphanFiles`)
names the virtual folder under the root that lists entries whose folder is gone: NTFS records whose parent is gone,
and parts of deleted FAT32 folders whose folder is not known (L243).

## FAT32 (`filesystem::fat32::Fat32Filesystem`)

### Opening

- The boot sector is validated field by field (`parseBootSector`). The result is
  `UnsupportedFilesystem` for anything that isn't FAT (exFAT and NTFS among them), and
  `CorruptedFilesystem` for impossible values. Since after P20 the module reads FAT16 and FAT12 too (see
  "FAT16 and FAT12" below).
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
- Active slots that are clearly other data are skipped too (P20, found by the corruption corpus: a folder
  cluster overwritten with a photo listed some 30 junk files): an invalid 8.3 name *and* reserved attribute bits,
  a first cluster outside the volume, or a file size beyond the volume. The folder reports how many with a
  `DirectoryInvalid` scan issue ("N slots hold no directory entry (other data in the directory's clusters)"). An
  active slot with an invalid 8.3 name alone is still listed and flagged `InvalidShortName` (see L200 in
  [../limitations.md](../limitations.md)).
- A name Windows cannot create on FAT is flagged `InvalidName` (after P20, L10): empty, `.` or `..`, or holding one
  of `"*/:<>?\|` or a control character (the characters Windows forbids in long names).
- Timestamps are decoded as local time with no zone (`Timestamp::local`). Out-of-range fields set
  `InvalidTimestamp`.

### Deleted files and directories

When Windows deletes a file it frees the cluster chain, so only the start cluster and size remain.
Deleted files therefore get `ContiguousGuess` allocations, which are flagged:

- `ClustersInUse` when any guessed cluster is allocated again;
- `BeyondVolume` when the guess would run past the last cluster.

A deleted directory is listed only if its first cluster is still free and still starts with a `.` entry
that points to itself. The files found inside it have `parentDeleted = true`.

**Start clusters that lost their top 16 bits (after P20, L7).** Windows also clears bytes 20-21 of a deleted
entry, the top 16 bits of its start cluster (FAT32; seen on Windows 10, for files and folders, on an image it
wrote, L21). On a volume of more than 65,535 clusters
(`BootSector::deletedStartsAmbiguous`) such a deleted entry may start at any of several clusters 65,536 apart, and
the one recorded is the lowest. When a directory is listed, `DeletedEntries::inferStarts` places each such entry by
the strongest evidence there is, and keeps the start recorded when the evidence does not single one place out:

1. A folder by its first cluster: the one place, free, whose `.` entry names that cluster and whose `..` entry
   names the folder being listed (0, or the root's cluster, for the root).
2. A file by its neighbours in the listing, because files written one after the other lie one after the other: it
   starts where the entry before it ends (an active file's chain says where; the first entry starts after the
   folder's first cluster), or ends where the entry after it starts, when the low 16 bits agree. The folder's own
   clusters are skipped: a folder grows between the files written into it.
3. A run of files whose low bits say that they lie one after the other, by what starts at each place
   (`Fat32Options::startsFile`, which a scan gives from its formats: `carving::startsFileNamed`): the one place
   where their starts are mostly free, and where the first four whose names have a known type each start a file
   of that type. Of several such places, the one where the first of them ends exactly at its size (after P20, L242:
   `Fat32Options::endsFile`, from the scan's formats: `carving::endsFileNamed`, which runs the format's end
   detection from there, reading at most 64 KiB beyond the size). Otherwise the place recorded is kept when it
   qualifies too.
4. A file whose place recorded holds other data now (after P20, L242): more than half of its clusters are in use,
   or its first cluster is free and what starts there cannot start the file (no file of its type, or no text for a
   text file's name: `.txt`, `.csv`, `.log`, `.xml`, `.json`, source code and others). It goes to the one other
   place whose clusters are all free, whose first bytes are not blank (all 0x00 or all 0xFF: never written) and
   where nothing contradicts its name; of several, the one between the files around it in the listing (where the
   one before it ends and the one after it starts). A place recorded that may be the file's is kept: a file whose
   first cluster alone was taken since stays where the rest of it is. On a volume of more than 16.7 million clusters
   (more than 256 places), this step is left out: each place costs a read.

Step 2 is taken again after steps 3 and 4, around the files they placed.

An entry placed elsewhere than recorded is flagged `StartClusterInferred`, and its candidate's layout is `Guessed`
(`LayoutGuessed`) even for one cluster. A deleted entry whose recorded start reads as cluster 1 is kept as what is
left of a start 65,536 clusters on. Starts that kept their top bits (other systems delete so), and volumes of up to
65,535 clusters, are taken as recorded.

**A deleted folder's later clusters (after P20, L8).** A deleted folder's chain is gone too, so after its first
cluster its clusters are unknown. When the first cluster is full (no end marker), `DeletedEntries::nextCluster`
looks for the next one where writers put it: Windows and cameras write a new entry before the new file's data, so
a folder that fills up grows at the place after the data of the file written before, that is, after the last file
its clusters so far list; failing that, right after its previous cluster. When that last file's start lost its top
bits, every place it may have ended at is tried, and taken only if one alone qualifies. A cluster qualifies when it
is free, is not one of the folder's clusters yet, and holds directory slots only (`isPossibleSlot`: long-name slots
with their type and cluster fields zero, 8.3 entries with possible names, attributes, clusters and sizes), at least
one, and no `.` entry (another folder's first cluster). The search ends at the first cluster that ends the folder, at
one it cannot find, and at the FAT limit of 65,536 entries.

**Parts of deleted folders found apart from them (after P20, L243).** A folder that grew while other programs wrote
elsewhere, or whose first cluster was taken by other data, has clusters the search above cannot reach. Once a scan
has listed every folder, it looks for them among the free clusters right after the data of any file it listed
(active or deleted) and right after any folder's cluster, which is where a writer's next free cluster was when a
folder grew: a cluster that no folder listed and that holds folder slots only (as above) starts a part, which is
followed where it grew in turn. A part goes to the deleted folder that leads to it, among those whose search ended
at a full cluster:

- the data and folder clusters the scan listed lead from that folder's last file's data, or from its last cluster,
  to the part, one right after the other (what other programs wrote meanwhile);
- or a folder listed in the part names that folder's first cluster in its `..` entry.

Its entries are then read on from the folder's last cluster, so that a long name begun there is whole, placed as
in a listing of that folder (the first one's data right after the part's first cluster), and listed under the
folder's path. A part that no folder leads to, or that several do, is listed under `/$OrphanFiles`, its entries
flagged `ParentMissing`. Only `scan()` looks for parts: `readDirectory` lists a folder alone.

### Traversal limits

The traversal is depth-first and limited by `ScanLimits`: maximum depth (64), maximum number of entries
(10 million), whether to include deleted entries, and whether to go into deleted directories. Directory
cycles (entries pointing to an ancestor) are reported as `DirectoryLoop` and not followed. Cancellation is
checked for every directory.

### FAT16 and FAT12 (after P20)

The same module reads FAT16 (the user's production-readiness list of 2026-10-12) and FAT12 (the user's
decision of 2026-10-14, L225 in [../limitations.md](../limitations.md)): cards of 2 GB and less, older USB
sticks, floppy disks and volumes of a few megabytes. They differ from FAT32 in their layout, not in their
directories:

- A boot sector with a 16-bit FAT size or root entry count is FAT12 or FAT16. The count of data clusters
  decides which, as the specification says: fewer than 4,085 is FAT12, up to 65,524 FAT16, more is corrupt.
  The type label at offset 54 is not trusted. Errors found before the count is known name both ("FAT12/16
  boot sector: ...").
- The root directory is a fixed region of `rootEntries` slots between the FATs and cluster 2, read sector by
  sector, so an unreadable sector loses only its slots. Directory entries call it cluster 0, and so does the
  module (`rootDirectory()`).
- FAT16 entries are 16 bits. FAT12 entries are 12 bits, two in three bytes: an even entry is the low 12 bits
  of the 16 at its first byte, an odd one the high 12. Both are widened to the FAT32 values of the same
  meaning (free, next cluster, bad 0xFFF7 or 0xFF7, end of chain from 0xFFF8 or 0xFF8).
- A FAT12 entry can lie across two FAT sectors (entries 341 and 682 of every 1,024, with 512-byte sectors).
  When a page of the FAT is put together sector by sector, such an entry is unreadable if either of its
  sectors is unreadable in every copy.
- Directory entries hold only the low 16 bits of a start cluster (bytes 20-21 are reserved there).
- There is no FSInfo, and normally no backup boot sector. The copies of the FAT have no flag for one active
  copy, so they are read as mirrors.
- `FilesystemInfo::type` is `Fat16` or `Fat12`. Both are appended to `FilesystemType`, so that the values
  sessions recorded keep their meaning; the API's `FilesystemKind` is `fat16` or `fat12`.

Tests: `tests/filesystem/fat16_filesystem_test.cpp` and `fat12_filesystem_test.cpp` (built volumes, a
1.44 MB floppy disk among them), the corpus scenarios `fat16-card` and `fat12-floppy`, and images written by
mkfs.fat and mtools (`tests/reference/make_mtools_fat_reference.py`, see
[../testing/testing.md](../testing/testing.md)).

## Known limitations (P4)

All known limitations, with IDs for discussion, are listed in [../limitations.md](../limitations.md).

- Deleted fragmented files come back as a single contiguous guess, which is wrong for them. Fragment
  reconstruction (P13, [../recovery/fragment_recovery.md](../recovery/fragment_recovery.md)) resolves them.
- A deleted entry whose start lost its top 16 bits stays as recorded when the places it may start at all look alike
  (L248; L7 and L242 are resolved).
- The parts of deleted folders found apart from them are looked for only right after data or folder clusters the
  scan listed; a part no folder alone leads to is listed under `/$OrphanFiles`; of two deleted folders that grew
  next to each other, one may list the other's entries as well (duplicates; L249; L8 and L243 are resolved).
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
| Deleted, FAT-chained, chain intact | `ClusterChain` | The old chain. Deletion only has to update the bitmap, so the chain may survive (Windows 10 leaves it, seen after P20, L12). It is used only if it has exactly the right length and ends with end-of-chain | `ClustersInUse` |
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
  and File Name entries behind (Windows 10 does so when a folder has no room left at its end; where there is room
  it appends the new set, seen after P20). These are listed when the name matches the name hash, flagged `MetadataIncomplete`:
  attributes and timestamps are unknown. Deleted sets that fail their checksum are tried the same way. Such an entry
  is a file unless its data shows that it was a directory (after P20, L13): its size is whole clusters, its valid
  data length its size, and the first 4 KiB of its data that are still free, among its first four clusters, hold at
  least one File entry set, in use or deleted, whose checksum matches, or orphaned Stream Extension and File Name
  entries whose name matches its hash (L244), and no entry a directory other than the root cannot hold
  (`holdsEntrySets`). It is then listed as a directory and searched like any deleted one.
- Deleted-looking slots that pass neither check are ignored.
- A deleted directory is read where its clusters are still free (after P20, L244; until then one cluster allocated
  again lost all of its entries): the clusters in use again, or whose state the bitmap cannot tell, are left out,
  with a `DirectoryInvalid` scan issue that counts them, and no entry set continues across them. It is refused when
  none of its clusters is free, or the volume has no allocation bitmap. Files found in it have
  `parentDeleted = true`. Entries inside it that are still marked in use are reported as deleted.

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
- A deleted directory linked by a FAT chain whose first cluster is in use again cannot be followed: it is read as
  one run from there, where free; an orphaned entry whose first free clusters hold no entries stays a file (L250;
  L13 and L244 are resolved).
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
  name is its own short name. Empty names, `.`, `..`, and names containing `/` or NUL are flagged `InvalidName`
  (a `/` is `_` in the path, L10).
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
