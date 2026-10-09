#pragma once

// Shared by the CLI tests (P18): the CLI run in-process with its streams
// captured, its own sessions folder and interrupt; the scan tests' card as an
// image file and the files it was made from; simulated physical disks (no
// real device is ever opened); and a strict JSON reader for the reports.

#include "cli/cli.hpp"
#include "cli/interrupt.hpp"
#include "evaluation/evaluated_candidate.hpp"
#include "support/card_fixtures.hpp"
#include "support/fake_device.hpp"
#include "support/json_reader.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace recovery::cli {

// GoogleTest prints exit codes as numbers.
void PrintTo(ExitCode code, std::ostream* stream);

}  // namespace recovery::cli

namespace recovery::cli::test {

using Bytes = std::vector<std::byte>;

struct CliResult {
    ExitCode code = ExitCode::Error;
    std::string out;
    std::string err;
};

// For assertion messages: the exit code and both streams.
std::ostream& operator<<(std::ostream& stream, const CliResult& result);

// The CLI with captured streams. Every run gets a fresh interrupt; the
// environment (sessions folder, progress, disk opener and resolver, hook)
// carries over from run to run.
class Cli {
public:
    // Sessions are kept below `sessions` by default. Progress: a line for
    // every report of the engine (none is throttled). The disk resolver says
    // every destination is on disk 0 (simulated disks are numbered from 7).
    explicit Cli(std::filesystem::path sessions);

    [[nodiscard]] Environment& environment() noexcept { return environment_; }
    // The interrupt of the run under way (or the last one).
    [[nodiscard]] Interrupt& interrupt() noexcept { return *interrupt_; }

    CliResult run(std::vector<std::string> arguments);
    // Runs with `onProgress` told every progress report (to interrupt the
    // command at a chosen point, through interrupt()).
    CliResult run(std::vector<std::string> arguments, std::function<void(const ProgressEvent&)> onProgress);
    // Runs with Ctrl+C pressed before the command begins.
    CliResult runInterrupted(std::vector<std::string> arguments);

private:
    CliResult execute(std::vector<std::string> arguments, std::function<void(const ProgressEvent&)> onProgress,
                      bool interrupted);

    Environment environment_;
    std::unique_ptr<Interrupt> interrupt_;
};

// The id a scan printed ("Session <id>").
[[nodiscard]] std::optional<std::string> sessionIdOf(const std::string& out);

// A UTF-8 path as the CLI takes it.
[[nodiscard]] std::string arg(const std::filesystem::path& path);

// Shared with the API tests (support/card_fixtures.hpp).
using ::recovery::test::cardOriginals;
using ::recovery::test::filesBelow;
using ::recovery::test::SimulatedDisk;
using ::recovery::test::simulatedDisk;
using ::recovery::test::threeVolumeDisk;

// The scan tests' FAT32 card, and the card written as an image file.
[[nodiscard]] const Bytes& card();
std::filesystem::path writeImage(const std::filesystem::path& path, const Bytes& bytes);

// What a deep (or quick) scan of the card delivers, one line per candidate
// (scan::test::describe).
[[nodiscard]] const std::vector<std::string>& expectedCard(ScanMode mode = ScanMode::Deep);


// The candidates of the session `id` below `root`, one line each (opens the
// session: no command may have it open).
[[nodiscard]] std::vector<std::string> sessionCandidates(const std::filesystem::path& root, const std::string& id);




namespace json = ::recovery::test::json;

}  // namespace recovery::cli::test
