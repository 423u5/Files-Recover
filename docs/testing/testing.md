# Testing

## Running

```powershell
cmake --preset msvc-debug
cmake --build --preset msvc-debug
ctest --preset msvc-debug              # everything
ctest --preset msvc-debug -L unit      # unit tests only
ctest --preset msvc-debug -L integration
ctest --preset msvc-debug -L filesystem
ctest --preset msvc-debug -L corruption
ctest --preset msvc-debug -L recovery
ctest --preset msvc-debug -L carving
ctest --preset msvc-debug -L formats
ctest --preset msvc-debug -L validation
ctest --preset msvc-debug -L evaluation
ctest --preset msvc-debug -L scan
ctest --preset msvc-debug -L session
ctest --preset msvc-debug -L metadata
ctest --preset msvc-debug -L cli
ctest --preset msvc-debug -L api
ctest --preset msvc-debug -L corpus    # P20: the corruption corpus, large drives, the logging review
```

Run the same suite under AddressSanitizer with `msvc-asan`. Run static analysis on the engine code with
`cmake --build --preset msvc-analyze`. The fuzz targets (P20) have a preset of their own, `msvc-fuzz`, whose tests
(label `fuzz`) run each target briefly and replay the inputs that once failed (see "Fuzzing" below).

## Suites

| Label | Location | Touches |
| --- | --- | --- |
| `unit` | `tests/unit` | Memory only (core, storage, imaging metadata, partitions, reconstruction of hand-made candidates and their data as file content, output names), plus temporary files for the destination-guard tests |
| `integration` | `tests/integration` | Temporary files under `%TEMP%\recovery-tests\<random>`, deleted afterwards; the Windows disk resolver; the Windows disk list (P19: disks opened with desired access 0, nothing read) |
| `filesystem` | `tests/filesystem` | FAT32 (and FAT16 and FAT12, after P20), exFAT and NTFS volumes generated in memory: normal, deleted, fragmented, contiguous, resident, Unicode, different cluster, sector and record sizes; after P20 also deleted FAT32 entries whose starts lost their top 16 bits, deleted folders that grew (`fat32_deleted_entry_test.cpp`, L7, L8) and orphaned exFAT folders (L13) |
| `corruption` | `tests/corruption` | Damaged FAT32, exFAT and NTFS metadata: loops, cross-links, truncated chains, damaged entry sets, torn and damaged FILE records, invalid run lists, directory cycles, orphans, unreadable sectors, hostile boot sectors and sizes, random mutation (also of recovery candidates and their reconstruction) |
| `recovery` | `tests/recovery` | Recovery candidates from generated FAT32, exFAT and NTFS volumes (contiguous, fragmented, deleted, partly overwritten, sparse, resident, Unicode and duplicate names, partitioned disks), their reconstructed data compared with the original files, and recovered files written below `%TEMP%\recovery-tests\<random>`. MP4 recovery (P12): FILESYSTEM, HYBRID and CARVING candidates from volumes and virtual sources larger than 4 GiB. Fragment reconstruction (P13): fragmented files on volumes whose free space holds old data, every status, mutation fuzzing |
| `carving` | `tests/carving` | The carving framework with test-only formats: signatures, registration, scanning, candidate generation, boundaries, bad sectors, cancellation, and virtual images up to 1 TiB (memory only), plus one temporary disk image file |
| `formats` | `tests/formats` | The image formats (JPEG, PNG, WEBP, GIF, BMP), the audio formats (MP3, WAV, M4A, raw AAC) and ZIP archives (after P20): files from the test builders and from other encoders, truncation at every position, damaged headers, segments, chunks, frames and boxes, false signatures, fragmentation, mutation fuzzing, and carving whole images with every format registered. The MP4 parser: builder files of every layout, files from FFmpeg, GPAC and Media Foundation checked against FFmpeg's demuxer, corrupt and misplaced boxes, damaged tables, truncation, scattered files, limits and fuzzing. P12: movie fragments, the analysis (audio or video, moov discovery, sample framing), the MP4 carving format, and carving whole sources with video registered |
| `validation` | `tests/validation` | P14: the validation levels, and every media decoder on builder files, files of independent encoders and generated vectors (PNG zlib streams, libwebp coding tools, fdkaac CRC-protected ADTS, libx264 and libx265 video), data written bit by bit that breaks one rule each, damage the structure cannot see, truncation, limits and fuzzing; the Windows playability checker (WIC, Media Foundation), skipping missing codecs |
| `evaluation` | `tests/evaluation` | P14: content identity (SHA-256, preliminary hash, duplicates) and candidate evaluation on FAT32 cards with carves, MP4 candidates and reconstructions merged into one candidate per file |
| `scan` | `tests/scan` | P15: the scan coordinator on a used FAT32 card (against the stages run one after the other, on 1-8 workers, cancelled in every stage and after every update, its sink failing, its source failing, paused), the scan's source, checkpoints, a 6 GiB virtual image and heap measurements, and the recovery job writing to temporary directories |
| `session` | `tests/session` | P16: the encoding of session records, the journal (torn tails, damage, formats, one writer, appends from many threads), and sessions on the scan tests' card written below `%TEMP%\recovery-tests\<random>`: normal sessions and recovery jobs, pause and resume, journals cut after every record and damaged, other formats and engines, other sources, listing |
| `metadata` | `tests/metadata` | P17: media metadata of builder files and of files of independent writers checked against exiftool and ffprobe, previews and their bytes, invalid media (garbage, truncation at every position, mutation fuzzing, damaged Exif and ID3), missing metadata; conditions, duplicates under different names, and recovery status, on the scan tests' card and a session written below `%TEMP%\recovery-tests\<random>` |
| `cli` | `tests/cli`, `tests/CMakeLists.txt` | P18: the command line run in-process on the scan tests' card written as an image file, a disk with three volumes and simulated physical disks: every command, usage errors, invalid paths, errors, Ctrl+C at every stage, journals cut as crashes leave them; files and sessions below `%TEMP%\recovery-tests\<random>`. `recovery.exe` run as a process: exit codes, Unicode paths, a real Ctrl+Break |
| `api` | `tests/api`, `tests/CMakeLists.txt` | P19: the GUI-facing API on the scan tests' card as an image file and as simulated physical disks, a disk of three volumes, a simulated disk list: a user interface's workflow compiled against the API's public headers alone, scans, events, concurrent queries, cancellation in every stage, pause and resume, recovery, sessions restored after a close and after a crash, reports, details and previews, imaging around bad sectors; files and sessions below `%TEMP%\recovery-tests\<random>` |
| `corpus` | `tests/corpus`, `tests/support/corpus.cpp` | P20: the corruption corpus (32 generated disks: P20's 26, a FAT16 card, ZIP archives, a FAT12 floppy disk, documents, a FAT32 volume Windows deleted from and an orphaned exFAT folder since, covering the plan's 16 categories, each scanned Deep and Quick through the API, recovered, checked against its original files and its recorded baseline), drives of up to 16 TiB as virtual sources, an opt-in sparse image of tens of GiB, and the logging review; images and sessions in memory or below `%TEMP%\recovery-tests\<random>` |

## Test images

The FAT32 tests use volumes built by `tests/support/fat32_builder`. It lays volumes out the way Windows
does: backup boot sector, FSInfo, mirrored FATs, long names with 8.3 aliases, deletion by 0xE5 with the
chain freed.

The exFAT tests use `tests/support/exfat_builder`. It writes main and backup boot regions with checksums,
the allocation bitmap, a compressed up-case table (ASCII, Latin-1, Greek, Cyrillic) and the root directory
with FAT chains, entry sets with set checksums and name hashes, NoFatChain runs for files written in one
piece, FAT chains for fragmented files and grown directories, and deletion by clearing the InUse bits and
the bitmap. It computes its checksums and hashes with its own code, not the engine's. Partition tests use `tests/support/partition_builder`, which writes MBR/EBR/GPT with correct
CRCs. The tests then corrupt specific fields.

The NTFS tests use `tests/support/ntfs_builder`. It lays volumes out the way mkntfs does: boot sector and
backup in the last sector, `$MFT` after `$Boot`, system records 0-11, reserved records 12-15, user records
from 64, `$MFTMirr` in the middle of the volume, FILE records protected by update sequence fixups,
`$STANDARD_INFORMATION`, `$FILE_NAME` and `$DATA` attributes, data resident in the record when it fits and
in data runs otherwise, `$Bitmap` and the MFT bitmap, and `$BadClus:$Bad`. It deletes like Windows: it clears
the in-use flag, increments the sequence number and frees the clusters and the record; a new file takes the
lowest free record, so deleting a directory and adding a file orphans the directory's children. It encodes
run lists, fixups and attributes with its own code. Its directory indexes are empty (see L33 in
[../limitations.md](../limitations.md)); the tests edit records through `NtfsImageBuilder::record()` before
`build()` applies the fixups, or damage the built image directly.

