# The GUI-facing API (P19)

`recovery_api` (`include/api/`, `src/api/`, namespace `recovery::api`) is what a user interface builds on. One
object, `RecoveryApi`, finds the disks, inspects and images sources, scans them in sessions, lists what the scans
found with each file's details and previews, recovers files, and exports reports. A user interface built on it needs
no knowledge of FAT32, exFAT, NTFS, carving or the engine's types. It never reads the source, never writes a
recovered file and never touches a session's journal: the engine does all of that.

```
 user interface ──► include/api/recovery_api.hpp, api_types.hpp     (std + recovery/error.hpp + recovery/result.hpp)
                           │
                    recovery_api  ── RecoveryApi ── OpenSession (one per open session) ── its operation's thread
                           │                    └─ the event thread (EventDispatcher) ──► ApiOptions::onEvent
                           │                    └─ imagings (one thread each)
                           ▼
   recovery_report (the session report, shared with the CLI) ─► recovery_metadata ─► recovery_session ─► ...
   recovery_imaging, recovery_playability, storage (DiskLister)
```

## The plan's items

| The plan's item | The API |
| --- | --- |
| inspectSource() | `inspectSource(SourceRef)`: the source (size, sectors, model), an image's metadata, the partition table, and each volume's filesystem, label, serial number and cluster size, as a scan would plan them. Only partition tables and boot records are read |
| createImage() | `createImage(SourceRef, ImagingOptions)` → `ImagingId`; `getImagingProgress`, `cancelImaging`, `waitForImaging`. Unreadable sectors are narrowed down, zero-filled and listed in `<image>.imgmeta` |
| startScan() | `startScan(SourceRef, ScanSettings)`: a new session, its scan started; returns the session's id |
| pauseScan(), resumeScan(), cancelScan() | By session id. `resumeScan` also runs again a scan that did not complete (cancelled, failed, interrupted by a crash, paused when the program ended) from its last update |
| getProgress() | `getProgress(session)` from any thread at any time: the operation, its state, its stage and figures, live from the engine, or how the last operation ended |
| getCandidates() | `getCandidates(session, CandidateQuery)`: a page of the candidates delivered so far, filtered by kind, condition, recovery state, deleted or not, duplicates |
| recoverCandidate(), recoverCandidates(), recoverAll() | Recovery jobs of the session to a destination folder (`RecoveryOptions`), converging on it; `recoverAll` takes a `CandidateFilter`. `pauseRecovery`, `resumeRecovery`, `cancelRecovery` |
| getSession() | `getSession(session)`: everything the session holds but its candidates: source, settings, scan state and history, volumes, recovery jobs, errors, unreadable regions, repairs. `listSessions`, `openSession`, `closeSession` |
| exportReport() | `exportReport(session, file, ReportOptions)`: the session report as JSON (`recovery-session-report`, the CLI's document) or text, to a new file |
| GUI must not access FAT32/exFAT/NTFS internals or raw filesystem structures | The API's own value types (`api_types.hpp`). `CandidateInfo` has a name, path, kind, format, size, condition, validation, SHA-256, duplicates and recovery state, and no clusters, records, offsets or carving evidence. `VolumeInfo` has a filesystem kind, label, serial and cluster size, and no boot sector |
| GUI must not perform recovery itself | Recovery, imaging and reports are written by the engine. Previews hand out bytes that are only read, never written |

Additions the user asked for: `listSources()`, which lists the physical disks (number, model, size, bus, drive
letters, whether one holds Windows or the sessions folder), and `getCandidateDetails()` and `readPreview()`, which
expose P17's media metadata and preview bytes.

## Headers

| Header | For |
| --- | --- |
| `api/recovery_api.hpp` | User interfaces: `RecoveryApi`, `ApiOptions`, `PlatformHooks` (declared only) |
| `api/api_types.hpp` | User interfaces: every value the API takes and hands out, `toString` of each enumeration (stable names, lower case with `-`), `formatSize`, `kApiVersion` |
| `api/api_platform.hpp` | Tests and tools, not user interfaces: `PlatformHooks` (disk opener, disk resolver, disk lister, system folder, playability checker, a hook on every progress report, the event queue's file limit) and `createRecoveryApi(options, hooks)`. It includes engine headers |

The GUI-facing headers include the standard library, each other and the engine's structured errors
(`recovery/error.hpp`, `recovery/result.hpp`: the error categories of the plan's section 30) and nothing else.
The tests prove it: `tests/api/gui_workflow.cpp`, a user interface's whole workflow, is compiled against a copy of
those four headers alone (`build/<preset>/tests/api_only_include`). No other engine header can be reached from
there. `ApiWorkflowTest.TheGuiFacingHeadersIncludeNothingOfTheEngine` checks the include lines too.

The API is C++20, a static library with its own value types (pimpl: `RecoveryApi::Impl`). `kApiVersion` (1) goes
up with changes that break callers. A C interface for other languages (.NET) could be layered on it later.

## Values

- Text is UTF-8. Names, labels and tags come from untrusted disks: they are valid UTF-8 but may hold any character,
  so a user interface must not interpret them (as markup, format strings or paths to open). Paths are
  `std::filesystem::path`.
- Times are UTC (`Time` = `sys_time<milliseconds>`). A FAT time has no zone: `FileTime::local` is set, and `time`
  holds the recorded clock reading, to be shown as it is. Media times (`MediaTime`) are ISO 8601 text to the
  precision the file records, with the instant when the zone is known.
- `CandidateId` (1 for the first candidate delivered), `ImagingId` and session ids (the engine's
  `YYYYMMDD-HHMMSS-xxxxxxxx`) identify things. A session id the API takes is checked: letters, digits, `-` and `_`,
  at most 128 characters, so it can never name a path outside the sessions folder.
- Errors are the engine's `Error` (`code`, `message` for people, `systemErrorCode`). `InvalidInput` covers a
  request that cannot be done now or at all: an unknown session or candidate, a busy session, a complete scan,
  settings out of range. `IoError` covers a source that cannot be read, with a hint: administrator rights, or "no
  such disk". `DestinationError` covers a destination, report, image or sessions folder that cannot be used: on
  the source, a file in the way, an existing file.

## Sources

`listSources()` reads nothing from any disk. The platform's `DiskLister` (`include/storage/disk_list.hpp`,
`src/storage/windows/windows_disk_list.cpp`) lists the present `GUID_DEVINTERFACE_DISK` interfaces (SetupAPI). It
opens each with desired access 0, which needs no administrator rights, and asks its number, description, bus and
size. It then maps the fixed and removable drive letters to disks with the destination guard's resolver. The API
marks the disk that holds `%SystemRoot%` (`system`) and the one that holds the sessions folder
(`holdsSessions`): a session cannot be kept on the disk it scans, so scanning that disk needs another sessions
folder. `description` is for people: `Disk 7: Generic STORAGE DEVICE, 29.7 GiB, USB (E:)`.

A `SourceRef` names a physical disk by number or an image file (with its sector size; 0: the image metadata's,
else 512). Device paths, folders and drives are refused as image files, with what to do instead. An image's
metadata (`<image>.imgmeta`) gives a scan its unreadable regions: the image holds zeros there, and the scan treats
them as unreadable (`ScanConfiguration::knownBadRegions`; recovery jobs too).

## Sessions and operations

Sessions are kept below `ApiOptions::sessionsRoot`, `%LOCALAPPDATA%\RecoveryEngine\Sessions` by default (the
CLI's). `startScan` creates one and leaves it open. `openSession` opens an existing one alone: a program that has it
open, this or another, makes the open fail. Opening repairs what a crash or damage left (`SessionDetails::repairs`).
`closeSession` closes it; it is refused while an operation runs. `listSessions` reads every session's summary
without opening it. Sessions open here show their live state; one recorded as running elsewhere shows `Running`,
and only opening it tells an interrupted one (`RunState::Interrupted`).

An operation (a scan, or a recovery of one or more jobs) runs on a thread of the API's. The call that starts it
returns at once, after checking what it can: settings, the source opens and is the session's (its fingerprint), the
destination is safe. A session runs one operation at a time. Another request fails with `InvalidInput` (busy) and
records nothing. Several sessions, and imagings, run at once. Pausing and cancelling go through the session (P16),
so they are recorded. A pause or cancellation asked before the engine's operation has begun (the session can only
act on a running one) is applied at the engine's first progress report.

### Recovery converges on its folder

`recoverCandidate`, `recoverCandidates` and `recoverAll` first find the session's recovery jobs that write to the
same folder (the same folder on disk, else the same text, case-folded). Jobs there that did not end are run first,
so their files are finished under the names they had. A new job then writes the requested candidates that no job
recovered there before (all of them with `RecoveryOptions::again`, which gives them new names: `photo (1).jpg`) and
that no unfinished job there writes. Candidates a job failed to write are tried again: an explicit request.
`RecoveryStart` tells what was requested, left out and run. When nothing is left, it lists no jobs and starts no
operation. Recovery runs whenever no other operation runs in the session, also when its scan did not complete
(the candidates delivered so far). The CLI refuses that (L172). A scan delivers its candidates in its last stage,
so a scan stopped before it has nothing to recover. `resumeRecovery` resumes a paused recovery, or runs every job
of the session that did not end, each to its own folder.

### Progress

`Progress` says which operation (`OperationKind`), its state (`Idle`, `Running`, `Paused`, or how the last one
ended: `Completed`, `Cancelled`, `Failed` with `error`), and its figures. `getProgress` takes the engine's live
figures while an operation runs (concurrent queries are what P15's `progress()` is for). A session idle since it
was opened reports what its scan recorded.

`ScanProgress::fraction` is an estimate for a progress bar. The stages are weighted by their usual share of a
scan's time: Deep: volumes 5%, MP4 examination 2%, fragment seeds 2%, the source pass 70%, MP4 delivery 1%,
fragments 5%, evaluation 15%. Quick: volumes and evaluation 50% each. Within a stage the weight is shared by its
units done. A stage's total can grow as it is learnt, so the API keeps the highest fraction told for the operation,
and a progress bar never goes back. A recovery's fraction counts every job's files.

## Events

`ApiOptions::onEvent` is told, on one thread of the API's (the `EventDispatcher`):

| Event | When | Carries |
| --- | --- | --- |
| `OperationStarted` | A scan, recovery or imaging began (before it can report anything) | `progress` |
| `Progress` | The engine reported progress (every `ApiOptions::progressInterval`, 250 ms, and at each stage's start and end) | `progress` |
| `Paused`, `Resumed` | The user paused or resumed (a pause asked before `OperationStarted` is in its state) | `progress` |
| `CandidatesFound` | The scan delivered candidates (now in `getCandidates`) | `firstCandidate`, `candidateCount` |
| `FilesRecovered` | A recovery job is done with files (each update of the job) | `files`: candidate, job, the file written with its size and missing and unreadable bytes, or why not |
| `OperationFinished` | The operation ended | `progress.state`, `progress.error` |

Every event names its session (`session`) or its imaging (`imaging`). Events are delivered one at a time, in the
order they were posted, with no lock of the API held, so the callback may call any member function (not the
destructor). An exception from the callback is caught and dropped. The queue never makes an operation wait, and
its memory is bounded:

- a `Progress` event replaces the one of the same operation still queued, and goes last (the latest state, after
  every earlier event);
- `CandidatesFound` events of a session that follow each other merge into one range;
- `FilesRecovered` events are dropped once 100,000 files wait (`PlatformHooks::maxQueuedFiles` in tests). The next
  `FilesRecovered` or `OperationFinished` event of that session says how many in `filesDropped`, and
  `getCandidates` has their state.

A user interface hands events to its own thread: it posts, never sends. The callbacks, and what they refer to, must
stay valid until the API's destructor returns: its threads may call them until then. Without a callback no event thread runs, and
`getProgress` and the `wait` functions tell the same. `ApiOptions::onLog` gets the engine's log records (`LogEntry`:
level, component, message, fields, one escaped line), on the thread that logs. It must be quick and must not call
the API.

## Candidates, details and previews

The API keeps every candidate of an open session as a list shows it (`CandidateInfo`, made with P17's
`describeCandidate`). It also keeps what each recovery job did with each candidate: a `RecoveryRecord` per job,
updated through P16's update hooks as the scan and the jobs record their updates. `getCandidates` filters and pages
that list without touching the engine. A candidate's `recovery` state follows P17's rule, first match wins:
Recovered, Pending, Failed, NotRecovered (`ApiRecoveryTest.TheListsRecoveryStateAgreesWithTheEnginesIndex`
checks it against `metadata::RecoveryJobIndex`).

`getCandidateDetails` adds the engine's explanation (text: how the file was found and checked), every job's record,
and, with `readMedia`, the media metadata read from the source on demand (`readMediaMetadata`). The source must be
attached and unchanged: when it is not, `mediaError` says so, and the rest is there. `readPreview(session,
candidate, preview, offset, maxBytes)` hands out a preview's bytes: the content, an Exif thumbnail, cover art.
They come from `offset`, at most 64 MiB a call, so a player can stream a video piece by piece. Previews are read,
never written. The source for details and previews is opened once per session and kept, with the metadata of up to
8 candidates for previews read piece by piece.

## Reports

`exportReport` writes `report::jsonReport` or `report::textReport` (the `recovery_report` library: P18's report
builder, moved out of the CLI so the CLI and the API write the same document; `docs/recovery/cli.md` lists its
members). The file is new (`CREATE_NEW`: an existing one is never overwritten) and must not be on the session's
source. Reports can be written while an operation runs: the report is as of the call.

## Lifetime and threads

| Object or thread | Rule |
| --- | --- |
| `RecoveryApi` | Every member function from any thread at any time, also from the event callback (not the destructor). |
| An operation's thread | Runs the engine's operation, the session's update hooks and the progress callback. It holds a reference to its `OpenSession` until it returns. |
| The event thread | Calls `onEvent`, one event at a time, with no API lock held. |
| An imaging's thread | Runs `ImageWriter`; cancelled through a `CancellationSource`. |
| `OpenSession` | Shared: the API's map, each call that uses it and its operation's thread hold references. The last one closes the session, on whichever thread drops it. On the operation's own thread its `std::thread` is detached, at the thread's very end. |
| Locks | `OpenSession::opMutex_` (the operation's state) may be held while the session's own locks are taken (pause, resume and cancel record a state change), never the other way. The candidate index and the preview state have their own locks, taken alone. The session calls its hooks with none of its locks held. |

The destructor first refuses new operations. It then cancels every operation and imaging (sessions record the scans
and jobs as cancelled: they resume later) and waits for them, still delivering their events. Then it closes the
sessions, waiting for the last reference a callback may hold, and stops the event thread. Events still queued then
are not delivered.

## What changed in earlier phases

- P16: `SessionOptions::onScanUpdate` and `onJobUpdate` are told each update once the session has recorded it (in
  the journal and in what its accessors return), on the operation's thread, outside the session's locks. They are
  not told anything when a journal is replayed. Both are empty by default, and the session behaves as before
  (`RecoverySessionTest.TheUpdateHooksAreToldEachUpdateOnceItIsRecorded`).
- P18: the report builder (`report.cpp`, `json.cpp`, `utf8.hpp` and the text formatting of `format.cpp`) moved to
  the new library `recovery_report` (`include/report/`, `src/report/`, namespace `recovery::report`). The CLI uses
  it through using-declarations. Its output and its 88 tests are unchanged. The CLI tests' JSON reader and card
  fixtures moved to `tests/support/` (`json_reader`, `card_fixtures`), shared with the API tests.
- Storage: `DiskLister` and its Windows implementation (`setupapi`).

## Windows assumptions

- Disks are listed through SetupAPI's present `GUID_DEVINTERFACE_DISK` interfaces, each opened with desired access 0
  and asked `IOCTL_STORAGE_GET_DEVICE_NUMBER`, `IOCTL_STORAGE_QUERY_PROPERTY` and `IOCTL_DISK_GET_DRIVE_GEOMETRY_EX`
  (all `FILE_ANY_ACCESS`). A reader without a medium has no geometry: size 0.
- Drive letters come from `GetLogicalDrives`. Only fixed and removable drives are resolved, with critical-error
  dialogs turned off for the calling thread (`SetThreadErrorMode`), so an empty card reader does not ask for a disk.
- `%LOCALAPPDATA%` and `%SystemRoot%` are read from the C runtime's copy of the environment (`_wdupenv_s`).
- Reading a physical disk needs administrator rights. Listing disks does not.

## Tests

`tests/api` (`recovery_api_tests`, label `api`, 59 tests) runs the API on the scan tests' card, written as an image
file and as a simulated physical disk (`DiskRack`: disks by number, opens that fail on demand), with a disk
resolver that puts chosen folders on chosen disks and every other path on disk 0. Every test has its own
`ApiWorld`: a temporary folder, a sessions folder, the API with `progressInterval` 0 and its events recorded.

| File | What it shows |
| --- | --- |
| `api_workflow_test.cpp` | P19's acceptance: `gui_workflow.cpp` is compiled against the GUI-facing headers alone. Through the API alone it lists the disks, inspects, scans (pausing and resuming), lists, details and previews every candidate, recovers everything, exports the report and closes the session, on an image file and on a physical disk. The files written are the card's originals byte for byte. The headers include nothing of the engine |
| `api_scan_test.cpp` | Deep and Quick scans deliver what the engine's stages deliver; the list's values; settings refused before a session is made, or kept with it; the events of a scan in order (progress never goes back, every candidate found once); one operation per session, two sessions at once; an idle session's progress |
| `api_events_test.cpp` | One event at a time on one thread of the API's; the callback calls the API (and starts the recovery from the scan's last event); a callback blocked through a whole scan gets merged events and the scan does not wait; a full queue drops files and says how many; exceptions from the callback; no callback; the log callback |
| `api_concurrency_test.cpp` | Eight threads query progress, candidates, the session, the session list and details while a scan and then a recovery run: consistent answers, nothing goes back; two sessions scanning at once |
| `api_cancellation_test.cpp` | A scan cancelled in each of its 7 stages, then resumed, delivers the same candidates; a cancellation before the first report; pause, resume, cancellation while paused; a cancelled recovery finished by asking again, or by `resumeRecovery`, with no file written twice; the API destroyed while a scan runs, and the scan resumed by the next one |
| `api_recovery_test.cpp` | Every file written and told once, byte for byte; nothing twice in the same folder (also spelt otherwise), `again`, another folder; one candidate, filters; requests refused before anything is recorded (unknown candidates, a file in the way, a changed source, a folder on the source disk); the list's recovery states against the engine's index |
| `api_session_test.cpp` | A session restored after the program closes; a scan interrupted by a crash (the session folder copied at the source pass) resumed to the same candidates; one program at a time; ids checked; JSON and text reports, never overwriting, never on the source; a session never kept on the disk it scans |
| `api_details_test.cpp` | A photo's metadata and its content preview, whole and in pieces; audio and video details; requests out of range; no media read; the source gone and back; what recovery jobs did |
| `api_sources_test.cpp` | The disk list (by number, letters, system and sessions disks, descriptions); inspecting the card and a disk of three volumes; sources that cannot be read and the hints; imaging a disk with bad sectors (zeros, their list, a scan of the image), cancelled and resumed, never overwriting, never on the source |

Also: `RecoverySessionTest.TheUpdateHooksAreToldEachUpdateOnceItIsRecorded` (label `session`) and
`DiskListTest` (label `integration`: the real disk list of the computer, read-only, no administrator rights).

## Known limitations

See [../limitations.md](../limitations.md), section "GUI API (P19)": L181–L196 (and the P19 notes on L168, L172,
L177, L178 and L179).
