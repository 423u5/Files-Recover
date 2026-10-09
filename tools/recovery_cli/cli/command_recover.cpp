// recovery recover: writes a session's candidates to a destination folder,
// as recovery jobs of the session (P16), so that an interrupted run goes on
// where it stopped without writing a file twice.
//
// For one destination the command converges: it first finishes the jobs
// that write there and did not end, then adds one job for the selected
// candidates that no job has recovered there yet (and, with --retry-failed,
// those a job could not write). Running it again after it completed finds
// nothing to do.

#include "cli/commands.hpp"
#include "cli/format.hpp"
#include "cli/sessions.hpp"
#include "cli/sources.hpp"
#include "metadata/candidate_metadata.hpp"
#include "recovery/text.hpp"
#include "session/recovery_session.hpp"
#include "storage/bad_region.hpp"
#include "storage/destination_guard.hpp"

#include <algorithm>
#include <array>
#include <cwctype>
#include <map>
#include <ostream>
#include <set>
#include <system_error>

namespace recovery::cli {

namespace {

constexpr OptionSpec kSessionOption{"--session", "", "ID", "The session whose files to recover"};
constexpr OptionSpec kOutputOption{"--output", "", "FOLDER", "Where to write them (never on the source)"};
constexpr OptionSpec kSessionsDirOption{kSessionsDirName, "", "FOLDER",
                                        "Where sessions are kept (default %LOCALAPPDATA%\\RecoveryEngine\\Sessions)"};
constexpr OptionSpec kIdsOption{"--ids", "", "LIST", "Only these candidates: 4 or 1,4-9,12 (ids as the report shows)"};
constexpr OptionSpec kKindOption{"--kind", "", "LIST", "Only these kinds: image, audio, video, other"};
constexpr OptionSpec kConditionOption{"--condition", "", "LIST",
                                      "Only files in these conditions: complete, unverified, corrupted, partial,"
                                      " unrecoverable, ambiguous"};
constexpr OptionSpec kSkipDuplicatesOption{"--skip-duplicates", "", "",
                                           "Leave out files whose content an earlier file has"};
constexpr OptionSpec kRetryFailedOption{"--retry-failed", "", "",
                                        "Try again the files an earlier run could not write there"};
constexpr OptionSpec kWorkersOption{"--workers", "", "N", "Worker threads (default: from the processors)"};

constexpr std::array kOptions = {kSessionOption,        kOutputOption,      kSessionsDirOption, kIdsOption,
                                 kKindOption,           kConditionOption,   kSkipDuplicatesOption,
                                 kRetryFailedOption,    kWorkersOption,     kLogOption,
                                 kQuietOption,          kHelpOption};

constexpr std::array<std::string_view, 4> kKinds = {"image", "audio", "video", "other"};
constexpr std::array<std::string_view, 6> kConditions = {"complete", "unverified", "corrupted",
                                                          "partial",  "unrecoverable", "ambiguous"};

// Files listed in full before a list is cut short.
constexpr std::size_t kListed = 20;

std::string_view kindName(metadata::MediaKind kind) {
    switch (kind) {
    case metadata::MediaKind::Image:
        return "image";
    case metadata::MediaKind::Audio:
        return "audio";
    case metadata::MediaKind::Video:
        return "video";
    case metadata::MediaKind::Unknown:
        break;
    }
    return "other";
}

std::string_view conditionName(metadata::RecoveryCondition condition) {
    switch (condition) {
    case metadata::RecoveryCondition::Complete:
        return "complete";
    case metadata::RecoveryCondition::Unverified:
        return "unverified";
    case metadata::RecoveryCondition::Corrupted:
        return "corrupted";
    case metadata::RecoveryCondition::Partial:
        return "partial";
    case metadata::RecoveryCondition::Unrecoverable:
        return "unrecoverable";
    case metadata::RecoveryCondition::Ambiguous:
        return "ambiguous";
    }
    return "complete";
}

struct Filters {
    std::optional<IdSelection> ids;
    std::vector<std::string> kinds;
    std::vector<std::string> conditions;
    bool skipDuplicates = false;

    [[nodiscard]] bool any() const noexcept {
        return ids.has_value() || !kinds.empty() || !conditions.empty() || skipDuplicates;
    }

