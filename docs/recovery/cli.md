# The command line (P18)

`recovery` is the command-line program around the engine. It is built in two parts. The library
`recovery_cli_lib` (`tools/recovery_cli/cli/`, namespace `recovery::cli`) holds every command; its entry point
is `cli::run(arguments, environment)`. The program `recovery.exe` (`tools/recovery_cli/main.cpp`,
`windows/console.cpp`) adds what only a process has: the command line in UTF-16, the console's streams and Ctrl+C.
Tests run the library in-process with their own streams, interrupt, simulated disks and disk resolver.

The CLI uses the engine's public API only: the headers in `include/` (the engine libraries export no other
include path). It holds no recovery logic of its own. It parses the command line, opens sources and sessions,
runs the engine's operations, and presents what they return.

```
recovery inspect --source SOURCE                         what a source is: size, partitions, filesystems
recovery image   --source SOURCE --output FILE [--resume] a raw image, around bad sectors (P2)
recovery scan    --source SOURCE [--mode quick|deep]     a scan in a new session (P15, P16); prints its id
recovery scan    --session ID                            the session's scan, resumed
recovery recover --session ID --output FOLDER [filters]  the files, written by recovery jobs of the session
recovery report  [--session ID] [--format text|json]     the session's report (P17), or the sessions
```

## The plan's items

| The plan's item | Where |
| --- | --- |
| inspect, image, scan, recover, report | The five commands; `recovery help <command>` shows each one's options |
| source | `--source`: a disk image file or a physical disk (`\\.\PhysicalDriveN`). A session's commands reopen the source the session records |
| image | `recovery image`; images are sources like disks. Their metadata (`FILE.imgmeta`) gives a scan and recovery the image's sector size and its unreadable regions |
| output | `--output`: the image file (`image`), the recovery folder (`recover`), the report file (`report`). Never on the source, never an existing file |
| scan mode | `scan --mode quick` (filesystem metadata) or `deep` (the default: also carving, MP4 recovery, fragments) |
| session ID | Printed by `scan` first of all (`Session <id>`); `--session ID` in `scan`, `recover` and `report`; `report` without it lists the sessions; `--sessions-dir` |
| cancellation | Ctrl+C: the command stops at its next consistent point, keeps what it did, says how to go on, exits with 3. The command then goes on from there when it is run again |
| progress | On the error stream: one line redrawn on a console, a line every few seconds into a file or pipe, none with `--quiet` |
| errors | Exit codes, and `recovery <command>: error: ...` on the error stream with the engine's error and what to do |

## Sources

