#include "carving/file_carver.hpp"

#include "source_reads.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace recovery::carving {

namespace {

using diagnostics::field;
using diagnostics::LogLevel;

constexpr std::string_view kComponent = "carving";

static_assert(static_cast<std::size_t>(RejectionReason::FormatFailed) + 1 == kRejectionReasonCount);
static_assert(static_cast<std::size_t>(ValidationStatus::Invalid) + 1 == kValidationStatusCount);

bool isKnown(EndStatus status) noexcept {
    switch (status) {
    case EndStatus::Found:
    case EndStatus::Truncated:
    case EndStatus::Broken:
    case EndStatus::Unknown:
        return true;
    }
    return false;
}

bool isKnown(ValidationStatus status) noexcept {
    return static_cast<std::size_t>(status) < kValidationStatusCount;
}

// Adds a carve's source reads to a total when the carve ends, whichever way it ends.
class ReadAccounting {
public:
    ReadAccounting(const SourceContentReader& reader, std::uint64_t& total) noexcept
        : reader_(reader), total_(total) {}
    ~ReadAccounting() { total_ += reader_.bytesRead(); }
    ReadAccounting(const ReadAccounting&) = delete;
    ReadAccounting& operator=(const ReadAccounting&) = delete;
    ReadAccounting(ReadAccounting&&) = delete;
    ReadAccounting& operator=(ReadAccounting&&) = delete;

private:
    const SourceContentReader& reader_;
    std::uint64_t& total_;
};

}  // namespace

std::string_view toString(RejectionReason reason) noexcept {
    switch (reason) {
    case RejectionReason::TooLittleData:
        return "TooLittleData";
    case RejectionReason::HeaderRejected:
        return "HeaderRejected";
    case RejectionReason::TooSmall:
        return "TooSmall";
    case RejectionReason::InvalidEnd:
        return "InvalidEnd";
    case RejectionReason::FormatFailed:
        return "FormatFailed";
    }
    return "Unknown";
}

bool CarveSkipState::skips(const SignatureHit& hit) const noexcept {
    return trusted.holds(hit.fileOffset) ||
           (hit.formatIndex < streams.size() && streams[hit.formatIndex].holds(hit.fileOffset));
}

void CarveSkipState::record(const SignatureHit& hit, const FileCandidate& candidate) {
    if (candidate.end.status == EndStatus::Found && candidate.validation.status == ValidationStatus::Valid &&
        candidate.sourceEnd() > trusted.end) {
        trusted = Interval{candidate.sourceOffset, candidate.sourceEnd()};
    }
    if (hit.format != nullptr && hit.format->descriptor().selfSynchronizing) {
        if (hit.formatIndex >= streams.size()) {
            streams.resize(hit.formatIndex + 1);
        }
        if (candidate.sourceEnd() > streams[hit.formatIndex].end) {
            streams[hit.formatIndex] = Interval{candidate.sourceOffset, candidate.sourceEnd()};
        }
    }
}

std::uint64_t CarveReport::count(ValidationStatus status) const noexcept {
    const auto index = static_cast<std::size_t>(status);
    return index < validation.size() ? validation[index] : 0;
}

std::uint64_t CarveReport::count(RejectionReason reason) const noexcept {
    const auto index = static_cast<std::size_t>(reason);
    return index < rejected.size() ? rejected[index] : 0;
}

std::uint64_t CarveReport::rejectedTotal() const noexcept {
    std::uint64_t total = 0;
    for (const std::uint64_t count : rejected) {
        total += count;
    }
    return total;
}

FileCarver::FileCarver(storage::IStorageSource& source, CarveOptions options)
    : source_(&source), options_(std::move(options)), nextId_(options_.firstId) {}

Status FileCarver::validateOptions() const {
    if (!source_->isOpen()) {
        return makeError(ErrorCode::InvalidInput, "carving source is not open");
    }
    if (options_.readCacheSize < SourceContentReader::kMinCacheSize ||
        options_.readCacheSize > IContentReader::kMaxReadLength) {
        return makeError(ErrorCode::InvalidInput, "readCacheSize must lie between " +
                                                      std::to_string(SourceContentReader::kMinCacheSize) + " and " +
                                                      std::to_string(IContentReader::kMaxReadLength));
    }
    return validate(options_.scan.reads);
}

Result<CarveOutcome> FileCarver::carve(const SignatureHit& hit) {
    if (Status valid = validateOptions(); !valid.ok()) {
        return valid.error();
    }
    // A hand-made hit may name a format no registry has checked. (run() gets
    // its hits from a scanner, which checked every descriptor.)
    if (hit.format != nullptr) {
        if (Status valid = validateDescriptor(hit.format->descriptor()); !valid.ok()) {
            return valid.error();
        }
    }
    return carveValidated(hit);
}

