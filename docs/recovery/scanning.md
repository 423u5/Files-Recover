# Scanning (P15)

A scan of a source runs every stage of the engine as one job: `ScanCoordinator` (`include/scan/scan_coordinator.hpp`,
library `recovery_scan`). It runs the stages on a bounded pool of workers. It can be cancelled and paused from
any thread, reports its progress and metrics, and hands out its work as updates at consistent points. A later scan
resumes from those updates without doing again what they record. `RecoveryJob` (`include/scan/recovery_job.hpp`)
writes the candidates of a scan to a destination with the same controls.

```
                       ┌──────────── ScanCoordinator::run() (the calling thread) ────────────┐
Volumes ─► MP4 examination ─► fragment seeds ─► source pass ─► MP4 delivery ─► fragments ─► evaluation
   │             │                  │               │                                          │
 workers      workers            workers      scanner thread + workers                      workers
(per volume) (per candidate)   (per candidate)  (carves, analyses)                       (validate, hash)
                                                                                                │
                                              updates (ScanUpdate) ──► caller ──► ScanCheckpoint
```

A Quick scan runs Volumes and Evaluation only: the filesystems' candidates, validated, hashed and checked for
duplicates. A Deep scan runs every stage. A stage the configuration turns off (`carving`, `mp4`, `fragments`)
completes at once, with an update of its own.

## Stages

| Stage | What it does | Parallel unit | Checkpointed unit |
| --- | --- | --- | --- |
| Volumes | `readPartitionTable`, then for each partition (or the whole device when unpartitioned or unrecognised) `openFilesystemRecovery` and `findCandidates` (P3–P7). A volume without a usable filesystem is recorded with its error. | volume | volume |
| MP4 examination | `Mp4RecoverySteps::examine` (P12): each filesystem candidate's data analysed when its name or content says MP4 | candidate | candidate (batches of `checkpointItems`) |
| Fragment seeds | `FragmentRecoverySteps::examineSeed` (P13): each deleted file with a guessed layout, its format told by its content or name | candidate | candidate |
| Source pass | One signature scan of the whole source for carving (P8–P10), MP4 recovery's carving (P12) and fragment reconstruction's carving (P13) | hit (carve, MP4 analysis) | position: every hit before it committed |
| MP4 delivery | `Mp4RecoverySteps::deliver`: ids, names, warnings | — | the stage |
| Fragments | `FragmentRecoverySteps::reconstructNext`, seed after seed (each settled seed is evidence for the next) | — | seed |
| Evaluation | `CandidateEvaluation` (P14) with a pool: each candidate validated and hashed on a worker, delivered in order | candidate | delivered candidate |

Each stage function first restores what earlier runs did (from the checkpoint). It replays recorded examinations
into the steps objects, restores MP4 recovery's state, replays fragment reconstruction's pass events and the claims
of reconstructions already delivered. Then it runs what is left.

## The source pass

A deep scan reads the source once. The pass has three participants:

* **The scanner thread** runs `SignatureScanner::scan` over the whole source, from the checkpoint's position. Its
  registry holds the carving formats, the MP4 format of MP4 recovery when the registry has none, and the moov probe
  of fragment reconstruction (`FragmentRecoverySteps::extraFormats`). Each hit goes to a bounded queue; the
  scanner waits while the queue is full.
* **The coordinator thread** takes hits into a window (`window`, at least 64). For each hit it decides which stages
  want work done ahead: carving (the format is a carving format and `CarveSkipState::skips` says no), MP4 recovery
  (`Mp4RecoverySteps::skips` says no) and fragment reconstruction (`FragmentRecoverySteps::wants`). It hands that
  work to a worker. It then commits the hits in source order, one offset at a time (every hit at an offset
  together), on its own thread, through each stage's rules.
* **The workers** carve hits (`FileCarver::carve` with validation, shared by carving and fragment reconstruction,
  which carve alike) and prepare MP4 hits (`Mp4RecoverySteps::prepare`: the carve without validation, and the
  analysis when the commit will need it).

The decisions made when a hit is taken are hints. The commit decides with the state of every commit before it, and
makes on the spot any work a stage needs that was not prepared. Two hits at the same offset can make a hint wrong:
a carve committed at an offset changes what skips the next format's hit there. A stage therefore commits exactly
what it commits when it scans on its own, with the same carve ids (they are given at the commit). Work prepared for
a hit that a commit then skips is wasted, never used. Hits of a self-synchronizing format (MP3, ADTS frames) are
held, not prepared, while an earlier hit of their format is not committed. Every frame of a stream is a hit, and
the stream's first carve usually covers them all.

