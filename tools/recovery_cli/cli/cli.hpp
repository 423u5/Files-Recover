#pragma once

// The recovery command-line interface (P18): inspect, image, scan, recover
// and report, built on the engine's public API only (the headers in
// include/; the engine's private headers are not on this library's include
// path).
//
//   recovery inspect --source SOURCE
//   recovery image   --source SOURCE --output FILE [--resume]
//   recovery scan    --source SOURCE [--mode quick|deep]      (a new session)
//   recovery scan    --session ID                             (resumes its scan)
//   recovery recover --session ID --output FOLDER [filters]
//   recovery report  [--session ID] [--format text|json] [--output FILE]
//
// A SOURCE is a disk image file or a physical disk (\\.\PhysicalDriveN). The
// source is only ever read. Sessions (P16) keep a scan and its recovery jobs
// on disk, below %LOCALAPPDATA%\RecoveryEngine\Sessions unless
// --sessions-dir names another folder; a command that was interrupted
// (Ctrl+C, a crash) goes on from where it stopped when it is run again.
//
// run() is the whole program except its platform glue (main.cpp: wide
// arguments, the console, Ctrl+C), so tests run it in-process with their
// own streams, interrupts, simulated disks and disk resolver.

#include "cli/interrupt.hpp"
#include "imaging/image_writer.hpp"
#include "scan/recovery_job.hpp"
#include "scan/scan_coordinator.hpp"
#include "storage/destination_guard.hpp"
#include "storage/device_io.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace recovery::cli {

// The process exit codes.
enum class ExitCode : int {
    // Done.
    Success = 0,
    // The command line is wrong: an unknown command or option, a missing or
    // malformed value, options that do not go together. Nothing was done.
    Usage = 1,
    // The command failed: the source, a session or a destination could not
    // be used, or the engine reported an error. What was done is kept.
    Error = 2,
    // Stopped by the user (Ctrl+C). What was done is kept, and the command
    // says how to go on.
    Cancelled = 3,
    // Done, but not everything could be read or written: unreadable source
    // sectors (image, scan), files that could not be written or were
    // written with bytes missing or unreadable (recover).
    Incomplete = 4,
};

enum class ProgressStyle : std::uint8_t {
    // One line on the error stream, redrawn in place (a console).
    Console,
    // A line now and then (a file or a pipe).
    Lines,
    // None.
    None,
};

// A progress report of a running command, as the engine gave it.
struct ProgressEvent {
    std::optional<imaging::ImagingProgress> imaging;
    std::optional<scan::ScanProgress> scan;
    std::optional<scan::RecoveryJobProgress> recovery;
};

struct Environment {
    // Results go to `out`; progress, messages and errors to `err`. Text is UTF-8.
    std::ostream* out = nullptr;
    std::ostream* err = nullptr;
    ProgressStyle progress = ProgressStyle::Lines;
    // Lines: at most one line per interval (and one at each stage's start).
    std::chrono::milliseconds lineInterval{5000};
    // Console: the width of the console's lines (0: unknown).
    std::size_t consoleWidth = 0;
    // How often the engine reports progress while a command runs.
    std::chrono::milliseconds engineProgressInterval{250};
    // Ctrl+C. Null: commands cannot be interrupted.
    Interrupt* interrupt = nullptr;
    // The sessions folder without --sessions-dir (%LOCALAPPDATA%\RecoveryEngine\Sessions).
    std::optional<std::filesystem::path> defaultSessionsRoot;
    // Opens physical disks (null: the platform's, CreateFileW read-only).
    // Tests simulate disks with it; image files always use the platform's.
    std::shared_ptr<storage::IDeviceOpener> diskOpener;
    // Tells which physical disks a destination is on (empty: the platform's).
    storage::DiskResolver diskResolver;
    // Told every progress report before it is shown (tests interrupt a
    // command at a chosen point from here). On the command's thread.
    std::function<void(const ProgressEvent&)> onProgress;
};

// Runs the command line `arguments` (without the program's name) and returns
// the exit code. Never throws for expected failures.
[[nodiscard]] ExitCode run(std::span<const std::string> arguments, Environment& environment);

// The general usage text (recovery --help).
[[nodiscard]] std::string usageText();

}  // namespace recovery::cli
