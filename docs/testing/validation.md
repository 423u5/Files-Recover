# Final engineering validation (P20)

What P20 checked, how, what it found and fixed, and what is still open. The plan's rule applies: building and
passing tests does not make the engine production-ready; that needs every acceptance test to pass and every known
limitation to be documented ([../limitations.md](../limitations.md), L198–L212 for P20).

Machine for the local runs: Intel Core i5-4440 (4 cores, 3.1 GHz), 16 GB RAM, SATA SSD, Windows 10 Pro 22H2
(19045), Visual Studio with MSVC 14.51 and clang-tidy 22.1.3. CI: GitHub's `windows-latest` runners.

## Summary

| Plan item | How | Result |
| --- | --- | --- |
| MSVC warning-clean build | Every preset builds with `/W4 /WX` plus 17 off-by-default warnings | Clean: debug, release, asan, analyze, fuzz |
| Static analysis | `/analyze` (preset msvc-analyze, `/WX`); clang-tidy over the engine and the CLI, every finding triaged | `/analyze` clean; clang-tidy 81 + 27 findings, no defect |
| Sanitizers | AddressSanitizer on the whole test suite (msvc-asan) and under every fuzz target | Suite 1,591/1,591; fuzzing about 15.7 million inputs, 1 defect fixed |
| RAII audit | Review of every resource the engine owns | No defect |
| Thread-safety audit | Review of every shared object and thread, clang-tidy concurrency checks | 1 defect fixed (WorkerPool) |
| Integer-overflow audit | Review of arithmetic on values read from disks, checked-math use, a dedicated clang-tidy pass | No defect (27 findings triaged) |
| Bounds-check audit | Review of every raw memory access; byte loads checked in every build; ASan and fuzzing | No defect |
| Error-path audit | Review of discarded results, catch blocks, cleanup on failure | No defect |
| Cancellation audit | Every long loop and read checked for cancellation, a cancellation test per stage | No defect |
| Memory-usage audit | Every allocation sized by disk data checked against a cap; heap measured against the image size and the number of files | No defect; limits documented (L202, L209, L212) |
| I/O failure audit | Every read and write failure path, device removal | 1 defect fixed (DeviceLost) |
| Security review | Read-only source, destination safety, untrusted names, journals, logs, binaries | 1 defect fixed (journal paths), 2 hardenings (runtime, mitigations) |
| Logging review | Plan §26 against the records, content never logged | 3 gaps fixed, test added |
| Performance benchmarks | `recovery_benchmark` against targets ([performance.md](performance.md)) | Every target met; the hardening costs nothing measurable; memory per file found registered (L212) |
| Large-drive tests | 16 TiB GPT and 2 TiB+ MBR virtual disks; a sparse image of 64 GiB | Pass; 64 GiB deep-scanned in 46 s with a 76 MiB peak |
| Bad-sector tests | Simulated bad, flaky and lost devices at every layer; the corpus's bad regions | Pass, device loss added |
| Corruption corpus | 26 generated disks, the plan's 16 categories | 3 findings fixed (F1, F2, F4), 2 registered (L198, L199) |
| Regression suite | The whole ctest suite plus the corpus's baselines | 1,591/1,591 in debug, release and ASan; 11/11 fuzz |
| CI builds | GitHub Actions: debug, release, asan, fuzz, analyze; failures annotated | Workflow ready; not yet run on the P20 changes (the owner pushes them) |

## Defects found and fixed

Each has a regression test of the behaviour the defect broke; the journal test was also run against the unfixed
code, where it failed (the file outside the destination was deleted).

| Found by | Defect | Fix | Test |
| --- | --- | --- | --- |
| Corpus (F1) | A carve in clusters allocated to no listed file (a lost chain, an overwritten folder) was called Corrupted (`ClustersReallocated`) though it validated whole | Only bytes in clusters that hold a listed active file's data are reallocated (`candidate_evaluation.cpp`, `mp4_recovery.cpp`) | `CandidateEvaluationTest.ACarveIsReallocatedOnlyWhereAListedFileHoldsItsClusters`, `Mp4RecoveryTest.CarvedFilesCarryWhereTheyLieInTheAllocation` |
| Corpus (F2) | A FAT32 folder cluster overwritten with other data listed about 30 junk files | Active slots with an invalid name and an impossible field are skipped, with a `DirectoryInvalid` issue (`directory_parser.cpp`) | `Fat32CorruptionTest.ActiveSlotsThatAreClearlyGarbageAreSkipped` and three more |
| Corpus (F4) | The files of a disk's several volumes were recovered into one merged tree | A folder per volume when the candidates lie on several (`recovery_session.cpp`, `recovery_writer.cpp`, `recovery_job.cpp`) | `RecoverySessionTest.EachVolumeOfASourceWithSeveralGetsItsOwnFolder`, `RecoveryWriterTest.AFolderNameFromTheSourceStaysOneSafeComponent` |
| Fuzzing (`fuzz_imgmeta`) | Image metadata parsed values holding a NUL or CR that it cannot write back | The parser refuses them (`image_metadata.cpp`) | `ImageMetadataParseTest` cases `NulInValue`, `CarriageReturnInValue`; `fuzz/regressions/imgmeta` |
| Thread-safety audit | A task destroying its own `WorkerPool` detached its thread, which then used the freed pool | The destructor never detaches: it joins, and stops the process with a message when run on its own worker | `WorkerPoolDeathTest.ATaskThatDestroysItsOwnPoolStopsTheProcess`, `WorkerPoolTest.ATaskMayShutItsOwnPoolDown` |
| I/O failure audit | A device pulled out mid-read was taken for bad sectors: imaging and scans read the rest of the device sector by sector, recording it all as unreadable | `ReadStatus::DeviceLost` for "gone or not ready" errors; operations stop and resume (`storage_source.cpp`, `device_backed_source.cpp`) | `PhysicalDiskSourceTest.ADeviceThatIsGoneIsNotABadSector`, `ImageWriterSafetyTest.ADiskThatIsGoneStopsImagingAtOnceAndItResumesWhenBack`, `ScanCoordinatorTest.AScanOfADiskThatIsGoneStopsAtOnceAndResumesWhenItIsBack` |
| Security review | A resumed recovery job deleted the files the journal said it had begun, wherever they were: a damaged or foreign journal could delete any file the user can write (no larger than a candidate) | Only begun files below the job's destination are removed (`recovery_session.cpp`) | `RecoverySessionTest.AResumedJobRemovesOnlyFilesBegunInsideItsDestination` |
| Logging review | The scan's start lacked the source type, sector size, device and known bad regions; its end, most metrics; the recovery job's end, bytes read, speed and time | Added (`scan_coordinator.cpp`, `recovery_job.cpp`) | `LoggingReviewTest.ScansAndRecoveriesLogWhatThePlanAsksAndNoContent` |