Consistent point: the offset of the oldest hit not committed (or the scanner's position when none is waiting).
Every file start before it has been examined and its hits committed by every stage; none after it. An update is
handed out every `checkpointBytes` of the source or `checkpointInterval`, before a pause, and at the end. It holds
the pass's state (`PassState`: position, scan report so far, carving's counts and skip state, the next carve id),
the carves committed since, MP4 recovery's changes since (`Mp4RecoverySteps::takeChanges`) and fragment
reconstruction's pass events since (`FragmentRecoverySteps::takeEvents`: the hits and carves that changed its
state, replayed on resume). On resume the scanner starts at the position with the hit limit minus the hits so far.

## Parallel processing and what it does not change

A scan delivers what the stages deliver when run one after the other on one thread: `FilesystemRecovery`,
`FileCarver::run`, `Mp4Recovery::run`, `FragmentRecovery::run` and `CandidateEvaluation::run` on the same source.
The candidates have the same ids, in the same order, with the same evidence. This holds whatever the number of
workers, and across pauses and interruptions. The test `ADeepScanDeliversWhatTheStagesDeliverOneAfterTheOther`
compares every candidate line for line. Why it holds:

* Work that depends on nothing before it runs in parallel: a volume's traversal, an examination, a carve, an MP4
  analysis, a candidate's validation and hash. Its result is used in order (`runOrdered` in
  `include/recovery/ordered_work.hpp`: at most `window` items in flight, results consumed in item order).
* Everything that depends on order runs on the coordinator thread, in order: the commits of the pass, the merges of
  MP4 recovery, fragment reconstruction's claims, the evaluation's ids and duplicates.
* A filesystem (`IFilesystem`, not thread-safe) is used by one thread at a time. A volume's traversal has one
  task. Fragment reconstruction's cluster-state cache has a lock. The evaluation holds a lock per volume around
  allocation queries. Everything else touching a filesystem runs on the coordinator thread.

## Updates and checkpoints

`ScanUpdate` (`include/scan/scan_state.hpp`) is the scan's only output. Each update is numbered (`sequence`, across
resumes) and belongs to one stage. It holds the work done since the update before, and the stage's state where a
stage has one. The first update holds the scan's identity. The evaluation's updates hold the results: the evaluated
candidates, in order. An update is a consistent point: everything before it is complete and nothing after it has
begun.

| Stage | An update holds |
| --- | --- |
| Volumes | the partition table and the volumes planned (the first); then each volume done: its filesystem and `CandidateScan`, or its error |
| MP4 examination, fragment seeds | the candidates examined since, in scan order |
| Source pass | `PassState`, the carves since, MP4 recovery's changes, fragment reconstruction's events |
| MP4 delivery | every MP4 candidate and MP4 recovery's report |
| Fragments | the reconstructions delivered since, and the counts so far |
| Evaluation | the evaluated candidates delivered since; the report with the last |

Every update also holds the unreadable regions found since and the metrics.

`ScanCheckpoint::apply` adds an update to a checkpoint, all of it or nothing. It refuses an update that does not
follow: the wrong sequence number, no identity in the first, an identity in a later one, a stage other than the
one in progress, parts of another stage, volumes or positions that do not fit, carves or candidates numbered out
of order. A refused update changes nothing. A checkpoint keeps every stage's results, except the evaluated
candidates: it keeps records of them (`EvaluationRecord`: id, method, status, content identity), and the
candidates themselves are the caller's.

`run(sink, &checkpoint)` resumes. The checkpoint must be of the same scan: the same `ScanIdentity`, which is the
engine version, the checkpoint format version, the source (type, path, size, sector size), the mode, the
configuration (as text) and the ids of the carving formats and media validators. Run options (workers, cache,
windows, checkpoint intervals) are not part of the identity: a scan may resume with others. A scan also checks the
source against the checkpoint: the partitions planned must be the same, the volumes scanned must open again with
the same filesystem, and replayed examinations, events and reconstructions must fit. A checkpoint that does not fit
is refused with `InvalidInput`, and nothing runs.

A run fails without corrupting anything. A sink that fails stops the scan; the update it refused is not applied to
the coordinator's own checkpoint either (`checkpoint()`), so a crash before an update is saved resumes from the one
before. A failing source stops the scan after a last consistent point. A cancelled scan returns
`ScanOutcome::Cancelled` after a last consistent point.

What is done again on resume: the unit in progress at the interruption, never a unit an update recorded. The
units are a volume's traversal, an examined candidate, the pass from its last consistent point, a seed's
reconstruction and a candidate's evaluation. The tests resume a scan after every one of its updates
(`AScanResumedAfterEveryUpdateEndsAsAnUninterruptedOne`) and after a cancellation in every stage, and compare
every candidate.

## Cancellation and pause

`JobControl` (`include/recovery/job_control.hpp`) is shared by the caller and the scan (copies share state).

