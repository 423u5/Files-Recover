#pragma once

// What a command works with: the environment and its streams, messages to
// the user, the progress display, the engine's log (--log), and the user's
// interrupt.

#include "cli/cli.hpp"
#include "cli/progress.hpp"
#include "diagnostics/logger.hpp"
#include "recovery/error.hpp"
#include "recovery/result.hpp"
#include "storage/storage_source.hpp"

#include <filesystem>
#include <iosfwd>
#include <memory>
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

    // Reports that `what` failed with `error`, and returns ExitCode::Error.
    [[nodiscard]] ExitCode fail(std::string_view what, const Error& error);

    // --log: the engine's log records (Info and above) appended to `path`,
    // which must not be on `source` (checked as a recovery destination is;
    // none: no source to protect). Fails with the destination's error.
    [[nodiscard]] Status openLog(const std::filesystem::path& path, const storage::SourceInfo* source);
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

private:
    Environment& environment_;
    std::string command_;
    bool quiet_ = false;
    ProgressDisplay progress_;
    std::unique_ptr<diagnostics::Logger> logger_;
};

}  // namespace recovery::cli
