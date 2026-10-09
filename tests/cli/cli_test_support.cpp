#include "cli_test_support.hpp"

#include "recovery/text.hpp"
#include "scan/scan_test_support.hpp"
#include "session/recovery_session.hpp"
#include "support/audio_builders.hpp"
#include "support/exfat_builder.hpp"
#include "support/fat32_builder.hpp"
#include "support/image_builders.hpp"
#include "support/memory_source.hpp"
#include "support/mp4_builders.hpp"
#include "support/ntfs_builder.hpp"
#include "support/partition_builder.hpp"
#include "support/test_files.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <charconv>
#include <sstream>

namespace recovery::cli {

void PrintTo(ExitCode code, std::ostream* stream) {
    *stream << static_cast<int>(code);
}

}  // namespace recovery::cli

namespace recovery::cli::test {

std::ostream& operator<<(std::ostream& stream, const CliResult& result) {
    // The error stream holds a progress line per report: its end says what went wrong.
    constexpr std::size_t kTail = 3000;
    const std::string_view err = result.err;
    stream << "exit code " << static_cast<int>(result.code) << "\n--- out ---\n" << result.out << "--- err ---\n";
    if (err.size() > kTail) {
        stream << "[... " << err.size() - kTail << " bytes of progress ...]\n";
    }
    return stream << err.substr(err.size() > kTail ? err.size() - kTail : 0);
}

Cli::Cli(std::filesystem::path sessions) : interrupt_(std::make_unique<Interrupt>()) {
    environment_.progress = ProgressStyle::Lines;
    environment_.lineInterval = std::chrono::milliseconds{0};
    environment_.engineProgressInterval = std::chrono::milliseconds{0};
    environment_.defaultSessionsRoot = std::move(sessions);
    environment_.diskResolver = [](const std::filesystem::path&) -> Result<std::vector<std::uint32_t>> {
        return std::vector<std::uint32_t>{0};
    };
}

CliResult Cli::run(std::vector<std::string> arguments) {
    return execute(std::move(arguments), {}, false);
}

CliResult Cli::run(std::vector<std::string> arguments, std::function<void(const ProgressEvent&)> onProgress) {
    return execute(std::move(arguments), std::move(onProgress), false);
}

CliResult Cli::runInterrupted(std::vector<std::string> arguments) {
    return execute(std::move(arguments), {}, true);
}

CliResult Cli::execute(std::vector<std::string> arguments, std::function<void(const ProgressEvent&)> onProgress,
                       bool interrupted) {
    std::ostringstream out;
    std::ostringstream err;
    interrupt_ = std::make_unique<Interrupt>();
    if (interrupted) {
        (void)interrupt_->request();
    }
    environment_.out = &out;
    environment_.err = &err;
    environment_.interrupt = interrupt_.get();
    environment_.onProgress = std::move(onProgress);
    CliResult result;
    result.code = cli::run(arguments, environment_);
    environment_.out = nullptr;
    environment_.err = nullptr;
    environment_.onProgress = {};
    result.out = out.str();
    result.err = err.str();
    return result;
}

std::optional<std::string> sessionIdOf(const std::string& out) {
    constexpr std::string_view kPrefix = "Session ";
    std::istringstream lines(out);
    for (std::string line; std::getline(lines, line);) {
        if (line.starts_with(kPrefix) && line.find(' ', kPrefix.size()) == std::string::npos) {
            return line.substr(kPrefix.size());
        }
    }
    return std::nullopt;
}

std::string arg(const std::filesystem::path& path) {
    return toUtf8(path);
}

const Bytes& card() {
    static const Bytes bytes = scan::test::makeCard();
    return bytes;
}

std::filesystem::path writeImage(const std::filesystem::path& path, const Bytes& bytes) {
    ::recovery::test::writeFile(path, bytes);
    return path;
}

const std::vector<std::string>& expectedCard(ScanMode mode) {
    static const std::vector<std::string> deep = [] {
        ::recovery::test::MemoryStorageSource source(card());
        const bool opened = source.open().ok();
        return opened ? scan::test::describe(scan::test::referenceScan(source, ScanMode::Deep))
                      : std::vector<std::string>{};
    }();
    static const std::vector<std::string> quick = [] {
        ::recovery::test::MemoryStorageSource source(card());
        const bool opened = source.open().ok();
        return opened ? scan::test::describe(scan::test::referenceScan(source, ScanMode::Quick))
                      : std::vector<std::string>{};
    }();
    return mode == ScanMode::Deep ? deep : quick;
}

std::vector<std::string> sessionCandidates(const std::filesystem::path& root, const std::string& id) {
    Result<std::unique_ptr<session::RecoverySession>> opened = session::RecoverySession::open(root / id);
    EXPECT_TRUE(opened.ok()) << (opened.ok() ? std::string() : describe(opened.error()));
    if (!opened.ok()) {
        return {};
    }
    return scan::test::describe((*opened)->candidates());
}


}  // namespace recovery::cli::test
