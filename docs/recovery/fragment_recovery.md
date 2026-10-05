# Fragment reconstruction (P13)

Files whose start is known but whose layout no metadata records, put back together from the evidence around them.
Filesystem recovery (P7, [filesystem_recovery.md](filesystem_recovery.md)) can only guess the layout of a deleted
FAT32 file, or of a deleted exFAT file whose chain was cleared: it assumes the clusters after the first one. Carving
(P8, [carving.md](carving.md)) stops where a file's structure breaks. Fragment reconstruction tries layouts, never
a blind concatenation of clusters: each one is a hypothesis, validated on its own by the file's format, and ranked
by what the rest of the evidence says. The result says how well the file came back: COMPLETE, PARTIAL, CORRUPTED,
AMBIGUOUS or UNRECOVERABLE.

Library `recovery_fragments` (namespace `recovery`, header `include/recovery/fragment_recovery.hpp`), on top of
`recovery_mp4` (the MP4 analysis and `CandidateContentReader`), `recovery_candidates` and `recovery_formats`:

| Header | Contents |
| --- | --- |
| `fragment_recovery.hpp` | `FragmentRecovery`, `FragmentRecoveryOptions`, `FragmentSearchLimits`, `FragmentRecoveryReport`, `FragmentCandidate`, `ReconstructionHypothesis`, `HypothesisEvidence`, `SearchStats`, `ReconstructionStatus`, `SeedOrigin`, `LayoutSource`, `ClusterRun` |

Private sources in `src/recovery/`: `fragment_evidence.*` (cluster geometry, claims, allocation cache, where the
next fragment may start, layouts as source regions), `fragment_search.*` (the search for one file: base layouts,
the gap search, the assessment, the ranking), `fragment_mp4.cpp` (MP4 and M4A placed by their sample tables) and
`fragment_recovery.cpp` (seeds, the carving pass, statuses, delivery).

P7's model gained one method, `RecoveryMethod::Fragmented` (the plan's FRAGMENTED). `reconstructCandidate` and
`RecoveryWriter` take a reconstruction's data as it is.

```cpp
carving::FormatRegistry formats;                       // every format that can validate a file
formats::registerImageFormats(formats);
formats::registerAudioFormats(formats);
formats::registerVideoFormats(formats);

FragmentRecovery recovery(disk, formats);              // the source: a disk, an image or a partition, open
if (Status added = recovery.addVolume(*volume, scan); !added.ok()) { ... }   // each volume: P7's recovery and scan
Result<FragmentRecoveryReport> report = recovery.run([&](FragmentCandidate&& candidate) -> Status {
    if (const ReconstructionHypothesis* chosen = candidate.reconstruction()) {  // not AMBIGUOUS or UNRECOVERABLE
        Result<RecoveredFile> file = writer.recover(chosen->data);
        ...
    }
    return success();
});
```

## Flow

```
volumes (P7 scans) ──► seeds: deleted files with a guessed layout ───────────────┐
the source ──► carving pass ──► seeds: carves that break in free space ──────────┼─► per seed: layout hypotheses
                            └─► evidence: carves that validate, file starts,     │   ──► independent validation
                                moov boxes in free clusters                     ─┘   ──► ranking ──► status ──► sink
```

1. **Evidence.** Each added volume's candidates claim clusters: active files their data, deleted files their
   recorded layout (exFAT chains and runs, NTFS runs), deleted files with a guessed layout their first cluster
   (strongly) and the rest of their guess (softly).
2. **Filesystem seeds** (`useFilesystem`): every candidate whose layout P7 could only guess (`LayoutEvidence::Guessed`).
   Its format is the one whose signature and header check accept the first bytes (the one its extension names
   first, when several do); if none does, the extension's format, and the file cannot be recovered. A file that no
   format knows is skipped (`filesystemSkipped`).
3. **Carving pass.** The source is scanned once with every registered format and a probe for moov boxes. Only hits
   at the start of a free cluster of an added volume are carved (a file starts at a cluster; clusters in use hold
   other data). A carve that validates claims its clusters; any carve claims its first cluster; a carve that breaks
   claims its consistent bytes softly and becomes a **carve seed** (`carve`), unless it starts where a filesystem
   seed starts: then it is that seed's `carve`, as evidence. moov boxes in free clusters are kept as anchors for MP4.
4. **Reconstruction**, filesystem seeds first (volume by volume, in scan order), then carve seeds (in source
   order). A carve seed whose first cluster an earlier reconstruction has taken is part of that file and is
   dropped. Every reconstruction that places data claims its clusters for the rest of the run.

