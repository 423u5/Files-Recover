# Candidate evaluation (P14)

Every way of finding a file ends in one model: `EvaluatedCandidate` (`include/evaluation/evaluated_candidate.hpp`),
built by `CandidateEvaluation` (`candidate_evaluation.hpp`, library `recovery_evaluation`). It merges the candidates
of every stage into one per file, validates each at every level
([../formats/media_validation.md](../formats/media_validation.md)), identifies it by the SHA-256 of its content, and
marks duplicates.

```
filesystem candidates (P7) ─┐
carves (P8–P10, P12) ───────┼─► one per file ─► validation levels ─► SHA-256 ─► duplicates ─► sink
MP4 candidates (P12) ───────┤
fragment candidates (P13) ──┘
```

## The evaluated candidate

The data layout is P7's `RecoveryCandidate`, unchanged, so `reconstructCandidate` and `RecoveryWriter` write `data`
as they always did. The fields of the plan's `RecoveryCandidate` are these:

| Plan | `EvaluatedCandidate` |
| --- | --- |
| id | `id` (from `EvaluationOptions::firstId`, in delivery order) |
| format | `formatId` (`FormatDescriptor::id`; empty when no format recognises the content) |
| filename | `data.filename` (`data.extension`) |
| sourceOffset | `sourceOffset()` |
| sourceRegions | `data.sourceRegions` |
| expectedSize | `data.expectedSize` |
| recoveredSize | `recoveredSize()`: what recovery writes (`identity.size`) |
| filesystemEvidence | `data.filesystemEvidence`, when `hasFilesystemEvidence()` |
| signatureEvidence | `signature()`: the carve's |
| structureEvidence | `validation.structural`, `carve` (end detection and the carve's own verdict), `otherCarves`, `mp4`, `fragments`, `allocation` |
| fragmentationCount | `fragmentationCount()` (`data.fragmentation`) |
| validationStatus | `validationStatus()`, with every level in `validation` |
| recoveryMethod | `recoveryMethod()` (`data.method`: FILESYSTEM, CARVING, HYBRID, FRAGMENTED) |
| warnings | `data.warnings` (the layout), `carve->warnings`, `mp4Warnings`, `warnings` (the evaluation) |

There is no confidence score: every field is a fact about the evidence or the result of one check.
`explain(candidate)` says them in words, one line per fact, never file content:

```
Candidate 2: Trip photo.jpg (jpeg), FRAGMENTED: 12059 bytes recovered of 12059 expected, from source offset 100352 (0x18800), 2 fragments (the layout is inferred, not recorded)
Filesystem: FAT32 volume at offset 0, deleted entry /Trip photo.jpg (metadata at 50304); layout Guessed; allocation issues: ClustersInUse
Layout warnings: LayoutGuessed
Carving: jpeg signature 'JPEG SOI' at source offset 100352, 5153 bytes; end StructureWalk Broken (reserved marker 0xB8); the carve's structure Truncated; StructureBroken
Fragment reconstruction (filesystem seed): COMPLETE (the layout (2 fragments) validates: SOF0 256x192, 3 components, 1 scan, EOI); layout from skip-allocated: 2 fragments, 24 clusters (0 allocated now, 0 claimed by other files), 12059 bytes placed, 0 missing; 2 layouts reported
Structural level: passed (jpeg structure): SOF0 256x192, 3 components, 1 scan, EOI
Media level: passed (jpeg decoder): baseline, 8-bit, 256x192, 3 components: 1 scan, 192 MCUs decoded
Playability level: not run: not requested
Validation status: Valid
Content: 12059 bytes, SHA-256 764a9f7d...; preliminary hash: CRC-32 c54a764b of the first and c54a764b of the last 12059 bytes
```

The evaluation's own warnings: `STRUCTURE_INVALID`, `CONTENT_TRUNCATED`, `MEDIA_INVALID`, `PLAYBACK_FAILED`,
`FORMAT_UNKNOWN`, `DATA_UNREADABLE` (stored bytes that read as zeros: they are zeros in the checks and the hash too),
`DUPLICATE_CONTENT`, `ALTERNATIVE_LAYOUT`, `RECONSTRUCTION_FAILED`, `INSIDE_ACTIVE_FILE`.

## One candidate per file

Inputs come in through `addVolume` (a filesystem recovery and its scan), `addCarve`, `addMp4Candidate` and
`addFragmentCandidate`; `run(sink)` merges them by where each file starts on the source (a filesystem candidate
starts at its first stored byte, or where its metadata puts its first cluster):

- A fragment reconstruction replaces its seed's layout: the deleted file's guessed layout, or the broken carve.
  AMBIGUOUS gives one candidate per tied layout, each with `ALTERNATIVE_LAYOUT` and its place among them;
  UNRECOVERABLE keeps the seed's layout, with the reconstruction's evidence and `RECONSTRUCTION_FAILED`.
- An MP4 candidate replaces its filesystem candidate and its carve (unless a reconstruction replaced the filesystem
  candidate already).
- Carves are grouped by their first byte; the best of a group (Valid, Truncated, not validated, Invalid; then the
  longest) speaks for it and the others are kept as `otherCarves`. A start that a replacement holds takes the whole
  group as evidence.
- A carve that starts where a filesystem candidate starts is the same file, as P12 decided for MP4, here for every
  format: the metadata's layout stands when its own data validates; otherwise a carve that validates (or any carve,
  when the metadata locates no data at all) gives the layout (HYBRID: the metadata's name and evidence, the carve's
  layout and length; a deleted file's bytes in clusters allocated now are marked reallocated); otherwise the
  metadata's layout stands. A deleted file whose first cluster is allocated to other data now takes no carve: what
  starts there is the new owner's file. A deleted file whose guessed layout validates on its own is HYBRID too (P12's
  rule).
