# Open limitations, ranked

The open items of [limitations.md](limitations.md) as of 2026-10-17, ranked by what each one costs a person trying to
get files back. The project's first rule is that no file that could come back is lost, so items that lose files or
recover them wrong come first. [limitations.md](limitations.md) stays the reference for each item's details and
status: when an item is solved or decided there, take it off this list.

Of the register's 260 items, the 89 below are open: 84 Open, 4 Partly resolved (L46, L212, L213, L249) and 1
Mitigated (L197). The 115 Accepted and 56 Resolved items are not listed (L7, L8 and L13 were resolved on 2026-10-15;
on 2026-10-16 L245, a defect found and fixed, L12, L19 and L21, by the images Windows wrote, L147, L172 and L177,
which left L246, accepted by the user's choice, and L247, and L10, L242, L243 and L244, which left L248, L249 and
L250; on 2026-10-17 L252, a defect found on the user's stick and fixed, which showed L253, L249 partly, which showed
L254 to L258, and L251 was found; then L253, L256, L257 and L258 were resolved, which left L259 and L260).

Sizes compare the items with each other: **small** is a contained change, **medium** a new part of an existing
component with its tests, **large** a new component (a file format, a filesystem structure) with reference samples.
The fixes are proposals, to be confirmed when the work starts.

Every fix is proven the same way: new tests that fail without it; the corpus baselines match, or each difference is
reviewed; the test stick image is scanned before and after the change and the two reports are compared file by file
with `tools/regression/compare_reports.py` (nothing lost or worse); the register's Status line is updated.

## 1. Files lost or recovered wrong today

| ID | What goes wrong | Possible fix | Size |
| --- | --- | --- | --- |
| L23 | **NTFS.** Attribute lists are not followed. A heavily fragmented file (typically a large video on a full disk) comes back short, a file whose data lies wholly in extension records has none, and a fragmented `$MFT` is read only as far as its first record describes: the files after it are not listed. | Read `$ATTRIBUTE_LIST` and the extension records it names, for files and for the `$MFT` itself. | medium to large |
| L36, L24 | **NTFS.** Compressed files (LZNT1) are not recovered at all: every byte is missing and the writer refuses them. Encrypted (EFS) data cannot be decrypted without the user's key. | Decompress LZNT1, one compression unit at a time. EFS stays out. | medium |

## 2. File types carving does not know

These are recovered only while a filesystem entry still names them, and then unverified (L128). After a format, or
once their entries are overwritten, they are lost.

| ID | Types and who has them | Possible approach | Size |
| --- | --- | --- | --- |
| L226 | TIFF and camera raw (Canon CR2 and CR3, Nikon NEF, Sony ARW, Adobe DNG, Olympus ORF, Panasonic RW2 and others), AVIF, JPEG 2000. Photographers' cards. The JPEG previews inside raw files may still be carved as separate JPEGs (not tried). | Most raw formats are TIFF inside: a file ends at the furthest strip, tile or sub-image its directories (IFDs) point to. CR3 is built like MP4 (P11's parser). | large |
| L227 | AVCHD (MTS, M2TS) from camcorders, AVI from older cameras and dashcams, Matroska and WebM, MPEG program streams (MPG, VOB), Windows Media (WMV, ASF), Flash video (FLV). | MPEG transport streams are runs of 188- or 192-byte packets; AVI has RIFF sizes like WAV; Matroska has EBML sizes. | large (MTS and AVI alone: medium) |
| L79 | FLAC, Ogg (Vorbis, Opus), WMA, AMR. Voice notes (WhatsApp keeps them as Ogg Opus), voice recorders (AMR), music. AMR inside 3GP or M4A is already carved as M4A. | Ogg pages and FLAC frames carry checksums: strong validation. | medium to large |
| L75 | WAV recordings that were never finished (the RIFF size left at 0 or 0xFFFFFFFF after a power loss or a pulled card: the header is refused), RF64 and BW64 (over 4 GiB, field recorders), big-endian RIFX. Unfinished recordings are typical recovery cases. | Carve an unfinished WAV as far as its audio goes (bounded); read RF64's 64-bit sizes. | medium |

## 3. Recovered, but less than it could be