Result<CarveOutcome> FileCarver::carveValidated(const SignatureHit& hit) {
    if (hit.format == nullptr) {
        return makeError(ErrorCode::InvalidInput, "signature hit without a format");
    }
    const IFileFormat& format = *hit.format;
    const FormatDescriptor& descriptor = format.descriptor();
    if (hit.signatureIndex >= descriptor.signatures.size()) {
        return makeError(ErrorCode::InvalidInput, "signature hit for format '" + descriptor.id +
                                                      "' names signature " + std::to_string(hit.signatureIndex) +
                                                      " of " + std::to_string(descriptor.signatures.size()));
    }
    if (options_.scan.reads.cancellation.isCancellationRequested()) {
        return detail::cancelledError();
    }
    const FileSignature& signature = descriptor.signatures[hit.signatureIndex];
    const auto reject = [&](RejectionReason reason, std::string detail) -> Result<CarveOutcome> {
        log(LogLevel::Debug, "signature hit rejected",
            {field("format", descriptor.id), field("offset", hit.fileOffset), field("reason", toString(reason)),
             field("detail", detail)});
        return CarveOutcome(CarveRejection{reason, std::move(detail)});
    };
    // The reader failed (cancellation or the source), or the format failed on its own.
    const auto failed = [&](const Error& error, const SourceContentReader& content,
                            std::string_view step) -> Result<CarveOutcome> {
        if (error.code == ErrorCode::Cancelled || content.sourceFailure().has_value()) {
            return error;
        }
        return reject(RejectionReason::FormatFailed, std::string(step) + " failed: " + describe(error));
    };

    const std::uint64_t sourceSize = source_->size();
    const std::uint64_t remaining = hit.fileOffset < sourceSize ? sourceSize - hit.fileOffset : 0;
    const std::uint64_t available = std::min(descriptor.maximumSize, remaining);
    const std::uint64_t minimum = std::max<std::uint64_t>(descriptor.minimumSize, signature.reach());
    if (available < minimum) {
        return reject(RejectionReason::TooLittleData, std::to_string(remaining) +
                                                          " bytes left before the end of the source; the format "
                                                          "needs at least " +
                                                          std::to_string(minimum));
    }

    Result<std::unique_ptr<SourceContentReader>> opened = SourceContentReader::open(
        *source_, hit.fileOffset, available, options_.scan.reads, options_.readCacheSize);
    if (!opened.ok()) {
        return opened.error();
    }
    SourceContentReader& content = **opened;
    const ReadAccounting accounting(content, bytesRead_);

    // Header detection. The header holds the signature: headerSize and
    // available are both at least its reach.
    const auto headerLength = static_cast<std::size_t>(std::min<std::uint64_t>(descriptor.headerSize, available));
    Result<std::span<const std::byte>> header = content.read(0, headerLength);
    if (!header.ok()) {
        return header.error();
    }
    if (!signature.matches(header->subspan(signature.offset))) {
        return reject(RejectionReason::HeaderRejected, "signature '" + signature.name + "' does not match");
    }
    HeaderCheck check = format.checkHeader(*header);
    if (!check.plausible) {
        return reject(RejectionReason::HeaderRejected, std::move(check.reason));
    }

    // End detection.
    Result<EndDetection> end = format.findEnd(content);
    if (!end.ok()) {
        return failed(end.error(), content, "end detection");
    }
    if (!isKnown(end->status)) {
        return reject(RejectionReason::FormatFailed, "end detection returned an unknown status");
    }
    if (end->length > available) {
        return reject(RejectionReason::InvalidEnd, "end detection gave " + std::to_string(end->length) +
                                                       " bytes, but only " + std::to_string(available) +
                                                       " were available");
    }
    if (end->length < minimum) {
        return reject(RejectionReason::TooSmall, "end detection (" + std::string(toString(end->status)) + ": " +
                                                     end->detail + ") gave " + std::to_string(end->length) +
                                                     " bytes; the format needs at least " + std::to_string(minimum));
    }

    // Extraction: the only strategy so far is one contiguous piece.
    FileCandidate candidate;
    candidate.formatId = descriptor.id;
    candidate.extension = descriptor.extension;
    candidate.sourceOffset = hit.fileOffset;
    candidate.length = end->length;
    candidate.extraction = descriptor.extraction;
    candidate.extents.push_back(CarvedExtent{0, hit.fileOffset, end->length});
    candidate.signature.signatureIndex = hit.signatureIndex;
    candidate.signature.signatureName = signature.name;
    candidate.signature.matchOffset = hit.fileOffset + signature.offset;
    candidate.end.method = descriptor.endDetection;
    candidate.end.status = end->status;
    candidate.end.detail = std::move(end->detail);
    candidate.end.available = available;
    switch (end->status) {
    case EndStatus::Found:
        break;
    case EndStatus::Truncated:
        candidate.warnings.push_back(remaining <= descriptor.maximumSize ? CarveWarning::TruncatedBySourceEnd
                                                                         : CarveWarning::TruncatedByMaximumSize);
        break;
    case EndStatus::Broken:
        candidate.warnings.push_back(CarveWarning::StructureBroken);
        break;
    case EndStatus::Unknown:
        candidate.warnings.push_back(CarveWarning::EndUnknown);
        break;
    }

    // Structural validation of exactly the carved bytes.
    if (options_.validate) {
        content.truncate(candidate.length);
        Result<ValidationResult> validation = format.validator().validate(content);
        if (!validation.ok()) {
            return failed(validation.error(), content, "validation");
        }
        if (!isKnown(validation->status) || validation->status == ValidationStatus::NotValidated) {
            return reject(RejectionReason::FormatFailed, "validation returned no verdict");
        }
        candidate.validation = std::move(*validation);
        candidate.validation.validBytes = std::min(candidate.validation.validBytes, candidate.length);
    }

    for (storage::BadRegion region : content.unreadable().overlapping(candidate.sourceOffset, candidate.length)) {
        const std::uint64_t regionEnd = std::min(region.end(), candidate.sourceEnd());
        region.offset = std::max(region.offset, candidate.sourceOffset);
        region.length = regionEnd - region.offset;
        candidate.unreadableRegions.push_back(region);
    }
    if (!candidate.unreadableRegions.empty()) {
        candidate.warnings.push_back(CarveWarning::UnreadableData);
    }

    candidate.id = FileCandidateId{nextId_++};
    log(LogLevel::Debug, "file candidate carved",
        {field("id", candidate.id.value()), field("format", candidate.formatId),
         field("offset", candidate.sourceOffset), field("length", candidate.length),
         field("end", toString(candidate.end.status)), field("validation", toString(candidate.validation.status))});
    if (candidate.validation.status == ValidationStatus::Invalid) {
        log(LogLevel::Info, "carved candidate failed validation",
            {field("id", candidate.id.value()), field("format", candidate.formatId),
             field("offset", candidate.sourceOffset), field("valid_bytes", candidate.validation.validBytes),
             field("detail", candidate.validation.detail)});
    }
    return CarveOutcome(std::move(candidate));
}