* `requestCancellation()`: the job's token (in every stage's read options) is cancelled. Each stage stops at its
  next check: between units, inside long reads and walks (the stages' own checks). Work in flight when it comes is
  finished and handed out. A stage that is cancelled hands out a last update with what it completed.
* `pause()`: every read of the source waits at the `ScanSource`'s gate (`waitWhilePaused`). The coordinator thread
  hands out an update at its next safe point (between units, in the pass's loop), then waits. `ScanProgress::paused`
  is true once the coordinator waits, or once no read is under way. `resume()` goes on, and cancelling a paused scan
  ends it.

The time paused does not count in `ScanMetrics::elapsed` or the scan speed.

## Progress and metrics

`ScanCoordinator::progress()` (any thread) gives the stage, its units done and to do (volumes, candidates
examined, bytes of the pass, seeds, candidates evaluated), whether the scan is paused, and the metrics. The
`onProgress` callback runs on the scan's thread at most every `progressInterval`, and at every stage's start and
end.

| Plan metric | Where |
| --- | --- |
| bytes scanned | `ScanMetrics::bytesScanned`: the pass's position (0 in a Quick scan); `bytesRead`: bytes read from the source by every stage |
| scan speed | `ScanMetrics::scanSpeed`: bytes read per second of running time in this run |
| candidates | `ScanMetrics::candidates` (evaluated, one per file); `filesFound`, `carves`, `mp4Candidates`, `fragmentCandidates` by stage |
| recovered files, failed files | `RecoveryJobMetrics::recoveredFiles`, `failedFiles` (and `bytesRecovered`) |
| unreadable bytes | `ScanMetrics::unreadableBytes`: sectors that failed when read on their own; `RecoveryJobMetrics::unreadableBytes`: bytes written as zeros |
| validation failures | `ScanMetrics::validationFailures`: candidates delivered `Invalid` |
| elapsed time | `ScanMetrics::elapsed`, `RecoveryJobMetrics::elapsed`: running time over every run |

The metrics add up over every run of a scan: a resumed scan goes on from the checkpoint's.

## Reading the source

Every stage reads the source through one `ScanSource` (`include/scan/scan_source.hpp`), a read-only wrapper:

* **Pause gate**: a read waits while the job is paused.
* **Cache**: reads are served block by block (`blockSize`). A block is read from the source whole and kept in a
  cache of the `cacheBlocks` blocks used most recently. The scanner, the carves, the analyses and the evaluation
  read the same parts of the source one after the other, and a block still in the cache is not read again. A
  block whose read fails (a bad sector in it) is never cached. Its requests go to the source as they were made,
  so a reader sees the source's own answer and its sector-by-sector retry finds the bad sectors as it would
  without the cache.
* **Counts**: bytes delivered by the source, bytes asked for, bytes served from the cache, and unreadable bytes
  (sectors whose read on their own failed: every reader of the engine retries a failed read sector by sector).
  The unreadable regions found go into the updates.

## Memory

Bounded by the run options, whatever the source's size:

* the cache: `cacheBlocks` × `blockSize` (64 × 1 MiB by default), and one block per reader loading one;
* the pass: the scanner's block, a queue of hits, the window of hits in flight (each with its carve or analysis
  being made), and per worker the carve's reader cache (256 KiB);
* the other stages: the window of items in flight;
* the pool: `workerThreads` threads and a bounded queue.

What grows with the number of files found: the stages' results (filesystem candidates, carves, MP4 candidates,
reconstructions), kept until the evaluation, as the stages keep them on their own (L40, L99, L115, L129). They are
in the checkpoint too: a caller that keeps a checkpoint in memory keeps a second copy (L139).

Measured (`MemoryStaysBoundedWhateverTheImageSize`, 16 cache blocks of 1 MiB, 4 workers): the heap peaked at 19.8 MB
while scanning a 1.25 GiB image and at 20.1 MB for a 5 GiB one.

## The recovery job

`RecoveryJob::run(candidates, sink, resume)` writes evaluated candidates with `RecoveryWriter` (P7, unchanged),
one writer per worker. Its updates (`RecoveryJobUpdate`) name the candidates done since the last one: written (the
file, and what reconstruction found) or not (why). Every file an update names is complete and flushed.
`RecoveryJobCheckpoint` applies them, all or nothing, and refuses a candidate done twice. A resumed job writes only
the candidates not done. It must be the same job: the same destination, and candidates among those given.