| ID | What is lost | Possible fix | Size |
| --- | --- | --- | --- |
| L260 | A deleted file damaged in place (its entry intact, its end found, its structure invalid) is delivered only as its reconstruction's layouts, which may hold its first clusters only: the entry's own layout and the carve at its start are not delivered beside them, as a carve seed's carve is (L253). | Deliver the entry's candidate with the carve too when the layouts share fewer of the carve's bytes than it has. | small |
| L220 | A `CORRUPTED` archive is written whole: its intact members are not delivered as files of their own (only the stored images, audio and video that carving finds inside it). | Deliver the members whose checksum passes. A design question: the engine does not extract archives today. | medium |
| L219 | When an archive's start is overwritten or lost, a carve from a later member ends before the central directory (`PARTIAL`); repair tools could read its members from that directory. | Carve on to the central directory even though it places the start earlier (still `PARTIAL`). | medium |
| L233 | A PDF stream whose `/Length` is far off ends the carve at that object: the rest of the document is left out (`PARTIAL`). | Search for `endstream` as readers do, within a bound, instead of stopping. | small to medium |
| L231 | A PDF with bytes before its `%PDF-` header (readers accept up to 1,024) is `CORRUPTED` when named, and a carve leaves those bytes out. | Accept up to 1,024 bytes before the header. | small |
| L234 | An incremental PDF update whose cross-reference chain is damaged is left out: the carve is the earlier revision. | Take an update whose objects parse, even when its chain is damaged, without gluing on another file's leftovers. | medium |
| L237 | Compound files (`.doc`, `.xls`, `.ppt`) in pieces are not put back together: a deleted fragmented `.doc` on FAT comes back as its clusters from the first one on (`CORRUPTED`). Any layout that keeps the header, FAT and directory in place validates. | Check the streams' own structures (Word's FIB and piece table, a workbook's records) so that only the right layout validates. | large |
| L238 | A carved compound file ends at its last sector in use: it opens, but it is shorter than the original and its SHA-256 differs. | Decision needed: carving on to the end of what the FAT covers could take another file's data. | small |
| L60 | A JPEG ends at its first end marker: a second image appended after it (Multi-Picture Format previews, depth maps) is carved as a separate JPEG, and a vendor trailer that is not a JPEG is lost. | Read the MPF index in APP2 to carve the whole file. | medium |
| L229 | Carving skips every hit inside a verified archive, also in the archive's own records (names, comments, an APK signing block), which are not files of it. None was seen in practice. | Skip only inside the members' data. | small to medium |
| L248 | A deleted FAT32 file whose start lost its top 16 bits stays where its start records when the places it may start at all look alike: several start a file of its type and none or several end at its size, or several free places hold data that may start it (L242's remainder). | Check the whole file at each place left, as candidate evaluation does, or offer each place as a candidate. | medium |
| L249 | Partly resolved: a deep scan finds a formatted volume's folders by their first clusters (on the user's stick 4,069 files got their names back). Left: a part of a folder found apart from it (a cluster after its first) is looked for only right after data the scan listed; a folder that no folder found holds keeps no name; an entry whose size disagrees with what was found names nothing. Of two deleted folders that grew next to each other, one may list the other's entries too (duplicates, nothing lost). | Search every free cluster for folder slots, as for first clusters; use the size Windows records for a deleted folder. | medium |
| L254 | The folders of a formatted exFAT volume (cards of 64 GB and more) are not searched for in its free space: their files come back carved, without names. | Look at every free cluster for entry sets whose checksums and name hashes hold (as L244 does), in the same read. | medium |
| L250 | A deleted exFAT directory linked by a FAT chain whose first cluster was reused cannot be followed (it is read as one run instead); an orphan whose first free clusters hold no entries stays a file (L244's remainder). | Look for its clusters by their entries, as L243 does on FAT32. | medium |

## 4. What the user sees (no file is lost)

| ID | What it is | Possible fix | Size |
| --- | --- | --- | --- |
| L42 | A recovered file with missing or unreadable bytes carries no mark; only the CLI's output and the session's report say so. | Write the list of incomplete files into the output folder, or mark their names. | small |
| L176 | Bidirectional control characters stay in recovered file names: `invoice<U+202E>txt.jpg` shows as `invoicegpj.txt` in Explorer, a disguised extension. | Replace them in `safeFileName`. | small |
| L171 | An image's known unreadable regions (from its `.imgmeta`) are not counted in the scan's metrics: the scan reports `0 B unreadable`. | Count them. | small |
| L232 | A PDF with damage that readers repair (offsets elsewhere, `/Length` a few bytes off, a missing `endobj`) is `CORRUPTED`, though it opens. | Accept what readers repair, noting it as an issue. | medium |
| L230 | A PDF with a stream the engine cannot check (JPEG 2000, JBIG2, CCITT fax, LZW, run-length), binary data without a filter, or encryption is `UNVERIFIED` and never trusted. Scanned and password-protected documents often are. | Check LZW and run-length (small); the others need decoders (large). | small to large |
| L93 | The same situation (a chunk beyond the end of a complete file) is `Invalid` for M4A and `Truncated` for MP4. | Decide on one reading. | small |
| L198 | A carve inside another file's overlong allocation is linked to that file as its container: the report's "inside" evidence is wrong, though the carve is recovered whole. | Link only to what the file's data covers. | small |
| L239 | Naming by content guesses for some files: templates are named as documents (`.dotx` as `docx`), OpenXPS as `xps`, encrypted Office files `.ole`, JARs and ZIPs by their first member. | Refine the rules where content tells more. | small |
| L180 | `--log` always writes Info and above, and a scan always uses every format (they are part of its identity). | Add a log level option. | small |
| L259 | A file whose content fits several formats, none validating it, is judged as the first: a damaged video whose name says no format is `audio/m4a` in a quick scan (its bytes and its CORRUPTED condition are the same as MP4; a deep scan reconstructs it as MP4). | The format whose structure got furthest, or for MPEG-4 the kind its brand and tracks give. | small |
| L251 | `--log FILE` appends to a file that exists and is refused only on the source image itself: given the image's `.imgmeta`, it appends log lines to it, the metadata is no longer read, and a later scan takes the zeros of the unreadable regions for data. | Refuse the image's metadata as a log too. | small |

## 5. Checking against real Windows

These find bugs rather than fix them.

| ID | What is not checked | Possible approach | Size |
| --- | --- | --- | --- |
| L34 | Only ntfs-3g wrote the NTFS test images: no NTFS volume formatted and filled by Windows is in the reference set. | An NTFS part for `tests/reference/make_windows_fat_reference.ps1`, which makes the FAT32 and exFAT ones (L21). | small |
| L211 | Performance was measured on one desktop with images on an SSD: no real USB device, and CI does not run the benchmark. | Measure on the test stick and a card reader. | small |
| L204 | Fuzzing ran for fixed times; inputs of the carving and scan fuzzers are capped at 256 KiB. | Longer runs, larger seeds. | small |

## 6. Large drives: memory and speed

Only large drives (hundreds of thousands of files, hundreds of GB) feel these. Speed work waits until it is asked
for: recovery comes before speed (the decision of 2026-10-14).

| ID | What it is |
| --- | --- |
| L212 | About 2 KB of memory per file found while a session is open: about 2 GB for a million files. Nothing streams the candidates to disk. |
| L29 | NTFS: the catalog of the whole MFT is built before the first listing and kept (up to 16.7 million records). |
| L40 | A volume's candidates are all kept in memory with their regions and resident data. |
| L99 | MP4 candidates are kept until the end of the run. |
| L115 | Fragment reconstruction keeps every file's claims, the carve seeds and the moov anchors for the whole run. |
| L139 | A checkpoint holds every stage's results; carves, MP4 and fragment candidates are copied into each checkpoint. |
| L144 | A session's journal is never compacted: opening a session replays all of it. |
| L145 | An open session holds its whole checkpoint and every candidate in memory. |
| L223 | A deep scan reads its source about eight times (252.5 GB for the 28.6 GiB stick). On hold by the rule above. |
| L255 | A deep scan reads a FAT volume's free space once more to find the folders nothing leads to (L249): the user's formatted stick took 12:30 instead of 8:26. The source pass could find them. |
| L49 | A carved file's bytes are read at least twice (seeks on hard disks). |
| L97 | MP4 recovery reads each file two to four times. |
| L121 | Validation and hashing read every candidate's data twice. |
| L41 | Around bad sectors, reading goes sector by sector with retries, with no intermediate block size: slow on failing media. Imaging the source first avoids it. |
| L52 | That sector-by-sector fallback is written twice (a tidy-up). |
| L50 | An image crafted with many valid headers can cost hits times the maximum size in reads; there is no budget per scan in bytes or time. |
| L61 | A JPEG header followed by zeros is read to the maximum size (256 MiB) for each such hit. |
| L63 | A GIF header followed by erased flash (0xFF) is read to the maximum size. |

## 7. For a GUI, and design questions

The GUI is set aside for now; these matter when one is built.

| ID | What it is |
| --- | --- |
| L182 | A scan delivers its candidates only in its last stage: nothing can be listed or recovered before it, also when a failing drive dies earlier. |
| L185 | No recovery while a scan runs. |
| L181 | Recovered files can be left out of events once 100,000 wait; the user interface must read their state again. |
| L183 | The API keeps an index of every candidate and a record per recovery job (the register's text predates the L212 work, which cut the index to 4 bytes per candidate). |
| L184 | No sorting or text search in `getCandidates`. |
| L186 | `listSessions` cannot tell an interrupted session from a running one. |
| L247 | A session that recorded no serial number (journal format 1 or 2, or a device that reports none) tells its disk by vendor and product: a copy of its card in a reader of the same model, attached alone, is taken for the session's disk. |
| L187 | Imaging has no pause and no list. |
| L188 | Previews are read one at a time per session, each reopening the content. |
| L190 | The disk list asks Windows only: some virtual disks are missing, and folders a volume is mounted on are not mapped. |
| L191 | Progress is an estimate from fixed shares per stage; there is no time-remaining estimate. |
| L192 | A slow log callback slows the engine, and a callback that waits for the thread destroying the API deadlocks it. |
| L193 | The API opens only sessions whose ids are letters, digits, `-` and `_`. |
| L194 | The test hooks header lives next to the GUI-facing headers. |
| L195 | The CLI and the API each open sources and plan recovery (the code exists twice). |
| L196 | A recovery whose thread cannot start leaves its job recorded. |
| L207 | Volume folders are decided when a job runs, so jobs during and after a scan can differ. |
| L56 | Carving progress shows the scan only, not the files carved. |
| L154 | Pictures inside unsynchronised or compressed ID3 tags are not offered as previews. |
| L155 | Previews are the Exif JPEG thumbnail and the image itself only. |
| L156 | HE-AAC shows as AAC LC unless its configuration says otherwise. |
| L157 | Video profiles are read for AVC and HEVC only. |
| L158 | Macintosh language codes are not translated. |
| L160 | Durations leave out encoder delay, padding and edit lists. |
| L161 | Time zones are what files say; local times have no UTC instant. |
| L162 | Rotation is a quarter turn of the track header; Exif orientation only for images. |
| L163 | Duplicate groups are as complete as the page of candidates given. |
| L164 | Recovery status is a snapshot of the session; recovered files are not checked again. |
| L165 | Text is cleaned and cut at 1,024 bytes; text in other code pages comes out wrong. |
| L166 | A damaged file still offers its content as a preview. |
| L209 | Out of memory is an exception, not an error result: some paths end the process. |
| L235 | Limits on what one PDF or compound file may hold (they bound memory and time). |
| L236 | Compound files are never trusted, so the pictures inside a `.doc` are carved as extra copies (a design decision). |

## Decided, nothing to do

- L46: carving extracts contiguous files; fragments in free space are reconstructed (P13), and the rest was accepted
  on 2026-10-14.
- L197: the heap test under load is mitigated, and was accepted as it is on 2026-10-14.
- L213: archives, PDFs and compound files are carved now; text and source files are not, by the choice of
  2026-10-12 (ZIP carving without text carving).
- Accepted items that could be reopened: HEIC and HEIF photos (L224, accepted for this release).

## Suggested order

1. L23 and L36: NTFS.
2. File types: L226 (raw photos) first, then MTS and AVI from L227, then L79 and L75.