### Cross-checking against other FAT implementations

A builder and a parser written together can share the same misreading of the specification. To guard
against that, `Fat32ReferenceImageTest` compares the parser with images written by other software. It is
skipped unless `RECOVERY_FAT32_REFERENCE_DIR` names a directory of `<name>.img` / `<name>.manifest`
pairs (the manifest format is documented in the test).

`tests/reference/make_pyfatfs_reference.py` creates such a pair with pyfatfs 1.0.5:

```powershell
python -m venv venv; venv\Scripts\pip install "pyfatfs==1.0.5" "setuptools<81"
venv\Scripts\python tests\reference\make_pyfatfs_reference.py <dir>
$env:RECOVERY_FAT32_REFERENCE_DIR = "<dir>"; ctest --preset msvc-debug -R Reference
```

Things pyfatfs gets wrong, found during the cross-check, and therefore not used from it:

- It writes invalid 8.3 aliases (raw UTF-8 bytes) with mismatched checksums for non-ASCII names. Both
  the engine and pyfatfs itself reject those long names.
- It compacts the directory on delete instead of marking entries 0xE5, so it can't produce deleted files.

**FAT12 and FAT16 (after P20).** `tests/reference/make_mtools_fat_reference.py` makes three images with
mkfs.fat (dosfstools) and mtools, which write, fragment and delete files in an image without mounting it: a
1.44 MB FAT12 floppy disk, an MBR disk with a FAT12 partition of 4 KiB clusters, and a FAT16 volume of 2 KiB
clusters. They hold folders, long and non-ASCII names, an empty file, fragmented files, files whose chains
run through FAT12 entries that lie across two FAT sectors, and deleted files and folders. `fsck.fat -n`
checks each volume. Neither tool needs root: unpack the Ubuntu packages in WSL, as for exiftool.

```sh
apt-get -o Dir::State::Lists=$HOME/apt/lists -o Dir::Cache=$HOME/apt/cache download dosfstools mtools
for deb in *.deb; do dpkg -x "$deb" ~/fattools/root; done
export PATH=~/fattools/root/usr/bin:~/fattools/root/usr/sbin:/usr/sbin:$PATH
python3 tests/reference/make_mtools_fat_reference.py <dir>
```

```powershell
$env:RECOVERY_FAT32_REFERENCE_DIR = "<dir>"; ctest --preset msvc-debug -R Reference
```

mtools puts a new entry into the slots of a deleted one when they are enough. The script therefore writes
nothing more into a folder that holds a deleted entry, so that every deleted name its manifest lists is still
there. On 2026-10-14 (dosfstools 4.2, mtools 4.0.43) `Fat32ReferenceImageTest` and
`RecoveryReferenceImageTest` passed on all three images.

Images formatted and populated by Windows itself are the preferred reference. After P20 (2026-10-15, with L7,
L8 and L13), `tests/reference/make_windows_fat_reference.ps1` makes two with Windows' own drivers, on virtual
disks it creates (VHD files; it checks that each attached disk is the empty file-backed disk of its own file
before it formats it, and detaches it at the end; a disk it cannot finish is detached and its file removed):

- `win_fat32.img`: 600 MB, FAT32 of 4 KiB clusters (about 153,000). A 260 MB file puts what follows beyond cluster
  65,536; a camera folder of 150 photos with long names grows over several clusters, and every seventh photo is
  deleted; a folder of 120 photos and a subfolder is deleted whole; a voice note is deleted.
- `win_exfat.img`: 256 MB, exFAT of 4 KiB clusters: a file made fragmented by appending to it after another file
  was written, then deleted (whether its chain survives is L12's question, so only its name is listed); a folder
  filled to 126 of the 128 entries its cluster holds (a note of five clusters, a folder of photos, forty small
  files), whose note and photos' folder are deleted; then a newer file whose four entries cannot go at the end of
  the folder's cluster, so that Windows reuses the deleted entries (orphaning the old folder's, L13) or extends the
  folder.

The photos are JPEG files GDI+ encodes. Each manifest also lists every deleted file with its path, size and CRC-32
(`deleted_file`), which `Fat32ReferenceImageTest`, `ExFatReferenceImageTest` and `RecoveryReferenceImageTest` check
byte for byte. What Windows does shaped the scenarios (seen on the first images, 2026-10-16): it drops what it has
not yet written of a file deleted at once, so the script flushes the volume before it deletes; on exFAT it puts a
new entry set at the end of a folder that has room, and new data in the lowest free clusters, so what is written
after the deletions fits where the note was. Run it from an elevated PowerShell (it creates and formats virtual
disks), into a folder of its own (it never overwrites an image). It formats each new volume before giving it a
drive letter, so File Explorer does not offer to format it (it did when the letter came first; should it ever, Cancel):

```powershell
powershell -ExecutionPolicy Bypass -File tests\reference\make_windows_fat_reference.ps1 <dir>
$env:RECOVERY_FAT32_REFERENCE_DIR = "<dir>\fat32"; $env:RECOVERY_EXFAT_REFERENCE_DIR = "<dir>\exfat"
ctest --preset msvc-debug -R Reference
```

On 2026-10-16 (Windows 10 22H2), `Fat32ReferenceImageTest`, `ExFatReferenceImageTest` and both
`RecoveryReferenceImageTest` cases passed on its images: every active file, and every deleted file the manifests
list, byte for byte. What the images show Windows doing is in [../limitations.md](../limitations.md) (L7, L8, L12,
L13, L21): it clears the top 16 bits of a deleted FAT32 start, leaves a deleted folder's "." entry whole, grows a
folder after the data written before, keeps a deleted exFAT file's FAT chain, and reuses deleted exFAT entries
only when a folder has no room left at its end.

### Cross-checking exFAT

Two checks use exfatprogs, an independent exFAT implementation. Neither needs root. On Windows, run them
in WSL. Without root, the tools can be unpacked with `apt download exfatprogs` and `dpkg -x`.

1. **The parser reads images made by `mkfs.exfat`.** `tests/reference/make_exfatprogs_reference.sh`
   formats images with 512-byte and 128 KiB clusters, the default 4 KiB clusters, a Unicode label, a
   packed bitmap, and an MBR partition. `ExFatReferenceImageTest` then reads them. Each must open with no
   warnings, find the bitmap and use the volume's up-case table, scan with no issues, and account for every
   allocated cluster. The manifest format is documented in the test.

   ```sh
   MKFS_EXFAT=mkfs.exfat sh tests/reference/make_exfatprogs_reference.sh <dir>
   ```
   ```powershell
   $env:RECOVERY_EXFAT_REFERENCE_DIR = "<dir>"; ctest --preset msvc-debug -R ExFatReference
   ```

2. **`fsck.exfat` accepts the builder's images.** `ExFatBuilderExport` writes representative builder
   images (directories, fragmented, empty and deleted files, Unicode and 255-character names, a directory
   grown into a FAT chain, several geometries). `check_exfat_builder_images.sh` runs `fsck.exfat -n`
   (read-only) on each. fsck.exfat verifies boot checksums, the bitmap against every chain, set checksums
   and name hashes.

   ```powershell
   $env:RECOVERY_EXFAT_EXPORT_DIR = "<dir>"; ctest --preset msvc-debug -R ExFatBuilderExport
   ```
   ```sh
   FSCK_EXFAT=fsck.exfat sh tests/reference/check_exfat_builder_images.sh <dir>
   ```

mkfs.exfat cannot add files, and exfatprogs has no tool that writes them, so neither check covers files
written by another implementation. fsck.exfat 1.2.2 also rejects vendor extension entries, which the
specification allows, so the exported images leave them out; the engine's own tests cover them. Images
with files written by Windows are still the best reference: make them as described above for FAT32.

### Cross-checking NTFS

Two checks use ntfs-3g 2022.10.3, an independent NTFS implementation. Unlike the exFAT checks, the first
one covers files written **and deleted** by that implementation.

**Setting up ntfs-3g without root (WSL).** The tools can be unpacked from the Ubuntu packages, and FUSE
works inside an unprivileged user and mount namespace. ntfs-3g then tries to drop privileges with
`setgroups()`, which the namespace forbids, so a one-line library makes that call succeed:

```sh
mkdir -p ~/ntfs3g && cd ~/ntfs3g
apt download ntfs-3g libntfs-3g89t64
for f in *.deb; do dpkg -x "$f" root; done
printf '#include <grp.h>\nint setgroups(size_t n, const gid_t *l) { (void)n; (void)l; return 0; }\n' > shim.c
gcc -shared -fPIC -o shim.so shim.c
export PATH="$HOME/ntfs3g/root/bin:$HOME/ntfs3g/root/sbin:$PATH"
export LD_LIBRARY_PATH="$HOME/ntfs3g/root/lib/x86_64-linux-gnu"
export NTFS3G_PRELOAD="$HOME/ntfs3g/shim.so"
```

The shim is only loaded into the ntfs-3g process that mounts a test image, inside the namespace.

1. **The parser reads volumes written by ntfs-3g.** `tests/reference/make_ntfs3g_reference.py` formats
   images with mkntfs, mounts each with the ntfs-3g FUSE driver (inside `unshare -rm` when not root), writes
   files and deletes some of them, and writes a manifest. The images cover normal, resident, empty,
   Unicode and long names, nested directories, a sparse file, deleted files, a deleted directory with its
   files, a file fragmented into the holes of a full volume, 600 files in one directory (120 deleted),
   512-byte and 64 KiB clusters, 4 KiB sectors, and an MBR partition. `NtfsReferenceImageTest` then checks
   that each opens with no warnings and scans with no issues, that every allocated cluster is referenced,
   and every manifest line: sizes and CRC-32s of active and deleted files, deleted directories, fragments
   and sparse flags. The manifest format is documented in the test.

   ```sh
   python3 tests/reference/make_ntfs3g_reference.py <dir>
   ```
   ```powershell
   $env:RECOVERY_NTFS_REFERENCE_DIR = "<dir>"; ctest --preset msvc-debug -R NtfsReference
   ```

2. **ntfs-3g reads the builder's images.** `NtfsBuilderExport` writes representative builder images
   (directories, resident data crossing a fixup position, empty, Unicode, fragmented and deleted files, bad
   clusters, a fragmented MFT, several geometries). `check_ntfs_builder_images.sh` mounts each with ntfsinfo,
   reads every active file by record number with ntfscat (size, CRC-32, name) and recovers every deleted one
   with ntfsundelete. All of it is read-only.

   ```powershell
   $env:RECOVERY_NTFS_EXPORT_DIR = "<dir>"; ctest --preset msvc-debug -R NtfsBuilderExport
   ```
   ```sh
   sh tests/reference/check_ntfs_builder_images.sh <dir>
   ```

What the cross-check found: mkntfs formats its free reserved records 16-23 with record number 0, which the
parser first rejected as misplaced records (it now accepts 0 on free records); and ntfs-3g increments a
record's sequence number when it deletes it (the Linux NTFS documentation describes the same for Windows),
so the files of a deleted directory refer to the directory's previous sequence number. The builder's images need a `$Secure` entry in the root index
before ntfs-3g will mount them.

### Cross-checking recovery

`RecoveryReferenceImageTest` uses the same reference directories (`RECOVERY_NTFS_REFERENCE_DIR`,
`RECOVERY_FAT32_REFERENCE_DIR`) for filesystem-based recovery, end to end. Each image is opened as a
`DiskImageSource`, and its partition table is read. Every volume is opened with `openFilesystemRecovery`. Every
file the manifest lists with a CRC-32 (active files and intact deleted files) is recovered with `RecoveryWriter`
into a temporary directory. The written file must then match the manifest's size and CRC-32, with nothing
missing, unreadable or reallocated. Fragmented files must have a recorded layout of several fragments, and
sparse files must have zero regions and no missing data.

```powershell
$env:RECOVERY_NTFS_REFERENCE_DIR = "<dir>"; $env:RECOVERY_FAT32_REFERENCE_DIR = "<dir>"
ctest --preset msvc-debug -R RecoveryReference
```