## Hypotheses

A layout maps the file's clusters, in file order, to clusters of the volume. Every hypothesis is validated on
exactly the bytes it would recover (`CandidateContentReader` over its regions), by the format's own end detection
and validation.

| `LayoutSource` | Layout |
| --- | --- |
| `Contiguous` | The clusters after the first, in order: P7's guess |
| `SkipAllocated` | The clusters after the first that are not allocated to other data now: a file written around files that still exist |
| `SkipClaimed` | The clusters after the first that are neither allocated now nor claimed strongly by other files |
| `GapSearch` | A continuation found where the structure of a layout broke |
| `SampleTables` | MP4 and M4A: clusters placed sample by sample |

**Assessment.** The format's end detection runs first: it says where the structure ends or breaks. When it ends
(`Found`), the format's validation checks the file it delimits. With a recorded size, a structure that ends before
it has skipped part of the file (`endedEarly`: a dead end, ranked below the others), and one that runs past it
(a length taken from foreign data) is located by the format's validation. A carve seed's length is where its
structure ends, looked for within `maxUnknownLength`.

**Gap search** (the generic search, every format). From each layout that broke after some consistent bytes (the
best `beamWidth`), at the cluster boundaries nearest the break (`maxBoundaries`; the structure may notice foreign
data a little after it starts, or before it for a checksummed chunk), the clusters that could start the next
fragment are tried (`maxContinuations`), each continued the way its parent was. They are looked for from the end of
the fragment on, in the order its writer would have met free clusters, wrapping around at the end of the volume
(`maxSearchClusters`): first the next cluster no other file claims at all, then the clusters that start a free run
(after clusters in use or claimed), then every candidate. A cluster is a candidate when it is free now (or its state
is unknown), no other file claims it strongly, and the layout does not hold it. A continuation counts as found once
its structure holds for `minimumContinuation` bytes (4 KiB) past its start, or the file validates: foreign data can
pass for a while (JPEG entropy-coded data, a segment whose length foreign bytes give). The search stops once a layout
validates, after trying the other continuations at that layout's boundaries.

**MP4 and M4A** (`fragment_mp4.cpp`). The movie comes from the first fragment (moov before the media data, parsed
without issues and inside the consistent part), or from an anchor: the first mdat's size says where the moov starts
when it follows the media data, so a moov box found in free space at that offset within its cluster, of a size that
fits the file, whose media data lies in that mdat, is the file's (at most `maxMovieAnchors`). The video samples
(AVC and HEVC, NAL unit length fields) are then walked in file order, `probeSamples` at a time, through the layout:
each sample's NAL units must fill it (mp4::checkSampleFraming on a window of samples). Where a sample does not, every
cluster boundary between the start of the last sample that framed and the end of this one is tried, with the cluster
that joins the moov's fragment first, then the gap search's candidates; a continuation is taken when the samples after
the boundary frame through it. Continuations the samples cannot tell apart (a boundary in audio data or inside a NAL
unit) are all kept, up to the beam, and end as separate layouts. When none frames:

- later samples may frame through the layout as it is: the part between is damaged where it lies (overwritten);
- or they frame in the moov's fragment further on: the part between is missing, and the moov's fragment starts at
  the cluster of the first sample that frames there;
- otherwise the rest is not placed (the moov stays where it was found).

A walk that ends before the moov's fragment switches to it at one of the cluster boundaries after its last checked
sample (the samples cannot tell which: every one is a layout). Each finished walk is validated in full like any other
layout. The layouts validated before are confirmed only as far as their samples frame (the top-level walk passes over
the media data unchecked), and once a movie is known the generic search, which validates whole layouts, is left out.

## Ranking and status

The best supported layout ranks first:

1. Valid before anything else.
2. For layouts that do not validate: a structure that does not end before the recorded size, then more **confirmed
   bytes**, then Truncated before Invalid. Confirmed bytes are what a layout delivers when it does not validate: a
   base layout, the clusters before the one where its structure broke; a gap-search continuation, the file up to the
   boundary it starts at (its own data is not confirmed); an MP4 walk, the file's length less its missing and damaged
   bytes.
3. Fewer clusters allocated to other data now.
4. Fewer clusters claimed by other files.
5. Fewer fragments.

Layouts equal in all of that whose delivered bytes differ are **equally supported**; equal bytes (a duplicate) count
once. Then, first match wins:

| Status | When | `hypotheses` |
| --- | --- | --- |
| `AMBIGUOUS` | Two or more equally supported layouts with different bytes; the reason gives how many | The first `tied` are the tied ones (at most `maxAlternatives`), then the others; none is chosen |
| `UNRECOVERABLE` | The first cluster is allocated to other data now, or holds no file of the format its name says, or the recorded size is larger than the volume; or nothing of the file validates | What was tried, whole; `reconstruction()` is null |
| `PARTIAL` | The best layout does not validate, or leaves bytes it could not place | The reconstruction: what is confirmed, Missing after (or between, for MP4) |
| `CORRUPTED` | The whole file is placed, but its structure does not validate (MP4 samples damaged in place), some of its clusters are allocated to other data now, or some bytes could not be read | The reconstruction, whole |
| `COMPLETE` | The best layout validates, every byte placed and read, none in clusters in use | The reconstruction, whole |

After the chosen one, the other layouts follow by rank (the alternatives that lost), each delivering a part of the
file no earlier one delivers, up to `maxAlternatives`.

## Candidates

A `FragmentCandidate` is one seed:

| Field | Meaning |
| --- | --- |
| `id`, `status`, `reason` | From `firstId` in delivery order; the status and why (never file content) |
| `origin`, `formatId`, `name`, `path` | Filesystem or carving; the format; the metadata's name and path, or `recovered_<id>.<ext>` |
| `filesystemCandidate`, `recordedSize` | Filesystem seeds: the P7 candidate and the size its metadata records |
| `carve` | The carve starting where the file starts: a carve seed's own, or a filesystem seed's first cluster carved |
| `filesystem`, `volumeOffset`, `clusterSize` | The volume |
| `hypotheses`, `tied` | The reconstructions (see the table above) |
| `search` | Layouts validated, sample windows probed, bytes read, and the limit that stopped the search, if one did |

A `ReconstructionHypothesis`:

| Field | Meaning |
| --- | --- |
| `data` | The reconstruction as a `RecoveryCandidate`: `Fragmented` when it has several fragments, otherwise `Hybrid` (a filesystem seed, as in P12) or `Carving`; the metadata's name, path, times and evidence kept; placed clusters Stored (split where they are allocated now: `reallocated`), the rest Missing; `fragmentation.known` is false |
| `clusters` | The placed clusters, as the volume numbers them |
| `source` | How the layout was put together |
| `validation` | The format's validator on exactly the bytes `data` delivers |
| `evidence` | Fragments; clusters placed, allocated to other data now, claimed by other files; bytes placed, missing and unreadable; `dataChecked` |
| `mp4` | MP4 and M4A: P12's `Mp4Structure` for the layout, each sample counted under the worst damage of its bytes |

`dataChecked` (valid layouts): the validator rejects the layout when the first cluster of any fragment after the first
(in one piece, the middle cluster) holds other bytes, every offset unchanged. The other bytes are a fixed pseudo-random
pattern with `FF 01` every 64 bytes, as compressed data of a few kilobytes holds. False means the structure does not
see the data where the pieces join (BMP pixels, WAV samples, audio in MP4, a cluster inside a NAL unit): the layout
rests on the allocation evidence alone.

## Report

`FragmentRecoveryReport`: filesystem seeds and the guessed files skipped; the scan, the hits carved, carve seeds and
carves that validated; candidates by status; searches a limit stopped; layouts validated; the time taken.

`run` fails with `InvalidInput` for invalid options (cache size, reads, MP4 parse limits, a search limit of 0), a
source that is not open, no sink or an empty registry; with `Cancelled`; with a read error that is not an I/O error;
and with the sink's error, which stops the run. `addVolume` refuses a scan of another volume and a volume without a
cluster area.

## Safety and bounds

- The source is only read.
- Everything read is untrusted: layouts never leave a volume's cluster area, every offset is checked, and the formats
  bound their own parsing.
- Work per seed: `maxValidations` layouts validated (2048), `maxReadBytes` read (4 GiB), `maxSampleProbes` windows
  probed, `maxFragments` fragments, `maxBoundaries` × `maxContinuations` tries per break, `beamWidth` layouts carried,
  `maxSearchClusters` clusters looked at per break. A search a limit stopped says so (`SearchStats`); when it ended
  without a valid layout, a continuation list that was cut says so too.
- Memory: claims (interval maps), carve seeds and moov anchors for the run; cluster states cached per volume (at
  most 16 MiB); each seed's hypotheses until it is delivered.

