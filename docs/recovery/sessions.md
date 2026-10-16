# Sessions (P16)

A recovery session (`RecoverySession`, `include/session/recovery_session.hpp`, library `recovery_session`) is one
scan of one source and the recovery jobs that write its candidates, kept on disk as they go. The application can
close, crash or be killed at any point: the session opens again as it was at its last record, and a scan or a job
that did not end resumes from there instead of starting again.

```
RecoverySession::create(root, source, configuration)        <root>/<id>/session.journal
        │
        ├─ runScan(source, formats, media)      ── ScanCoordinator (P15) ──► ScanUpdate ──► journal, then memory
        ├─ addRecoveryJob(destination, ids)                                            JobCreated
        └─ runRecovery(job, source)             ── RecoveryJob (P15) ──► FileStarted, RecoveryJobUpdate ──► journal

RecoverySession::open(<root>/<id>)  ── replays the journal: ScanCheckpoint, candidates, jobs' checkpoints, states
```

A session folder is `<root>/<id>`. The id is the time the session was created and 32 random bits
(`20261006-101530-3fa94c2e`). The folder holds the journal, `session.journal`, and the copies of a journal found
damaged (`session.journal.damaged-<n>`). The root is chosen by the caller. It must not be on the source: it is
checked as a recovery destination is (`checkDestinationSafety`), before anything is created and again after.

## What a session stores