    [[nodiscard]] bool selects(const metadata::CandidateMetadata& candidate) const {
        const auto has = [](const std::vector<std::string>& list, std::string_view name) {
            return list.empty() || std::find(list.begin(), list.end(), name) != list.end();
        };
        return (!ids.has_value() || ids->contains(candidate.id.value())) && has(kinds, kindName(candidate.kind)) &&
               has(conditions, conditionName(candidate.condition)) &&
               !(skipDuplicates && candidate.duplicateOf.has_value());
    }
};

// A folder compared by its text: case folded (ASCII), without a trailing separator.
std::wstring folderKey(const std::filesystem::path& folder) {
    std::wstring text = folder.lexically_normal().native();
    while (text.size() > 3 && (text.back() == L'\\' || text.back() == L'/')) {
        text.pop_back();
    }
    for (wchar_t& c : text) {
        c = static_cast<wchar_t>(std::towlower(c));
        if (c == L'/') {
            c = L'\\';
        }
    }
    return text;
}

// Whether a job's destination (absolute, normalised UTF-8) is `folder`: the
// same folder on disk when both exist, else the same text.
bool sameFolder(std::string_view recorded, const std::filesystem::path& folder) {
    const Result<std::filesystem::path> path = pathFromUtf8(recorded);
    if (!path.ok()) {
        return false;
    }
    std::error_code ec;
    if (std::filesystem::equivalent(*path, folder, ec) && !ec) {
        return true;
    }
    return folderKey(*path) == folderKey(folder);
}

// The path of a recovered file, relative to the destination when it is below it.
std::string shownPath(const std::filesystem::path& file, const std::filesystem::path& destination) {
    const std::wstring fileText = folderKey(file);
    const std::wstring root = folderKey(destination) + L"\\";
    if (fileText.size() > root.size() && fileText.starts_with(root)) {
        return displayPath(file.lexically_normal().native().substr(root.size()));
    }
    return displayPath(file);
}

std::string lacking(const ReconstructionReport& report) {
    std::string text;
    const auto add = [&](std::uint64_t bytes, std::string_view what) {
        if (bytes != 0) {
            text += text.empty() ? "" : ", ";
            text += formatBytes(bytes) + " " + std::string(what);
        }
    };
    add(report.missingBytes, "missing");
    add(report.unreadableBytes, "unreadable");
    return text;
}

ExitCode runRecover(const ParsedOptions& options, Context& context) {
    const std::optional<std::string_view> idText = options.value(kSessionOption.name);
    const std::optional<std::string_view> outputText = options.value(kOutputOption.name);
    if (!idText.has_value() || !outputText.has_value()) {
        return usageError(context, makeError(ErrorCode::InvalidInput, "--session and --output are required"));
    }
    Result<std::string> id = parseSessionId(kSessionOption.name, *idText);
    if (!id.ok()) {
        return usageError(context, id.error());
    }
    Result<std::filesystem::path> output = pathFromUtf8(*outputText);
    if (!output.ok() || output->empty()) {
        return usageError(context, makeError(ErrorCode::InvalidInput, "--output needs a folder"));
    }
    Filters filters;
    if (const std::optional<std::string_view> text = options.value(kIdsOption.name); text.has_value()) {
        Result<IdSelection> ids = parseIds(kIdsOption.name, *text);
        if (!ids.ok()) {
            return usageError(context, ids.error());
        }
        filters.ids = std::move(ids).value();
    }
    if (const std::optional<std::string_view> text = options.value(kKindOption.name); text.has_value()) {
        Result<std::vector<std::string>> kinds = parseChoices(kKindOption.name, *text, kKinds);
        if (!kinds.ok()) {
            return usageError(context, kinds.error());
        }
        filters.kinds = std::move(kinds).value();
    }
    if (const std::optional<std::string_view> text = options.value(kConditionOption.name); text.has_value()) {
        Result<std::vector<std::string>> conditions = parseChoices(kConditionOption.name, *text, kConditions);
        if (!conditions.ok()) {
            return usageError(context, conditions.error());
        }
        filters.conditions = std::move(conditions).value();
    }
    filters.skipDuplicates = options.has(kSkipDuplicatesOption.name);
    const bool retryFailed = options.has(kRetryFailedOption.name);
    std::uint32_t workers = 0;
    if (const std::optional<std::string_view> text = options.value(kWorkersOption.name); text.has_value()) {
        Result<std::uint32_t> parsed = parseWorkers(kWorkersOption.name, *text);
        if (!parsed.ok()) {
            return usageError(context, parsed.error());
        }
        workers = *parsed;
    }

    Result<std::filesystem::path> root = sessionsRoot(options, context.environment());
    if (!root.ok()) {
        return context.fail("no sessions folder", root.error());
    }
    Result<std::unique_ptr<session::RecoverySession>> opened = openSession(*root, *id, context);
    if (!opened.ok()) {
        return context.fail("cannot recover", opened.error());
    }
    session::RecoverySession& session = **opened;
    const session::SessionInfo info = session.info();
    const std::string arguments = sessionArguments(info.id, options);
    if (info.scan.state != session::SessionState::Completed) {
        context.error("the scan of session " + printable(info.id) + " is not complete (" +
                      (info.scan.state.has_value() ? std::string(session::toString(*info.scan.state)) + " at stage " +
                                                         stageName(info.scan.stage)
                                                   : std::string("not started")) +
                      "): finish it first with recovery scan " + arguments);
        return ExitCode::Error;
    }
    const std::vector<evaluation::EvaluatedCandidate> candidates = session.candidates();
    if (filters.ids.has_value() && filters.ids->last() > candidates.size()) {
        context.error("there is no candidate " + std::to_string(filters.ids->last()) + ": session " +
                      printable(info.id) + " has " + formatCount(candidates.size()));
        return ExitCode::Error;
    }
    std::vector<metadata::CandidateMetadata> described;
    described.reserve(candidates.size());
    std::vector<evaluation::EvaluatedCandidateId> selected;
    for (const evaluation::EvaluatedCandidate& candidate : candidates) {
        described.push_back(metadata::describeCandidate(candidate));
        if (filters.selects(described.back())) {
            selected.push_back(candidate.id);
        }
    }
    if (selected.empty()) {
        context.note(candidates.empty() ? "The scan found nothing to recover."
                                        : "No candidate matches the filters: nothing to recover.");
        return ExitCode::Success;
    }

    std::error_code ec;
    std::filesystem::path destination = std::filesystem::absolute(*output, ec).lexically_normal();
    if (ec) {
        context.error("the folder '" + displayPath(*output) + "' cannot be made absolute");
        return ExitCode::Error;
    }
    if (destination.has_relative_path() && !destination.has_filename()) {
        destination = destination.parent_path();
    }
    if (const std::filesystem::file_status status = std::filesystem::status(destination, ec);
        std::filesystem::exists(status) && !std::filesystem::is_directory(status)) {
        context.error("'" + displayPath(destination) + "' exists and is not a folder");
        return ExitCode::Error;
    }
    Result<OpenedSource> source = openRecordedSource(info.source, context);
    if (!source.ok()) {
        const ExitCode code = context.fail("cannot read the session's source", source.error());
        context.note("The session's files are read from " +
                     describeSource(info.source.type, info.source.path, info.source.diskNumber, info.source.vendor,
                                    info.source.product) +
                     ": it must be attached and unchanged.");
        return code;
    }
    if (!openLogOption(options, &source->info, context)) {
        return ExitCode::Error;
    }
    // Checked before a job is recorded: a changed source would leave a job
    // that cannot run.
    if (Status same = session::checkSameSource(info.source, *source->source); !same.ok()) {
        const ExitCode code = context.fail("the source is not the session's", same.error());
        context.note("The session's files are read from " +
                     describeSource(info.source.type, info.source.path, info.source.diskNumber, info.source.vendor,
                                    info.source.product) +
                     ": it must be attached and unchanged.");
        return code;
    }
    if (Status safe = storage::checkDestinationSafety(source->info, destination, context.diskResolver());
        !safe.ok()) {
        return context.fail("cannot recover to '" + displayPath(destination) + "'", safe.error());
    }

    // What jobs did and do here.
    std::vector<std::uint32_t> unfinished;
    std::set<std::uint64_t> recoveredHere;
    std::set<std::uint64_t> failedHere;
    std::set<std::uint64_t> pendingHere;
    for (const session::RecoveryJobStatus& job : info.jobs) {
        if (!sameFolder(job.destination, destination)) {
            continue;
        }
        std::set<std::uint64_t> done;
        for (const scan::RecoveredItem& item : session.recoveredItems(job.id)) {
            done.insert(item.candidate.value());
            (item.file.has_value() ? recoveredHere : failedHere).insert(item.candidate.value());
        }
        if (job.state != session::SessionState::Completed) {
            unfinished.push_back(job.id);
            for (const evaluation::EvaluatedCandidateId candidate : job.candidates) {
                if (!done.contains(candidate.value())) {
                    pendingHere.insert(candidate.value());
                }
            }
        }
    }
    std::vector<evaluation::EvaluatedCandidateId> remaining;
    std::uint64_t failedBefore = 0;
    for (const evaluation::EvaluatedCandidateId candidate : selected) {
        const std::uint64_t value = candidate.value();
        if (recoveredHere.contains(value) || pendingHere.contains(value)) {
            continue;
        }
        if (failedHere.contains(value) && !retryFailed) {
            ++failedBefore;
            continue;
        }
        remaining.push_back(candidate);
    }

    storage::BadRegionMap knownBad;
    for (const storage::BadRegion& region : info.configuration.knownBadRegions) {
        if (Status added = knownBad.add(region); !added.ok()) {
            return context.fail("the session's known unreadable regions", added.error());
        }
    }
    const auto runJob = [&](std::uint32_t job) -> Result<scan::RecoveryJobSummary> {
        scan::RecoveryJobOptions jobOptions;
        jobOptions.workerThreads = workers;
        jobOptions.writer.reconstruction.knownBadRegions = knownBad.empty() ? nullptr : &knownBad;
        jobOptions.writer.diskResolver = context.diskResolver();
        jobOptions.writer.logger = context.logger();
        jobOptions.logger = context.logger();
        jobOptions.progressInterval = context.environment().engineProgressInterval;
        jobOptions.onProgress = [&](const scan::RecoveryJobProgress& progress) {
            ProgressEvent event;
            event.recovery = progress;
            if (context.reportProgress(event)) {
                session.cancel();
            }
            context.progress().show(recoveryLine(progress), false);
        };
        const Interrupt::Scope scope(context.interrupt(), [&session] { session.cancel(); });
        Result<scan::RecoveryJobSummary> summary = session.runRecovery(job, *source->source, std::move(jobOptions));
        context.progress().clear();
        return summary;
    };

    if (unfinished.empty() && remaining.empty()) {
        context.note("Nothing new to recover: the selected files were written to " + displayPath(destination) +
                     " before.");
    }
    bool cancelled = false;
    for (const std::uint32_t job : unfinished) {
        if (context.interrupted()) {
            cancelled = true;
            break;
        }
        context.note("Resuming recovery job " + std::to_string(job) + " (it writes to " +
                     displayPath(destination) + ")");
        Result<scan::RecoveryJobSummary> summary = runJob(job);
        if (!summary.ok() && summary.error().code != ErrorCode::Cancelled) {
            const ExitCode code = context.fail("recovery job " + std::to_string(job) + " failed", summary.error());
            context.note("What it wrote is kept. Once the cause is fixed, run the same command again.");
            return code;
        }
        if (!summary.ok() || summary->outcome == scan::RecoveryJobOutcome::Cancelled) {
            cancelled = true;
            break;
        }
    }
    if (!cancelled && !remaining.empty()) {
        if (context.interrupted()) {
            cancelled = true;
        } else {
            Result<std::uint32_t> job = session.addRecoveryJob(destination, remaining);
            if (!job.ok()) {
                return context.fail("cannot add a recovery job", job.error());
            }
            context.note("Recovering " + formatCount(remaining.size()) + (remaining.size() == 1 ? " file" : " files") +
                         " to " + displayPath(destination) + " (recovery job " + std::to_string(*job) + ")");
            Result<scan::RecoveryJobSummary> summary = runJob(*job);
            if (!summary.ok() && summary.error().code != ErrorCode::Cancelled) {
                const ExitCode code = context.fail("recovery job " + std::to_string(*job) + " failed", summary.error());
                context.note("What it wrote is kept. Once the cause is fixed, run the same command again.");
                return code;
            }
            cancelled = !summary.ok() || summary->outcome == scan::RecoveryJobOutcome::Cancelled;
        }
    }

    // The destination now: every selected candidate a job is done with there.
    std::map<std::uint64_t, scan::RecoveredItem> results;
    for (const session::RecoveryJobStatus& job : session.info().jobs) {
        if (!sameFolder(job.destination, destination)) {
            continue;
        }
        for (scan::RecoveredItem& item : session.recoveredItems(job.id)) {
            const auto [at, inserted] = results.try_emplace(item.candidate.value(), item);
            if (!inserted && !at->second.file.has_value() && item.file.has_value()) {
                at->second = std::move(item);
            }
        }
    }
    std::uint64_t written = 0;
    std::uint64_t bytes = 0;
    std::uint64_t notDone = 0;
    std::vector<const scan::RecoveredItem*> failed;
    std::vector<const scan::RecoveredItem*> incomplete;
    for (const evaluation::EvaluatedCandidateId candidate : selected) {
        const auto found = results.find(candidate.value());
        if (found == results.end()) {
            ++notDone;
            continue;
        }
        const scan::RecoveredItem& item = found->second;
        if (item.file.has_value()) {
            ++written;
            bytes += item.file->report.outputSize;
            if (!item.file->report.allBytesRead()) {
                incomplete.push_back(&item);
            }
        } else {
            failed.push_back(&item);
        }
    }
    std::ostream& out = context.out();
    out << "Destination:     " << displayPath(destination) << '\n';
    out << "Selected:        " << formatCount(selected.size()) << " of " << formatCount(candidates.size())
        << " candidates" << (filters.any() ? " (filtered)" : "") << '\n';
    out << "Recovered:       " << formatCount(written) << (written == 1 ? " file, " : " files, ") << formatSize(bytes)
        << '\n';
    const auto nameOf = [&](const scan::RecoveredItem& item) {
        const metadata::CandidateMetadata& candidate = described[item.candidate.value() - 1];
        return printable(candidate.path.empty() ? candidate.name : candidate.path);
    };
    if (!incomplete.empty()) {
        out << "Incomplete:      " << formatCount(incomplete.size())
            << " written with bytes missing or unreadable (zeros in the file, or cut short)\n";
        for (std::size_t i = 0; i < std::min(incomplete.size(), kListed); ++i) {
            const scan::RecoveredItem& item = *incomplete[i];
            out << "  " << item.candidate.value() << "  " << shownPath(item.file->path, destination) << ": "
                << lacking(item.file->report) << '\n';
        }
        if (incomplete.size() > kListed) {
            out << "  ... and " << formatCount(incomplete.size() - kListed) << " more (recovery report "
                << arguments << ")\n";
        }
    }
    if (!failed.empty()) {
        out << "Failed:          " << formatCount(failed.size()) << " could not be written"
            << (failedBefore != 0 && !retryFailed ? " (--retry-failed tries them again)" : "") << '\n';
        for (std::size_t i = 0; i < std::min(failed.size(), kListed); ++i) {
            const scan::RecoveredItem& item = *failed[i];
            out << "  " << item.candidate.value() << "  " << nameOf(item) << ": "
                << (item.error.has_value() ? printable(describe(*item.error)) : std::string("unknown error")) << '\n';
        }
        if (failed.size() > kListed) {
            out << "  ... and " << formatCount(failed.size() - kListed) << " more (recovery report " << arguments
                << ")\n";
        }
    }
    if (notDone != 0) {
        out << "Not done yet:    " << formatCount(notDone) << '\n';
    }
    out.flush();
    if (cancelled) {
        context.note("Recovery was stopped; what was written is kept. To go on, run the same command again.");
        return ExitCode::Cancelled;
    }
    if (!failed.empty() || !incomplete.empty()) {
        return ExitCode::Incomplete;
    }
    return ExitCode::Success;
}

}  // namespace

const CommandSpec& recoverCommand() {
    static const CommandSpec command{
        "recover",
        "Write a session's files to a destination folder",
        "recovery recover --session ID --output FOLDER [filters] [options]",
        "Writes the files a session's scan found to FOLDER, which must not be on the source. By default\n"
        "every candidate is written, damaged ones and duplicates too; --ids, --kind, --condition and\n"
        "--skip-duplicates narrow that down (recovery report shows ids and conditions). Names come from the\n"
        "filesystem where it knows them, directories included; carved files are named recovered_NNNNNN.\n"
        "No file is ever overwritten: a name that is taken gets \" (1)\", \" (2)\", ...\n"
        "\n"
        "The session records every file written. After Ctrl+C or a crash, run the same command again: it\n"
        "finishes the files that were under way and writes no file twice. Run again later, it only writes\n"
        "selected files that are not in FOLDER yet.",
        kOptions,
        &runRecover,
    };
    return command;
}

}  // namespace recovery::cli