`--source` takes a disk image file or a physical disk, `\\.\PhysicalDriveN` (case does not matter; N from 0 to
1023). Reading a physical disk needs administrator rights. Other device paths (`\\.\E:`, `\\?\Volume{...}`) are
refused: the engine reads whole disks and image files. A drive (`E:` or `E:\`) is refused too, and the message
names the physical disk it is on when Windows can tell.

An image's logical sector size is `--sector-size` when given, else its metadata's, else 512. When an image has
metadata (`FILE.imgmeta`, written by `recovery image`), the CLI reads it. The metadata's unreadable regions become
the scan's `knownBadRegions`, so the zeros there are not taken for data. An image the metadata says is not
complete gets a warning. Metadata that cannot be read gets a warning too, and is not used.

The source is only ever read: images through `DiskImageSource` (sharing reading only, so no program can write the
image meanwhile), disks through `PhysicalDiskSource` (read access only).

## Sessions

`scan --source` creates a session (P16) below the sessions folder: `--sessions-dir FOLDER`, or else
`%LOCALAPPDATA%\RecoveryEngine\Sessions` (the user's P18 choice). The engine refuses a folder on the source disk,
so scanning the system disk needs `--sessions-dir` on another disk. The session's id (`20261008-105223-7ac715b8`) is
the name of its folder. `scan` prints it before anything else, so that a script has it even when the scan is
interrupted.

A session belongs to its source: `scan --session` and `recover` reopen the source it records (the same image file
or disk). Before anything is recorded, they check that it is the same and unchanged (`checkSameSource`: type,
path, size, sector size, fingerprint). Settings that decide what a scan finds (`--mode`, `--no-carving`,
`--alignment`, ...) cannot be given with `--session`: a session keeps the settings its scan began with.

A command that finds its session in use (another command has it open) stops: the session's journal is open for
writing in one place at a time. `report` then shows the session's summary instead (`readSessionSummary`). Opening a
session repairs what a crash left (P16): a torn last record is dropped (`The session's last record was
incomplete`), and damage is cut off after a copy is kept (a warning names the copy).

## Commands

Every command takes `-h`/`--help`, `-q`/`--quiet` (no progress, no notes; results, warnings and errors remain) and
`--log FILE` (the engine's log records, Info and above, appended to FILE; FILE must not be on the source).

### inspect

`recovery inspect --source SOURCE [--sector-size BYTES]`

Shows the source (type, path or disk, model, size, sector sizes, removable, read-only), what an image's metadata
says (state, bytes imaged, where from, unreadable regions), the partition table (scheme, GPT disk GUID and header
validity, partitions with start, size, type, name and flags, issues), and each volume a scan would read (the
partitions of an MBR or GPT, else the whole source). For each volume it shows the filesystem: type, label,
serial number, cluster size and count, volume size and warnings, or why none can be read. Only partition tables
and boot records are read.

### image

`recovery image --source SOURCE --output FILE [--resume] [--block-size SIZE] [--retries N] [--sector-size BYTES]`

P2's `ImageWriter`: the source read once from start to end in blocks (`--block-size`, default 1M, at most 64M).
A failing read is narrowed down to 64 KiB pieces, then to sectors. A sector that still fails after `--retries`
more reads (default 2, at most 16) is zero-filled and listed in `FILE.imgmeta`. The output must not exist, unless
`--resume`. A partly written image is refused without `--resume`, which the message says. The summary lists the
unreadable regions (the first 20; all of them are in the metadata).

### scan

`recovery scan --source SOURCE [options]` or `recovery scan --session ID [--workers N] [--block-size SIZE]`

| Option | Meaning (default) |
| --- | --- |
| `--mode quick\|deep` | Quick: filesystem metadata, deleted entries included. Deep (default): also carving, MP4 recovery and fragment reconstruction |
| `--no-carving`, `--no-mp4`, `--no-fragments` | Deep scans: leave out that stage |
| `--deleted-only` | Filesystem candidates: deleted files only (active files are left out) |
| `--alignment BYTES` | Carve files that start at multiples of BYTES only (1) |
| `--max-hits N` | Stop carving after N signatures (10,000,000) |
| `--sector-retries N` | Reads of a failing sector after the first (1) |
| `--no-media` | Validate structure only, not the media data |
| `--playability` | Also decode every file with Windows' decoders (P14's playability level; slow) |
| `--sector-size BYTES` | An image's logical sector size |
| `--workers N`, `--block-size SIZE` | How the scan runs (not what it finds): worker threads (from the processors), size of its reads (1M) |
| `--sessions-dir FOLDER` | Where sessions are kept |

The formats are always every format the engine carves, and every media validator, so that a resumed scan has the
identity it began with. The summary gives the session, the source, the state and time, the bytes read and
unreadable, what each stage found, the candidates by condition and kind, duplicates, validation failures, and the
commands that go on (`recovery report`, `recovery recover`).

### recover

`recovery recover --session ID --output FOLDER [--ids LIST] [--kind LIST] [--condition LIST] [--skip-duplicates]
[--retry-failed] [--workers N]`

By default every candidate is written, damaged files and duplicates too (the user's P18 choice). The filters
narrow that down, and every one given must match:

| Option | Selects |
| --- | --- |
| `--ids LIST` | Candidate ids and ranges, as the report shows them: `4`, `1,4-9,12` |
| `--kind LIST` | `image`, `audio`, `video`, `other` (no format known) |
| `--condition LIST` | `complete`, `unverified`, `corrupted`, `partial`, `unrecoverable`, `ambiguous` (P17's conditions) |
| `--skip-duplicates` | Leaves out candidates whose content an earlier candidate has |

The scan must be complete. Files are written by P15's `RecoveryJob` as recovery jobs of the session (P16), with
P7's naming: original names and folders where the filesystem knows them, `recovered_NNNNNN.ext` for carved files,
`" (1)"`, `" (2)"`, ... for names taken, and nothing overwritten. An image's known unreadable regions are passed on
(`ReconstructionOptions::knownBadRegions`): their bytes are written as zeros and reported as unreadable.

For one output folder the command converges:

1. Jobs that write there and did not end (stopped, crashed, failed) are resumed first. A file a crash left half
   written is removed and written again under the same name (P16).
2. A new job writes the selected candidates that no job has recovered there yet. Candidates a job could not write
   there are left out unless `--retry-failed`.

Run again after it completed, it finds nothing new to do. The folder is the same however it is spelled
(`std::filesystem::equivalent` when it exists). The summary is the state of the folder for the selected
candidates: files recovered and their bytes, files written with bytes missing or unreadable (and what each lacks,
L42), files that could not be written and why.

### report

`recovery report --session ID [--format text|json] [--output FILE] [--details]`, or `recovery report
[--format text|json] [--output FILE]` to list the sessions.

The text report shows:

- the session (folder, creation, engine, journal format, damage repaired) and the source (fingerprint included);
- the scan: mode, settings, known bad regions, state with its history (each state change with its time and
  engine), updates, bytes, what each stage found, candidates;
- the volumes (partition, filesystem, label, serial, cluster size, files, why one could not be used);
- one line per candidate: id, condition, kind, format, size, method, deleted, validation, recovery state, name;
- the duplicate groups;
- every recovery job with its state, history, counts, and every file: written complete, written incomplete (with
  the bytes missing, unreadable or from reallocated clusters: L42), or failed (why);
- the errors, the unreadable source regions, and the journal's damage.

`--details` adds each candidate's condition reasons, levels, SHA-256, duplicate, times and evidence (P14's
`explain()`). `--output FILE` writes the report to a new file instead of the output stream. FILE must not exist
(nothing is overwritten) and must not be on the source.

The report is built by the `recovery_report` library (`include/report/session_report.hpp`, namespace
`recovery::report`): P19 moved it out of the CLI, with the JSON writer and the text formatting, so that the API's
`exportReport()` writes the same document ([api.md](api.md)). The CLI's output did not change.

The JSON report (the user's P18 choice) holds all of it. States, conditions and levels are named as the engine
names them (`toString`). Times are ISO 8601: UTC with `Z`, and FAT's local times without a zone (`"local": true`).

```
{
  "format": "recovery-session-report", "formatVersion": 1, "engine": "RecoveryEngine 0.1.0", "generated": "...",
  "session": {id, folder, journalFormat, engineVersion, created, updated, tornBytesDropped, recordsSkipped, damage[]},
  "source": {type, path, size, sectorSize, physicalSectorSize, diskNumber, vendor, product, removable,
             fingerprint: {sha256, bytes, unreadableBytes}},
  "scan": {configuration: {mode, carving, mp4, fragments, includeActive, includeDeleted, alignment, maxHits,
                           sectorRetryCount, knownBadRegions[], media, playability, sha256, preliminaryHash},
           state, interrupted, runnable, notRunnable, stage, updates, candidates, history[], metrics{...}},
  "partitionScheme": "...",
  "volumes": [{offset, size, partition, scanned, filesystem, label, serialNumber, clusterSize, files, error}],
  "candidates": [{id, name, path, extension, deleted, method, kind, format, mediaType, size, expectedSize,
                  sourceOffset, fragments, created, modified,
                  validation: {status, structural, media, playability, deepestPassed},
                  condition, reasons[], sha256, duplicateOf, container, unreadableBytes, warnings[],
                  recovery: {state, complete, jobs: [{job, state, path, complete, error}]}, evidence[]}],
  "duplicateGroups": [{original, members[], sha256, size}],
  "jobs": [{id, destination, created, state, interrupted, candidates, done, recovered, failed, filesInProgress,
            history[], metrics{...}, files: [{candidate, path, report: {expectedSize, outputSize, storedBytes,
            embeddedBytes, zeroBytes, missingBytes, unreadableBytes, outsideSourceBytes, reallocatedBytes,
            allBytesRead, unreadableRegions[]}, error}]}],
  "errors": [{time, context, error: {code, message, systemErrorCode}}],
  "unreadableRegions": [{offset, length, errorCode}]
}
```

A session in use gives `{"format": ..., "inUse": true, "summary": {...}}`, and the listing gives
`{"format": "recovery-session-list", ..., "folder", "sessions": [summary, ...]}`. Integers are written as they are
(64-bit). Strings are escaped, and invalid UTF-8 in them becomes U+FFFD, so a damaged disk's names always give a
valid document.

## Exit codes

| Code | Meaning |
| --- | --- |
| 0 | Done |
| 1 | The command line is wrong: an unknown command or option, a missing or malformed value, options that do not go together. Nothing was done |
| 2 | The command failed: the source, a session or a destination could not be used, or the engine reported an error. What was done is kept |
| 3 | Stopped by the user (Ctrl+C). What was done is kept; the command says how to go on |
| 4 | Done, but not everything could be read or written: `image` and `scan` met unreadable sectors (a scan of an image also when the image's metadata lists some), `recover` left selected files unwritten or wrote some with bytes missing or unreadable |

Exit code 1 is only for what the command line itself says, whatever the disks hold. `recover --ids 99` on a
session with 10 candidates is an error (2), not a usage error.

## Ctrl+C

The console handler (`windows/console.cpp`) passes the first Ctrl+C or Ctrl+Break to the command's `Interrupt`.
The command registered an action there: it cancels the scan or the recovery job through the session, or the
imaging through its cancellation token. The engine stops at its next consistent point, and the session (or the
image's metadata) keeps what was done. The command prints its summary and how to go on, and exits with 3.
A second Ctrl+C is not handled, so Windows' default handler ends the process at once. The session journal and the
image metadata are made to survive that (P2, P16).

Requests are sticky. `RecoverySession::cancel()` acts on a running operation only, so a Ctrl+C that comes while a
scan or job is starting would be lost. The command therefore checks the interrupt before each step, and its
progress callback cancels again at the operation's first report. A Ctrl+C before the scan begins leaves the session
created and not started (`Stopped before the scan began`). Closing the console window stops the command the same
way and gives it up to four seconds to record its end, before Windows ends the process.

## Progress and messages

Results go to the output stream, everything else to the error stream: progress, notes (with what to do next),
warnings and errors (`recovery scan: warning: ...`, `recovery scan: error: ...`). The output stream can be
redirected without progress mixing into it.

Progress follows the engine's reports. On a console it is one line, redrawn in place and cut to the console's
width: the stage (`Scanning 4/7 source pass`), how far it is, speed, files and carves found, unreadable bytes and
running time; `Recovering 45/120 files ...` for a job; `Imaging 45.2% ...` for an image. Into a file or a pipe it
is a line at each stage's start and at most every five seconds. Messages remove the progress line from the console
before they are printed.

Text read from a disk (names, labels, paths, tags) and the user's own arguments are printed through `printable()`.
Invalid UTF-8 becomes U+FFFD. C0 controls and DEL become `\xNN`. C1 controls, the bidirectional formatting
characters (U+061C, U+200E, U+200F, U+202A-U+202E, U+2066-U+2069) and the line and paragraph separators become
`\u{XXXX}`. A name on a damaged or hostile disk can therefore neither control the terminal (escape sequences)
nor disguise its extension (`invoice<U+202E>txt.jpg`). JSON escapes instead; the names there are exact.

## Safety

- Sources are only read (see Sources).
- Everything the CLI writes is checked as a recovery destination is (`checkDestinationSafety`): not the source
  image, not on the source disk, and the check fails closed when the disk of a folder cannot be determined. This
  covers the image file (by `ImageWriter`), the sessions folder (by `RecoverySession::create`), the recovery folder
  (by the CLI, before a job is recorded, and by `RecoveryWriter`), the report file and the log file.
- Nothing is overwritten. Images and reports are created with `CreateNew`, recovered files get free names. The log
  is the one file appended to.
- Session ids are folder names: separators, `.`, `..` and the characters Windows forbids are refused, so `--session`
  cannot reach outside the sessions folder.

## Windows assumptions

- `wmain` gets the arguments in UTF-16. They are converted to UTF-8 (`WideCharToMultiByte`); unpaired surrogates,
  which UTF-8 cannot hold, become U+FFFD (L169).
- On a console, the output and error streams are written with `WriteConsoleW` (a `std::streambuf` that collects
  UTF-8 and writes whole characters), so every character shows whatever the console's code page. Into a file or a
  pipe they are UTF-8 bytes, with the C runtime's text-mode line ends (`\r\n`).
- Console handlers run on a thread Windows creates for each event. Returning FALSE for the second Ctrl+C passes it
  to the default handler, which ends the process (exit code `STATUS_CONTROL_C_EXIT`). For `CTRL_CLOSE_EVENT`
  Windows ends the process when the handler returns, and after about five seconds anyway.
- `%LOCALAPPDATA%` (`GetEnvironmentVariableW`) gives the default sessions folder.
- `windows/console.cpp` is the only file of the CLI that includes `<windows.h>`.

## Threads

| Object | Rule |
| --- | --- |
| `cli::run` | One command on the calling thread. Its engine operations call progress back on that thread (the scan and job runs' own thread). |
| `Interrupt` | Every member from any thread. The action runs on the requesting thread (the console handler's), under the object's lock; `Scope`'s destructor waits for an action under way. |
| `ConsoleSession` | One, in `wmain`, for the program's lifetime. The `Interrupt` it serves is static, so a console event during the program's end still finds it. |

## Tests

`tests/cli` (executable `recovery_cli_tests`, label `cli`) runs the CLI in-process through `cli::run`. Its streams
are captured, and every run gets a fresh interrupt. The sources are the scan tests' card written as an image file,
an MBR disk with FAT32, exFAT and NTFS volumes, and simulated physical disks (`MockDeviceOpener`, with failing
byte ranges). A disk resolver says where folders are. No real device is opened. Besides, `CliProcessTest` runs
`recovery.exe` itself.

- `cli_arguments_test.cpp`: the parser (values, flags, short options, `=`, what it refuses), numbers, sizes,
  choices, id lists, session ids, help wrapping; and every usage error through `run()`, none of which creates
  anything.
- `cli_output_test.cpp`: `printable()` (controls, bidirectional characters, invalid UTF-8), sizes, counts, times,
  durations, the quoting of suggested commands, tables, the JSON writer against a strict reader; the interrupt
  (first and later requests, sticky requests, nested scopes, a scope waiting for an action under way).
- `cli_commands_test.cpp`: every command. inspect on the card, three volumes, data without a filesystem and a disk.
  image, a byte-for-byte copy, then a disk with bad sectors whose zeros and metadata a later scan takes into
  account. Deep and quick scans deliver the reference scan's candidates; settings kept in the session; another
  sessions folder. End to end, `recover` gives back the files the card was made from, byte for byte, and the
  carved GIF, MP3 and MP4; then nothing on a second run. Filters, one folder converging, each folder its own copy.
  Text and JSON reports, the JSON checked against the engine's own descriptions; listings; a session in use. A
  recovered file with an unreadable sector says so in `recover`'s summary, the text report and the JSON (L42).
  The log file.
- `cli_errors_test.cpp`: sources that do not exist, are folders or drives, cannot be opened (access denied, no such
  disk) or are written by another program. Outputs that exist, are the source, or are on the source disk. Sessions
  folders that are files, missing or on the source disk. Sessions that do not exist, are not sessions (left
  untouched) or are in use. Recovery to a file or to the source disk, ids the session does not have. A source gone
  or changed (no job recorded). Reports never overwrite. Names that would control the terminal.
- `cli_cancellation_test.cpp`: a scan stopped in each of its seven stages, then resumed to the reference's
  candidates; Ctrl+C before a scan begins; recovery refused for an unfinished scan; a recovery job stopped after
  every file, then resumed to the same files under the same names; Ctrl+C before recovering (no job recorded);
  imaging stopped and resumed with `--resume` to the same bytes; Ctrl+C before imaging and inspecting.
- `cli_resume_test.cpp`: the session journal cut after every record and in the middle of every update, as a
  crash leaves it, then `scan --session`. Also: a scan complete but not recorded so; a crash shown as
  `interrupted` by the report; a recovery job killed while writing its first, fourth and tenth file, the files
  on disk as the crash left them (the last one half written), resumed to the same files. A complete scan is not
  run again (its journal unchanged). A changed or missing source is refused without recording anything, and the
  scan goes on once the source is back.
- `cli_process_test.cpp`: `recovery.exe` as a process. Exit codes. Cyrillic, Japanese and emoji paths through
  inspect, scan and recover, with UTF-8 to a pipe. The quoting of suggested commands checked by
  `CommandLineToArgvW`. A real Ctrl+Break (`GenerateConsoleCtrlEvent` to a process group of its own) stops a scan
  with exit code 3 and the scan resumes. A test process without a console makes a hidden one for that test.

CTest also runs the program directly: `cli.version`, `cli.help`, `cli.no_arguments_fails`,
`cli.unknown_command_fails`, `cli.scan_without_source_fails`.

## Known limitations

L168-L180 in [../limitations.md](../limitations.md). L42 and L167 changed with P18.