| The plan's item | Where |
| --- | --- |
| source information, source type | `SessionInfo::source`: what `getInfo()` said (type, path, size, sector sizes, disk number, vendor, product, removable; a physical disk's serial number from journal format 3 on) and the source's fingerprint; a physical disk as the session last found it (L147) |
| filesystem | `SessionInfo::volumes`: each volume's filesystem, label, serial number, cluster size, files, or why it could not be used; the scan's checkpoint holds the volumes' whole `CandidateScan` |
| configuration | `SessionInfo::configuration`: the scan's `ScanConfiguration` (the playability checker is not stored: `playability` says whether the scan uses one) |
| progress | `ScanStatus`: the state and its history, the stage, the updates recorded, the metrics (P15's `ScanMetrics`); `progress()` while it runs |
| candidates | `candidates()`, `candidate(id)`: every evaluated candidate the scan delivered, with all its evidence |
| recovered files | `recoveredItems(job)`: for each candidate a job is done with, the file written (its path and its `ReconstructionReport`: missing, unreadable and reallocated bytes) or why not; `RecoveryJobStatus` per job |
| errors | `errors()`: runs that failed (with their error), volumes that could not be used, files that could not be written, each with the time it was recorded |
| unreadable regions | `unreadableRegions()`: the source ranges the scan found unreadable, merged |
| timestamps | when the session was created and last written; the time of every state change of the scan and of each job; when each job was added |
| engine version | the engine that created the session, and the one of every run (each state change names it) |

## States

The scan and each recovery job have a state, recorded with every change:

| State | Meaning | Resumes |
| --- | --- | --- |
| `Started` | Running. A started scan or job that is not running in this session object is *interrupted*: its process ended without recording an end (a crash, a kill, a power loss). | yes |
| `Paused` | Paused through the session (`pause()`). A process that ends while paused leaves it paused. | yes |
| `Cancelled` | Stopped through the session (`cancel()`). What it did is kept. | yes |
| `Completed` | Done. | no |
| `Failed` | Stopped by an error, recorded with it (the source failed, the journal could not be written). | yes |

Resuming is running again: `runScan()` and `runRecovery()` go on from the last update recorded, whatever the state
(the user's P16 decision). A scan with no state yet has never run. `pause()` and `resume()` act on the operation
under way and record `Paused` and `Started`; a pause asked for while the operation is starting is recorded right
after its `Started`.

## The journal

The journal is append-only. Each record is one thing that happened:

| Record | Written | Holds |
| --- | --- | --- |
| `SessionCreated` | first, by `create()` | the id, the engine, the source and its fingerprint, the configuration |
| `ScanState` | at each state change of the scan | the state, the engine, the error (Failed), the stage and metrics then |
| `ScanUpdate` | for every update the scan hands out (P15) | the `ScanUpdate`, all of it |
| `JobCreated` | by `addRecoveryJob()` | the job's number, its destination (absolute, normalised), its candidates |
| `JobState` | at each state change of a job | as `ScanState`, with the job's metrics |
| `JobUpdate` | for every update a job hands out | the job's number and its `RecoveryJobUpdate` |
| `FileStarted` | when a job has created a file, before its data | the job, the candidate, the file's path |
| `Damage` (optional) | when the session found its journal damaged | where, what was dropped, the copy kept |
| `SourceFound` (optional) | when a scan or a job begins on the session's physical disk under another number, or described otherwise (L147) | the disk's path and number, vendor, product, removability and serial number now |

```
header  "RCVSESSN"  version (u32)  CRC-32 (u32)                                                           16 bytes
record  "SREC"  type (u16)  flags (u16)  time (i64, ms, UTC)  length (u64)  payload CRC-32  header CRC-32   32 bytes
        payload                                                                                         length bytes
```

Payloads use a compact binary encoding (`src/session/session_codec.cpp`): LEB128 for unsigned integers (the
shortest form only), zigzag for signed ones, a length before strings and lists, a presence byte before optional
values. One function per type lists its members once, and both the encoder and the decoder are driven by it, so
what is written is what is read. Decoding treats the bytes as untrusted. A length must fit in the bytes left; an
element takes at least one byte, so a list's count is bounded by them. An enumerator must be one of its type, and a
number must fit its field. Candidates must keep their invariants (`validateCandidate`, `validateFileCandidate`).
A payload must be used up exactly.

### Crash safety

A record is written and flushed (`FlushFileBuffers`) before the scan or job that made it goes on: the session's
sink returns only then, so P15's rule holds on disk. An update that is not on the device was not taken, and a
resumed run hands it out again. If writing a record fails, the file is cut back to where the record began. If even
that fails, or a flush fails, the journal is *broken*: nothing more is written, the operation stops, and opening
the session again reads what reached the file. The session writes a record before it takes it in, so what it
holds never runs ahead of the journal.

Files a recovery job begins are recorded from the job's workers, several at a time. Each waits until its record
is flushed, and threads waiting together share one flush (group commit).

### Reading

`JournalReader` checks each record before anything of it is used: its magic, its header CRC (which protects the
length), that it fits in the file, its payload CRC. Where a record does not check, reading stops, and the reader
says how the journal ends:

* **torn tail**: nothing intact follows. This is what a crash while writing the last record leaves: the record is
  incomplete, or the file grew and its data did not reach the device.
* **damage**: an intact record follows (found by looking for the next record magic whose record checks).

A record with flags this engine does not know is of a newer format: reading fails.

## Opening a session

`open()` takes the journal first: `JournalFile` opens it for writing with `FILE_SHARE_READ`, so no other session
object, in this process or another, can open it until it is closed. Readers (`readSessionSummary`,
`listSessions`) still can. Then it replays the records in order: the session's record, state changes, the scan's
updates (applied to a `ScanCheckpoint`, and their candidates kept), jobs, their updates (applied to each job's
`RecoveryJobCheckpoint`) and the files they began. Nothing is changed before the replay is done, so a session that
is refused is left as it is.

It then repairs what a crash or damage left:

* a torn tail is cut off;
* damage: a copy of the whole journal is kept as `session.journal.damaged-<n>`. The journal is cut where the damage
  begins, and a `Damage` record says what was dropped (the bytes, and the intact records after the damage). The
  session is what the records before the damage say, a consistent earlier point of the scan, and resuming does the
  lost work again (the user's P16 decision). A record that checks but cannot be used is damage too: one that does
  not decode, or an update that does not follow (P15's checkpoint refuses it);
* a run whose last update was recorded but not its end (a crash in between) is complete: its `Completed` is
  recorded.

Refused, unchanged: a file that is not a journal or whose header is damaged; a journal whose session record does not
check (its creation did not finish); a newer format.

## Running the scan

`runScan()` runs the scan, or resumes it. It refuses, without recording anything, when:

* another operation runs;
* the scan is complete;
* another engine started it (see Versions);
* the playability checker does not match the configuration;
* the source is not the session's;
* the formats, validators or settings are not those of the scan's first run (P15's scan identity).

Then it records `Started` and runs a `ScanCoordinator` with the replayed checkpoint. The session's own `JobControl`
replaces the run options' (pause, resume and cancel go through the session). Each update goes to the journal
(flushed), then into the session's checkpoint and candidates. When the run returns, its end is recorded: `Completed`,
`Cancelled`, or `Failed` with the error.

What a resumed scan does again is P15's unit in progress (L134), never a unit an update recorded: the tests check
that no update of a stage the cut journal had finished is handed out again, and that the first new update follows
the last recorded.

### The source

The source must be the session's (`checkSameSource`): the same type, size and sector size, and the same
fingerprint. The fingerprint is SHA-256 of the first 64 KiB of the source and the first 64 KiB of every partition
its partition table lists, each preceded by its offset and length, unreadable sectors as zeros. Two cards of one
model and size differ there: their volume serial numbers, at least. A card written to since (FAT32's free count,
exFAT's percentage in use) does too. A scan of another medium in the same reader, or of a changed one, is refused
instead of mixing two media in one result (L146).

An image file must be under the same path (L141). A physical disk may have any number (L147): Windows numbers disks
as they are attached, so a card reader plugged in again, or found after a restart, may be another
`\\.\PhysicalDriveN`, and its old number another disk. It must be the same device, not only hold the same content
(the user's choice, 2026-10-16): when the session recorded a serial number, the disk must report it; when it
recorded none (a session of an older format, a device that reports none), the disk's vendor and product must be the
recorded ones (`deviceDifference`). A card reader and a USB enclosure report their own serial number, so a card in
another reader, or a disk in another enclosure, is another device (L246).

`findSessionDisk` opens the session's disk for the CLI and the API: the disk under the recorded number first, then
every disk attached (`storage::DiskLister`) of the session's size and sector size that is the same device, by its
content. It fails, saying why each disk tried is not the session's, when none is; when several are (copies in readers
of one model); and at once when the recorded disk cannot be opened for want of administrator rights. A scan or a job
that begins on the disk under another number, or on a disk that describes itself otherwise (the serial number of
an older session), records a `SourceFound` record first: the session's source takes the disk as it is now, and so does
the scan's identity (`ScanCheckpoint::moveSource`), so that the scan resumes there. The record is optional: an older
engine skips it and knows the disk by its old number. `possibleSessionSources` gives every disk attached that may be
the session's (its device, its size or no size told; the recorded number when the list does not show it): a report
or a log written without opening the disk is kept off all of them.

## Recovery jobs

`addRecoveryJob(destination, ids)` records a job: the candidates to write (none given: every candidate delivered
so far), and its destination. `runRecovery(job, source)` runs it, or resumes it: P15's `RecoveryJob` writes the
candidates not done yet, with the names one writer would give them. Its updates go to the journal like the scan's.
A job that writes the first candidates in order (every candidate, the usual case) uses the session's own list.
Otherwise it uses copies.

A crash while a file is written (L140): the session records each file as soon as `RecoveryWriter` has created it
and before any of its data is written (`FileStarted`, flushed; P7's `FileCreatedCallback`, passed through P15's
`RecoveryJobOptions::onFileCreated`). A file whose candidate no update says is done may be incomplete. Before the
job resumes, the session removes it if it is still a regular file no larger than the candidate's expected size (it
cannot be anything else the job wrote) and lies below the job's destination (P20 security review: the path comes
from the journal, and a damaged or foreign journal must not make a resume delete a file elsewhere; such a file is
left alone with a warning, L201). The job then writes it again, under the same name. The window this leaves:
a crash between the file's creation and the flush of its record leaves an empty file the session does not know of,
and the job writes the candidate under the next free name (L148).

Each file's `ReconstructionReport` is kept with the job's update, so what a file lacks (missing, unreadable,
reallocated bytes) is recorded next to where it went (L42).

## Versions

* **Journal format**: version 2 (after P20: the source pass's stage stops, `PassState::stops` and
  `ScanMetrics::limitStops`; L228). An engine reads its own format and older ones, and refuses a newer one without
  changing anything. A journal keeps its format: records appended to a format-1 session are encoded in format 1
  (the codec writes a field only in the formats that have it), so an older engine can still open it. The stops are
  then not kept, which matters only to a scan run in it, and an unfinished format-1 scan does not resume anyway
  (its checkpoint format is older). Records of types it does not know are skipped when they are marked optional (an older reader
  may skip them), and the journal is refused otherwise, as for flags it does not know.
* **Engine**: a session another engine version created opens. Its candidates, recovered files and recovery jobs can
  be read and run. An unfinished scan resumes only with the engine that started it: P15's scan identity holds the
  engine version (L141), since two engines' results would mix in one scan. A scan that never recorded an update can
  be started by any engine.
* **Checkpoint format**: an unfinished scan also resumes only in the checkpoint format it began in (2 since L228:
  before it, the hit limit counted signatures). The session says so before anything runs (`ScanStatus::runnable`,
  `notRunnable`: "start a new session"); its candidates are read and recovered as usual, and its report names its
  limit as it counted (`ScanStatus::checkpointVersion`).

## Listing sessions

`readSessionSummary(folder)` reads a session without opening it, so a session another process runs can be read. It
reads the session's record, its scan states and its jobs, and skips the updates unread. `listSessions(root)` does
that for every folder below `root` that holds a journal. A journal that cannot be read gives a summary with its
error (a newer format, for one).

## Update hooks (P19)

`SessionOptions::onScanUpdate` and `onJobUpdate` are told each update of the scan and of a recovery job once the
session has recorded it: it is in the journal, and the accessors return what it added (candidates, recovered
files). They run on the operation's thread, outside the session's locks, before the operation goes on. They are not
told anything when a journal is replayed (`open()`). Both are empty by default; the API (P19) uses them to keep its
candidate list and recovery states up to date and to raise its events. They must be quick and must not throw; they
may call the accessors and `pause()`, `resume()` and `cancel()`.

## Threads

| Object | Rule |
| --- | --- |
| `RecoverySession` | `runScan()` and `runRecovery()` on the calling thread, one operation at a time (another fails as busy). `pause()`, `resume()`, `cancel()`, `progress()` and every accessor but `checkpoint()` from any thread at any time: they return copies. `checkpoint()` only while no operation runs. The destructor cancels an operation under way and waits for it. |
| `JournalFile` | `append()` from any thread (records written one after the other, flushes shared); the rest one owner. |
| `JournalReader` | One owner. |

Inside, the session has two locks: one for what the records say (the checkpoint, candidates, jobs, states), taken
by the operation's thread, a job's workers (files begun) and the accessors; and one for the operation under way
(its control, its state). They are always taken in that order, and the journal's own locks between them. The
scan's sink and progress callback run on the scan's thread and take no lock the accessors hold for long.

## Memory and disk

Memory: a session holds what P15's caller holds: the scan's checkpoint (every stage's results, L139; the volumes'
candidates are shared with the coordinator's own checkpoint) and every evaluated candidate (about 1 KB each, plus its
names and regions: L212). Opening a session builds them from the journal (L145).

Disk: the journal holds every update (L144). Its size is about the scan's results: the volumes' candidates, carves,
MP4 and fragment candidates, the evaluated candidates, and a few hundred bytes per pass update. On the scan tests'
card (2 MiB, 10 candidates) it is 17.6 KB in 33 records.

## Tests

`tests/session` (executable `recovery_session_tests`, label `session`) holds the following.

* `session_codec_test.cpp` covers the payloads:
  * every update of a deep scan, and every kind of value, comes back as it was: decoding and encoding again gives
    the same bytes, and the candidates explain themselves the same;
  * decoded updates make the same checkpoint;
  * the stage stops come back in format 2, and format 1 encodes and decodes the rest as before;
  * a disk's serial number comes back in format 3 and is not in older formats' encoding; the record of a disk found
    again is encoded alike in every format;
  * payloads cut at every byte are refused, and so are values out of range, numbers not in their shortest form,
    lengths beyond the data, broken candidates and paths that are not UTF-8;
  * random damage never crashes the decoder.
* `session_journal_test.cpp` covers the journal:
  * records come back as written;
  * a cut anywhere in the last record, or garbage after it, is a torn tail;
  * damage before intact records is told apart from it;
  * headers of other formats or damaged are refused, and so are records with unknown flags;
  * there is one writer at a time;
  * appends from 8 threads all land.
* `recovery_session_test.cpp` covers the plan's tests:
  * a normal session, opened again with everything it stores, and its recovery jobs with their files and reports;
  * pause and resume, live and after the process ended while paused;
  * crash simulation: the journal cut after every record, and in the middle of records, then opened and resumed to
    the uninterrupted result without doing again a recorded unit; a recovery job cut after every record of its files,
    with the destination as a crash leaves it (the file begun empty, half written or whole), resumed with the same
    names; a run complete but not recorded so;
  * corrupt data: damage in headers and payloads, damaged journal headers and session records, records that check
    but cannot be used, and random damage;
  * version compatibility: newer formats, record types and flags refused untouched, optional records skipped, a
    session of another engine opened without resuming its scan, a format-1 session that keeps its format, and
    an unfinished scan of checkpoint format 1 that says it does not resume;
  * besides those:
    * progress and results read from three threads all through a scan that is paused and resumed meanwhile;
    * only the session's source;
    * a scan going on only with the formats it began with;
    * one session object at a time;
    * the destructor cancelling;
    * the playability checker;
    * errors and unreadable regions;
    * listing.
* `session_disk_test.cpp` covers a physical disk under another number (L147), on simulated disks
  (`tests/support/disk_rack.hpp`):
  * the same device with the same content is the session's under any number; the same content in another device
    (another serial number, none, another model for a session that recorded none) and the same device written to
    since are not; an image file is known by its path;
  * the disk is found under its number, or under another when another disk took its number; when it is not attached,
    every disk tried says why; twice the same device and content is refused; without administrator rights, at once;
    without a disk list, the recorded number alone;
  * reports stay off every disk that may be the session's, and not off its old number when another device has it;
  * a scan stopped in the source pass, its disk plugged in again under another number: the old number is refused
    with nothing recorded, the scan resumes on the new one to the uninterrupted result, recovers there, one optional
    `SourceFound` record says so, and the session opened again and its listing show the new number;
  * a session of format 2 learns its disk's serial number (in format 2), and refuses another serial from then on.

## Known limitations

L144–L152 in [../limitations.md](../limitations.md). L138 and L140 are resolved by P16; L40, L42, L99, L139 and L141
changed with it. L152 is resolved after P20: journal format 2 (L228), and format-1 sessions are read and kept.
L147 is resolved after P20 (2026-10-16): journal format 3 records a disk's serial number, and a session finds its
disk under another number; L246 and L247 are what that leaves.