## Logging

Component `fragments`, through `carving.scan.logger`: the start and the end of a run (seeds, statuses, searches
limited, layouts validated, time) at `Info`, each candidate (id, origin, format, status, hypotheses, fragments,
layouts validated, whether the search was complete) at `Debug`. No file content is logged.

## Steps (P15)

`FragmentRecoverySteps` holds what `run()` does, as steps a scan drives itself ([scanning.md](scanning.md)); `run()`
drives the same steps one after the other:
1. `begin()`: the claims of the volumes' files;
2. `examineSeed()` per filesystem candidate, concurrently, then `addSeedExamination()` in scan order;
3. `commit(hit, outcome)` for the pass's hits in source order. A scan shares one pass among its stages and passes
   the carve with validation that carving made of the hit, since the two carve alike. `extraFormats()` adds the
   moov probe to that pass, and `wants(hit)` tells whether a commit would carve the hit;
4. `reconstructNext()`, seed after seed.

For checkpoints, the commits that changed the steps' state are kept as events (`recordEvents`, `takeEvents`). A
restored object replays them (`replay`), and replays each reconstruction already delivered
(`replayReconstruction`: its claims, from the chosen hypothesis's clusters, without the search). It then goes on
exactly where the first stopped.

## Thread safety

None: one owner at a time. The source, the registry, the volumes and their scans must outlive the
`FragmentRecovery`. The sink is called on the thread of `run()`. `FragmentRecoverySteps::examineSeed()` may run
concurrently with itself: the cache of cluster states it reads has a lock.

## Tests

`tests/recovery/fragment_recovery_test.cpp` (label `recovery`). The volumes are FAT32 "cards" whose free clusters hold
old data (noise), as on media that has been used, and one exFAT volume. Every candidate is checked: its hypotheses
validate as they say when the test validates their data again, their evidence adds up, their names and methods
follow the rules, and the chosen reconstruction is compared with the original file.

| Plan's case | Tests |
| --- | --- |
| Known fragmented files | JPEG around a file that still exists (skip-allocated) and in three pieces around deleted files whose entries survive; PNG, MP3 and a carved PNG across old data (gap search); WAV and BMP around files that still exist; MP4 in two and three pieces with the moov after the media data (anchor, samples) and before it; carved JPEG and MP4; an exFAT file whose chain was cleared; contiguous files confirmed in one piece (HYBRID) |
| Missing fragments | A JPEG whose second piece is gone (PARTIAL: the first piece, Missing after); an MP4 whose middle piece is gone (PARTIAL: first and last pieces placed, the middle Missing, the samples counted) |
| Wrong fragment ordering | A JPEG whose second piece lies before the first (found by wrapping around), and the same pieces in disk order rejected by the validator |
| Overwritten fragments | A JPEG with restart markers three of whose clusters a new file took (PARTIAL: the part before); an MP4 likewise (CORRUPTED: placed where it lay, the samples there damaged); a first cluster another file took, and one holding other data (UNRECOVERABLE); an unreadable sector (CORRUPTED) |
| Ambiguous layouts | Two tails that validate equally (AMBIGUOUS, both reported), and two identical ones (COMPLETE); M4A whose moov's fragment could start anywhere in audio data (AMBIGUOUS) unless a file that still exists says where (COMPLETE, not data-checked); BMP decided by the allocation, with the layout through the other file's clusters listed |

And: search limits reported; files of no format skipped; reconstructions written by `RecoveryWriter` and validated
again; a partitioned disk; 40 rounds of mutated volumes (bounds, consistency); cancellation before the run, during a
search and during the scan of a 256 MiB source; sink errors; invalid use; logging.

## Known limitations

All known limitations, with IDs for discussion, are in [../limitations.md](../limitations.md): L104-L115 for P13, and
L6, L11, L46, L62, L65, L69, L71 and L96 updated.

- Structural validation cannot see everything: JPEG entropy-coded data takes foreign data (zeros especially), BMP
  pixels, WAV samples, VP8 and audio in MP4 have no structure. The search delivers only what the structure confirms,
  and `dataChecked` says where it could not look; such files are placed by the allocation evidence.
- The search is bounded: a gap of more than `maxContinuations` candidate clusters without evidence around it is not
  crossed, and the search says so.
- Every generic layout is validated from the file's start: the cost grows with the file's size times the layouts
  tried. MP4 uses cheap sample probes instead.
