# MP4 recovery (P12)

MP4 videos (MP4, MOV, M4V, 3GP), deleted or not, recovered from filesystem evidence and carving evidence
together. Filesystem recovery (P7, [filesystem_recovery.md](filesystem_recovery.md)) knows names, sizes and
layouts; carving (P8, [carving.md](carving.md)) with the MP4 format ([../formats/mp4.md](../formats/mp4.md)) finds
files by their structure. MP4 recovery puts the two together, so every file becomes one candidate that carries the
evidence of both, and validates every candidate's data with the MP4 structure: the boxes, the sample tables, the
movie fragments, and the NAL unit framing of its AVC and HEVC samples.

Library `recovery_mp4` (namespace `recovery`, headers in `include/recovery/`), on top of `recovery_candidates` and
`recovery_formats`:

| Header | Contents |
| --- | --- |
| `mp4_recovery.hpp` | `Mp4Recovery`, `Mp4RecoveryOptions`, `Mp4RecoveryReport`, `Mp4Candidate`, `Mp4Structure`, `Mp4TrackEvidence`, `Mp4Allocation`, `Mp4Warning`, `analyzeMp4`, `mp4Extension` |
| `candidate_content.hpp` | `CandidateContentReader`: a `RecoveryCandidate`'s data as a file's content (`carving::IContentReader`), read from the source without being written anywhere |

P7's model gained two methods, `RecoveryMethod::Carving` and `RecoveryMethod::Hybrid`; nothing else of P7 changed.
`reconstructCandidate` and `RecoveryWriter` take an MP4 candidate's data as it is.

```cpp
Mp4Recovery recovery(disk);                              // the source: a disk, an image or a partition, open
if (Status added = recovery.addVolume(*volume, scan); !added.ok()) { ... }   // each volume: P7's recovery and scan
Result<Mp4RecoveryReport> report = recovery.run([&](Mp4Candidate&& candidate) -> Status {
    Result<RecoveredFile> file = writer.recover(candidate.data);  // or keep it: its evidence is in `candidate`
    if (!file.ok()) {
        return file.error();
    }
    return success();
});
```

## Flow

```
filesystem candidates (P7) ──► MP4 by name or content? ──► structure of their data ──┐
                                                                                     ├─► Mp4Candidate: FILESYSTEM,
the source ──► carving with the MP4 format ──► carves ──► same start? ───────────────┘   HYBRID or CARVING
```

1. **Filesystem candidates.** A candidate of an added volume is examined when its extension is a video one (mp4,
   m4v, mov, qt, 3gp, 3g2, 3gpp, 3gp2), or when the first 4 KiB of its data pass the MP4 format's header check.
   Its data is read through a `CandidateContentReader` (so a file of several gigabytes needs no copy) and analysed
   (below). A file examined for its content alone is kept only when it is a video file: audio files (M4A, or a
   generic brand with sound only) and images are left to their own formats. A file named as a video is kept
   whatever its data holds, so the user sees what became of it. An MP4 name whose metadata locates no data (size
   0, or nothing stored) waits for a carve that starts at its first cluster.
2. **Carving.** The source is scanned with the MP4 format alone (`Mp4RecoveryOptions::carving`: range, block size,
   alignment, reads, logger). Each hit is carved (`FileCarver::carve`, end detection only) and the carve analysed
   like a filesystem candidate's data. Hits strictly inside a carve that ended `Found` and validated are skipped. A
   carve that starts where a filesystem candidate stored in one run, of the carve's length and with an intact
   structure, starts is not analysed again: it holds the same bytes.
3. **Same start.** A carve that starts at a filesystem candidate's first byte (its first stored byte, or the first
   cluster its metadata records) is the same file. When the candidate's own structure is valid (no misframed
   sample), or the carve's is not, the carve is attached to the candidate as evidence. Otherwise, or when the
   metadata located no data, the candidate becomes HYBRID: its name, path, times and metadata evidence, with the
   carve's layout and length. Not so for a deleted entry whose first cluster is allocated to another file now:
   the MP4 that starts there is the new owner's, so the carve is only attached (and an entry that located no data
   is not delivered).