At the end of P7, the 7 ntfs-3g images (540 active and 128 deleted files, plus 43 deleted files whose clusters
may be reused) and the pyfatfs image (7 files) all passed. One reference name is long enough that the recovered
path exceeds `MAX_PATH`; the test reads it through `\\?\` (L44).

## Carving tests

The framework itself is tested with formats that exist only in the tests
(`tests/support/carving_formats.hpp`): a size-field format with a CRC-32, an end-marker format, a box-structured
format whose signature sits 4 bytes into the file, a masked-signature format without end information, and a
scripted format whose steps each test supplies (for broken and hostile format modules). Their generated payloads
never contain the first byte of any test signature, so hits only occur where a test plants a file.

`test::VirtualSource` is a source of any size (up to 2^63 - 1 bytes) whose content is computed: zeros or
deterministic noise, with byte strings planted at chosen offsets. It records the number, size and order of reads,
so the streaming tests can check that a scan reads sequentially, in bounded blocks, each byte once. It can also
fail sectors like `MemoryStorageSource`.

The carving executable replaces the global `operator new` and `operator delete`
(`tests/carving/heap_tracking.cpp`) to measure heap growth. The streaming tests use it to show that scanning the
last 64 MiB of a 1 TiB image, a full 128 MiB scan and a carving run of over 200 files each stay within about one
read block plus one cache. Under AddressSanitizer the replacement is left out and only these memory assertions are
skipped (L58).

## Image format tests

The image formats (P9) are tested against files this project writes and files it does not.

**The builders** (`tests/support/image_builders.hpp`) write real files, not fixtures: a baseline and progressive
JPEG encoder (DCT, quantization, Huffman coding, byte stuffing, restart intervals, JFIF and Exif segments with a
thumbnail JPEG inside them), PNG with a zlib stream of stored blocks split over IDAT chunks (every color type and
bit depth, Adam7 interlacing, ancillary chunks, APNG), GIF with LZW data (87a and 89a, global and local color
tables, frames, interlacing, extensions), BMP with every header version and 1 to 32 bits per pixel including RLE4
and RLE8 and a V5 color profile, and WebP containers (simple, extended, alpha, metadata, animation) around VP8 and
VP8L bitstreams that libwebp made. Every file comes from a seed, so it is reproducible.

**The samples** (`tests/support/image_samples.hpp`, generated into `image_samples.cpp`) are 25 small files written
by libjpeg-turbo, libwebp, giflib and Windows GDI+: baseline, progressive, restart-interval, grayscale, 4:4:4 and
arithmetic-coded JPEGs, lossy/lossless/alpha/metadata/animated WebPs, GIFs, PNGs with and without alpha, and BMPs
with Windows, OS/2 and V3 headers. `ImageSamplesTest` checks that each one is recognised by its own format and by
no other, validated, and carved back byte for byte out of a noisy image.

To make them again (the tool versions are recorded in the generated file):

```sh
# in WSL, with cjpeg/djpeg/jpegtran/cwebp/dwebp/img2webp/webpmux/gif2rgb on PATH
sh tests/reference/make_image_samples.sh <dir>
```
```powershell
powershell -File tests\reference\make_image_samples.ps1 <dir>   # GDI+
python3 tests/reference/embed_image_samples.py tests/support/image_samples.cpp <dir> <dir2>
```

**A larger corpus** can be pointed at with `RECOVERY_IMAGE_REFERENCE_DIR`: every `.jpg`, `.jpeg`, `.png`, `.webp`,
`.gif` and `.bmp` file in that directory must be intact by the engine's judgement and carved back exactly.
Photos from a camera or phone are the most valuable input here. The same scripts with the argument `large` make a
corpus of 320x240 to 640x480 images:

```powershell
$env:RECOVERY_IMAGE_REFERENCE_DIR = "<dir>"; ctest --preset msvc-debug -R ImageReference
```

**The other direction**: `ImageBuilderExport` writes the builders' own files to `RECOVERY_IMAGE_EXPORT_DIR`, and
independent decoders then read them, so that a builder and a format module cannot share a misreading of a
specification:

```powershell
$env:RECOVERY_IMAGE_EXPORT_DIR = "<dir>"; ctest --preset msvc-debug -R ImageBuilderExport
powershell -File tests\reference\check_image_builder_files.ps1 <dir>      # GDI+
```
```sh
sh tests/reference/check_image_builder_files.sh <dir>   # libjpeg-turbo, libpng, giflib, libwebp
```

At the end of P9, all 30 exported files were decoded by GDI+ (RLE4, RLE8 and V5 bitmaps included), and every JPEG,
PNG, GIF and WebP by djpeg, pngcheck, giftext, webpinfo and dwebp. cjpeg's BMP reader takes only 8, 24 and 32 bits
per pixel with a core or `BITMAPINFOHEADER` header, so the other bitmaps are left to GDI+. 25 reference images made
with the `large` scripts were validated and carved exactly.

**Fuzzing**: each format runs 1000 to 2000 damaged copies of an intact file (bytes replaced, cut short, lengths
inflated, runs of zeros or noise) through its header check, end detection and validation. Nothing may crash, read
outside the data it was given (the AddressSanitizer build), or report more bytes than it was given.

## Audio format tests

The audio formats (P10) are tested the same way as the images: against files this project writes, files it does
not, and with the builders' files checked by other decoders.

**The builders** (`tests/support/audio_builders.hpp`) write files that decoders play:

- MP3: MPEG-1, 2 and 2.5 Layer III frames with real side information and main data (count1 quadruples coded with
  Huffman table B, which decode to quiet noise), a working bit reservoir, CRCs, every channel mode, constant and
  variable bitrates, a Xing or Info tag with or without a LAME extension (music length, music CRC, tag CRC), and
  ID3v2 (2.2, 2.3, 2.4, with a picture), APEv2, Lyrics3 and ID3v1 tags.
- ADTS: frames of AAC raw data blocks that decode to silence (channel elements without scale factor bands),
  padded with fill elements to varying sizes; MPEG-2 and MPEG-4 headers, profiles, rates, mono and stereo.
- M4A: ftyp, moov (mvhd, trak, tkhd, mdia, mdhd, hdlr, minf, smhd, dinf, stbl with stsd/mp4a/esds, stts, stsc,
  stsz, stco or co64) and mdat around the same AAC frames; moov before or after mdat, 64-bit mdat sizes,
  iTunes-style metadata, audio and generic brands, a video track.
- WAV: PCM of 8 to 32 bits, IEEE float, A-law, mu-law and IMA ADPCM, WAVE_FORMAT_EXTENSIBLE, JUNK, bext, fact,
  LIST INFO and cue chunks.

**The samples** (`tests/support/audio_samples.hpp`, generated into `audio_samples.cpp`) are 36 short files: LAME
(CBR, VBR, CRC, no Info tag, MPEG-2 mono, MPEG-2.5, ID3v2 and ID3v1 tags), FFmpeg (tagged and bare MP3, ADTS, M4A
with moov last and first, a generic `mp42` brand, fragmented with moof boxes, ALAC; PCM and float WAV with a LIST
chunk), faac (ADTS, M4A through mp4v2), fdkaac (ADTS, M4A), SoX (8/16/24-bit, float, mu-law, A-law, IMA and MS
ADPCM WAV), Python's `wave` module, and Windows itself: the speech synthesizer (PCM, mu-law and A-law WAV) and
Media Foundation (an M4A with the generic `mp42` brand and a `uuid` box, and an MP3 that is an ID3v2 tag in front
of frames without an info tag), which is what the Voice Recorder and other Windows apps write.
`AudioSamplesTest` checks that each one is recognised by its own format and by no other, validated, and carved
back byte for byte out of a noisy image with every image and audio format registered.

The WSL tools were unpacked without root (apt-get download and dpkg-deb -x) into `~/audiotools/root`; the
distribution's package index was refreshed into the home directory with
`apt-get -o Dir::State::Lists=$HOME/apt/lists -o Dir::Cache=$HOME/apt/cache -o Debug::NoLocking=1 update`.
To make the samples again:

```sh
# in WSL, with sox, lame, ffmpeg, faac, fdkaac and python3 on PATH
sh tests/reference/make_audio_samples.sh <dir>
```
```powershell
powershell -File tests\reference\make_audio_samples.ps1 <dir>   # speech synthesizer, Media Foundation
python3 tests/reference/embed_audio_samples.py tests/support/audio_samples.cpp <dir> <dir2>
```

**A larger corpus**: every `.mp3`, `.wav`, `.m4a` and `.aac` file in `RECOVERY_AUDIO_REFERENCE_DIR` must be intact
by the engine's judgement and carved back exactly. Recordings from phones and voice recorders are the most valuable
input. The same scripts with the argument `large` make 20-second files; `C:\Windows\Media` is a ready-made corpus of
WAV files:

```powershell
$env:RECOVERY_AUDIO_REFERENCE_DIR = "<dir>"; ctest --preset msvc-debug -R AudioReference
```

**The other direction**: `AudioBuilderExport` writes the builders' files to `RECOVERY_AUDIO_EXPORT_DIR`, and FFmpeg
(stopping at the first error, with CRC checks), mpg123 and SoX decode them:

```powershell
$env:RECOVERY_AUDIO_EXPORT_DIR = "<dir>"; ctest --preset msvc-debug -R AudioBuilderExport
```
```sh
sh tests/reference/check_audio_builder_files.sh <dir>
```

At the end of P10, all 22 exported files were decoded by FFmpeg, the MP3 files by mpg123 and the WAV files by SoX
without an error. As a control, a copy with one damaged byte of side information and a copy with a chunk offset
outside the file were refused by FFmpeg. 36 reference files made with the `large` scripts and the 70 WAV files in
`C:\Windows\Media` were validated and carved exactly.

**Fuzzing**: as for the images, 300 to 3000 damaged copies of builder files and of every sample per format.

## MP4 tests

The MP4 parser (P11) and the MP4 format (P12, [../formats/mp4.md](../formats/mp4.md)) are tested against files this
project writes, files it does not, and with the builder's files checked by other demuxers; MP4 recovery (P12) on
generated volumes and sources.

**The builder** (`tests/support/mp4_builders.hpp`) writes video, audio (AAC frames of silence with an esds box)
and 3GPP timed text tracks in any layout: moov before, after or between mdat boxes; the media data in one or
several mdat boxes, interleaved or one track after the other; 32-bit or 64-bit sizes for mdat, moov and every box
inside moov; stsz or stz2 (4, 8 or 16 bits), one size for all samples, stco or co64; version 0 or 1 headers; an mdat
to the end of the file; ftyp or none; free, wide and uuid boxes; edit lists and user data; children in reverse
order. It returns where every sample and chunk went, and `tests/formats/mp4_test_helpers.hpp` compares the parser's
chunks and samples with that. Since P12 it also writes movie fragments (moov with mvex, then moof and mdat pairs,
with each of the three kinds of base data offset, and optionally samples in moov's tables), HEVC tracks, and video
samples made of NAL units after length fields of 1, 2 or 4 bytes that fill each sample exactly, as an encoder's
would (the coded bytes are still not H.264); `spreadMp4` moves a file's media data gigabytes further on, to plant
files larger than 4 GiB in a virtual source.

**The samples** (`tests/support/mp4_samples.hpp`, generated into `mp4_samples.cpp`) are 24 files of half a second:
FFmpeg (MP4 with moov last and first, video only, audio only, two audio tracks, B-frames with ctts and edit lists, a
mov_text subtitle track, fragments in four ways (`empty_moov`, CMAF with `default_base_moof`, a moov with samples
then fragments, `separate_moof`), H.265, MPEG-4 Part 2; MOV; 3GP), GPAC's MP4Box (interleaved with moov first,
flat, co64, stz2, moov padding, fragments) and Windows' Media Foundation (a clip with and without AAC audio from
MediaComposition, and the same transcoded by MediaTranscoder, as the Photos and Camera apps write). With each file,
`embed_mp4_samples.py` stores what FFmpeg's demuxer finds: per track the kind, the codec, the size or channels and
rate, the number of samples and an FNV-1a hash of every sample's offset and size (`ffprobe -ignore_editlist 1
-show_packets`); since P12 the samples of fragmented files are hashed as well. `Mp4SamplesTest` requires the parser
to find the same in every file (the movie fragments' samples included), every prefix to be `Truncated` (or `Valid`
at a box boundary: GPAC's trailing free box can go), and fuzzing not to crash it.

The GPAC tools were unpacked without root like the audio tools, into `~/mp4tools/root` (`source ~/mp4tools/env.sh`
also sets up `~/audiotools`). To make the samples again:

```sh
# in WSL, with ffmpeg (libx264, libx265) and MP4Box on PATH
sh tests/reference/make_mp4_samples.sh <dir>
```
```powershell
powershell -File tests\reference\make_mp4_samples.ps1 <dir2>        # Media Foundation
```
```sh
python3 tests/reference/embed_mp4_samples.py tests/support/mp4_samples.cpp <dir> <dir2>   # needs ffprobe
```

**A larger corpus**: every `.mp4`, `.mov`, `.m4v`, `.m4a` and `.3gp` file in `RECOVERY_MP4_REFERENCE_DIR` must parse
as `Valid` (streamed through a disk image source, so size is no limit), and since P12 be classified, found whole
(`Found` at its size) by the MP4 or the M4A format, and validated by it, sample framing included. Phone and camera
recordings are the most valuable input; the same scripts with the argument `large` make 20-second files at
640x360:

```powershell
$env:RECOVERY_MP4_REFERENCE_DIR = "<dir>"; ctest --preset msvc-debug -R Mp4Reference
```

**The other direction**: `Mp4BuilderExport` writes seven builder layouts to `RECOVERY_MP4_EXPORT_DIR`, each with a
`.samples` file of where every sample is. `check_mp4_builder_files.sh` requires FFmpeg to list exactly those samples
and to remux the file without an error, and MP4Box to read it (what FFmpeg's video decoder says about the
placeholder video while probing is set aside):

```powershell
$env:RECOVERY_MP4_EXPORT_DIR = "<dir>"; ctest --preset msvc-debug -R Mp4BuilderExport
```
```sh
sh tests/reference/check_mp4_builder_files.sh <dir>
```

At the end of P11, all 21 samples matched FFmpeg's reading of them; 48 larger files (the scripts' 20-second files and
the 27 MP4 videos in Visual Studio's `Common7\IDE\CommonExtensions\Platform`, made by screen recorders and an online
GIF converter) parsed as `Valid`; and all seven builder layouts passed FFmpeg and MP4Box. As a control, a copy with
one chunk offset moved by 4 bytes was caught.

**Fuzzing**: 1500 damaged copies of each of five builder layouts and 300 of every sample, in the AddressSanitizer
build too. **Corruption**: the size of every top-level box set to values below its header, to one or eight bytes
more or less, to 0xFFFFFFFF, and to 0 where a moov follows it (never `Valid`); the size of every box of the plan's
list inside moov set to 0, 1, 5, one or eight bytes more or less, and 0xFFFFFFFF (always `Invalid`).

At the end of P12, all 24 samples matched FFmpeg's reading of them, movie fragments included, and the corpus of 51
files (P11's 48 and three larger Media Foundation files) was carved whole by the right format and validated, with
the NAL units of every AVC and HEVC sample (tens of thousands) filling them: no false alarm of the framing check.

**MP4 recovery** (`tests/recovery/mp4_recovery_test.cpp`, `tests/unit/recovery/candidate_content_test.cpp`;
[../recovery/mp4_recovery.md](../recovery/mp4_recovery.md)) builds FAT32, exFAT and NTFS volumes (and a partitioned
disk) holding builder MP4 files, active, deleted, fragmented on disk, partly overwritten by later files, with
damaged metadata or on bad sectors, and planted in free space or inside an active file. Every candidate's data is
reconstructed and compared with the original file, and every sample's damage is counted exactly from where the
builder put it. Sources larger than 4 GiB are `VirtualSource`s with files spread over them by `spreadMp4`.

## Archive format tests (after P20)

`tests/formats/zip_format_test.cpp` checks ZIP ([../formats/archives.md](../formats/archives.md)) on archives that
Python's `zipfile` wrote and on reworked ones (`tests/support/zip_samples.cpp`): DEFLATE with Huffman codes, stored,
empty members and folders, data descriptors for every method, ZIP64 extra fields and end records, a comment, an APK
signing block, a DOCX's first members, LZMA and bzip2 members (not checked: `NotValidated`), encrypted members; no
prefix complete; damaged data, central directory entries, end records and sizes; an archive seen from a later member;
header checks; the limits; the DEFLATE decoder; carving with the image formats (a PNG stored in a verified archive is
skipped, in a damaged one carved); fuzzing. `tests/validation/zip_validation_test.cpp` checks the validation levels,
`tests/recovery/fragment_recovery_test.cpp` that a damaged archive in one piece is no seed and that an archive in two
pieces is reconstructed, and the corpus scenario `zip-archives` all of it through the API. The samples are written by

```bash
python3 tests/reference/make_zip_samples.py tests/support/zip_samples.cpp   # Windows' Python too; writes the .cpp itself
```

which checks every archive with `zipfile.testzip()` first; `tests/support/zip_builders.cpp` builds archives of stored
members in C++. A real APK that Android Studio ships (its signing block aligned by `apksigner`, L240) is checked by
`ZipFormatTest.AnApkWhoseSigningBlockApksignerAlignedIsIntact` in its reworked form.

## Document format tests (after P20)

`tests/formats/pdf_format_test.cpp` and `tests/formats/ole2_format_test.cpp` check PDF and OLE2 compound files
([../formats/documents.md](../formats/documents.md)) on documents the test builders write
(`tests/support/pdf_builders.cpp`: `PdfWriter`, one-page documents with a JPEG, updates, object and
cross-reference streams, hybrid-reference and linearized layouts; `tests/support/ole2_builders.cpp`: compound files of
versions 3 and 4, Word, Excel, PowerPoint and Outlook files, free sectors) and on 11 that real writers made
(`tests/support/document_samples.cpp`): every layout intact, damaged streams, tables, trailers, FATs, directories and
chains, truncation, header checks, the limits, carving with the image formats, and naming by content.
`tests/formats/document_reference_test.cpp` holds three tests:

- `DocumentSamplesTest` (always): every sample is recognised by its format only, validated (`Valid`; the AES-256 PDF
  `NotValidated`), named by its content, and carved back byte for byte with the images, documents and archives
  registered, one candidate each.
- `DocumentReferenceTest` reads folders of documents made elsewhere, recursively: `RECOVERY_DOCUMENT_REFERENCE_DIR`,
  folders separated by `;`. Every `.pdf`, OLE2 file (`.doc`, `.xls`, `.ppt`, `.msg`, `.msi`, `.msp`, `.mst`, ...) and
  file built on ZIP (`.docx`, `.xlsx`, `.pptx`, `.odt`, `.epub`, `.jar`, `.apk`, ...) that its format's header check
  takes must be carved to its end from among other data (or short of it only by a PDF's padding or an OLE2 file's free
  sectors), never be called damaged (`Valid` or `NotValidated`), validate the same as carved and as named, and be
  named by its content as its extension says (a `.jar` without a manifest that starts with neither `META-INF/` nor a
  class file: as a plain archive). It prints the verdicts, why documents were not checked, and how files were named.
  Files that the header check rejects (text named `.msg`) are counted, not checked.
- `DocumentBuilderExport` writes the builders' files to `RECOVERY_DOCUMENT_EXPORT_DIR` for independent readers.

```powershell
$env:RECOVERY_DOCUMENT_REFERENCE_DIR = "C:\Windows\Installer;D:\VS\Packages;C:\Program Files\Android"
ctest --preset msvc-debug -R DocumentReferenceTest --output-on-failure -V
$env:RECOVERY_DOCUMENT_EXPORT_DIR = "$env:TEMP\docexport"
ctest --preset msvc-debug -R DocumentBuilderExport
```

The samples are written by `tests/reference/make_document_samples.ps1` (Windows: Microsoft Edge headless, the
"Microsoft Print to PDF" and "Microsoft XPS Document Writer" printers through `System.Drawing.Printing`, ole32's
`StgCreateStorageEx`, the `WindowsInstaller.Installer` COM object), then `make_document_samples.sh` (qpdf's
linearized, object-stream and AES-256 variants, each passing `qpdf --check`), and embedded by
`embed_document_samples.py` (runs of 64 or more zeros are left out: compound files are mostly empty sectors):

```bash
powershell -File tests/reference/make_document_samples.ps1 <folder>
wsl sh tests/reference/make_document_samples.sh /mnt/<folder>        # qpdf 11.9.0 made the embedded ones
py -3 -I tests/reference/embed_document_samples.py tests/support/document_samples.cpp <folder>
wsl sh tests/reference/check_document_builder_files.sh /mnt/<export folder>   # qpdf --check and olefile
```

qpdf 11.9.0 and python3-olefile 0.46 were unpacked without root into WSL (`apt-get download`, `dpkg -x`):
`PATH`, `LD_LIBRARY_PATH` and `PYTHONPATH` point into that folder. The corpus scenario `documents` checks all of it
through the API (below).

## Fragment reconstruction tests

`tests/recovery/fragment_recovery_test.cpp` ([../recovery/fragment_recovery.md](../recovery/fragment_recovery.md))
builds FAT32 "cards" whose free clusters hold old data (deterministic noise, filled before any file is written, so
cluster slack and gaps hold it too, as on a card that has been used) and one exFAT volume that clears the chains of
deleted files. Files from the image, audio and MP4 builders are written in pieces with `addFileInClusters` around
files that still exist, around deleted files whose entries survive, across old data, or out of order; pieces are
wiped, overwritten by new files or left unreadable; files without entries are planted in free clusters for the
carving seeds. Every candidate is checked: each hypothesis's data is validated again by the test and must give the
verdict the hypothesis states; its evidence adds up with its regions and clusters; names, paths and methods follow
the rules; and the chosen reconstruction is compared byte for byte with the original file. MP4 pieces end where the
samples can tell (`checkedSplit`): a boundary inside audio data or a NAL unit is ambiguous by design (L108), which
`AudioInMp4IsPlacedByTheAllocationOrReportedAmbiguous` shows. `FragmentRecoveryFuzzTest.MutatedVolumesStayBounded`
mutates the FATs, the root directory and the files' data 40 times with tight search limits and checks the bounds.

## Validation and evaluation tests

`tests/validation` ([../formats/media_validation.md](../formats/media_validation.md)) runs every media decoder
through `IMediaValidator` (`media_test_helpers.hpp`: the registered formats and decoders, `BitWriter` for syntax
written field by field, mutation fuzzing with `fuzzMedia`). The vectors are generated in WSL and embedded as C++
arrays by `tests/reference/embed_media_vectors.py` (each at most 64 KiB):

```bash
# in WSL; tools as for the image, audio and MP4 samples above
python3 tests/reference/make_png_media_vectors.py tests/validation/png_media_vectors.cpp  # writes the .cpp itself
tests/reference/make_webp_media_vectors.sh <dir>    # then embed_webp_media_vectors.py
tests/reference/make_aac_media_vectors.sh <dir>     # then embed_media_vectors.py ... aac_vectors aac <dir>
tests/reference/make_video_media_vectors.sh <dir>   # then embed_media_vectors.py ... video_vectors mp4 <dir>
```

The AVC and HEVC slice header parsing was cross-checked once against FFmpeg's `trace_headers` bitstream filter
(`ffmpeg -i file.mp4 -c copy -bsf:v trace_headers -f null -`): every slice header of the 23 video vectors ends at the
bit where FFmpeg ends it. `playability_test.cpp` needs WIC and Media Foundation; the WebP and HEVC cases skip when
their Store extensions are not installed.

`tests/evaluation` ([../recovery/candidate_evaluation.md](../recovery/candidate_evaluation.md)) builds FAT32 cards
whose free clusters hold old data, adds carves of the whole card, MP4 recovery candidates and fragment
reconstructions (real and synthetic), and checks every evaluated candidate against what `reconstructCandidate`
delivers: its SHA-256, its warnings, its duplicate links and its explanation.

## Scan tests

`tests/scan` ([../recovery/scanning.md](../recovery/scanning.md)) checks every scan against what the stages deliver
when run one after the other on one thread (`test::referenceScan` in `scan_test_support.cpp`):
- `test::describe` writes each evaluated candidate as one line holding its id, name, method, layout, validation,
  identity, duplicate and container links, evidence and warnings;
- a scan passes when its lines are those of the reference, in order.

The card (`test::makeCard`) is a FAT32 volume whose free clusters hold old data. Its files exercise every stage:
- active files, and a copy under another name;
- a deleted file whose guessed layout holds;
- a deleted file in two pieces 24 clusters apart (within fragment reconstruction's reach, L104);
- a GIF, an MP3 and an MP4 that no entry names.

Interruptions are made through the run options, so that the tests run the scan's own paths:
- the progress callback requests a cancellation in a given stage;
- a hook in the collecting sink cancels the scan, or fails as a session store would;
- `MemoryStorageSource::addFatalSector` fails the source.

Cancellation tests use a window of one item, so that it lands between two items. A resumed scan starts from the
checkpoint the collector built from the updates it took, never from the coordinator's own.

`scan_large_test.cpp` builds a 6 GiB `VirtualSource` with an MBR (a FAT32 partition and an empty one) and files
planted beyond 4 GiB at sector starts, and scans it with `alignment` 512. Zeros at sector alignment keep a whole
scan to seconds. The executable replaces `operator new` like the carving tests (`carving/heap_tracking.cpp`), so
`MemoryStaysBoundedWhateverTheImageSize` measures the heap while scanning images of 1.25 and 5 GiB (skipped under
AddressSanitizer). It records the peaks as test properties (`--gtest_output=xml`). The recovery job's tests write
below `%TEMP%\recovery-tests\<random>` and compare every file's path and bytes with those one `RecoveryWriter` gives
when it writes the candidates one after the other.

## Session tests

`tests/session` ([../recovery/sessions.md](../recovery/sessions.md)) uses the scan tests' card and reference
(`scan_test_support.cpp` is built into it too). Sessions are written below `%TEMP%\recovery-tests\<random>`.

Crashes are simulated on the journal's bytes:
- `scanned()` runs one deep scan in a session and keeps its journal and its records (`test::readRecords`);
- a test writes a prefix of those bytes into a new session folder: the journal cut after a record, or in the
  middle of the next one (`test::writeJournal`), which is what a process ending there leaves;
- it opens that session as an application would after a restart, and resumes it.

A resumed scan passes when its candidates are the reference's lines, ids 1 to n. It also must not do again what
was recorded: its first update follows the last recorded one, and none of its updates is of a stage the cut
journal had finished.

Recovery jobs are cut the same way. They are run with one worker, one file at a time and an update per file, so
that every cut has one state of the destination:
- the files done before the cut are there, whole;
- the file begun at the cut is there empty, half written or whole (but not recorded done).

The resumed job must leave exactly the files and names of the uninterrupted job: a half-written file it did not
remove would show as a second file, "name (1)".

Damage and other formats are made by flipping bytes, or by building journals from records
(`test::buildJournal`, `test::recordOf`): records that check but cannot be used, unknown types and flags, newer
headers, and sessions rewritten as another engine's (`asEngine`). Refused sessions must be left byte for byte as
they were.

## Metadata tests

`tests/metadata` ([../recovery/metadata.md](../recovery/metadata.md)) reads metadata from the builders' files
(the JPEG builder writes Exif fields when asked: orientation, make, model, date taken and its offset, in either byte
order) and from files put together in the tests where the builders do not reach: ID3 tags of any frames, versions
and encodings (`test::id3Tag`), MP4 user data appended to moov (`test::userData`, `test::withMoovChild`), patched
track matrices and header times. The card tests reuse the scan tests' card and reference scan
(`scan_test_support.cpp` is built into the suite).

`metadata_reference_test.cpp` checks the engine against exiftool (images: size, orientation, camera, date taken,
frames, loop count, Exif thumbnail) and ffprobe (audio and video: codecs, profiles, levels, rates, channels, sizes,
frame rates, rotation, languages, brands, creation time, tags, cover size, duration) on files of independent
writers. `tests/metadata/metadata_samples.cpp` holds their expectations, and the bytes of the files made for P17;
the other files are the image, audio and MP4 samples already embedded in `tests/support`, read back by name:

```bash
# in WSL: source ~/audiotools/env.sh, the imgtools PATH (cwebp), exiftool on PATH (a wrapper around
# perl -I ~/exiftool/root/usr/share/perl5 ~/exiftool/root/usr/bin/exiftool), python3 with mutagen
tests/reference/make_metadata_samples.sh ~/metasamples
python3 tests/reference/embed_metadata_samples.py tests/metadata/metadata_samples.cpp ~/metasamples \
    --embedded tests/support/image_samples.cpp tests/support/audio_samples.cpp tests/support/mp4_samples.cpp
