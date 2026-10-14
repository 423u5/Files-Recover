# Performance (P20)

## Targets

The engine must not be what limits a recovery from removable media: a USB 3 flash drive or card reader delivers
about 100 MB/s, a USB 2 one about 40 MB/s. Set with the user in P20 (device-based targets):

| Benchmark | What it measures | Target |
| --- | --- | --- |
| `deep-noise` | A deep scan, 4 workers, of a 2 GiB image of random data: the content of a used card's free space and of compressed media, where every signature is checked and almost nothing validates | ≥ 100 MB/s |
| `deep-media` | A deep scan, 4 workers, of a 1 GiB FAT32 card full of photos, videos and music (1,374 files, 137 deleted): every file is decoded by the engine's decoders and hashed | ≥ 40 MB/s |
| `quick-files` | A quick scan of a FAT32 card of 20,000 small files: the filesystem pass, each file validated and hashed | ≥ 2,000 files/s ("much faster than a deep scan") |
| `recover` | Writing every candidate of the media card to a folder on an SSD (938 MB) | ≥ 100 MB/s |
| `imaging` | Imaging the 2 GiB image to a file on an SSD | ≥ 100 MB/s |
| `read` | The engine's own sequential reads of the image file | Reference only |
| Memory | Peak working set of each benchmark | Bounded, not growing with the source's size |

`recovery_benchmark` (`tools/benchmark`, see [testing.md](testing.md)) makes the inputs, runs the engine on them as
the CLI and the API do, and exits 3 when a target is missed. The images are files on the local SSD, read through the
file cache: the numbers are the engine's own speed (decoding, hashing, carving), not a device's.

## Results

Release build as shipped (static runtime, Control Flow Guard, CET), Intel Core i5-4440 (4 cores), 16 GB RAM, SATA
SSD, Windows 10 Pro 22H2, 2026-10-12. Each benchmark run alone (`--only`) after a warm-up; the scans' medians are
from the three rounds of the comparison below, whose runs spread by up to ±8%.

| Benchmark | Result | Target | |
| --- | --- | --- | --- |
| `deep-noise` | 172 MB/s (2 GiB in 12.6 s; 157–183 over the rounds) | ≥ 100 MB/s | Met |
| `deep-media` | 78 MB/s (1,374 files in 14 s, 99 files/s; 73–81) | ≥ 40 MB/s | Met |
| `quick-files` | 28,365 files/s (20,000 files in 0.7 s; 26,715–28,385) | ≥ 2,000 files/s | Met |
| `recover` | 194 and 280 MB/s (938 MB, 1,374 files; two runs) | ≥ 100 MB/s | Met (see the notes) |
| `imaging` | 181 and 238 MB/s (2 GiB; two runs) | ≥ 100 MB/s | Met (see the notes) |
| `read` | 3,550–3,760 MB/s (from the file cache) | Reference | |

The deep scans run at about 1.7 times a USB 3 device's speed on random data and twice a USB 2 device's on a card of
media, so the device, not the engine, sets the pace.

## Memory

Peak working set of the whole process, each benchmark run alone:

| Benchmark | Peak |
| --- | --- |
| `deep-noise` (2 GiB) | 74 MiB |
| Deep scan of a 64 GiB sparse image through the API (`LargeDriveTest`, opt-in; 45.6 s) | 76 MiB |
| `deep-media` (1 GiB, 1,374 files) | 91 MiB |
| `quick-files`, 5,000 / 20,000 / 40,000 files | 90 / 204 / 368 MiB |
| `recover` (1,374 files) | 83 MiB |
| `imaging` (2 GiB) | 4 MiB |

Memory does not grow with the source's size: a scan reads through a fixed cache and a bounded window of work, the heap
peaks of scans of 1.25 and 5 GiB images differ by under 2 MiB (`ScanLargeImageTest`), and a 16 TiB virtual disk is
scanned reading under 1 GiB (`LargeDriveTest`). It grows with the number of files found: each candidate is kept with
its evidence, validation and SHA-256 for the session, about 8 KiB per file here (`quick-files`: 278 MiB more for
35,000 more files). A drive holding a million files would need about 8 GB (L212). These peaks include making the
benchmark's inputs in the same process; the next section measures with the inputs made beforehand.

### Memory per file found (after P20, L212)

Most of that memory was copies the engine held while scanning (L212). With them gone, measured on the same machine
with the inputs made beforehand, each run alone, release builds with `RECOVERY_HARDENING=OFF` (the A/B builds of
the hardening comparison), the engine before and after the change built with the same benchmark code (2026-10-13):

