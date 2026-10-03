# Disk imaging (P2)

`imaging::ImageWriter` copies an open `IStorageSource` into a raw image file. It reads the source
sequentially and never writes to it.

## Read flow and bad sectors

```
read block (blockSize, default 1 MiB)
  └─ IoError / PartialRead
       read each intermediate piece (default 64 KiB)
         └─ failure
              read each sector
                └─ failure → retry up to sectorRetryCount times (default 2)
                     └─ still failing → zero-fill the sector in the image,
                                        record a BadRegion, continue
```

- A single read error never stops imaging. Only non-I/O failures stop it: an invalid request, a closed
  source, or a destination write failure. In that case the metadata is saved with state `failed`, and the
  image can be resumed.
- Unreadable sectors are zero-filled in the image. The metadata records exactly which byte ranges those are.
- `BadRegionMap` merges runs of adjacent sectors that have the same error code.

## Files

| File | Contents |
| --- | --- |
| `<name>` | Raw image. Bytes `[0, bytes_completed)` are final. |
| `<name>.imgmeta` | Metadata (below). Replaced atomically through `<name>.imgmeta.tmp`. |

A new image is never created over an existing image or metadata file.

## Checkpoints and resume

Every `checkpointIntervalBytes` (default 64 MiB), and at the end, the writer:

1. flushes the image (`FlushFileBuffers`);
2. writes the metadata with the new `bytes_completed` and the bad regions so far.

Because the data is flushed first, the metadata never claims more than the disk holds. On resume
(`ImagingOptions::resume = true`), the writer:

- requires metadata whose source type, path, size and sector size match the current source;
- refuses images in state `completed`;
- refuses an image file shorter than `bytes_completed`;
- truncates the image to `bytes_completed`, since data after the last checkpoint was never committed;
- reloads the recorded bad regions and continues from `bytes_completed`.

When imaging is cancelled or fails mid-block, that block is discarded and read again on resume. Metadata
only ever lists bad regions inside `[0, bytes_completed)`, the part of the image that is committed.

## Metadata format (version 1)

A UTF-8 text file with one `key=value` per line. Lines starting with `#` are comments. Unknown keys are
ignored.

```
# RecoveryEngine disk image metadata. Generated file; do not edit.
format_version=1
engine_version=0.1.0
state=in_progress | completed | cancelled | failed
source_type=PhysicalDisk | DiskImage | Synthetic
source_path=\\.\PhysicalDrive2
source_vendor=...
source_product=...
source_size=<bytes>
sector_size=<bytes>
block_size=<bytes>
bytes_completed=<bytes>
started_utc=2026-09-19T08:00:00.000Z
updated_utc=2026-09-19T08:05:00.000Z
bad_region=<offset>,<length>,<win32 error code>     (repeated)
```

The parser treats the file as untrusted input:

- the file may be at most 64 MiB, each line at most 4096 bytes, with at most 1,000,000 `bad_region` lines;
- numbers must be plain decimal and must not overflow;
- duplicate keys are rejected;
- `bytes_completed` may not exceed `source_size`;
- bad regions must lie inside the source.

## Destination safety

Before anything is created, `storage::checkDestinationSafety` rejects:

- device paths;
- the source image file itself (compared by normalized path and by file identity);
- for physical sources, any destination whose volume is on the source disk, or whose disk can't be
  determined.

## Known limitations

All known limitations, with IDs for discussion, are listed in [../limitations.md](../limitations.md).

- No image hash (for example SHA-256) is computed yet. Integrity is currently checked by the tests
  comparing bytes.
- Imaging uses one thread, and progress is reported at most every `progressInterval`.
- The metadata replace is only atomic on NTFS and ReFS destinations.
- Only raw images are written. There is no E01 or other forensic container format.
