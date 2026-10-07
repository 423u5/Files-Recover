# RecoveryEngine

A read-only file recovery engine for Windows 11, written in C++20. It is aimed at deleted media on USB flash
drives and other removable storage.

> **Status: early development.** Phases P0–P17 are implemented: project skeleton, read-only storage
> access, disk imaging with bad-sector handling, MBR/GPT partition detection, FAT32 and exFAT parsing,
> the NTFS foundation (MFT, FILE records, resident and non-resident data, data runs), all including
> deleted entries, and filesystem-based recovery: recovery candidates from FAT32, exFAT and NTFS metadata,
> their reconstruction from the source, and writing them to a separate destination. P8 adds the generic
> carving framework: signature scanning, file carving, structural validation and a format registry, and P9
> the first formats for it: JPEG, PNG, WEBP, GIF and BMP, each with its own header check, end detection and
> structural validation. P10 adds the audio formats: MP3, WAV, M4A and raw AAC (ADTS), and P11 a dedicated
> MP4 (ISO base media file format) parser: boxes, tracks, sample tables and chunk offsets, checked against the
> file's media data. P12 recovers MP4 files from filesystem and carving evidence together (FILESYSTEM, HYBRID and
> CARVING candidates), and P13 reconstructs fragmented files whose layout no metadata records: deleted FAT32 and
> exFAT files and carves that break, from layout hypotheses that each format validates, reported as COMPLETE,
> PARTIAL, CORRUPTED, AMBIGUOUS or UNRECOVERABLE. P14 merges the candidates of every stage into one per file,
> validated at three levels (structure; media, by the engine's own decoders; optionally playability, by Windows'
> decoders), identified by SHA-256 with duplicates marked and their evidence explained. P15 runs all of it as one
> scan (Quick or Deep): on a bounded pool of workers, with one sequential pass over the source that every carving
> stage shares, cancellable, pausable, reporting progress and metrics, and resumable from the checkpoint updates it
> hands out without doing the work again; a recovery job writes the candidates out the same way. P16 keeps a scan
> and its recovery jobs in a session on disk, record by record, so that both survive a restart or a crash and resume
> from where they were. P17 gives a future user interface what it shows, without depending on any user interface
> library: each file's condition, duplicates under different names, its media metadata (image size and Exif,
> audio and video streams, durations, tags) and previews read on demand, and what recovery jobs did with it.
> There is no report or user-facing recovery command yet. Passing builds and tests do **not**
> mean the engine is production-ready.

## Safety model

- The source device is **never written**. The storage interface (`IStorageSource`) has no write operation.
  Devices and image files are opened with `GENERIC_READ` only.
- All output goes through `DestinationFile`. It refuses device paths and never overwrites an existing file.
  Recovered files get collision-safe names (`photo (1).jpg`), and names read from the source are made safe for
  Windows first, so they cannot leave the destination.
- Before imaging, the engine checks that the destination is not the source image. For a physical disk, it
  also checks that the destination volume is not on that disk. If the engine can't tell which disk the
  destination is on, it refuses to write.

## Layout

| Path | Contents |
| --- | --- |
| `include/recovery`, `src/recovery` | Core types: `Result`/`Status`, `Error`, strong types, checked math, cancellation, configuration |
| `include/diagnostics`, `src/diagnostics` | Structured logging |
| `include/storage`, `src/storage` | `IStorageSource`, `PhysicalDiskSource`, `DiskImageSource`, bad-region map, destination file and guard |
| `src/storage/windows` | All Win32 storage code (private) |
| `include/imaging`, `src/imaging` | `ImageWriter` and image metadata |
| `include/partition`, `src/partition` | MBR/GPT detection, `PartitionSource` |
| `include/filesystem`, `src/filesystem` | Filesystem-independent model, FAT32, exFAT, NTFS |
| `include/recovery`, `src/recovery` | Also the `recovery_candidates` library: recovery candidates, filesystem-based recovery, reconstruction, recovery output; `recovery_mp4`: MP4 recovery (P12); `recovery_fragments`: fragment reconstruction (P13) |
| `include/carving`, `src/carving` | Carving framework: signatures, format interface and registry, signature scanner, file carver, validators |
| `include/formats`, `src/formats` | Carvable formats: the images of P9 (JPEG, PNG, WEBP, GIF, BMP), the audio of P10 (MP3, WAV, M4A, AAC) and the MP4 video of P12; the MP4 parser of P11 |
| `include/validation`, `src/validation` | Validation levels and the media decoders (P14); `src/validation/windows`: the playability checker on WIC and Media Foundation (private) |
| `include/evaluation`, `src/evaluation` | Candidate evaluation (P14): one evaluated candidate per file, content identity, duplicates |
| `include/scan`, `src/scan` | Scanning (P15): the scan coordinator, the scan's source (pause gate, block cache), updates and checkpoints, the recovery job |
| `include/session`, `src/session` | Recovery sessions (P16): the session, its journal, the encoding of its records, the source's fingerprint |
| `include/metadata`, `src/metadata` | Metadata for a user interface (P17): candidate descriptions and conditions, duplicate groups, media metadata and previews read from a candidate's content, recovery status |
| `tools/recovery_cli` | `recovery` CLI (skeleton only) |
| `tests/unit`, `tests/integration`, `tests/filesystem`, `tests/corruption`, `tests/recovery`, `tests/carving`, `tests/formats`, `tests/validation`, `tests/evaluation`, `tests/scan`, `tests/session`, `tests/metadata` | GoogleTest suites; `tests/support` holds the simulated devices, volume and image file builders, samples from other encoders, and test-only carving formats |
| `docs/` | Architecture, recovery and testing notes; known limitations in `docs/limitations.md` |

Directories for later phases (`tools/disk_image_tool`, `tools/test_image_generator`) already exist and are empty.

## Building on Windows 11

Requirements: Visual Studio 2022 or later with the "Desktop development with C++" workload (MSVC, CMake,
Ninja), and Git. The first configure downloads GoogleTest.

From a **Developer PowerShell / x64 Native Tools prompt**:

```powershell
cmake --preset msvc-debug
cmake --build --preset msvc-debug
ctest --preset msvc-debug
```

Other presets:

| Preset | Purpose |
| --- | --- |
| `msvc-release` | Optimised build |
| `msvc-asan` | AddressSanitizer (RelWithDebInfo) |
| `msvc-analyze` | MSVC `/analyze` on engine and tool code |

Warnings are errors (`/W4 /WX`). To run only one group of tests: `ctest --preset msvc-debug -L unit`,
`-L integration`, `-L filesystem`, `-L corruption`, `-L recovery`, `-L carving`, `-L formats`, `-L validation`,
`-L evaluation`, `-L scan` or `-L cli`.

See [docs/testing/testing.md](docs/testing/testing.md) for how to test safely.
