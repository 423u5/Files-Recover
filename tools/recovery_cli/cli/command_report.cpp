// recovery report: a session's report, as text or JSON, on the standard
// output or in a new file; without --session, the sessions there are.

#include "cli/commands.hpp"
#include "cli/format.hpp"
#include "cli/sessions.hpp"
#include "cli/sources.hpp"
#include "report/session_report.hpp"
#include "storage/destination_file.hpp"
#include "storage/destination_guard.hpp"

#include <array>
#include <ostream>
#include <span>

namespace recovery::cli {

namespace {

constexpr OptionSpec kSessionOption{"--session", "", "ID", "The session to report (none: list the sessions)"};
constexpr OptionSpec kSessionsDirOption{kSessionsDirName, "", "FOLDER",
                                        "Where sessions are kept (default %LOCALAPPDATA%\\RecoveryEngine\\Sessions)"};
constexpr OptionSpec kFormatOption{"--format", "", "FORMAT", "text (default) or json"};
constexpr OptionSpec kOutputOption{"--output", "", "FILE",
                                   "Write the report to a new FILE (never on the source) instead of the output"};
constexpr OptionSpec kDetailsOption{"--details", "", "", "Text: every candidate's evidence and checks too"};

constexpr std::array kOptions = {kSessionOption, kSessionsDirOption, kFormatOption, kOutputOption, kDetailsOption,
                                 kLogOption,     kQuietOption,       kHelpOption};

constexpr std::array<std::string_view, 2> kFormats = {"text", "json"};

// ERROR_SHARING_VIOLATION: another command has the session open.
constexpr std::uint32_t kSharingViolation = 32;

// Writes `text` to the output stream, or to the new file `path` (checked
// against `source` when there is one).
ExitCode deliver(const std::string& text, const std::optional<std::filesystem::path>& path,
                 const storage::SourceInfo* source, Context& context) {
    if (!path.has_value()) {
        context.out() << text;
        context.out().flush();
        return ExitCode::Success;
    }
    if (source != nullptr) {
        if (Status safe = storage::checkDestinationSafety(*source, *path, context.diskResolver()); !safe.ok()) {
            return context.fail("cannot write the report to '" + displayPath(*path) + "'", safe.error());
        }
    }
    Result<storage::DestinationFile> file =
        storage::DestinationFile::open(*path, storage::DestinationFile::OpenMode::CreateNew);
    if (!file.ok()) {
        return context.fail("cannot create the report '" + displayPath(*path) +
                                "' (an existing file is never overwritten)",
                            file.error());
    }
    const auto bytes = std::as_bytes(std::span(text.data(), text.size()));
    Status written = file->writeAt(0, bytes);
    if (written.ok()) {
        written = file->flush();
    }
    if (!written.ok()) {
        return context.fail("cannot write the report '" + displayPath(*path) + "'", written.error());
    }
    context.note("Report written to " + displayPath(*path));
    return ExitCode::Success;
}

ExitCode runReport(const ParsedOptions& options, Context& context) {
    bool json = false;
    if (const std::optional<std::string_view> text = options.value(kFormatOption.name); text.has_value()) {
        Result<std::string> format = parseChoice(kFormatOption.name, *text, kFormats);
        if (!format.ok()) {
            return usageError(context, format.error());
        }
        json = *format == "json";
    }
    std::optional<std::filesystem::path> output;
    if (const std::optional<std::string_view> text = options.value(kOutputOption.name); text.has_value()) {
        Result<std::filesystem::path> path = pathFromUtf8(*text);
        if (!path.ok() || path->empty()) {
            return usageError(context, makeError(ErrorCode::InvalidInput, "--output needs a file name"));
        }
        output = std::move(path).value();
    }
    const bool details = options.has(kDetailsOption.name);
    if (details && json) {
        return usageError(context, makeError(ErrorCode::InvalidInput,
                                             "--details is for text reports: a JSON report holds everything"));
    }
    std::optional<std::string> id;
    if (const std::optional<std::string_view> text = options.value(kSessionOption.name); text.has_value()) {
        Result<std::string> parsed = parseSessionId(kSessionOption.name, *text);
        if (!parsed.ok()) {
            return usageError(context, parsed.error());
        }
        id = std::move(parsed).value();
    } else if (details) {
        return usageError(context, makeError(ErrorCode::InvalidInput, "--details needs --session"));
    }
    Result<std::filesystem::path> root = sessionsRoot(options, context.environment());
    if (!root.ok()) {
        return context.fail("no sessions folder", root.error());
    }

    if (!id.has_value()) {
        if (!openLogOption(options, nullptr, context)) {
            return ExitCode::Error;
        }
        Result<std::vector<session::SessionSummary>> sessions = session::listSessions(*root);
        if (!sessions.ok()) {
            return context.fail("cannot list the sessions in '" + displayPath(*root) + "'", sessions.error());
        }
        return deliver(json ? report::jsonSessionList(*root, *sessions) : report::textSessionList(*root, *sessions),
                       output, nullptr, context);
    }

    Result<std::unique_ptr<session::RecoverySession>> opened = openSession(*root, *id, context);
    if (!opened.ok()) {
        const Error& error = opened.error();
        if (error.code != ErrorCode::DestinationError || error.systemErrorCode != kSharingViolation) {
            return context.fail("cannot report", error);
        }
        // Another command runs the session: its summary can still be read.
        Result<std::filesystem::path> name = pathFromUtf8(*id);
        Result<session::SessionSummary> summary =
            name.ok() ? session::readSessionSummary(*root / *name) : Result<session::SessionSummary>(name.error());
        if (!summary.ok()) {
            return context.fail("cannot report", summary.error());
        }
        context.warning("session " + printable(*id) +
                        " is in use by another recovery command: only its summary can be read now");
        const storage::SourceInfo source = sourceInfoOf(summary->sourceType, summary->sourcePath);
        if (!openLogOption(options, &source, context)) {
            return ExitCode::Error;
        }
        return deliver(json ? report::jsonSummary(*summary) : report::textSummary(*summary), output, &source,
                       context);
    }
    const storage::SourceInfo source = sourceInfoOf((*opened)->info().source);
    if (!openLogOption(options, &source, context)) {
        return ExitCode::Error;
    }
    const report::SessionReport gathered = report::gatherReport(**opened);
    opened->reset();
    return deliver(json ? report::jsonReport(gathered) : report::textReport(gathered, details), output, &source,
                   context);
}

}  // namespace

const CommandSpec& reportCommand() {
    static const CommandSpec command{
        "report",
        "Show a session's report (text or JSON), or list the sessions",
        "recovery report --session ID [--format text|json] [--output FILE] [--details]\n"
        "       recovery report [--format text|json]",
        "Reports what a session holds: its source, its scan (state, history, metrics, settings), the\n"
        "volumes and filesystems found, every candidate (condition, validation, duplicates, whether it was\n"
        "recovered), its recovery jobs with every file written (and the bytes it lacks) or why not, errors\n"
        "and unreadable regions. JSON holds all of it, and each candidate's evidence. Without --session,\n"
        "lists the sessions in the sessions folder.\n"
        "\n"
        "A session another command is running can only be summarised. Opening a session repairs what a\n"
        "crash left in its journal, as every command does.",
        kOptions,
        &runReport,
    };
    return command;
}

}  // namespace recovery::cli
