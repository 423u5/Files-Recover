// recovery scan: a Quick or Deep scan of a source in a new session (P16), or
// the scan of an existing session resumed from its last update. The session
// keeps every update as the scan goes, so Ctrl+C or a crash loses at most the
// work since the last one.

#include "cli/commands.hpp"
#include "cli/format.hpp"
#include "cli/sessions.hpp"
#include "cli/sources.hpp"
#include "formats/audio_formats.hpp"
#include "formats/image_formats.hpp"
#include "formats/video_formats.hpp"
#include "metadata/candidate_metadata.hpp"
#include "recovery/config.hpp"
#include "session/recovery_session.hpp"
#include "validation/media_validator.hpp"
#include "validation/windows_playability.hpp"

#include <array>
#include <map>
#include <ostream>

namespace recovery::cli {

namespace {

constexpr OptionSpec kSourceOption{"--source", "", "SOURCE",
                                   "Scan this disk image file or physical disk in a new session"};
constexpr OptionSpec kSessionOption{"--session", "", "ID", "Resume the scan of this session"};
constexpr OptionSpec kSessionsDirOption{kSessionsDirName, "", "FOLDER",
                                        "Where sessions are kept (default %LOCALAPPDATA%\\RecoveryEngine\\Sessions)"};
constexpr OptionSpec kModeOption{"--mode", "", "MODE",
                                 "quick (filesystem metadata) or deep (also carving and fragments; default)"};
constexpr OptionSpec kNoCarvingOption{"--no-carving", "", "", "Deep scan: do not carve files by their content"};
constexpr OptionSpec kNoMp4Option{"--no-mp4", "", "", "Deep scan: no MP4 recovery from metadata and carving"};
constexpr OptionSpec kNoFragmentsOption{"--no-fragments", "", "", "Deep scan: no reconstruction of fragmented files"};
constexpr OptionSpec kDeletedOnlyOption{"--deleted-only", "", "", "Only deleted files from filesystem metadata"};
constexpr OptionSpec kAlignmentOption{"--alignment", "", "BYTES",
                                      "Carve files that start at multiples of BYTES only (default 1)"};
constexpr OptionSpec kMaxHitsOption{"--max-hits", "", "N", "Stop carving after N signatures (default 10000000)"};
constexpr OptionSpec kSectorRetriesOption{"--sector-retries", "", "N",
                                          "Reads of a failing sector after the first (default 1)"};
constexpr OptionSpec kNoMediaOption{"--no-media", "", "", "Check structure only, not the media data"};
constexpr OptionSpec kPlayabilityOption{"--playability", "", "",
                                        "Also decode every file with Windows' decoders (slow)"};
constexpr OptionSpec kSectorSizeOption{"--sector-size", "", "BYTES",
                                       "An image's logical sector size (default: its metadata's, else 512)"};
constexpr OptionSpec kWorkersOption{"--workers", "", "N", "Worker threads (default: from the processors)"};
constexpr OptionSpec kBlockSizeOption{"--block-size", "", "SIZE", "Size of the scan's reads (default 1M)"};

constexpr std::array kOptions = {kSourceOption,      kSessionOption,      kSessionsDirOption, kModeOption,
                                 kNoCarvingOption,   kNoMp4Option,        kNoFragmentsOption, kDeletedOnlyOption,
                                 kAlignmentOption,   kMaxHitsOption,      kSectorRetriesOption, kNoMediaOption,
                                 kPlayabilityOption, kSectorSizeOption,   kWorkersOption,     kBlockSizeOption,
                                 kLogOption,         kQuietOption,        kHelpOption};

// The options that decide what a scan finds: a session keeps them.
constexpr std::array kConfigurationOptions = {kModeOption,      kNoCarvingOption,     kNoMp4Option,
                                              kNoFragmentsOption, kDeletedOnlyOption, kAlignmentOption,
                                              kMaxHitsOption,   kSectorRetriesOption, kNoMediaOption,
                                              kPlayabilityOption, kSectorSizeOption};

constexpr std::array<std::string_view, 2> kModes = {"quick", "deep"};

Result<scan::ScanConfiguration> configurationOf(const ParsedOptions& options) {
    scan::ScanConfiguration configuration;
    if (const std::optional<std::string_view> text = options.value(kModeOption.name); text.has_value()) {
        Result<std::string> mode = parseChoice(kModeOption.name, *text, kModes);
        if (!mode.ok()) {
            return mode.error();
        }
        configuration.mode = *parseScanMode(*mode);
    }
    const bool quick = configuration.mode == ScanMode::Quick;
    for (const OptionSpec& deepOnly : {kNoCarvingOption, kNoMp4Option, kNoFragmentsOption, kAlignmentOption,
                                       kMaxHitsOption}) {
        if (quick && options.has(deepOnly.name)) {
            return makeError(ErrorCode::InvalidInput,
                             std::string(deepOnly.name) + " is for deep scans: a quick scan does not carve");
        }
    }
    configuration.carving = !options.has(kNoCarvingOption.name);
    configuration.mp4 = !options.has(kNoMp4Option.name);
    configuration.fragments = !options.has(kNoFragmentsOption.name);
    configuration.includeActive = !options.has(kDeletedOnlyOption.name);
    configuration.media = !options.has(kNoMediaOption.name);
    if (const std::optional<std::string_view> text = options.value(kAlignmentOption.name); text.has_value()) {
        Result<std::uint64_t> alignment = parseUnsigned(kAlignmentOption.name, *text, 1, 1u << 20);
        if (!alignment.ok()) {
            return alignment.error();
        }
        configuration.alignment = static_cast<std::uint32_t>(*alignment);
    }
    if (const std::optional<std::string_view> text = options.value(kMaxHitsOption.name); text.has_value()) {
        Result<std::uint64_t> hits = parseUnsigned(kMaxHitsOption.name, *text, 1, 1'000'000'000'000ull);
        if (!hits.ok()) {
            return hits.error();
        }
        configuration.maxHits = *hits;
    }
    if (const std::optional<std::string_view> text = options.value(kSectorRetriesOption.name); text.has_value()) {
        Result<std::uint64_t> retries = parseUnsigned(kSectorRetriesOption.name, *text, 0, 16);
        if (!retries.ok()) {
            return retries.error();
        }
        configuration.sectorRetryCount = static_cast<std::uint32_t>(*retries);
    }
    return configuration;
}

Result<scan::ScanRunOptions> runOptionsOf(const ParsedOptions& options) {
    scan::ScanRunOptions run;
    if (const std::optional<std::string_view> text = options.value(kWorkersOption.name); text.has_value()) {
        Result<std::uint32_t> workers = parseWorkers(kWorkersOption.name, *text);
        if (!workers.ok()) {
            return workers.error();
        }
        run.workerThreads = *workers;
    }
    if (const std::optional<std::string_view> text = options.value(kBlockSizeOption.name); text.has_value()) {
        Result<std::uint64_t> size = parseSize(kBlockSizeOption.name, *text, 4096, storage::kMaxReadSize);
        if (!size.ok()) {
            return size.error();
        }
        run.blockSize = static_cast<std::size_t>(*size);
    }
    return run;
}

// Every format the engine carves, and every media validator: the same on
// every run, so that a resumed scan has the identity it began with.
struct Registries {
    carving::FormatRegistry formats;
    validation::MediaValidatorRegistry media;
};

Result<Registries> makeRegistries() {
    Registries registries;
    for (Status registered : {formats::registerImageFormats(registries.formats),
                              formats::registerAudioFormats(registries.formats),
                              formats::registerVideoFormats(registries.formats),
                              validation::registerMediaValidators(registries.media)}) {
        if (!registered.ok()) {
            return registered.error();
        }
    }
    return registries;
}

void printScanSummary(std::ostream& out, const session::RecoverySession& session, bool showNext,
                      const ParsedOptions& options) {
    const session::SessionInfo info = session.info();
    const scan::ScanMetrics& metrics = info.scan.metrics;
    out << "Session:         " << printable(info.id) << '\n';
    out << "Folder:          " << displayPath(info.folder) << '\n';
    out << "Source:          "
        << describeSource(info.source.type, info.source.path, info.source.diskNumber, info.source.vendor,
                          info.source.product)
        << ", " << formatSize(info.source.size) << '\n';
    out << "Scan:            " << toString(info.configuration.mode) << ", ";
    if (!info.scan.state.has_value()) {
        out << "not started\n";
    } else if (*info.scan.state == session::SessionState::Completed) {
        out << "completed in " << formatDuration(metrics.elapsed) << '\n';
    } else {
        out << session::toString(*info.scan.state) << " at stage " << stageName(info.scan.stage) << " after "
            << formatDuration(metrics.elapsed) << '\n';
    }
    out << "Read:            " << formatSize(metrics.bytesRead) << " read, " << formatSize(metrics.unreadableBytes)
        << " unreadable\n";
    out << "Found:           " << formatCount(metrics.filesFound) << " files in filesystem metadata";
    if (info.configuration.mode == ScanMode::Deep) {
        out << ", " << formatCount(metrics.carves) << " carves, " << formatCount(metrics.mp4Candidates)
            << " MP4 candidates, " << formatCount(metrics.fragmentCandidates) << " reconstructions";
    }
    out << '\n';

    const std::vector<evaluation::EvaluatedCandidate> candidates = session.candidates();
    std::map<metadata::RecoveryCondition, std::uint64_t> conditions;
    std::map<metadata::MediaKind, std::uint64_t> kinds;
    std::uint64_t duplicates = 0;
    for (const evaluation::EvaluatedCandidate& candidate : candidates) {
        const metadata::CandidateMetadata described = metadata::describeCandidate(candidate);
        ++conditions[described.condition];
        ++kinds[described.kind];
        duplicates += described.duplicateOf.has_value() ? 1 : 0;
    }
    out << "Candidates:      " << formatCount(candidates.size());
    std::string separator = ": ";
    for (const auto& [condition, count] : conditions) {
        out << separator << formatCount(count) << ' ' << metadata::toString(condition);
        separator = ", ";
    }
    out << '\n';
    if (!kinds.empty()) {
        out << "Kinds:           ";
        separator.clear();
        for (const auto& [kind, count] : kinds) {
            out << separator << formatCount(count) << ' ' << metadata::toString(kind);
            separator = ", ";
        }
        out << '\n';
    }
    out << "Duplicates:      " << formatCount(duplicates) << " (the same content as an earlier candidate)\n";
    if (metrics.validationFailures != 0) {
        out << "Invalid:         " << formatCount(metrics.validationFailures) << " failed validation\n";
    }
    if (showNext) {
        const std::string arguments = sessionArguments(info.id, options);
        out << "Next:            recovery report " << arguments << '\n';
        out << "                 recovery recover " << arguments << " --output FOLDER\n";
    }
}

ExitCode runScan(const ParsedOptions& options, Context& context) {
    const bool resume = options.has(kSessionOption.name);
    if (resume == options.has(kSourceOption.name)) {
        return usageError(context, makeError(ErrorCode::InvalidInput,
                                             "give --source for a new scan, or --session to resume one"));
    }
    if (resume) {
        for (const OptionSpec& option : kConfigurationOptions) {
            if (options.has(option.name)) {
                return usageError(context, makeError(ErrorCode::InvalidInput,
                                                     std::string(option.name) +
                                                         " cannot be used with --session: a session keeps the "
                                                         "settings its scan began with"));
            }
        }
    }
    Result<scan::ScanRunOptions> runOptions = runOptionsOf(options);
    if (!runOptions.ok()) {
        return usageError(context, runOptions.error());
    }
    Result<scan::ScanConfiguration> configuration = resume ? scan::ScanConfiguration{} : configurationOf(options);
    if (!configuration.ok()) {
        return usageError(context, configuration.error());
    }
    std::optional<SourceSpec> spec;
    std::optional<std::uint32_t> sectorSize;
    std::string id;
    if (resume) {
        Result<std::string> parsed = parseSessionId(kSessionOption.name, *options.value(kSessionOption.name));
        if (!parsed.ok()) {
            return usageError(context, parsed.error());
        }
        id = std::move(parsed).value();
    } else {
        Result<SourceSpec> parsed = parseSource(kSourceOption.name, *options.value(kSourceOption.name));
        if (!parsed.ok()) {
            return usageError(context, parsed.error());
        }
        spec = std::move(parsed).value();
        if (const std::optional<std::string_view> text = options.value(kSectorSizeOption.name); text.has_value()) {
            Result<std::uint32_t> size = parseSectorSize(kSectorSizeOption.name, *text);
            if (!size.ok()) {
                return usageError(context, size.error());
            }
            sectorSize = *size;
        }
    }
    Result<std::filesystem::path> root = sessionsRoot(options, context.environment());
    if (!root.ok()) {
        return context.fail("no sessions folder", root.error());
    }
    Result<Registries> registries = makeRegistries();
    if (!registries.ok()) {
        return context.fail("cannot register the formats", registries.error());
    }

    std::unique_ptr<session::RecoverySession> session;
    std::optional<OpenedSource> source;
    if (resume) {
        Result<std::unique_ptr<session::RecoverySession>> opened = openSession(*root, id, context);
        if (!opened.ok()) {
            return context.fail("cannot resume the scan", opened.error());
        }
        session = std::move(opened).value();
        const session::SessionInfo info = session->info();
        if (info.scan.state == session::SessionState::Completed) {
            context.note("The scan of session " + printable(id) + " is complete; there is nothing to resume.");
            printScanSummary(context.out(), *session, true, options);
            return ExitCode::Success;
        }
        if (!info.scan.runnable) {
            context.error("the scan of session " + printable(id) + " cannot be resumed: " +
                          printable(info.scan.notRunnable));
            return ExitCode::Error;
        }
        Result<OpenedSource> reopened = openRecordedSource(info.source, context);
        if (!reopened.ok()) {
            const ExitCode code = context.fail("cannot read the session's source", reopened.error());
            context.note("The session scans " +
                         describeSource(info.source.type, info.source.path, info.source.diskNumber,
                                        info.source.vendor, info.source.product) +
                         ": it must be attached and unchanged.");
            return code;
        }
        source = std::move(reopened).value();
        if (!openLogOption(options, &source->info, context)) {
            return ExitCode::Error;
        }
        if (Status same = session::checkSameSource(info.source, *source->source); !same.ok()) {
            const ExitCode code = context.fail("the source is not the session's", same.error());
            context.note("The session scans " +
                         describeSource(info.source.type, info.source.path, info.source.diskNumber,
                                        info.source.vendor, info.source.product) +
                         ": it must be attached and unchanged.");
            return code;
        }
        configuration = info.configuration;
        context.note("Resuming the " + std::string(toString(info.configuration.mode)) + " scan of session " +
                     printable(id) + " (" +
                     (info.scan.state.has_value() ? std::string(session::toString(*info.scan.state)) + " at stage " +
                                                        stageName(info.scan.stage)
                                                  : std::string("not started")) +
                     ")");
    } else {
        Result<OpenedSource> opened = openSource(*spec, sectorSize, context);
        if (!opened.ok()) {
            return context.fail("cannot read the source", opened.error());
        }
        source = std::move(opened).value();
        if (!openLogOption(options, &source->info, context)) {
            return ExitCode::Error;
        }
        configuration->knownBadRegions = knownBadRegions(*source);
        if (!configuration->knownBadRegions.empty()) {
            std::uint64_t bytes = 0;
            for (const storage::BadRegion& region : configuration->knownBadRegions) {
                bytes += region.length;
            }
            context.note("The image's metadata lists " + formatCount(configuration->knownBadRegions.size()) +
                         " unreadable regions (" + formatSize(bytes) + "); the scan treats them as unreadable.");
        }
        session::SessionOptions sessionOptions;
        sessionOptions.logger = context.logger();
        sessionOptions.diskResolver = context.diskResolver();
        scan::ScanConfiguration created = *configuration;
        validation::WindowsPlayabilityChecker marker;
        // create() only records whether the scan validates playability; the
        // checker is given to runScan().
        created.playability = options.has(kPlayabilityOption.name) ? &marker : nullptr;
        Result<std::unique_ptr<session::RecoverySession>> made =
            session::RecoverySession::create(*root, *source->source, std::move(created), sessionOptions);
        if (!made.ok()) {
            return context.fail("cannot create a session in '" + displayPath(*root) + "'", made.error());
        }
        session = std::move(made).value();
        context.out() << "Session " << printable(session->id()) << '\n';
        context.out().flush();
        context.note("Scanning " + describeSource(source->info) + " (" + std::string(toString(configuration->mode)) +
                     " scan); the session is kept in " + displayPath(session->folder()));
    }

    const std::string arguments = sessionArguments(session->id(), options);
    if (context.interrupted()) {
        context.note("Stopped before the scan began. To scan: recovery scan " + arguments);
        return ExitCode::Cancelled;
    }
    validation::WindowsPlayabilityChecker checker;
    const bool playability = session->info().playability;
    const ScanMode mode = configuration->mode;
    scan::ScanRunOptions run = std::move(runOptions).value();
    run.logger = context.logger();
    run.progressInterval = context.environment().engineProgressInterval;
    std::optional<scan::ScanStage> shownStage;
    session::RecoverySession& live = *session;
    run.onProgress = [&](const scan::ScanProgress& progress) {
        ProgressEvent event;
        event.scan = progress;
        // cancel() acts on a running scan only: a request that came while the
        // scan was starting is passed on here, at its first report.
        if (context.reportProgress(event)) {
            live.cancel();
        }
        const bool force = !shownStage.has_value() || *shownStage != progress.stage;
        shownStage = progress.stage;
        context.progress().show(scanLine(progress, mode), force);
    };
    Result<scan::ScanSummary> summary = makeError(ErrorCode::InternalError, "the scan did not run");
    {
        const Interrupt::Scope scope(context.interrupt(), [&live] { live.cancel(); });
        summary = session->runScan(*source->source, registries->formats, registries->media, std::move(run),
                                   playability ? &checker : nullptr);
    }
    context.progress().clear();
    const bool cancelled = summary.ok() ? summary->outcome == scan::ScanOutcome::Cancelled
                                        : summary.error().code == ErrorCode::Cancelled;
    if (!summary.ok() && !cancelled) {
        const ExitCode code = context.fail("the scan failed", summary.error());
        context.note("What the scan did is kept in the session. Once the cause is fixed: recovery scan " +
                     arguments);
        return code;
    }
    printScanSummary(context.out(), *session, !cancelled, options);
    context.out().flush();
    if (cancelled) {
        context.note("The scan was stopped; the session keeps what it did. To go on: recovery scan " + arguments);
        return ExitCode::Cancelled;
    }
    if (summary->metrics.unreadableBytes != 0) {
        context.warning(formatBytes(summary->metrics.unreadableBytes) +
                        " of the source could not be read (see recovery report " + arguments + ")");
        return ExitCode::Incomplete;
    }
    // An image's gaps: the scan does not read them (nor count them), but the
    // files there lack those bytes all the same.
    std::uint64_t knownBad = 0;
    for (const storage::BadRegion& region : configuration->knownBadRegions) {
        knownBad += region.length;
    }
    if (knownBad != 0) {
        context.warning(formatBytes(knownBad) + " of the image's source could not be read when it was imaged: the "
                                                "image holds zeros there");
        return ExitCode::Incomplete;
    }
    return ExitCode::Success;
}

}  // namespace

const CommandSpec& scanCommand() {
    static const CommandSpec command{
        "scan",
        "Scan a source for recoverable files, in a session",
        "recovery scan --source SOURCE [--mode quick|deep] [options]\n"
        "       recovery scan --session ID [options]",
        "Scans a source for files to recover and keeps what it finds in a new session (--source), whose id\n"
        "it prints. A quick scan reads filesystem metadata (FAT32, exFAT, NTFS), deleted entries included; a\n"
        "deep scan (the default) also carves files by their content, recovers MP4 files and reconstructs\n"
        "fragmented ones. Every file found is validated and identified by SHA-256.\n"
        "\n"
        "The session records the scan as it goes. After Ctrl+C or a crash, --session ID resumes the scan\n"
        "where it stopped, with the settings it began with. The source is only read.",
        kOptions,
        &runScan,
    };
    return command;
}

}  // namespace recovery::cli