- Any other carve is a file of its own (CARVING), named `recovered_<id>.<ext>`, with where it lies in the volume's
  allocation (free and allocated clusters, the active files it overlaps). A carve inside one active file's data (a
  thumbnail, the video of a motion photo) is linked to that file (`container`, `INSIDE_ACTIVE_FILE`) and its clusters
  are not marked reallocated: they are the container's. Other carves in clusters allocated now get them marked
  reallocated (`ClustersReallocated`).

## Format and validation

The format is the carve's, MP4 recovery's or the reconstruction's. For a filesystem candidate it is the registered
format whose header check accepts the content: the one of the file's extension when several do, and when none does
(the header checks are carving's heuristics, and a damaged header fails them: the structure then says what is
wrong). Content no format recognises is `NotValidated` with `FORMAT_UNKNOWN`. `validateContent` runs the levels on
exactly the bytes recovery writes (`CandidateContentReader`); the format's verdict is reused when the evidence holds
it on exactly those bytes (a fragment hypothesis, a carve that is the whole layout).

## Identity and duplicates

`computeIdentity` (`content_identity.hpp`) hashes the same bytes: SHA-256, the final identity, and a preliminary hash
(the size and the CRC-32 of the first and the last 64 KiB) that is quick to compare: different preliminary hashes mean
different content, equal ones only that it may be the same. A later candidate with the SHA-256 of an earlier one is
its duplicate (`duplicateOf`, `DUPLICATE_CONTENT`): identified, not removed. Names play no part: a copy under another
name is a duplicate, another file under the same name is not. Empty content is never a duplicate, and without
SHA-256 (`IdentityOptions::sha256` off) there is no duplicate detection.

## Delivery

The candidates of the added volumes first (volume by volume, in scan order, each in the place of the filesystem
candidate it comes from), then the others in source order; ids follow from `firstId`. The report counts the
candidates by method and status, the duplicates, the carves merged or kept as evidence, the inputs another stage
replaced, the alternatives, and the bytes hashed.

## A pool, and runs that resume (P15)

`run()` first merges the inputs into the candidates to deliver, in delivery order (cheap, and the same every time).
With `EvaluationOptions::pool`, each candidate is then validated and hashed on the pool's workers, at most `window`
at a time. Candidates are still delivered one by one in order on the thread of `run()`: the ids, the duplicates and
the counts are given at delivery. A carved file's container id is known from the merge. The allocation queries of
the candidates in flight hold a lock per volume. What `run()` delivers does not depend on the pool.

`EvaluationOptions::resume` holds the records (`EvaluationRecord`, `recordOf`) of the candidates an earlier run with
the same inputs delivered. They are not evaluated or delivered again. The run counts them, and records their
content for duplicates, as if it had delivered them, then goes on with the next id. Records that are not numbered
from `firstId`, or whose duplicate flags do not fit their content, are refused.

## Cost

Each candidate's data is read at least twice (validation, hashing), video media data in full; the inputs are kept
(and copied once) until the end of the run (L121, L129).

## Tests

`tests/evaluation` (executable `recovery_evaluation_tests`, label `evaluation`): FAT32 cards whose free clusters hold
old data, with valid, truncated, invalid and unknown files, duplicates under other names and other content under one
name, carves of the whole card merged into filesystem candidates (FILESYSTEM and HYBRID), carves of their own and
inside an active file, a deleted file whose first cluster another file has taken, an MP4 recovery candidate, a fragment
reconstruction, synthetic AMBIGUOUS and UNRECOVERABLE reconstructions, unreadable sectors, options and cancellation.
Every candidate is checked: its SHA-256 equals the hash of what `reconstructCandidate` delivers, its warnings follow
from its levels and evidence, a duplicate's original is earlier with the same digest, and its explanation names its
status and digest.

## Known limitations

L126–L131 in [../limitations.md](../limitations.md); L47, L48, L51, L103 and L107 changed with P14.
