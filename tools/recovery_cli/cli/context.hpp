#pragma once

// What a command works with: the environment and its streams, messages to
// the user, the progress display, the engine's log (--log), and the user's
// interrupt.

#include "cli/cli.hpp"
#include "cli/progress.hpp"
#include "diagnostics/logger.hpp"
#include "recovery/error.hpp"
#include "recovery/result.hpp"
#include "storage/disk_list.hpp"
#include "storage/storage_source.hpp"

#include <filesystem>
#include <iosfwd>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace recovery::cli {

class Context {
public:
    Context(Environment& environment, std::string command);
    ~Context();
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    Context(Context&&) = delete;
    Context& operator=(Context&&) = delete;

    [[nodiscard]] Environment& environment() noexcept { return environment_; }
    [[nodiscard]] std::ostream& out() noexcept { return *environment_.out; }
    [[nodiscard]] std::ostream& err() noexcept { return *environment_.err; }
    // "scan"
    [[nodiscard]] const std::string& command() const noexcept { return command_; }

    // --quiet: no progress, no notes (results, warnings and errors remain).
    void setQuiet(bool quiet);
    [[nodiscard]] bool quiet() const noexcept { return quiet_; }
    [[nodiscard]] ProgressDisplay& progress() noexcept { return progress_; }

    // "recovery scan: error: ...", "recovery scan: warning: ..." on the error
    // stream, and notes (lines of their own; not with --quiet). Each removes
    // a progress line from the console first.
    void error(std::string_view message);
    void warning(std::string_view message);
    void note(std::string_view message);

    // Reports that `what` failed with `error`, and returns ExitCode::Error. A
    // Windows error code comes with Windows' own words for it.
    [[nodiscard]] ExitCode fail(std::string_view what, const Error& error);

    // The folder that will hold the file `path` (a log, an image, a report),
    // created when it does not exist yet, as `recover` creates its
    // destination. Checked first as a recovery destination is, against
    // `source` (none: no source to protect), so that nothing is ever created
    // on the source. Fails with DestinationError.
    [[nodiscard]] Status prepareFolderFor(const std::filesystem::path& path, const storage::SourceInfo* source);
    // The same against every one of `sources` (a session's disk is one of
    // them, when it is not opened to tell which, L147).
    [[nodiscard]] Status prepareFolderFor(const std::filesystem::path& path,
                                          std::span<const storage::SourceInfo> sources);

    // --log: the engine's log records (Info and above) appended to `path`,
    // which must not be on any of `sources` (checked as a recovery
    // destination is; none: no source to protect); its folder is created
    // when missing. Fails with the destination's error.
    [[nodiscard]] Status openLog(const std::filesystem::path& path, std::span<const storage::SourceInfo> sources);
    // The log, or null without --log.
    [[nodiscard]] diagnostics::Logger* logger() noexcept { return logger_.get(); }

    [[nodiscard]] Interrupt* interrupt() noexcept { return environment_.interrupt; }
    // Ctrl+C was pressed.
    [[nodiscard]] bool interrupted() const noexcept;
    // An engine progress report: given to the environment's hook, then shown
    // by the caller. Returns interrupted() (checked after the hook).
    [[nodiscard]] bool reportProgress(const ProgressEvent& event);

    // The disk resolver for destination checks (the environment's, else the
    // platform's).
    [[nodiscard]] storage::DiskResolver diskResolver() const;
    // The disk lister (the environment's, else the platform's).
    [[nodiscard]] storage::DiskLister diskLister() const;

private:
    Environment& environment_;
    std::string command_;
    bool quiet_ = false;
    ProgressDisplay progress_;
    std::unique_ptr<diagnostics::Logger> logger_;
};

}  // namespace recovery::cli
