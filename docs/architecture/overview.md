# Architecture overview

This file covers what is implemented (P0–P15). The target architecture for all phases is in the project plan.

## Modules and dependencies

```
tools/recovery_cli  ──► recovery_core
recovery_imaging    ──► recovery_storage ──► recovery_core
recovery_partition  ──► recovery_storage
recovery_filesystem ──► recovery_storage
recovery_candidates ──► recovery_filesystem, recovery_partition (boot record signatures only)
recovery_carving    ──► recovery_storage
recovery_formats    ──► recovery_carving
recovery_mp4        ──► recovery_candidates, recovery_formats
recovery_fragments  ──► recovery_mp4
recovery_validation ──► recovery_formats
recovery_playability ─► recovery_validation (Windows Imaging Component, Media Foundation)
recovery_evaluation ──► recovery_validation, recovery_mp4, recovery_fragments
recovery_scan       ──► recovery_evaluation, recovery_partition
```

The partition and filesystem modules are independent of each other, and carving is independent of both: it reads
any source (a disk, an image or a partition) by content alone. A filesystem is opened on any
`IStorageSource`: a `PartitionSource` for one partition, or the whole device when it is unpartitioned.
Recovery candidates record source offsets (volume offset plus offset in the volume), so they are
reconstructed from the whole device or image. `recovery_mp4` is the first module to use filesystem candidates and
carving together; the two stay independent of each other below it. `recovery_fragments` (P13) builds on it:
fragment reconstruction takes filesystem candidates and carves as seeds and validates every layout it tries with
the formats. P14 adds the validation levels (`recovery_validation`: the structural verdict and the engine's own media
decoders; `recovery_playability`: optional platform decoders) and `recovery_evaluation`, where every stage's
candidates end: one per file, validated at every level and identified by SHA-256. P15's `recovery_scan` runs them
all as one scan: on a bounded pool of workers, with one pass over the source that every carving stage shares,
cancellable, pausable and resumable from the updates it hands out; and writes candidates out the same way. The
stages it drives gained step interfaces for it (`CarveSkipState`, `Mp4RecoverySteps`, `FragmentRecoverySteps`, the
evaluation's pool and resume records); their own `run()` drives the same steps.

| Library | Namespace | Responsibility |
| --- | --- | --- |
| `recovery_core` | `recovery`, `recovery::diagnostics` | `Result<T>`/`Status`, `Error`/`ErrorCode`, strong types, checked arithmetic, bounds-checked byte access, CRC-32, SHA-256, UTF-16 conversion, cancellation, configuration, logging; the bounded worker pool, job control (cancel, pause) and `runOrdered` of P15 |
| `recovery_storage` | `recovery::storage` | Read-only sources, bad-region map, destination file, destination guard |
| `recovery_imaging` | `recovery::imaging` | Imaging a source into a raw image, with resumable metadata |
| `recovery_partition` | `recovery::partition` | MBR/GPT/superfloppy detection, and a `PartitionSource` view that confines reads to one partition ([partitions.md](partitions.md)) |
| `recovery_filesystem` | `recovery::filesystem` | The filesystem-independent model and `IFilesystem`, plus FAT32, exFAT and NTFS ([filesystems.md](filesystems.md)) |
| `recovery_candidates` | `recovery` | Recovery candidates, filesystem-based recovery for FAT32, exFAT and NTFS, reconstruction of candidate data, and recovery output ([../recovery/filesystem_recovery.md](../recovery/filesystem_recovery.md)) |
| `recovery_carving` | `recovery::carving` | The format-independent carving framework: file signatures, the format interface and registry, signature scanning, file carving and structural validators ([../recovery/carving.md](../recovery/carving.md)) |
| `recovery_formats` | `recovery::formats` | Carvable file formats, plugged into the framework: the images of P9 (JPEG, PNG, WEBP, GIF, BMP), the audio of P10 (MP3, WAV, M4A, raw AAC) and the MP4 video of P12, with their header checks, end detection and structural validation ([../formats/images.md](../formats/images.md), [../formats/audio.md](../formats/audio.md)); the MP4 parser of P11 with the movie fragments and analysis of P12 (`recovery::formats::mp4`: boxes, movie, tracks, sample tables, chunk map, fragments, audio or video, moov discovery, sample framing; [../formats/mp4.md](../formats/mp4.md)) |
| `recovery_mp4` | `recovery` | MP4 recovery (P12): MP4 candidates (FILESYSTEM, HYBRID, CARVING) from filesystem candidates and carving together, with structure, sample and allocation evidence, and a candidate's data as file content ([../recovery/mp4_recovery.md](../recovery/mp4_recovery.md)) |
| `recovery_validation` | `recovery::validation` | Validation levels (P14): structural, media (the engine's deterministic decoders: JPEG Huffman scans, PNG inflate, GIF LZW, BMP RLE, VP8L, VP8 headers, ADPCM blocks, AAC raw data blocks and ADTS CRCs, AVC and HEVC parameter sets and slice headers) and playability through a checker interface ([../formats/media_validation.md](../formats/media_validation.md)) |
| `recovery_playability` | `recovery::validation` | The playability level on Windows (P14): the Windows Imaging Component and Media Foundation decode whole files through a read-only stream over the content |
| `recovery_evaluation` | `recovery::evaluation` | Candidate evaluation (P14): filesystem candidates, carves, MP4 candidates and reconstructions merged into one `EvaluatedCandidate` per file, with its evidence, validation levels, SHA-256 and preliminary hash, duplicates and `explain()` ([../recovery/candidate_evaluation.md](../recovery/candidate_evaluation.md)) |
| `recovery_fragments` | `recovery` | Fragment reconstruction (P13): deleted files whose layout the metadata only guesses and carves that break, reconstructed from layout hypotheses that each format validates, ranked by the allocation and the evidence of other files, MP4 placed by its sample tables; COMPLETE, PARTIAL, CORRUPTED, AMBIGUOUS or UNRECOVERABLE ([../recovery/fragment_recovery.md](../recovery/fragment_recovery.md)) |
| `recovery_scan` | `recovery::scan` | Scanning (P15): `ScanCoordinator` runs a Quick or Deep scan (volumes, MP4 examination, fragment seeds, one shared source pass, MP4 delivery, fragments, evaluation) on a bounded worker pool, cancellable and pausable, with progress and metrics, handing out `ScanUpdate`s that a `ScanCheckpoint` resumes from; `ScanSource` (pause gate, shared block cache, read counts); `RecoveryJob` writes candidates the same way ([../recovery/scanning.md](../recovery/scanning.md)) |

Public headers live in `include/<module>/`. `<windows.h>` is only included from `src/storage/windows/` and
`src/validation/windows/` (the playability level's WIC and Media Foundation code), never from a public header.

## Error handling

- Expected failures return `Result<T>` or `Status` (`Result<void>`) and carry an `Error`. An `Error` holds an
  `ErrorCode` (the categories from the specification), a message, and the Win32 error code if there is one.
- Storage reads return a `ReadResult` with `status`, `requestedBytes`, `bytesRead`, `offset` and
  `systemErrorCode`. A partial read is reported as `PartialRead`, never as success.
- Exceptions are only used for programming errors and running out of memory. Reading the wrong alternative
  of a `Result` aborts the process.

## Thread ownership

| Object | Rule |
| --- | --- |
| `CancellationSource`/`Token` | Safe from any thread. |
| `Logger` | All methods are thread-safe. Sinks are called with the logger lock held. |
| `IStorageSource` (engine sources) | `open`/`close` must not overlap other calls. Reads are safe concurrently once open. |
| `BadRegionMap`, `DestinationFile` | Not thread-safe. Only one owner may use each at a time. |
| `ImageWriter` | `run()` runs on the calling thread. Cancellation can come from any thread. Progress callbacks run on the imaging thread. |
| `IFilesystem`, `FilesystemRecovery` | Not thread-safe (internal caches). One owner at a time. |
| `reconstructCandidate` | Runs on the calling thread and only reads the source; the sink is called on that thread. Concurrent calls are safe on sources that allow concurrent reads. |
| `RecoveryWriter` | Not thread-safe. One owner at a time; the source must outlive it. |
| `FormatRegistry` | `add` must not overlap other calls; the const functions are safe concurrently. |
| Carving formats and validators (the image and audio formats too) | Immutable once registered; every member is const and safe to call concurrently. |
| MP4 parser and analysis (`scanTopLevel`, `parseMovie`, `parseFile`, `classify`, `findMovie`, `checkSampleFraming`, `analyzeMp4`) | Free functions without shared state; concurrent calls are safe on different content readers (a reader has one owner at a time). |
| `Mp4Recovery`, `CandidateContentReader` | Not thread-safe. One owner at a time; the source, the volumes and their scans must outlive them. The sink is called on the thread of `run()`. |
| `FragmentRecovery` | Not thread-safe. One owner at a time; the source, the format registry, the volumes and their scans must outlive it. The sink is called on the thread of `run()`. |
| `SignatureScanner` | `scan` is const. Concurrent scans are safe on sources that allow concurrent reads. The sink and progress callback run on the scanning thread. |
| `FileCarver`, `SourceContentReader` | Not thread-safe. One owner at a time; the source must outlive them. |
| `WorkerPool` | Every member from any thread. Tasks must not throw (counted, never propagated), nor wait for tasks queued after them, nor submit to their own pool. |
| `JobControl` | Every member from any thread; copies share their state. |
| `runOrdered` | Called on a thread that is not one of the pool's workers; `consume` runs on it, `produce` on the workers. |
| `ScanSource` | Reads from any number of threads; `open`/`close` must not overlap other calls. |
| `ScanCoordinator`, `RecoveryJob` | `run()` has one owner and drives everything on its thread (commits, the sink, the progress callback); the pool's workers and, during the source pass, one scanner thread do the rest. `progress()` from any thread at any time. |
| `ScanCheckpoint`, `RecoveryJobCheckpoint` | Not thread-safe. One owner at a time. |
| `Mp4RecoverySteps` | `examine()` and `prepare()` concurrently with each other and with `commit()` of other hits; every other member one owner at a time. |
| `FragmentRecoverySteps` | `examineSeed()` concurrently with itself; every other member one owner at a time. |
| `CandidateEvaluation` with `EvaluationOptions::pool` | `run()` validates and hashes on the pool's workers (allocation queries under a lock per volume); the sink is called on the thread of `run()`. |

## Windows-specific assumptions

Each one is also documented where it is implemented.

- Physical drives are `\\.\PhysicalDriveN`. Opening one needs administrator rights and
  `FILE_SHARE_READ | FILE_SHARE_WRITE`, because mounted volumes on the disk hold their own handles.
- Raw disk reads must be sector-aligned in offset, length and buffer address. Unaligned requests go
  through an aligned bounce buffer.
- Offsets are passed to Windows as signed 64-bit values, so offsets of 2^63 or more are rejected.
- Image files are opened with `FILE_SHARE_READ` only, so nothing can modify them while they are open.
- `CREATE_NEW` is what guarantees that existing output files are never overwritten. `FlushFileBuffers`
  runs before metadata that refers to written data. `MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH)`
  replaces metadata atomically on NTFS and ReFS.
- The destination volume's disks are found with `IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS`. Network shares and
  SUBST drives can't be resolved this way and are rejected for physical-disk sources.
- Recovered files are created through `\\?\` paths, which Windows does not normalise and which are not limited
  to `MAX_PATH`. Every component comes from `safeFileName`: no `< > : " / \ | ? *` or control characters, no
  trailing dots or spaces (Windows would drop them), no reserved device names (`CON`, `NUL`, `COM1`, ...), and
  at most 255 UTF-16 units.
- A destination directory is only used when `symlink_status` reports a plain directory. MSVC reports symbolic
  links and junctions as types of their own, so the writer never follows a link out of the destination.