4. **Carving candidates.** Every other carve is a CARVING candidate, named `recovered_<id>.<ext>`, with where it
   lies in the allocation of the volume that holds it.
5. **Delivery.** Filesystem and hybrid candidates first (volume by volume, in scan order), then carving candidates
   (in source order), with ids from `Mp4RecoveryOptions::firstId` in that order.

## Candidates

An `Mp4Candidate` is one MP4 file:

| Field | Meaning |
| --- | --- |
| `data` | The file's data as a `RecoveryCandidate`: `method` is `Filesystem`, `Carving` or `Hybrid`, `id` the candidate's id; filesystem and hybrid candidates keep P7's name, path, times and evidence |
| `filesystemCandidate` | Filesystem and hybrid candidates: the id of the P7 candidate in its `CandidateScan` |
| `recordedSize` | Filesystem and hybrid candidates: the size the metadata records (a hybrid candidate's `data.expectedSize` is the carve's length) |
| `carving` | The carve of the same file (`carving::FileCandidate`): carving and hybrid candidates, and filesystem candidates that carving found too |
| `structure` | What the file's data holds (below) |
| `allocation` | Carving candidates on an added volume: where they lie in its allocation |
| `warnings` | What the evidence means (below) |

| Method | When | Layout |
| --- | --- | --- |
| `FILESYSTEM` | A filesystem candidate named or recognised as MP4 video | The metadata's (a recorded chain, run list or contiguous run; or a guess that the structure does not confirm) |
| `HYBRID` | The metadata gives the name and the start; the structure gives or confirms the layout and length: a guessed layout (deleted FAT32 or exFAT file) whose structure is `Valid` with no misframed sample, or a carve from the candidate's first byte that validates where the metadata's layout does not, or where the metadata locates no data. Never for a deleted entry whose first cluster another file has taken since | The guess; or the carve's run, from its start to its end |
| `CARVING` | A carve no metadata names | The carve's run |

A layout taken from a carve is one stored run: contiguity is assumed, so `fragmentation.known` is false, and the
metadata's `LayoutGuessed` warning stays when it was there. Its clusters that are allocated now are split into
regions marked `reallocated` (for a deleted entry, and for a carve that does not lie inside one active file), with
the warning `ClustersReallocated`. A file fragmented on disk is recovered whole only through a filesystem
candidate that records its layout; carving and guessing find its first piece (fragment reconstruction, P13,
[fragment_recovery.md](fragment_recovery.md), reconstructs the rest).

### Structure evidence

`Mp4Structure`, from the analysis of the file's data (`analyzeMp4` without a layout):

| Field | Meaning |
| --- | --- |
| `status`, `detail`, `issues`, `issueCount` | The parser's verdict (`Valid`, `Truncated`, `Invalid`), why, and its first 32 issues with their count; a moov found only by searching makes it `Invalid` |
| `kind`, `kindReason` | Audio, video or neither (`mp4::classify`) |
| `majorBrand` | ftyp's major brand, if any |
| `moovOffset`, `moovSize`, `moovFoundBySearch`, `moovBeforeMediaData` | The movie, and whether it was found only by searching the data (the boxes before it are damaged) |
| `mediaDataBoxes`, `movieFragments` | mdat boxes and moof boxes found |
| `media` | Where the sample tables and fragments put the media data (mdat discovery) |
| `structureEnd`, `dataSize` | Where the structure ends (its last box, or its media data if further), and the size of the data analysed |
| `tracks` | Per track: kind, codec, width and height or channels and rate, duration and time scale, samples and their bytes, and the sample analysis below |

`intact()` is true when the structure is `Valid`, has a moov, and every sample is intact and framed.

### Sample-table analysis

Every sample of every track (sample tables and movie fragments) is located and checked against the file's layout.
A sample counts once, under the first of:

| Count | The sample has bytes |
| --- | --- |
| `samplesBeyondData` | beyond the end of the file's data |
| `samplesMissing` | in `Missing` or `Zeros` regions: the metadata does not locate them, or knows they were never written |
| `samplesUnreadable` | in sectors that could not be read, or beyond the end of the source (a truncated image), as far as the analysis read them (L98) |
| `samplesReallocated` | in clusters allocated to other data since the deletion: they may have been overwritten |
| `samplesIntact` | none of the above |

and, for AVC and HEVC tracks, `samplesFramed` and `samplesMisframed`: the samples whose NAL units were walked, and
those they do not fill (another file's data in their place, or bytes that are not where the tables say, L95).

### Allocation evidence

A carving candidate on an added volume carries `Mp4Allocation`: the volume (filesystem and offset), how many of its
clusters the file covers and their state now (free, allocated, other: bad, invalid or unreadable in the allocation
table or bitmap), whether all were checked (at most `maxClusterChecks` per file), the paths of active files whose
data overlaps it (the first eight), and whether it lies entirely inside one active file (a motion photo's video,
L103).

### Warnings

| `Mp4Warning` | When |
| --- | --- |
| `StructureInvalid` | The structure is `Invalid` |
| `StructureTruncated` | The data ends before the structure does |
| `NoMovie` | No moov was found, not even by searching: the samples cannot be located |
| `MovieFoundBySearch` | moov was found only by searching: the boxes before it are damaged |
| `SamplesDamaged` | Some samples are beyond the data, missing, unreadable or reallocated |
| `SamplesMisframed` | Some AVC or HEVC samples' NAL units do not fill them |
| `SizeMismatch` | Filesystem and hybrid candidates: the structure ends before or after the recorded size (not for an `Invalid` structure, unless its only issue is data after its last box) |
| `AllocatedClusters` | Carving: some of the file's clusters are allocated to other data now |
| `InsideActiveFile` | Carving: the file lies inside one active file's data (instead of `AllocatedClusters`) |

P7's `CandidateWarning`s stay on `data` and keep their meaning for every method.

## Reading a candidate's data

`CandidateContentReader::open(source, candidate)` gives offsets `[0, size())` of the file, where `size()` is the end
of the last region that is not `Missing`, as for `reconstructCandidate`. Stored regions are read through a
`SourceContentReader` over the whole source (its cache, sector-by-sector retries, known bad regions); a read inside
one stored region is served by it directly. `Zeros` and `Missing` regions inside the file read as zeros, `Embedded`
ones come from the candidate. Stored bytes that cannot be read, or that lie beyond the end of the source, read as
zeros and are recorded (`unreadable()`, `outsideSource()`). It fails for a malformed candidate (`validateCandidate`),
a closed source and invalid options.

## Report

`Mp4RecoveryReport`: filesystem candidates examined and those that are MP4 video; the scan report, carves made and
rejected, hits skipped, carves attached to a filesystem or hybrid candidate; candidates delivered by method and by
structure (`valid`, `truncated`, `invalid`); the time taken.

`run` fails with `InvalidInput` for invalid options (cache size, reads, parse limits, `maxClusterChecks` of 0), a
closed source or no sink; with `Cancelled` (`carving.scan.reads.cancellation`, checked per candidate, per cluster
and by the scan); with a read error that is not an I/O error; and with the sink's error, which stops the run.
`addVolume` refuses a scan of another volume.

## Safety and bounds

- The source is only read: through `CandidateContentReader` and `SourceContentReader`, never written.
- Everything read is untrusted: the parser's bounds and limits (`Mp4FormatOptions::limits`), overflow-checked
  offsets, moov discovery limited to 64 attempts, and at most `maxClusterChecks` clusters checked per carved file.
- Memory: every file is read through bounded caches; the MP4 candidates themselves are kept until the end of the
  run (L99).

## Logging

Component `mp4_recovery`, through `carving.scan.logger`: the start and the end of a run (volumes, candidates by
method, carves, merges, verdicts, time) at `Info`, and each candidate (id, method, offset, size, structure,
samples, intact samples) at `Debug`. No file content is logged.

## Steps (P15)

`Mp4RecoverySteps` holds what `run()` does, as steps a scan drives itself ([scanning.md](scanning.md)); `run()`
drives the same steps one after the other:
1. `examine(volume, index)`, concurrently, then `addExamination()` in scan order;
2. for each hit of `format()` in source order, `commit(hit, work)`, with the work `prepare(hit)` made ahead
   (the carve and, when the commit will need it, its analysis) or with none. `skips(offset)` tells whether the
   commit would skip a hit;
3. `deliver()`.

Carve ids are given at the commit, as one carver numbers its carves. `state()`, `takeChanges()` and `restore()` save
the steps between hits and restore them. A merge changes only the candidates that start where its carve does, which
is why a hit's preparation can run while other hits are committed.

## Thread safety

None: one owner at a time. The source, the volumes and their scans must outlive the `Mp4Recovery`; a
`CandidateContentReader` keeps its own copy of the candidate's regions. `Mp4RecoverySteps::examine()` and `prepare()`
are const and may run concurrently with each other and with `commit()` of other hits.

## Tests

| File | Covers |
| --- | --- |
| `tests/recovery/mp4_recovery_test.cpp` | The analysis of every layout and of damaged files (moov found by searching, moov lost); names; FAT32 files of every layout (moov first and last, 64-bit, movie fragments, QuickTime, 1200 samples, fragmented on disk) as FILESYSTEM candidates with their carve attached; deleted exFAT and NTFS files whose chain or runs survived; HYBRID from a guess the structure confirms and from carves at an entry's first cluster (size 0, a short size, clusters taken since), and not when another file took the first cluster; partly overwritten files (samples reallocated, moov lost, start lost); damaged metadata; bad sectors; CARVING candidates in free space, partly overwritten, inside an active file, and QuickTime; files recognised by name or content; each kind of evidence alone; recovered files written and validated again; a partitioned disk; a file larger than 4 GiB recorded in three runs, and one carved beyond 4 GiB; cancellation, sink errors, invalid use, logging |
| `tests/unit/recovery/candidate_content_test.cpp` | `CandidateContentReader`: every region kind as reconstruction delivers it, reads inside and across regions, reads outside the file, bad sectors, a truncated source, offsets beyond 4 GiB, fatal source errors, malformed candidates and options |
| `tests/formats/video_carving_test.cpp` | The MP4 format carving whole sources: see [../formats/mp4.md](../formats/mp4.md) |

The plan's test list:

| Case | Where |
| --- | --- |
| Small MP4 | Every test: the builder's default file has 12 video and 12 audio samples |
| Large MP4 | A file of 1200 samples; files larger than 4 GiB, beyond 4 GiB, from a filesystem layout in three runs and from carving |
| Fragmented MP4 | Movie fragments (every kind of base offset), and files fragmented on disk whose FAT32 or exFAT chain or NTFS runs survived |
| moov before and after mdat | Every volume test holds both; `moovBeforeMediaData` is checked |
| Partial overwrite | Samples in reallocated clusters counted exactly; moov overwritten (`NoMovie`); the start overwritten (`MovieFoundBySearch`); a carve partly under a new file |
| Corrupt metadata | A lost stsz, an mvhd version, a chunk beyond the file; `Mp4FormatTest.CorruptMetadataIsCaught` for more |

## Known limitations

All known limitations, with IDs for discussion, are in [../limitations.md](../limitations.md): L92-L103 for P12,
and L47, L48 and L55 (carving) and L76, L77, L84, L86 and L91 (formats) updated or resolved. Files fragmented on
disk whose layout no metadata records are reconstructed by P13 ([fragment_recovery.md](fragment_recovery.md)).