Result<CarveReport> FileCarver::run(const SignatureScanner& scanner, const FileCandidateSink& sink) {
    if (Status valid = validateOptions(); !valid.ok()) {
        return valid.error();
    }
    if (!sink) {
        return makeError(ErrorCode::InvalidInput, "carving needs a candidate sink");
    }
    const auto started = std::chrono::steady_clock::now();
    bytesRead_ = 0;
    CarveReport report;
    // Hits strictly inside the validated candidate reaching furthest so far are
    // skipped, and so are a self-synchronizing format's own hits inside its
    // candidate reaching furthest, whatever its verdict.
    CarveSkipState skip;
    log(LogLevel::Info, "carving started",
        {field("formats", scanner.formats().size()), field("validate", options_.validate ? "yes" : "no")});

    const HitSink onHit = [&](const SignatureHit& hit) -> Status {
        if (options_.skipHitsInsideValidCandidates && skip.skips(hit)) {
            ++report.skippedInsideCandidates;
            return success();
        }
        Result<CarveOutcome> outcome = carveValidated(hit);
        if (!outcome.ok()) {
            return outcome.error();
        }
        if (const CarveRejection* rejection = std::get_if<CarveRejection>(&*outcome); rejection != nullptr) {
            ++report.rejected[static_cast<std::size_t>(rejection->reason)];
            return success();
        }
        FileCandidate& candidate = std::get<FileCandidate>(*outcome);
        ++report.candidates;
        ++report.validation[static_cast<std::size_t>(candidate.validation.status)];
        skip.record(hit, candidate);
        return sink(std::move(candidate));
    };

    Result<ScanReport> scan = scanner.scan(*source_, onHit, options_.scan);
    if (!scan.ok()) {
        return scan.error();
    }
    report.scan = std::move(*scan);
    report.carveBytesRead = bytesRead_;
    report.elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    log(report.scan.outcome == ScanOutcome::Completed ? LogLevel::Info : LogLevel::Warning, "carving ended",
        {field("outcome", toString(report.scan.outcome)), field("hits", report.scan.hits),
         field("candidates", report.candidates), field("valid", report.count(ValidationStatus::Valid)),
         field("invalid", report.count(ValidationStatus::Invalid)), field("rejected", report.rejectedTotal()),
         field("skipped", report.skippedInsideCandidates), field("carve_bytes_read", report.carveBytesRead),
         field("elapsed_ms", report.elapsed.count())});
    return report;
}

void FileCarver::log(LogLevel level, std::string_view message,
                     std::initializer_list<diagnostics::LogField> fields) const {
    if (options_.scan.logger != nullptr) {
        options_.scan.logger->log(level, kComponent, message, fields);
    }
}

}  // namespace recovery::carving