Names: `RecoveryWriter` gives a file the first free name ("photo.jpg", then "photo (1).jpg"...), so the order in
which files that could take the same name are written decides their names. The job writes two candidates in
candidate order when their names, made safe (`safeFileName`) and folded, could meet at the same place: the same
name, or one's name and a directory on the other's way, at the same level. The fold is ASCII letters to lower case
and anything beyond ASCII to one same character; " (n)" suffixes and stems beyond 200 units are ignored. Other
files are written in parallel. A job, resumed or not, on any number of workers, names every file as one writer
does, one file after the other (`FilesGetTheNamesOneWriterGivesThemOneAfterTheOther`).

A file that cannot be written is reported and the job goes on. That covers a candidate with no data located, a
read that fails for a reason other than a bad sector, and a destination error. A cancellation stops the job after
the files being written. `RecoveryWriter` removes a file it could not finish, so a resumed job writes it again
under the same name. Pause and progress work as in a scan.

## The stages in steps

The stage classes of earlier phases gained step interfaces, so that the coordinator can share one pass, run
independent work in parallel and save and restore state. Their own `run()` drives the same steps, one after the
other, so it behaves as before (every earlier test passes unchanged):

| Phase | Addition |
| --- | --- |
| P8 carving | `CarveSkipState`: `FileCarver::run`'s skip rules as a value |
| P12 MP4 recovery | `Mp4RecoverySteps`: `examine`/`addExamination`, `skips`/`prepare`/`commit` per hit, `state`/`takeChanges`/`restore`, `deliver` (`Mp4Recovery::run` drives them) |
| P13 fragment reconstruction | `FragmentRecoverySteps`: `begin`, `examineSeed`/`addSeedExamination`, `extraFormats`/`wants`/`commit` per hit, `recordEvents`/`takeEvents`/`replay`, `reconstructNext`/`replayReconstruction` (`FragmentRecovery::run` drives them) |
| P14 evaluation | `EvaluationOptions::pool`, `window` and `resume` (`EvaluationRecord`, `recordOf`) |

## Threads

| Object | Rule |
| --- | --- |
| `WorkerPool` | Every member from any thread. Tasks must not throw (counted, never propagated), nor wait for tasks queued after them, nor submit to their own pool. |
| `JobControl` | Every member from any thread; copies share state. |
| `runOrdered` | Called on a thread that is not one of the pool's workers; `consume` runs on it, `produce` on the workers. |
| `ScanSource` | Reads from any number of threads; `open`/`close` alone. |
| `ScanCoordinator` | `run()` needs one owner and drives everything on its thread: commits, the sink, the progress callback. `progress()` from any thread at any time. |
| `ScanCheckpoint`, `RecoveryJobCheckpoint` | One owner at a time. |
| `RecoveryJob` | As the coordinator. |
| `Mp4RecoverySteps` | `examine()` and `prepare()` concurrently with each other and with `commit()` of other hits; the rest one owner. |
| `FragmentRecoverySteps` | `examineSeed()` concurrently with itself; the rest one owner. |

## Tests

`tests/scan` (executable `recovery_scan_tests`, label `scan`) holds the following.

* `scan_coordinator_test.cpp` uses a used FAT32 card with a file for every stage: active, HYBRID, FRAGMENTED and
  carved files, a duplicate, MP3 and MP4 found by carving. It checks:
  * the deep scan against the stages run one after the other, line for line, and the metrics;
  * 1, 2 and 8 workers;
  * Quick, and the stages turned off;
  * a cancellation in every stage, then resumed;
  * a resume after every update;
  * an update the sink failed to save;
  * a source that fails, then reads again;
  * pause, and cancellation while paused;
  * checkpoints of another scan;
  * updates that do not follow;
  * unreadable sectors.
* `scan_source_test.cpp`: the cache, its bound, bad sectors, the pause gate, concurrent readers.
* `scan_large_test.cpp`:
  * a 6 GiB virtual image (MBR with a FAT32 partition and an empty one, files beyond 4 GiB in the space no
    partition holds) scanned whole, then interrupted beyond 4 GiB and resumed with other worker counts;
  * memory measured on the heap for images of 1.25 and 5 GiB.
* `recovery_job_test.cpp`:
  * names colliding in every way, on 1, 3 and 8 workers, against one writer;
  * interrupted jobs resumed;
  * files that cannot be written;
  * bad sectors;
  * pause;
  * refusals;
  * a scan's candidates written and hashed again.

`tests/unit/core` tests the pool, job control and `runOrdered`. The steps have tests in their own suites:
`Mp4RecoveryTest.TheStepsDeliverWhatRunDelivers`,
`FragmentRecoveryTest.TheStepsRestoreFromTheirEventsAndReconstructions`, and
`CandidateEvaluationTest.APoolChangesNothingButTheThreads` with `ARunResumesAfterTheCandidatesDeliveredBefore`.

## Known limitations

L132–L142 in [../limitations.md](../limitations.md). L49, L54, L72, L97 and L114 changed with P15.
