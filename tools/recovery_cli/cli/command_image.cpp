// recovery image: a raw image of a source (P2's ImageWriter), read
// sequentially; unreadable sectors are narrowed down, zero-filled and listed
// in the image's metadata (<image>.imgmeta); an interrupted image goes on
// with --resume.

#include "cli/commands.hpp"
#include "cli/format.hpp"
#include "cli/sources.hpp"
#include "imaging/image_metadata.hpp"
#include "imaging/image_writer.hpp"
#include "recovery/cancellation.hpp"
#include "storage/storage_source.hpp"

#include <algorithm>
#include <array>
#include <ostream>
#include <system_error>

namespace recovery::cli {

namespace {

constexpr OptionSpec kSourceOption{"--source", "", "SOURCE",
                                   "The disk to image (\\\\.\\PhysicalDriveN), or an image file to copy"};
constexpr OptionSpec kOutputOption{"--output", "", "FILE", "The image file to write (never on the source)"};
constexpr OptionSpec kResumeOption{"--resume", "", "", "Go on with an image an earlier run did not finish"};
constexpr OptionSpec kBlockSizeOption{"--block-size", "", "SIZE", "Size of each read (default 1M; up to 64M)"};
constexpr OptionSpec kRetriesOption{"--retries", "", "N", "Reads of a failing sector after the first (default 2)"};
constexpr OptionSpec kSectorSizeOption{"--sector-size", "", "BYTES",
                                       "An image source's logical sector size (default 512)"};

constexpr std::array kOptions = {kSourceOption,  kOutputOption,     kResumeOption, kBlockSizeOption,
                                 kRetriesOption, kSectorSizeOption, kLogOption,    kQuietOption,
                                 kHelpOption};

// Regions listed in full before the list is cut short.
constexpr std::size_t kRegionsShown = 20;

void printSummary(std::ostream& out, const imaging::ImagingSummary& summary, const OpenedSource& source) {
    out << "Image:           " << displayPath(summary.imagePath) << '\n';
    out << "Metadata:        " << displayPath(summary.metadataPath) << '\n';
    out << "Source:          " << describeSource(source.info) << '\n';
    out << "State:           "
        << (summary.outcome == imaging::ImagingOutcome::Completed ? "completed" : "stopped (resumable)") << '\n';
    out << "Imaged:          " << formatBytes(summary.bytesCompleted) << " of " << formatBytes(summary.totalBytes);
    if (summary.resumedFrom != 0) {
        out << " (resumed at " << formatCount(summary.resumedFrom) << ")";
    }
    out << '\n';
    out << "Unreadable:      " << formatBytes(summary.unreadableBytes) << " in "
        << formatCount(summary.badRegions.size()) << (summary.badRegions.size() == 1 ? " region" : " regions")
        << " (zero-filled in the image)\n";
    if (summary.badRegions.empty()) {
        return;
    }
    Table regions({{"Offset", true}, {"Length", true}, {"Error", true}});
    const std::size_t shown = std::min(summary.badRegions.size(), kRegionsShown);
    for (std::size_t i = 0; i < shown; ++i) {
        const storage::BadRegion& region = summary.badRegions[i];
        regions.addRow({formatCount(region.offset), formatCount(region.length), std::to_string(region.errorCode)});
    }
    regions.print(out);
    if (shown < summary.badRegions.size()) {
        out << "  ... and " << formatCount(summary.badRegions.size() - shown)
            << " more (all of them are in the metadata)\n";
    }
}

// Explains why an existing output cannot be used, before the writer refuses it.
std::optional<std::string> existingOutputProblem(const std::filesystem::path& output, bool resume) {
    std::error_code ec;
    const bool exists = std::filesystem::exists(output, ec);
    if (resume) {
        if (!exists) {
            return "there is no image '" + displayPath(output) + "' to resume: leave out --resume to start one";
        }
        return std::nullopt;
    }
    if (!exists) {
        return std::nullopt;
    }
    const Result<imaging::ImageMetadata> metadata = imaging::readImageMetadata(imaging::metadataPathFor(output));
    if (metadata.ok() && metadata->state != imaging::ImageState::Completed) {
        return "the image '" + displayPath(output) + "' exists and is not complete (" +
               std::string(imaging::toString(metadata->state)) + "): add --resume to go on with it";
    }
    return "the file '" + displayPath(output) + "' exists: the CLI never overwrites a file, choose another name";
}

ExitCode runImage(const ParsedOptions& options, Context& context) {
    const std::optional<std::string_view> sourceText = options.value(kSourceOption.name);
    const std::optional<std::string_view> outputText = options.value(kOutputOption.name);
    if (!sourceText.has_value() || !outputText.has_value()) {
        return usageError(context, makeError(ErrorCode::InvalidInput, "--source and --output are required"));
    }
    Result<SourceSpec> spec = parseSource(kSourceOption.name, *sourceText);
    if (!spec.ok()) {
        return usageError(context, spec.error());
    }
    Result<std::filesystem::path> output = pathFromUtf8(*outputText);
    if (!output.ok() || output->empty()) {
        return usageError(context, makeError(ErrorCode::InvalidInput, "--output needs a file name"));
    }
    imaging::ImagingOptions imaging;
    if (const std::optional<std::string_view> text = options.value(kBlockSizeOption.name); text.has_value()) {
        Result<std::uint64_t> size = parseSize(kBlockSizeOption.name, *text, 4096, storage::kMaxReadSize);
        if (!size.ok()) {
            return usageError(context, size.error());
        }
        imaging.blockSize = static_cast<std::size_t>(*size);
    }
    if (const std::optional<std::string_view> text = options.value(kRetriesOption.name); text.has_value()) {
        Result<std::uint64_t> retries =
            parseUnsigned(kRetriesOption.name, *text, 0, imaging::ImagingOptions::kMaxSectorRetries);
        if (!retries.ok()) {
            return usageError(context, retries.error());
        }
        imaging.sectorRetryCount = static_cast<std::uint32_t>(*retries);
    }
    std::optional<std::uint32_t> sectorSize;
    if (const std::optional<std::string_view> text = options.value(kSectorSizeOption.name); text.has_value()) {
        Result<std::uint32_t> parsed = parseSectorSize(kSectorSizeOption.name, *text);
        if (!parsed.ok()) {
            return usageError(context, parsed.error());
        }
        sectorSize = *parsed;
    }
    imaging.resume = options.has(kResumeOption.name);

    Result<OpenedSource> opened = openSource(*spec, sectorSize, context);
    if (!opened.ok()) {
        return context.fail("cannot read the source", opened.error());
    }
    if (!openLogOption(options, &opened->info, context)) {
        return ExitCode::Error;
    }
    if (const std::optional<std::string> problem = existingOutputProblem(*output, imaging.resume);
        problem.has_value()) {
        context.error(*problem);
        return ExitCode::Error;
    }

    CancellationSource cancellation;
    imaging.cancellation = cancellation.token();
    imaging.logger = context.logger();
    imaging.diskResolver = context.diskResolver();
    imaging.progressInterval = context.environment().engineProgressInterval;
    imaging.onProgress = [&](const imaging::ImagingProgress& progress) {
        ProgressEvent event;
        event.imaging = progress;
        if (context.reportProgress(event)) {
            cancellation.requestCancellation();
        }
        context.progress().show(imagingLine(progress), false);
    };
    context.note((imaging.resume ? "Resuming the image of " : "Imaging ") + describeSource(opened->info) + " to " +
                 displayPath(*output));
    Result<imaging::ImagingSummary> summary = makeError(ErrorCode::InternalError, "imaging did not run");
    {
        const Interrupt::Scope scope(context.interrupt(), [&cancellation] { cancellation.requestCancellation(); });
        imaging::ImageWriter writer(*opened->source, *output, std::move(imaging));
        summary = writer.run();
    }
    context.progress().clear();
    if (!summary.ok()) {
        const ExitCode code = context.fail("imaging failed", summary.error());
        // Only when an unfinished image is there to go on with.
        const Result<imaging::ImageMetadata> left = imaging::readImageMetadata(imaging::metadataPathFor(*output));
        if (left.ok() && left->state != imaging::ImageState::Completed) {
            context.note("What was imaged is kept: once the cause is fixed, run the same command with --resume.");
        }
        return code;
    }
    printSummary(context.out(), *summary, *opened);
    context.out().flush();
    if (summary->outcome == imaging::ImagingOutcome::Cancelled) {
        context.note("Imaging was stopped. To go on: recovery image --source " + quoteArgument(*sourceText) +
                     " --output " + quoteArgument(*outputText) + " --resume");
        return ExitCode::Cancelled;
    }
    if (summary->unreadableBytes != 0) {
        context.warning(formatBytes(summary->unreadableBytes) +
                        " of the source could not be read; the image holds zeros there, and its metadata lists them");
        return ExitCode::Incomplete;
    }
    return ExitCode::Success;
}

}  // namespace

const CommandSpec& imageCommand() {
    static const CommandSpec command{
        "image",
        "Copy a source into a raw image file, around bad sectors",
        "recovery image --source SOURCE --output FILE [--resume] [options]",
        "Copies a source into a raw image file, reading it once from start to end. A read that fails is\n"
        "narrowed down to single sectors; a sector that still fails is zero-filled in the image and listed\n"
        "in its metadata (FILE.imgmeta), which scans of the image take into account. The image is written\n"
        "in steps: after Ctrl+C or a crash, run the same command with --resume to go on. The source is\n"
        "only read, and the image may not be on it.",
        kOptions,
        &runImage,
    };
    return command;
}

}  // namespace recovery::cli