The corpus also found a defect in its own generator (unused MBR entries must be zeros), fixed there.

## Build and static analysis

Warnings: every target is compiled with `/W4 /WX /permissive- /sdl` plus 17 off-by-default warnings that catch
defects in binary parsing (conversions, sign extension, truncation, suspicious operators). With the final code
(2026-10-12), all five presets build without a warning: msvc-debug, msvc-release, msvc-asan, msvc-analyze and
msvc-fuzz (356, 356, 356, 356 and 176 build steps).

`/analyze`: preset msvc-analyze runs MSVC's analyzer on the engine and the tools with `/WX`: no warning.

clang-tidy 22.1.3 (`tools/static_analysis/run_clang_tidy.py`) over the 150 sources of the engine and the CLI, with
the bugprone, clang-analyzer, concurrency and performance families and 21 cert and misc checks: 81 findings, each
re-read in the code. None is a defect:

| Check | Findings | Verdict |
| --- | --- | --- |
| `bugprone-unchecked-optional-access` | 21 | Each access is guarded by a check the analysis cannot follow: a wait predicate, a member function (`animated()` is `canvas_.has_value() && ...`), an invariant of the container (`live` volumes always have candidates), a loop that sets the value on its first pass before the result is used |
| `bugprone-use-after-move` | 2 | The move happens on a loop's last pass only, and the function returns after the loop |
| `bugprone-misplaced-widening-cast` | 4 | No overflow is possible: `4 * popcount` ≤ 128; console widths are 16-bit; the Huffman decoders' indices are bounded by their tables' construction (canonical codes, Kraft's inequality, at most 256 symbols) |
| `bugprone-exception-escape` | 13 | Moves of classes holding `std::map` or `std::set` (MSVC's node containers allocate when moved): `std::bad_alloc` only, and none is declared `noexcept`; a destructor that terminates on out-of-memory or on a documented misuse (destroying the API from its own callback); a `std::visit` that cannot see a valueless variant; streams with no exception mask. Out of memory is registered (L209) |
| `bugprone-empty-catch` | 7 | Boundaries that must not throw (the logger, user callbacks, noexcept destructors), each commented |
| `bugprone-branch-clone` | 5 | Distinct conditions with the same action, kept apart for their own comments |
| `cert-err33-c` | 10 | `snprintf` into buffers that bounded formats cannot fill; diagnostics written just before `abort()` |
| `clang-analyzer-optin.performance.Padding` | 1 | A public API struct whose field order is its documented reading order |
| `performance-*` | 18 | Copies, a missing `reserve`, string concatenation: not defects, left as they are (completed phases change only for defects) |

A second pass for the integer-overflow audit is below.

## Sanitizers and fuzzing

AddressSanitizer (preset msvc-asan) runs the whole test suite: 1,591 tests, all pass (16 skipped: the debug run's 15
and the heap measurement, which AddressSanitizer's allocator hides). MSVC offers no thread, undefined
behaviour or memory sanitizer (L206); debug builds add `/RTC1` run-time checks and the checked iterators of the
standard library, and the debug suite runs with both.

libFuzzer (preset msvc-fuzz, with AddressSanitizer) has ten targets, one per parser of untrusted data
([testing.md](testing.md), "Fuzzing"), seeded with the test builders' files, the embedded samples of other tools and
real session journals. Campaign 1 (480 s per target, three at a time, about 3.3 million inputs) found one defect:
`.imgmeta` values holding a NUL or a CR parsed but could not be written back; fixed, with a unit test and a saved
regression. It also showed the carving and scan targets too slow on large inputs (5,820 and 1,587 inputs), so their
inputs are now capped at 256 KiB. Campaign 2 (20 minutes per target, three at a time, 2026-10-12) continued from
campaign 1's corpus on the final code and found nothing:

| Target | Inputs | | Target | Inputs |
| --- | --- | --- | --- | --- |
| `fuzz_partition` | 4,020,140 | | `fuzz_metadata` | 762,782 |
| `fuzz_imgmeta` | 2,962,540 | | `fuzz_mp4` | 666,605 |
| `fuzz_journal` | 2,329,580 | | `fuzz_formats` | 414,090 |
| `fuzz_media` | 939,706 | | `fuzz_filesystem` | 280,647 |
| `fuzz_carving` | 19,058 | | `fuzz_scan` | 7,833 |

About 12.4 million inputs (15.7 million with campaign 1), no crash, leak, timeout or out-of-memory; peak memory
562–890 MB per target, AddressSanitizer included. The whole-scan and carving targets are slow by nature (a whole
deep scan per input) and got the fewest inputs (L204).

After ZIP carving (2026-10-13), the targets that reach it ran again from campaign 2's corpus with the ZIP samples
added to the seeds: `fuzz_formats` 182,398 inputs in 10 minutes, `fuzz_media` 150,078 in 4, `fuzz_metadata` 309,483,
`fuzz_carving` 8,539 and `fuzz_scan` 3,779 in 10 each; nothing found. AddressSanitizer runs the whole suite with
ZIP: 1,639 tests, all pass.

After PDF and OLE2 carving (2026-10-15), the same five targets ran again from that corpus with 20 document seeds
added (the 11 samples real writers made, PDFs of every cross-reference layout, one with a JPEG, and compound files
of both versions; the carving and scan seed card holds 9 PDFs and 3 compound files): `fuzz_formats` 144,229 inputs,
`fuzz_media` 286,236, `fuzz_metadata` 346,914, `fuzz_carving` 8,739 and `fuzz_scan` 3,922, 10 minutes each; nothing
found. A review of the two parsers for what inputs no fuzzer is likely to build found two defects, fixed with tests:
a cross-reference stream's `/Index` counts could add up past 64 bits and pass for the few entries the stream holds,
which were then read past its data (`PdfFormatTest.IndexCountsThatAddUpPastSixtyFourBitsListTooManyEntries`); and a
compound file's chain was followed as far as its stream's size asked, which a version 4 file records in 64 bits, so a
chain that loops could grow without end (`Ole2FormatTest.AChainThatRunsInALoopIsDamage`). The same review bounded
the work a crafted document can cause: at most 4,096 PDF revisions and cross-reference sections (each revision's end
followed the chain of sections, found by a linear search: cubic time in their number), cross-reference streams
inflated within the document's 4 GiB budget, and a compound file's checks stop at the first chain that shares a
sector. With all of it, every CI preset ran again (2026-10-15): debug, release and AddressSanitizer 1,714 tests each,
all pass; the fuzz preset 11/11; `/analyze` clean; no warning in any build. The targets that reach the changed code
ran 10 minutes more each on it: `fuzz_formats` 149,113 inputs, `fuzz_carving` 10,736, `fuzz_scan` 4,823; nothing
found. clang-tidy (both passes) on the 18 engine and CLI sources the work changed: 21 and 6 findings, 3 of them in
new code and fixed (a `std::move` that moved nothing, a dead store, an `int` multiplication in a constant); the rest
are the code P20 triaged. All presets ran once more after those fixes: release and AddressSanitizer 1,714 of 1,714,
the fuzz preset 11/11, `/analyze` clean, no warning; debug 1,713 of 1,714: an API test that the work did not touch
failed once under load and passed in the full debug run after it and in every repeat (L241).

L241 was then found (2026-10-15): the API told an operation's end before it closed the operation's source, and the
test rewrites the card image, which a scan opens sharing reads only, as soon as the scan's end is told. Holding the
scan's thread for 200 ms after it told the end made the test fail with the same message. The API now tells an
operation's end once the source is closed. `ApiSourcesTest.AnOperationThatEndedHoldsItsSourceNoMore` makes closing a
simulated disk slow and fails on the old code. Every CI preset ran again: debug, release and AddressSanitizer 1,715
tests each, all pass; the fuzz preset 11/11; `/analyze` clean; no warning; clang-tidy (both passes) finds nothing
new in the two API sources changed.

## Audits

### RAII

Every resource is owned by an object that releases it: Win32 handles (`UniqueHandle`), device information sets
(`DeviceInfoSet`), COM (`ComScope`, `ComPtr`), Media Foundation (`MediaFoundationScope`), CoTaskMem strings, threads
(joined in destructors: `WorkerPool`, the scan's scanner thread, the API's operation and event threads), files
(`DestinationFile`, which deletes a half-written file on failure), and locks (`std::lock_guard`, `std::unique_lock`).
`new` appears only straight into a `std::unique_ptr` or `std::shared_ptr` (factories of classes with private
constructors), never with a `delete`; `free` only for the strings `_wdupenv_s` allocates, right after use;
`CoTaskMemAlloc` only where COM requires it (an `IStream`'s name, freed by its caller). No defect.

### Thread safety

Every object shared between threads is listed with its rule in
[../architecture/overview.md](../architecture/overview.md) ("Thread ownership"). Reviewed: the worker pool, ordered
work, the scan's source cache and pause gate, the scan coordinator's commit thread, the recovery job, the session
(operation lock, control lock, journal appends from many threads), the API (sessions, operations, events, previews),
the event dispatcher, the logger, the CLI's Ctrl+C handling. The scan, recovery and API suites run 1–8 workers,
cancellation in every stage and concurrent queries; clang-tidy's concurrency checks found nothing. One defect:
`WorkerPool`'s destructor detached the current thread when a task destroyed its own pool, and that thread then
touched the freed pool when its task returned; nothing in the engine does it, but nothing prevented it. Now the
destructor joins every worker and stops the process with a message when it runs on one of its own workers.
MSVC has no ThreadSanitizer (L206).

### Integer overflow

Values read from a disk reach arithmetic only through checks: `checkedAdd`/`checkedMul`/`rangeWithin`
(`include/recovery/checked_math.hpp`, 79 uses across the storage, partition, filesystem, carving, format,
validation, evaluation and metadata code) and subtraction-form bounds (`length > size - offset` after
`offset <= size`). The conversions from a cluster or sector number to a byte offset, the products most exposed to a
hostile value, all take a number validated against the volume first (`isValidCluster`, documented as their
precondition), and the volume's own size is a checked product, so they cannot wrap; run lengths are clipped to the
clusters left before they are multiplied. Storage reads check `first * sectorSize` and `count * sectorSize`.
MSVC's conversion warnings (`/W4` with C4242, C4826, C4287 and others) are errors.

A dedicated clang-tidy pass (`run_clang_tidy.py --integer`: multiplications widened after the fact, misplaced
widening casts, loop counters narrower than their bound, sign and `sizeof` mistakes, undefined operator results,
insecure C APIs) gave 27 findings, none a defect: 19 products of constants (`64 * 1024`, `1024 * 1024 * 1024` is
2^30 and fits), 4 products bounded by validated values (an AVC step of at most 4, HEVC palette sizes of at most
2 × 16 × 128 bits, VP8 partition offsets of at most 21), and the 4 widening casts triaged above. No defect.

### Bounds

The parsers read fields through `loadLe16/32/64`, `loadBe16/32/64` and `loadU8` (604 calls), which check the
offset against the span in every build and stop the process rather than read outside it (`failedBoundsCheck`):
an out-of-range read is a crash the fuzzers report, never a silent read of other memory. Spans and the content
readers (which return exactly the bytes asked for, or an error) carry their sizes. The ten `memcpy`/`memmove` calls
all copy sizes checked just before (Windows' structures: `VOLUME_DISK_EXTENTS` counts against the returned size,
`STORAGE_DEVICE_DESCRIPTOR` headers; the Media Foundation byte stream; the scanner's and the scan source's buffers).
The Huffman decoders' table lookups (JPEG, Deflate) are bounded by the table construction (canonical codes, Kraft's
inequality). AddressSanitizer runs the whole suite and every fuzz target. No defect.

### Error paths

Every `Result` and `Status` is `[[nodiscard]]`; results are discarded (`(void)`) only on paths that already fail or
clean up (closing on error, removing a half-written file). `catch` blocks sit at boundaries only (a thread's body,
callbacks of the user's, the logger, noexcept destructors) and each says why. A failure while writing a recovered
file deletes it; a failed imaging or scan saves its state for a resume. clang-tidy's `bugprone-unused-return-value`
and `cert-err33-c` found nothing beyond the diagnostics triaged above. No defect.

### Cancellation

Every stage reads through a reader that checks the cancellation token on each read (`SourceContentReader`,
`CandidateContentReader`, the scan's source), and every long loop checks it per block, per candidate or per file:
signature scan, carving, filesystem listing and reconstruction, MP4 recovery, fragment reconstruction, evaluation,
recovery writing, imaging, the source pass. Decoding a file is bounded by its size limits and Windows' decoders by
a per-sample timeout (10 s). Tests cancel each stage: the filesystem, carving, MP4, fragment and recovery suites at
least once each, the scan coordinator in every stage and after every update, the session, the API (8 tests) and the CLI's
Ctrl+C at every stage (7). No defect.

### Memory usage

The scan's memory does not grow with the source: blocks are read into a fixed cache, carves and analyses run ahead
by a bounded window, and the heap peaks of scans of a 1.25 GiB and a 5 GiB image differ by under 2 MiB
(`ScanLargeImageTest`); a 16 TiB virtual disk is scanned reading under 1 GiB. What grows is per file found: the
candidate list, its evidence and SHA-256. Every allocation sized by a value read from a disk is capped: GPT entry
arrays 4 MiB (16,384 entries), FAT32 directories 65,536 entries and the FAT read in pages, exFAT directories 256 MiB
and up-case tables 128 KiB, NTFS records 64 KiB, listings 10,000,000 entries 64 levels deep, MP4 sample tables
4,194,304 entries per movie, decoders 256 MiB of working memory and 4 GiB of output, text fields of metadata bounded,
previews 64 MiB per read, image metadata files 64 MiB, a journal record by the journal's size (L202). Reads are at
most 64 MiB. Measured peaks of the release build ([performance.md](performance.md)): 74 MiB for a deep scan of 2 GiB
of random data, 91 MiB for a 1 GiB card of 1,374 media files, 83 MiB recovering them, 4 MiB imaging; a quick scan of
5,000, 20,000 and 40,000 files 90, 204 and 368 MiB: about 8 KiB per file found, which is what a drive of millions of
files would need (L212). Out of memory is an exception (L209).

### I/O failures

Reads: a failed block read is retried as pieces and then sector by sector (`sectorRetryCount`), the bad sectors
recorded as unreadable regions, and their bytes given as zeros everywhere (imaging, scans, recovery), as the
existing tests show with permanent, flaky and short-read sectors. Writes: a destination that fails (full, removed,
denied) stops the operation with `DestinationError`, the half-written file deleted, and the job resumable. The defect:
a source device removed or no longer ready failed every read with "not ready" or "device removed", and each was taken
for a bad sector: imaging and scans then read the rest of the device sector by sector, slowly, and recorded it as
unreadable. `ReadStatus::DeviceLost` now stops the operation at once; it resumes when the device is back (L203 lists
the errors recognised).

## Security review

The engine reads disks and files that may be damaged or crafted, usually as an administrator (raw disks need it),
and writes to the user's folders. Reviewed, with the tests that hold each property:

| Property | How it holds | Result |
| --- | --- | --- |
| The source is never written | `IStorageSource` has no write; devices and images open with `GENERIC_READ` (queries with access 0); the only device controls are queries (`IOCTL_STORAGE_QUERY_PROPERTY`, `IOCTL_DISK_GET_DRIVE_GEOMETRY_EX`, `IOCTL_DISK_GET_LENGTH_INFO`, `IOCTL_STORAGE_GET_DEVICE_NUMBER`, `IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS`); no lock, dismount or mount. The only writable handles are destination files | Holds |
| Output never lands on the source | `checkDestinationSafety`: not the source image; for a disk, the destination volume's extents must not include it; unknown means refused; checked again for each directory created | Holds (destination guard and writer tests) |
| Nothing is overwritten or followed | Files are created with `CREATE_NEW` and numbered when taken; device paths are refused; links and junctions in the destination are never followed | Holds |
| Names from disks stay inside the destination | `safeFileName` for every component: traversal, separators, `:` streams, reserved device names, trailing dots, length. P20: the per-volume folder (it holds the volume label) too | Holds (`UnsafeNamesStayInsideTheDestination`, `AFolderNameFromTheSourceStaysOneSafeComponent`) |
| Hostile data cannot corrupt memory | Bounds-checked loads, checked arithmetic, allocation caps, AddressSanitizer on the suite, libFuzzer on every parser | Holds for what was tested (L204) |
| Hostile data cannot hang a scan | Walks are bounded (chain loop detection, depth 64, entry and table caps), every loop cancellable, Windows' decoders timed out per sample | Holds (corruption and cancellation suites) |
| Session journals and image metadata | Parsed as untrusted (CRCs, lengths against the file, fuzzed) | 1 defect fixed: a resume deleted begun files wherever the journal said; now only below the job's destination. The journal still names that destination (L201) |
| Logs hold no content | Records hold sizes, offsets, paths and names, never bytes of files | Holds (`LoggingReviewTest`) |
| Reports and console output | Names from disks are escaped: JSON escapes controls and U+2028/2029; text and console escape C0/C1 controls, DEL and invisible or bidirectional characters, so a name cannot forge lines or move the cursor | Holds (report and CLI tests) |
| Windows' decoders | The playability check, which hands files to WIC and Media Foundation in the process, is off unless asked for (`--playability`, `ScanSettings::playability`) | Accepted (L210) |
| The executables | ASLR, high-entropy ASLR, DEP, `/sdl`, `/GS` (defaults); P20 adds Control Flow Guard, CET shadow stack compatibility, imports from System32 only (`/DEPENDENTLOADFLAG:0x800`) and the static C++ runtime (nothing to install, no runtime DLL to plant). Checked with `dumpbin`: DLL characteristics 0xC160, extended 0x1 (CET), 217 CFG targets, dependent load flag 0x0800, imports MFPlat, MFReadWrite, ole32, KERNEL32 and one API set | Hardened |
| No network, no plugins | The engine opens no sockets and loads no code of its own at run time; GoogleTest is fetched at configure time, pinned by commit | Holds |

## Logging review

Plan §26 asks for the scan's start, each volume, unreadable regions, the evaluation, the scan's end with its metrics,
each recovered file and the recovery job's end; never file content. Three gaps were found and fixed (the source's
type, sector size, device and known bad regions at the start; every metric at the scan's end; bytes read, speed and
time at the job's end). `LoggingReviewTest` checks every record and field, and that a marker stored in a file's
content and in a JPEG comment appears in no record. Paths and names do appear (they are the user's own data, needed
to follow a run).

## Large drives and bad sectors

Large drives: `LargeDriveTest` puts a FAT32, an exFAT and an NTFS volume at 3, 8 and 15 TiB of a 16 TiB GPT disk
(a virtual source: zeros except what is written) and a partition across the 2 TiB boundary of an MBR disk; a quick
scan finds every file with its SHA-256 and reads under 1 GiB. Earlier phases scan virtual sources of 1 TiB (carving)
and 6 GiB (scan) and MP4 files beyond 4 GiB. The opt-in sparse image test, run once on the release build: a 64 GiB
sparse image file (a volume at its start, two files 8 and 64 MiB before its end) deep-scanned through the API in
45.6 s (1,508 MB/s, mostly unwritten space) with a peak working set of 76 MiB, the same as for a 2 GiB image.

Bad sectors are simulated by `MemoryStorageSource` (permanent, failing N times, short reads) and by the fake device
under `PhysicalDiskSource` (Win32 errors). About 60 tests cover them at every layer: the storage sources and the bad
region map, imaging (zero-filled, recorded, resumed), partitions, the three filesystems, the carving reader, scanner
and carver, MP4 and audio validation, candidate reconstruction (unreadable bytes as zeros and reported), the scan
coordinator, sessions, and the API's imaging; the corpus's `bad-regions` scenario scans an image whose metadata
records unreadable regions. P20 adds the device that disappears (`DeviceLost`), in the storage, imaging and scan
suites.

## Corruption corpus and regression suite

The corpus ([testing.md](testing.md), "Corruption corpus") is 26 generated disks covering the plan's 16 categories:

| Category | Scenarios |
| --- | --- |
| Normal files | fat32-normal, exfat-mixed, ntfs-mixed, raw-media, empty-noise |
| Deleted files | fat32-deleted, fat32-fragmented, fat32-overwritten, exfat-mixed, exfat-deleted-fragmented, ntfs-mixed, mbr-logical-partitions |
| Large files | large-files (5–14 MiB, deleted and fragmented) |
| Small files | fat32-normal, ntfs-mixed (resident) |
| Fragmented files | fat32-fragmented, exfat-mixed, exfat-deleted-fragmented, ntfs-mixed |
| Partially overwritten files | fat32-overwritten, nearly-full |
| Corrupt filesystem metadata | fat32-damaged-tables, damaged-boot-sectors, exfat-damaged-entries, ntfs-damaged-records |
| Corrupt media | corrupt-media |
| Duplicates | duplicates |
| Unicode names, long names | unicode-long-names (254-character names, paths beyond MAX_PATH, on all three filesystems) |
| Different cluster sizes | fat32-normal, exfat-mixed, ntfs-4k-sectors, cluster-sizes (512 B to 128 KiB), large-files |
| Nearly full drives | nearly-full |
| Quick-formatted drives | fat32-quick-formatted, exfat-quick-formatted, ntfs-quick-formatted |
| Bad regions | bad-regions |
| Multiple partitions | damaged-boot-sectors, exfat-deleted-fragmented, cluster-sizes, mbr-logical-partitions, gpt-multi, unicode-long-names |

Every scenario is scanned Deep and Quick through the API and recovered; the checks and the baselines (538 lines) are
described in [testing.md](testing.md). Writing the corpus found five things in the engine: three defects fixed (F1,
F2, F4 above), a carve's container taken from an overlong allocation (L198) and `recoverAll` refusing an empty
session (L199); and one bug in the generator itself.

The regression suite is the whole ctest suite, the corpus with its baselines included:

| Build | Tests | Result |
| --- | --- | --- |
| msvc-debug (`ctest -j 4`) | 1,591 | All pass (15 skipped: reference images and files of other tools that need `RECOVERY_*` folders, the HEVC decoder this computer lacks, the opt-in sparse image) |
| msvc-release (`ctest -j 4`) | 1,591 | All pass (the same 15 skipped) |
| msvc-asan (`ctest -j 3`) | 1,591 | All pass (16 skipped: the heap measurement too) |
| msvc-fuzz | 11 (10 smoke runs, 1 regression) | All pass |

## Continuous integration

`.github/workflows/windows-build.yml` builds and tests msvc-debug, msvc-release, msvc-asan and (P20) msvc-fuzz, and
builds msvc-analyze, on every push and pull request. Before P20 a failed CI test could not be identified: job logs
need admin rights to read, and the annotation said only "exit code 1". P20 turns every failed test into an
annotation naming it and its failure (`.github/scripts/report_failed_tests.ps1`, from ctest's JUnit output, tested
on a crafted report) and keeps the JUnit file and ctest's log for 14 days.

State before P20's changes were pushed (read from the public API, 2026-10-12): the last two runs failed,
4497e28 (P18–P19) in msvc-debug's Test step only (release, asan and analyze passed) and 970783b (P17) in msvc-asan;
73fc94e (P16) passed. Neither failure reproduced locally (the full debug suite passed 1,545 tests with
`--repeat until-fail:3` in P20, and passes 1,591 now). The P20 changes have not run in CI yet: they are committed
and pushed by the project's owner, and the run's annotations will name any test that fails there.

## After P20: a real USB stick

The first test on real hardware (2026-10-12 and 13), at the user's request: a 32 GB SanDisk USB stick (NTFS,
formatted before the test, with videos, pictures and Android project archives deleted from it), read only.

**Imaging.** The first image read nothing: every read failed with error 121 and the stick dropped off the bus
(Windows' System log: retried I/O at LBA 0, then the disk reported no media). That led to L203's media check
(`IOCTL_STORAGE_CHECK_VERIFY2` after an ambiguous error) and to the CLI's warning when nothing could be read. After
re-plugging, the second image was complete: 28.6 GiB, nothing unreadable, its SHA-256 in its metadata (L1).

**Scan and recovery** (release build, deep scan of the image, 2026-10-12): 11:48 minutes; 252.5 GB read for a
28.6 GiB image, read once by the source pass and about 221 GB more after it (L223); 307 NTFS entries (2 active, 305 deleted),
1,884 carves, 101 MP4 candidates, 6 reconstructions. 2,067 candidates: 1,635 COMPLETE, 173 UNVERIFIED, 159
CORRUPTED, 100 PARTIAL; 894 duplicates. All 95 deleted videos with names came back COMPLETE; FFmpeg decoded the ones
sampled without an error. Recovering everything wrote 2,067 files (30.0 GiB) in about 4.5 minutes, folders and
times restored (L37).

**Archives and text** (2026-10-12): the user asked about source files (.java, .xml). The stick had none with an
entry left, and no loose text file started at any cluster of the image: the projects' sources were inside ZIP
archives, which the engine recovered by name but could not check (UNVERIFIED), and Git objects. Two more intact
archives had no entry at all and were not found (L213). ZIP carving followed ([../formats/archives.md](../formats/archives.md)).

**With ZIP carving** (release build, 2026-10-13, the same image): 9 archives, every one COMPLETE with every member's
CRC-32 right: the 7 found by name (UNVERIFIED before) and the 2 without an entry, carved (42.8 MB holding 2,946 files,
and 148 KB). 529 candidates (144 COMPLETE, 166 UNVERIFIED, 120 CORRUPTED, 99 PARTIAL; 1 duplicate). Compared candidate
by candidate with the scan before: 1,540 are gone, every one inside a verified archive (1,499 PNG icons of Android
build output, 1 GIF, 40 damaged or partial images; L221); 2 are new (the carved archives); 7 changed their verdict
(the archives found by name); nothing else changed. The scan took 15:31 minutes with another build running on the
computer; scanning the largest archive alone (158 MB, 2,289 members, 210 MB unpacked) takes 7 seconds, most of it
decoding.

**With PDF and OLE2 carving** (release builds, 2026-10-15, the same image, read only): the build of the last commit
(60c16e8, before) and the build with the document formats (after) scanned it one after the other. All 529
candidates are identical field for field (`tools/regression/compare_reports.py`: none lost, none worse, none new),
as are the 5,495,720 signature hits: the stick holds no `%PDF-` and no compound file signature anywhere. 12:49 and
13:15 minutes. Since the stick has no documents, a 10 MB raw image of 23 real files was scanned the same way (no
filesystem, seeded noise between the files: the Edge, Microsoft Print to PDF and qpdf PDFs, the structured-storage
DOC and XLS files and the Windows Installer package of the samples, three packages from `C:\Windows\Installer`, five
JARs and the APK of Android Studio, a JPEG). Before: 8 of the 23 byte for byte (the JPEG and the ZIP-based files),
the APK cut short where `apksigner` aligned its signing block (L240), no PDF, DOC, XLS or MSI; 312 candidates, 299 of
them images stored in the APK, carved loose because the APK was not verified. After: all 23 byte for byte, named
`.pdf`, `.doc`, `.xls`, `.msi`, `.xps`, `.jar`, `.zip` and `.apk` (L239: two JARs without a manifest as `.zip`), the
AES-256 PDF `UNVERIFIED`, the others `COMPLETE`; 23 candidates. Each of the 299 images is inside the recovered APK,
byte for byte, as the user decided for archives (L221).

## After P20: deleted FAT32 starts and folders, orphaned exFAT folders

At the user's request (2026-10-15 and 16), three things deletion takes are found again (L7, L8 and L13 in
[../limitations.md](../limitations.md); how, in [../architecture/filesystems.md](../architecture/filesystems.md)):
the top 16 bits of a deleted FAT32 entry's start cluster, a deleted FAT32 folder's clusters after its first, and the
directory flag of an exFAT folder whose File entry a newer entry set took.

**The corpus.** Two scenarios lay volumes out the way Windows leaves them: `fat32-windows-deleted` (FAT32 of 70,000
clusters, entries written before their data; deleted photos on both sides of cluster 65,536, a deleted folder of 20
photos that grew three times, a voice note, a song) and `exfat-orphaned-folder` (a deleted folder whose File entry a
newer file's four entries took). With the three changes switched off, the deep scan recovered 24 of the FAT32
scenario's 31 files only as unnamed carves, and the exFAT folder's two photos too. The quick scan was worse: the
deleted folder's 20 photos and the exFAT photos were missing, and four deleted files beyond cluster 65,536 came back
under their own names holding other data (CORRUPTED or PARTIAL; one photo held the deleted song's bytes, from the
cluster its cleared start pointed to). With them, all 33 files come back under their names, complete, in both
scans. The other 30 scenarios' baselines did not change. Each part was also switched off on its own (nine
deliberate breaks, the three together among them): every one made a test fail.

**Checks** (2026-10-16, the final sources). msvc-debug, msvc-release and msvc-asan: 1,739 tests each, all passing;
msvc-analyze without a warning; the fuzz preset's 11 smoke runs and regressions. clang-tidy, both passes, over the changed sources: three
new findings, fixed (a branch duplicated in the run placement; the FAT32 options, which now hold a `std::function`,
copied where they could be moved); the rest predate the change and were triaged above.

**A defect found on the way (L245).** The last AddressSanitizer run before the docs were finished crashed once in
an API test: a worker of the scan's MP4 stage notified a condition variable that the thread waiting on it had
already destroyed. The order behind it, notifying after unlocking while the waiting thread may destroy the state as
soon as it takes the lock, was in the parallel runs of the scan and the evaluation since P15, and in the API's
count of open sessions. Both now notify under the lock. A stress test reproduces the crash on the old code in about
one AddressSanitizer run in three, and never with the fix (20 runs).

**Fuzzing.** `fuzz_filesystem` now places deleted starts with the formats' start check, as a scan does, and has a
seed for it (a FAT32 of 70,000 clusters cut short, its files and a folder that grew deleted the Windows way). Ten
minutes each on the corpora of the earlier campaigns: `fuzz_filesystem` ran 209,895 inputs and its coverage grew
from 26,608 to 30,582 features (the new code); `fuzz_scan` ran 5,657 whole scans. Nothing failed: no crash, leak,
timeout or excess memory.

**The test stick.** Its volume is NTFS, so nothing should change, and nothing did: the deep scan of
`E:\RecoveryTest\sandisk-f-2.img` with the new release build and the scan of the documents build (the last commit's
behaviour) are identical, all 529 candidates field for field (`tools/regression/compare_reports.py`: none lost,
worse, better or new), with the same 5,495,720 signature hits and 353 carves. The scan took 32 minutes instead of 13:
the AddressSanitizer suite ran beside it. The final build, with the fix of L245, scanned it again on its own:
identical again, in 13:11 minutes.

**Windows-written images** (Windows 10 22H2, 2026-10-16). `tests/reference/make_windows_fat_reference.ps1` had
Windows format two virtual disks, write files and delete some. Windows 10 clears the top 16 bits of a deleted FAT32
entry's start, for files and folders: the 21 photos deleted from a camera folder beyond cluster 65,536 kept only the
low bits, each pointing into another file. The engine placed every one where its data is; 19 match byte for byte,
and the other 2 had never reached the disk. The camera folder grew the way the L8 search assumes (each new cluster
after the data written before it), and Windows records a deleted folder's size in its entry (unused yet, L243).
Every active file of both images was read back byte for byte. The reference tests still failed, on the scenario:
files deleted seconds after being written, a folder of 125 photos among them, never reached the disk (Windows drops
what it has not written of a file deleted at once), and on exFAT Windows wrote a newer file's data over the deleted
folder's own cluster (it takes the lowest free clusters) and its entries at the end of the folder (no orphan,
L13). The script now flushes before it deletes and its exFAT part was redone. On its next images every reference
test passes: FAT32, 132 active files and all 147 deleted ones byte for byte, among them a deleted folder of 125
photos placed by its "." entry (which Windows leaves whole) and read across its three clusters (L8); exFAT, 43
active files, the six photos of a folder whose File entry Windows reused for a newer file when the folder had no room
left (the orphan of L13, now seen), and a deleted fragmented file through the FAT chain Windows kept (L12), its
clusters taken since reported as such. L12, L19 and L21 are resolved. A fourth run, with the script formatting each
disk before giving it a drive letter (File Explorer no longer offers to format it), made the same images, and every
reference test passes on them as well.

## After P20: a session's disk under another number, unfinished scans, the disk list

At the user's request (2026-10-16), three limitations of sessions and the command line are resolved (L147, L172 and
L177 in [../limitations.md](../limitations.md); how, in [../recovery/sessions.md](../recovery/sessions.md) and
[../recovery/cli.md](../recovery/cli.md)): a session finds its physical disk when Windows gives it another number,
`recover` writes what an unfinished scan delivered, and `recovery disks` lists the disks (with `--source F:` for the
whole disk a drive is on).

**The user's choice.** The disk must be the same device, not only hold the same content: the session's content in
another device (another serial number: a card moved to another card reader) is refused (L246, accepted). A session
that recorded no serial number tells its device by vendor and product (L247, open for review).

**A defect found on the way.** The guard that keeps reports and logs off the source (`report --output`, `--log`,
the API's `exportReport`) used the disk number the session recorded. After a renumbering it protected whichever disk
had that number, and not the session's own. It now protects every disk that may be the session's.

**Tests.** Simulated disks by number (`tests/support/disk_rack`), moved to other numbers with another card under the
old one: a session, the CLI and the API each stop a scan in the source pass, resume it on the disk under its new
number to the uninterrupted result, and recover there; the old number is refused with nothing recorded; the same
content in another device, a device written to since, and a journal's unusable `SourceFound` record (damage) are
refused. A scan stopped in its evaluation is recovered from (a warning, exit code 4), then the rest once the scan is
done: the folder ends as a recovery of a complete scan makes it (run 20 times, stable). On this computer,
`recovery disks` listed both disks with the serial numbers Windows reports (`Get-Disk`), without administrator rights.

**Checks** (2026-10-16, the final sources). msvc-debug, msvc-release and msvc-asan: 1,753 tests each, all passing;
msvc-analyze without a warning; the fuzz preset's 11 smoke runs and regressions; `fuzz_journal`, which knows the new
record type and has a seed of it, 3 minutes on the seeds (334,214 inputs): nothing found. clang-tidy, both passes,
over the session, API, CLI, storage and checkpoint sources (40 files): no finding on a line this work changed; the
rest predate it and were triaged above. The test stick was not
scanned again, with the user's agreement: nothing a scan reads or a recovery writes changed.

## Acceptance

| Criterion | Status |
| --- | --- |
| CI passes | Not known yet: the P20 changes have not been pushed. The last run before them (4497e28) failed in msvc-debug's tests, a failure that did not reproduce locally; the next run's annotations will say which test |
| Regression suite passes | Yes, locally: 1,591 tests in debug, release and AddressSanitizer builds, and the fuzz preset's 11 |
| Security review passes | Yes: one defect found and fixed (journal paths), the executables hardened; what remains is accepted and registered (L201, L210) |
| Performance targets are documented | Yes: [performance.md](performance.md); every target met on this computer, measured without a USB device (L211) |
| Known limitations are explicitly reported | Yes: [../limitations.md](../limitations.md), L198–L212 for P20, with every earlier phase's |

P20 does not declare the engine production-ready. The open limitations decide whether it is for a given use, among
them: memory grows with the number of files found (L212), device removal is recognised by a list not verified on
hardware (L203), performance was not measured on real USB devices (L211), fuzzing is time-boxed (L204), and every
limitation still Open from earlier phases.