```

The script stops when exiftool's rotation of a video is not ffprobe's (one counts clockwise, the other
counter-clockwise), and leaves out what the engine documents it does not read (profiles of codecs other than AVC
and HEVC, Macintosh language codes: L157, L158). Durations may differ by 70 ms or 3%: ffprobe applies encoder delay
and edit lists (L160).

## CLI tests

`tests/cli` ([../recovery/cli.md](../recovery/cli.md)) runs the command line through `cli::run`, in-process, with
`test::Cli`. Its output and error streams are captured, every run gets a fresh `Interrupt`, and engine progress is
reported at every step (intervals of 0 ms), so that a test can press Ctrl+C at a chosen point. `Cli::run(arguments,
onProgress)` calls `cli.interrupt().request()` from the hook; `runInterrupted` presses it before the command
begins. The disk resolver says every folder is on disk 0. The sources are:

- the scan tests' card written as `card.img` (`scan_test_support.cpp` is built into the suite), with the files it
  was made from rebuilt by the builders (`test::cardOriginals()` in `tests/support/card_fixtures`, shared with the
  API tests: what `recover` must give back byte for byte), and the reference scan's candidates
  (`test::expectedCard`);
- `test::threeVolumeDisk()`: an MBR with a FAT32, an exFAT and an NTFS volume;
- simulated physical disks (`test::simulatedDisk`: `MockDeviceOpener` and `MemoryDeviceIo` from `tests/support`,
  with byte ranges whose reads fail with `ERROR_CRC`), given to the CLI as `\\.\PhysicalDrive7`. Another resolver
  says a folder is on disk 7 when a test needs a destination on the source disk.

Reports are read with `test::json::parse` (`tests/support/json_reader`), a strict reader (UTF-8, escapes, no
duplicate names, nothing after the value). Sessions are checked through the engine (`RecoverySession::open`, `readSessionSummary`). Crashes are made
as the session tests make them (`session_test_support.cpp` is built in too): the journal cut after a record or in
the middle of one, and the destination left as a crash leaves it.

`CliProcessTest` starts `recovery.exe` (`RECOVERY_CLI_EXE`, from CMake) with `CreateProcessW` and pipes for its
streams. The Ctrl+Break test starts it in a process group of its own (`CREATE_NEW_PROCESS_GROUP`) and sends
`CTRL_BREAK_EVENT` through the console it shares with the test; a test process without a console makes a hidden one
(`AllocConsole`) for that test. The scan it stops is of a 16 MiB card, long enough to be under way.

## API tests

`tests/api` ([../recovery/api.md](../recovery/api.md)) runs `RecoveryApi` in-process. Each test makes an
`ApiWorld`: a temporary folder with the scan tests' card written as `card.img`, a sessions folder, and the API made
through `createRecoveryApi` with the platform simulated (`PlatformHooks`):

- physical disks are a `DiskRack`: simulated disks by number (from 7), any other number answers "no such disk", and
  opening one can be made to fail (access denied);
- the disk resolver puts the folders a test names (`placeOn`, created first: destination checks ask about the
  nearest folder that exists) on a chosen disk and every other path on disk 0, the "system disk" (`systemFolder`
  is in the test's folder);
- the disk list is a function the test gives (`diskLister`);
- `progressInterval` is 0 (every report of the engine is an event), and `onOperationProgress` lets a test pause or
  cancel an operation at a chosen point, on the operation's thread;
- the events are recorded by an `EventLog` (`waitFor`, `waitForFinished`), whose hook can block the event thread to
  play a slow user interface.

`gui_workflow.cpp` is the acceptance test's user interface. CMake builds it into `recovery_api_gui_client`, whose
include path is a copy of the GUI-facing headers alone (`build/<preset>/tests/api_only_include`: `api/` and the
engine's two error headers). It cannot compile against anything else of the engine. Scans and recoveries are
checked against the reference scan's candidates (`journalCandidates` reads a session's journal once the API has
closed it) and the card's originals, byte for byte. A crash is the session folder copied while the scan runs
(through shared reads, as the journal is open), then opened by a new API.

## Corruption corpus (P20)

`tests/support/corpus.cpp` holds 32 scenarios (P20's 26; since then `fat16-card`, `zip-archives`, `fat12-floppy`, `documents`, `fat32-windows-deleted` and `exfat-orphaned-folder`): disk images built the same, byte for byte, every time, each with the
original files it holds and what recovery must give back of each (found by the filesystem with its name, found by
carving only, found but damaged, lost, or stored inside an archive that is recovered: then no file of its own, and
byte for byte a stored member of a recovered Complete archive, also of an archive stored in one; or an image inside a
document that is recovered: no file of its own, and byte for byte a stream's data in a recovered Complete PDF; expected Complete,
Unverified or either; some only in a deep scan).
Together they cover the plan's 16 categories (normal, deleted, large, small, fragmented and partially overwritten
files, corrupt filesystem metadata, corrupt media, duplicates, Unicode and long names, cluster sizes, nearly full
and quick-formatted drives, bad regions, multiple partitions) on FAT32, exFAT and NTFS, MBR and GPT disks, 512 and
4096-byte sectors. `CorpusCoverageTest` fails when a category has no scenario or a baseline no scenario.

`CorpusTest/<scenario>` runs each through the API as a user interface would: a Deep and a Quick scan of the image
file, every candidate recovered to a temporary folder, and the reports exported. It checks that every original comes
back as its scenario says, that every Complete candidate is an original or a contiguous part of one (nothing false
is called complete), that each recovered file's SHA-256 is its candidate's, and that duplicates point at the first.
Then it compares one line per candidate (how it was found, condition and reasons, kind, path, sizes, validation,
SHA-256 prefix, duplicate and container links, unreadable bytes, what recovery wrote, and which original it is) with
`tests/corpus/baseline/<scenario>.txt`, whose header holds the image's SHA-256. This is the regression suite: any
change in what the engine finds, or how it judges it, fails here with the new lines printed. After a deliberate
change, run the suite with `RECOVERY_CORPUS_UPDATE=1` to rewrite the baselines and review them in the diff.

The images can be written as files, for manual testing:

```powershell
test_image_generator list                                    # the scenarios and their categories
test_image_generator manifest fat32-deleted                  # what an image holds and what must come back
test_image_generator write --output D:\corpus --originals    # every image, its manifest and its original files
```

## Comparing two builds on real data (after P20)

The corpus proves on built images that a change keeps what the engine finds. A change that can touch what a scan
finds or recovers is also checked on real data: no file the build before it recovered may be lost, or come back in a
worse condition (the user's rule of 2026-10-14). `tools/regression/compare_reports.py` compares two session reports of
one source, file by file:

```powershell
# 1. Build the commit before the change in its own tree, and the change itself
git worktree add ..\before HEAD                      # then cmake --preset msvc-release; cmake --build --preset msvc-release there
# 2. Scan the same image with both, each into its own sessions folder
..\before\build\msvc-release\tools\recovery_cli\recovery.exe scan --source D:\stick.img --sessions-dir D:\cmp\before
build\msvc-release\tools\recovery_cli\recovery.exe scan --source D:\stick.img --sessions-dir D:\cmp\after
# 3. Export both reports (the session ids are on the scans' first line)
recovery report --session <id> --sessions-dir D:\cmp\before --format json --output D:\cmp\before.json
recovery report --session <id> --sessions-dir D:\cmp\after --format json --output D:\cmp\after.json
# 4. Compare them
python -I tools\regression\compare_reports.py D:\cmp\before.json D:\cmp\after.json
```

It lists every file that is LOST (found before, not now), WORSE (in a worse condition), BETTER or NEW, then the other
differences field by field. A file is its content (SHA-256), or where it starts and its name when it has none, so a
file whose bytes changed shows as LOST and NEW. Exit code 0: nothing lost or worse; 1: review it before accepting the
change; 2: the reports cannot be read or are of different sources. `--ignore-recovery` compares two reports of one
session taken before and after a recovery job, `--all` lists everything, and `--self-test` checks the script itself
(ctest runs it as `CompareReportsScript.SelfTest` when CMake finds Python 3).

On 2026-10-14, on the test stick's image (28.6 GiB, read only): the builds before and after the hit-limit change
(L133, L228) found the same 527 files, none lost or worse; the build before it, its limit lowered to 2.7 million
signatures as a drive of about 55 GB would reach the default, missed 106 files the build after it finds (NEW); and
the scans of 2026-10-12 and 2026-10-13 differ by 647 LOST files: the ZIP change, whose files all lie in the recovered
archives (L221).

## Large drives (P20)

`LargeDriveTest` builds a 16 TiB GPT disk as a virtual source (zeros except where something is written) with a
FAT32, an exFAT and an NTFS volume at 3, 8 and 15 TiB, and an MBR disk whose partition crosses the 2 TiB boundary.
Quick scans find every volume and every file with its SHA-256 while reading less than 1 GiB. With
`RECOVERY_LARGE_IMAGE_DIR` set (and `RECOVERY_LARGE_IMAGE_GIB`, default 64), `ASparseImageFileOfTensOfGibIsScannedDeep`
also writes a sparse image file of that size there, with files at its start and end, deep-scans it through the API and
prints the time and the peak working set; otherwise it is skipped.

## Logging review (P20)

`LoggingReviewTest` runs a scan and a recovery with a memory sink at Trace level. It checks that the records the
plan asks for are there with their fields (the scan's start with the source, its type, sector size, device and known
unreadable regions; each volume; unreadable regions; the evaluation; the scan's end with every metric; each file
recovered; the recovery job's end with its bytes, speed and time), and that a marker stored in a file's content and
in a JPEG comment appears in no record: the logs never hold file content.

## Fuzzing (P20)

The `fuzz/` targets use MSVC's libFuzzer (`/fsanitize=fuzzer`) with AddressSanitizer. Preset `msvc-fuzz` builds the
engine libraries instrumented for coverage and the targets only (tests and tools cannot link the coverage
instrumentation without libFuzzer's runtime).

| Target | What the input is |
| --- | --- |
| `fuzz_partition` | The start of a disk (MBR with extended partitions, GPT and its backup, superfloppies); the first byte picks 512 or 4096-byte sectors |
| `fuzz_filesystem` | A FAT12, FAT16, FAT32, exFAT or NTFS volume: opened for recovery (backup boot records too), every candidate listed and its data reconstructed; deleted FAT32 starts that lost their top bits are placed with the formats' start check, as in a scan (L7) |
| `fuzz_formats` | A file's content for every carving format (ZIP too, since after P20, with the engine's DEFLATE decoder): header check, end detection and structural validation, never past the bytes given |
| `fuzz_media` | A file for the validation levels and every media decoder (JPEG, PNG inflate, GIF LZW, BMP RLE, VP8/VP8L, ADPCM, AAC, AVC, HEVC) |
| `fuzz_mp4` | A file for the MP4 parser and analysis: boxes, movie, sample tables, fragments, moov search, sample framing |
| `fuzz_metadata` | A file for media metadata and previews (Exif, ID3, RIFF INFO, MP4 user data, ...) |
| `fuzz_journal` | A session journal read back after a crash, or one record's payload, which must encode and decode to the same bytes |
| `fuzz_imgmeta` | An image's `.imgmeta` file: whatever parses must serialize and parse again to the same metadata |
| `fuzz_carving` | A source for one signature scan with every format and every hit carved |
| `fuzz_scan` | A disk for a whole deep scan (partitions, filesystems, MP4 recovery, fragment reconstruction, the source pass, the evaluation); the slowest |

Seeds come from the test builders and samples: `test_image_generator fuzz-seeds --output <seeds>` writes small
volumes, partition tables, every builder file and embedded sample (the ZIP samples and an archive with a PNG stored
in it too, the document samples, PDFs of every cross-reference layout and compound files of both versions), and real
session journals below
`<seeds>\<target>`. One volume is for `fuzz_filesystem` only (after P20, L7 and L8): a FAT32 of 70,000 clusters,
cut short a little past its last file (326 KiB, more than `fuzz_scan` takes), whose deleted files and deleted
folder that grew twice were deleted as Windows deletes them. From a developer prompt (the targets need AddressSanitizer's runtime DLL):

```powershell
cmake --preset msvc-fuzz
cmake --build --preset msvc-fuzz
build\msvc-debug\tools\test_image_generator\test_image_generator.exe fuzz-seeds --output D:\fuzz-seeds
powershell -File fuzz\run_fuzzers.ps1 -Seeds D:\fuzz-seeds -Work D:\fuzz-work -Seconds 1800 -Jobs 3
ctest --preset msvc-fuzz                 # the smoke runs and the saved regressions (label fuzz)
```

`run_fuzzers.ps1` runs every target for `-Seconds` (a few at once), keeps each target's growing corpus in
`<work>\corpus\<target>`, its log in `<work>\logs`, and any input that fails it in `<work>\artifacts\<target>`; it
exits 1 when a target failed. Each input that once failed a target goes, minimised, to `fuzz/regressions/<target>/`
after the fix, where `ctest` replays it (`fuzz.<target>.regressions`); `fuzz.<target>.smoke` runs each target from
nothing for 2,000 inputs (200 for `scan`). What the campaigns found is in [validation.md](validation.md).

## Static analysis (P20)

`cmake --build --preset msvc-analyze` runs MSVC's `/analyze` on the engine and the tools, warnings as errors.
`tools/static_analysis/run_clang_tidy.py` runs the clang-tidy that comes with Visual Studio over the engine and the
CLI with the `compile_commands.json` of a configured preset:

```powershell
python tools\static_analysis\run_clang_tidy.py build\msvc-debug build\clang-tidy 3              # the P20 checks
python tools\static_analysis\run_clang_tidy.py build\msvc-debug build\clang-tidy-int 3 --integer  # overflow pass
```

It writes one sorted line per finding to `<out>\findings.txt`. Every finding of P20's runs was triaged; see
[validation.md](validation.md).

## Benchmarks (P20)

`recovery_benchmark` (built with the tests, `tools/benchmark`) makes its inputs in a work folder (an image of noise, a
FAT32 card of photos, videos and music, a card of 20,000 small files), runs the engine on them as the CLI and the API
do, and prints throughput and peak memory against the targets of [performance.md](performance.md):

```powershell
build\msvc-release\tools\benchmark\recovery_benchmark.exe --work D:\bench [--only deep-noise,recover] [--keep] [--json D:\bench.json]
```

It exits 3 when a target is missed. Run it on a release build of an idle computer; `--only` one benchmark at a time
shows each one's peak memory alone (the process's peak includes building the inputs).

It counts the heap itself (its own `operator new` and `delete`), so `quick-files` also says, per file found, the heap
its candidates take once the scan is over ("kept", the sink keeping every candidate as a user interface does), the
scan's peak overall and in each stage, and the engine's peak alone: a second scan whose sink keeps nothing and that
runs without the read cache (a fixed cost, up to 64 MiB, that weighs on small scans only). `--sizes` prints the
sizes of what a scan keeps per file found (L212).

```text
quick-files     580.1 MB/s     1.41 s   816840704 bytes  40000 files (28405/s)  peak 158 MiB  target >= 2000 files/s: met
             heap kept 1335 B/file, peak 4353 B/file (volumes 2523, evaluation 4353, completed 3604, 166 MiB);
             engine alone, no read cache: peak 845 B/file (volumes 845, evaluation 702, completed 591)