| `quick-files` | Heap kept per file | Peak per file | Engine alone, no read cache | Peak working set |
| --- | --- | --- | --- | --- |
| 5,000 files | 2,432 → 1,396 B | 11,324 → 7,086 B | 4,138 → 868 B | 55 → 37 MiB |
| 20,000 files | 3,024 → 1,713 B | 11,875 → 6,757 B | 4,901 → 963 B | 204 → 125 MiB |
| 40,000 files | 2,318 → 1,335 B | 8,391 → 4,353 B | 3,987 → 845 B | 300 → 158 MiB |

"Kept" is what the sink keeps: every evaluated candidate (1,024 bytes instead of 1,912, plus its names and regions),
in a vector that grows by half each time. "Peak" adds the engine's working memory and its read cache (up to 64 MiB,
which weighs on small scans: per file, the peaks of 5,000 files look worst). "Engine alone" is a second scan whose
sink keeps nothing and that runs without the read cache: what the engine itself holds per file found.

The CLI (`recovery scan --mode quick` of the same cards: a session that keeps every candidate and journals the scan)
peaked at 38, 137 and 176 MiB of private memory, against 61, 246 and 351 MiB before: about 2 KB more per file found,
against 5.4 KB (the 20,000 to 40,000 file step). A drive holding a million files needs about 2 GB (L212).
`deep-media` (1,374 files) peaked at 87 MiB of working set (92 before): its peak is the cache and the decoders.
`recovery_benchmark --sizes` prints the sizes of what a scan keeps per file found.

## Cost of the hardening

P20's hardening links the C++ runtime statically and adds Control Flow Guard, CET compatibility and System32-only
imports. To measure it, the benchmark was built three ways and run in alternation, three rounds after a warm-up, on
the same inputs (medians; `RECOVERY_HARDENING` and `CMAKE_MSVC_RUNTIME_LIBRARY` choose the variant when configuring):

| Build | `deep-noise` | `deep-media` | `quick-files` |
| --- | --- | --- | --- |
| DLL runtime, no mitigations (before P20's hardening) | 172.5 MB/s | 77.7 MB/s | 28,097 files/s |
| Static runtime, no mitigations | 161.3 MB/s | 78.0 MB/s | 27,670 files/s |
| Static runtime and mitigations (shipped) | 172.1 MB/s | 77.8 MB/s | 28,365 files/s |

The shipped build is as fast as the one before the hardening: the differences are within 1% for the medians, while
single runs of one build spread by up to ±8%. The hardening has no measurable cost.

```powershell
cmake -S . -B build\release-nohard -G Ninja -DCMAKE_BUILD_TYPE=Release -DRECOVERY_HARDENING=OFF `
      -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL
cmake --build build\release-nohard --target recovery_benchmark
```

## Cost of ZIP carving (after P20)

ZIP carving adds a signature that every byte is checked against, and a format whose validation decodes every
member. An A/B on 2026-10-13 alternated the benchmark without the archive formats (before) and with them (after),
built from the same sources (`build/p20-scratch/release-nohard`, as above), four rounds each on the kept inputs.
Medians of the warm rounds (the first run of all was a cold start; the fourth round of both ran beside another
build):

| Build | deep-noise (2 GiB) | deep-media (1 GiB card) | candidates | peak memory |
| --- | --- | --- | --- | --- |
| Without ZIP | 158.8 MB/s | 73.1 MB/s | 10 / 1,374 | 73 / 92 MiB |
| With ZIP | 154.6 MB/s | 73.4 MB/s | 10 / 1,374 | 73 / 92 MiB |

The same candidates and the same peaks; the speeds differ by less than single rounds spread (±4%). Neither input
holds an archive: what ZIP costs where there are archives is decoding them (the stick of L213, below in
[validation.md](validation.md)).

## Notes

- The write benchmarks (`recover`, `imaging`) measure the SSD as much as the engine: imaging flushes its data before
  each metadata checkpoint, and recovered files are flushed before they are reported. Back-to-back runs of the same
  binary gave 86 and 280 MB/s for `recover` and 91 and 238 MB/s for `imaging` after several GiB had been written:
  the drive's write cache, not the engine, decided the first run, which missed the targets. The results above are
  of runs after the drive had rested.
- Real USB devices were not measured (L211): with their 40–100 MB/s, every scan above runs at the device's speed.