```

## Continuous integration

`.github/workflows/windows-build.yml` configures, builds and tests the presets `msvc-debug`, `msvc-release`,
`msvc-asan` and `msvc-fuzz` (P20: the fuzzers' smoke runs and regressions), and builds `msvc-analyze`, on GitHub's
Windows runners. P20: a failed test becomes an annotation that names it and says why
(`.github/scripts/report_failed_tests.ps1` reads ctest's JUnit output), since job logs need admin rights to read;
the JUnit file and ctest's log are kept as an artifact for 14 days.

## Testing safely

- **No automated test reads a physical drive.** Physical-disk behaviour is tested through
  `test::MockDeviceOpener` and `test::MemoryDeviceIo`. These simulate geometry, sector alignment rules,
  I/O errors and short reads. `DiskListTest` (P19) lists the computer's disks through handles opened with desired
  access 0, which can neither read nor change anything, as the destination resolver's tests do with volumes.
- Bad sectors are simulated deterministically by `test::MemoryStorageSource`, which supports permanent
  bad sectors, sectors that fail N times before succeeding, and short reads.
- Test data comes from a fixed-seed generator (`test::makePattern`). Every byte depends on its position,
  so shifted or misplaced data is always detected.

## Manual testing against real hardware

Automated tests don't need real hardware. Only do this with media whose contents you can afford to lose:

1. Use a spare USB stick. Never use your system disk or a disk holding the only copy of important data.
2. Open an elevated prompt and find the disk number: `Get-Disk` in PowerShell.
3. Make sure the destination folder is on a **different** physical disk. The engine refuses otherwise.
4. Image the stick first and do all further work on the image (P18):
   `recovery image --source \\.\PhysicalDrive<N> --output D:\stick.img`, then `recovery scan --source D:\stick.img`,
   `recovery recover --session <id> --output D:\Recovered` and `recovery report --session <id>`. A scan of the image
   takes the regions imaging could not read into account (their zeros are not data).
