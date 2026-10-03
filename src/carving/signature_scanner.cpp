#include "carving/signature_scanner.hpp"

#include "source_reads.hpp"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <string>
#include <utility>

namespace recovery::carving {

namespace {

using diagnostics::field;
using diagnostics::LogLevel;

constexpr std::string_view kComponent = "carving";
// Individual unreadable ranges logged before switching to the summary only.
constexpr std::uint64_t kMaxLoggedUnreadable = 100;

bool isPowerOfTwo(std::uint64_t value) noexcept {
    return value != 0 && (value & (value - 1)) == 0;
}

// Callers keep value below 2^63 and alignment at most kMaxAlignment, so this cannot overflow.
std::uint64_t alignUp(std::uint64_t value, std::uint64_t alignment) noexcept {
    const std::uint64_t remainder = value % alignment;
    return remainder == 0 ? value : value + (alignment - remainder);
}

}  // namespace

std::string_view toString(ScanOutcome outcome) noexcept {
    switch (outcome) {
    case ScanOutcome::Completed:
        return "Completed";
    case ScanOutcome::Cancelled:
        return "Cancelled";
    case ScanOutcome::HitLimitReached:
        return "HitLimitReached";
    }
    return "Unknown";
}

Result<SignatureScanner> SignatureScanner::create(const FormatRegistry& registry) {
    if (registry.empty()) {
        return makeError(ErrorCode::InvalidInput, "no formats to scan for");
    }
    SignatureScanner scanner;
    scanner.formats_ = registry.formats();
    for (std::size_t formatIndex = 0; formatIndex < scanner.formats_.size(); ++formatIndex) {
        const FormatDescriptor& descriptor = scanner.formats_[formatIndex]->descriptor();
        // The registry checked the descriptor; a format whose descriptor has
        // changed since breaks its contract, so check again.
        if (Status valid = validateDescriptor(descriptor); !valid.ok()) {
            return valid.error();
        }
        for (std::size_t signatureIndex = 0; signatureIndex < descriptor.signatures.size(); ++signatureIndex) {
            const FileSignature& signature = descriptor.signatures[signatureIndex];
            auto group = std::find_if(scanner.groups_.begin(), scanner.groups_.end(),
                                      [&](const Group& candidate) { return candidate.offset == signature.offset; });
            if (group == scanner.groups_.end()) {
                scanner.groups_.emplace_back();
                group = std::prev(scanner.groups_.end());
                group->offset = signature.offset;
            }
            const auto first = static_cast<std::uint8_t>(signature.pattern.front());
            group->firstByte[first] = true;
            group->entries[first].push_back(Entry{formatIndex, signatureIndex, signature});
            scanner.maxReach_ = std::max(scanner.maxReach_, signature.reach());
        }
    }
    std::sort(scanner.groups_.begin(), scanner.groups_.end(),
              [](const Group& a, const Group& b) { return a.offset < b.offset; });
    return scanner;
}

// One scan: the read buffer, the position and the report.
//
// The buffer holds source bytes [dataStart_, dataStart_ + filled_). A file
// start `position` is examined once the buffer holds its maxReach bytes, or
// everything up to readLimit_ (the last byte any examined position can
// need). Before each read, the bytes still needed are moved to the front, so
// the buffer never holds more than one block plus maxReach bytes.
class SignatureScanner::Run {
public:
    Run(const SignatureScanner& scanner, storage::IStorageSource& source, const HitSink& sink,
        const ScanOptions& options)
        : scanner_(scanner), source_(source), sink_(sink), options_(options) {}

    Result<ScanReport> execute() {
        if (Status valid = validate(); !valid.ok()) {
            return valid.error();
        }
        reach_ = scanner_.maxReach_;
        readLimit_ = end_ > start_ ? std::min<std::uint64_t>(source_.size(), end_ - 1 + reach_) : start_;
        for (const Group& group : scanner_.groups_) {
            offsets_.push_back(group.offset);
            tables_.push_back(group.firstByte.data());
        }
        report_.startOffset = start_;
        report_.endOffset = end_;
        report_.hitsPerFormat.assign(scanner_.formats_.size(), 0);
        started_ = std::chrono::steady_clock::now();
        lastProgress_ = started_;
        log(LogLevel::Info, "signature scan started",
            {field("start", start_), field("end", end_), field("block_size", options_.blockSize),
             field("alignment", options_.alignment), field("formats", scanner_.formats_.size())});

        buffer_.resize(options_.blockSize + reach_);
        dataStart_ = start_ - start_ % sectorSize_;
        readPos_ = dataStart_;
        std::uint64_t position = alignUp(start_, options_.alignment);
        ScanOutcome outcome = ScanOutcome::Completed;
        bool stop = false;
        while (position < end_ && !stop) {
            if (options_.reads.cancellation.isCancellationRequested()) {
                outcome = ScanOutcome::Cancelled;
                break;
            }
            const std::uint64_t dataEnd = dataStart_ + filled_;
            if (position + reach_ > dataEnd && dataEnd < readLimit_) {
                if (Status read = readMore(position); !read.ok()) {
                    if (read.error().code == ErrorCode::Cancelled) {
                        outcome = ScanOutcome::Cancelled;
                        break;
                    }
                    log(LogLevel::Error, "signature scan failed",
                        {field("offset", readPos_), field("error", describe(read.error()))});
                    return read.error();
                }
                reportProgress(position, false);
                continue;
            }

            // Every position before processEnd has all the data it can need.
            // The hot loop uses raw pointers: container accessors are checked
            // calls in debug builds, and this runs for every examined byte.
            const std::uint64_t processEnd = dataEnd >= readLimit_ ? end_ : std::min(end_, dataEnd - reach_ + 1);
            const std::byte* data = buffer_.data();
            const std::size_t* offsets = offsets_.data();
            const bool* const* tables = tables_.data();
            const std::size_t groupCount = offsets_.size();
            for (; position < processEnd; position += options_.alignment) {
                const auto base = static_cast<std::size_t>(position - dataStart_);
                bool mayMatch = false;
                for (std::size_t group = 0; group < groupCount && !mayMatch; ++group) {
                    const std::size_t at = base + offsets[group];
                    mayMatch = at < filled_ && tables[group][static_cast<std::uint8_t>(data[at])];
                }
                if (!mayMatch) {
                    continue;
                }
                if (Status examined = examine(position); !examined.ok()) {
                    if (examined.error().code != ErrorCode::Cancelled) {
                        return examined.error();
                    }
                    outcome = ScanOutcome::Cancelled;
                    stop = true;
                    break;
                }
                if (report_.hits >= options_.maxHits) {
                    outcome = ScanOutcome::HitLimitReached;
                    position += options_.alignment;
                    stop = true;
                    break;
                }
            }
        }

        report_.outcome = outcome;
        report_.nextOffset = std::min(position, end_);
        report_.unreadableBytes = unreadable_.totalBytes();
        report_.unreadableRegions = unreadable_.regions();
        report_.elapsed = elapsed();
        reportProgress(report_.nextOffset, true);
        log(outcome == ScanOutcome::Completed ? LogLevel::Info : LogLevel::Warning, "signature scan ended",
            {field("outcome", toString(outcome)), field("next_offset", report_.nextOffset),
             field("bytes_read", report_.bytesRead), field("hits", report_.hits),
             field("unreadable_bytes", report_.unreadableBytes), field("elapsed_ms", report_.elapsed.count())});
        return std::move(report_);
    }

private:
    Status validate() {
        if (!source_.isOpen()) {
            return makeError(ErrorCode::InvalidInput, "scan source is not open");
        }
        if (!sink_) {
            return makeError(ErrorCode::InvalidInput, "scan needs a hit sink");
        }
        sectorSize_ = source_.sectorSize();
        if (!storage::isValidSectorSize(sectorSize_)) {
            return makeError(ErrorCode::InvalidInput, "scan source has an invalid sector size");
        }
        const std::uint64_t size = source_.size();
        start_ = options_.startOffset;
        end_ = options_.endOffset.value_or(size);
        if (end_ > size) {
            return makeError(ErrorCode::InvalidInput, "scan range ends at " + std::to_string(end_) +
                                                          ", beyond the source (" + std::to_string(size) + " bytes)");
        }
        if (start_ > end_) {
            return makeError(ErrorCode::InvalidInput, "scan range starts after it ends");
        }
        if (options_.blockSize == 0 || options_.blockSize > storage::kMaxReadSize ||
            options_.blockSize % sectorSize_ != 0) {
            return makeError(ErrorCode::InvalidInput, "blockSize must be a multiple of the sector size (" +
                                                          std::to_string(sectorSize_) + ") and at most " +
                                                          std::to_string(storage::kMaxReadSize));
        }
        if (!isPowerOfTwo(options_.alignment) || options_.alignment > ScanOptions::kMaxAlignment) {
            return makeError(ErrorCode::InvalidInput, "alignment must be a power of two up to " +
                                                          std::to_string(ScanOptions::kMaxAlignment));
        }
        if (options_.maxHits == 0) {
            return makeError(ErrorCode::InvalidInput, "maxHits must be at least 1");
        }
        return carving::validate(options_.reads);
    }

    // Reads the next block, keeping the buffered bytes from `position` on.
    Status readMore(std::uint64_t position) {
        const std::uint64_t dataEnd = dataStart_ + filled_;
        if (position >= dataEnd) {
            // Nothing buffered is needed any more (a large alignment may skip
            // whole blocks): continue reading at the sector holding position.
            readPos_ = std::max(readPos_, position - position % sectorSize_);
            dataStart_ = readPos_;
            filled_ = 0;
            bufferUnreadable_.clear();
        } else if (position > dataStart_) {
            const auto keep = static_cast<std::size_t>(dataEnd - position);
            std::memmove(buffer_.data(), buffer_.data() + (position - dataStart_), keep);
            dataStart_ = position;
            filled_ = keep;
            std::erase_if(bufferUnreadable_,
                          [&](const storage::BadRegion& region) { return region.end() <= dataStart_; });
        }

        const auto length = static_cast<std::size_t>(std::min<std::uint64_t>(options_.blockSize, readLimit_ - readPos_));
        const std::span<std::byte> target(buffer_.data() + filled_, length);
        found_.clear();
        if (Status filled = detail::fillFromSource(source_, readPos_, target, options_.reads, found_); !filled.ok()) {
            return filled;
        }
        for (const storage::BadRegion& region : found_) {
            // Cannot fail: the region is non-empty and lies inside the source.
            (void)unreadable_.add(region);
            bufferUnreadable_.push_back(region);
            if (unreadableLogged_ < kMaxLoggedUnreadable) {
                log(LogLevel::Warning, "unreadable source range skipped by the signature scan",
                    {field("offset", region.offset), field("length", region.length),
                     field("error_code", region.errorCode)});
            } else if (unreadableLogged_ == kMaxLoggedUnreadable) {
                log(LogLevel::Warning, "further unreadable ranges are only counted in the scan report");
            }
            ++unreadableLogged_;
        }
        readPos_ += length;
        filled_ += length;
        report_.bytesRead += length;
        return success();
    }

    // Delivers the hits at `position`: one per format, its first matching signature.
    Status examine(std::uint64_t position) {
        matches_.clear();
        const auto base = static_cast<std::size_t>(position - dataStart_);
        const std::span<const std::byte> data(buffer_.data(), filled_);
        for (const Group& group : scanner_.groups_) {
            const std::size_t at = base + group.offset;
            if (at >= filled_) {
                continue;
            }
            for (const Entry& entry : group.entries[static_cast<std::uint8_t>(data[at])]) {
                const std::size_t length = entry.signature.pattern.size();
                // A pattern cut off by the end of the source cannot match.
                if (filled_ - at < length || !entry.signature.matches(data.subspan(at, length)) ||
                    touchesUnreadable(dataStart_ + at, length)) {
                    continue;
                }
                matches_.emplace_back(entry.formatIndex, entry.signatureIndex);
            }
        }
        if (matches_.empty()) {
            return success();
        }
        std::sort(matches_.begin(), matches_.end());
        std::size_t lastFormat = scanner_.formats_.size();
        for (const auto& [formatIndex, signatureIndex] : matches_) {
            if (formatIndex == lastFormat) {
                continue;
            }
            lastFormat = formatIndex;
            SignatureHit hit;
            hit.format = scanner_.formats_[formatIndex].get();
            hit.formatIndex = formatIndex;
            hit.signatureIndex = signatureIndex;
            hit.fileOffset = position;
            ++report_.hits;
            ++report_.hitsPerFormat[formatIndex];
            if (Status delivered = sink_(hit); !delivered.ok()) {
                return delivered;
            }
        }
        return success();
    }

    [[nodiscard]] bool touchesUnreadable(std::uint64_t offset, std::size_t length) const noexcept {
        for (const storage::BadRegion& region : bufferUnreadable_) {
            if (region.offset < offset + length && offset < region.end()) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] std::chrono::milliseconds elapsed() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_);
    }

    void reportProgress(std::uint64_t position, bool force) {
        if (!options_.onProgress) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (!force && now - lastProgress_ < options_.progressInterval) {
            return;
        }
        lastProgress_ = now;
        ScanProgress progress;
        progress.position = position;
        progress.startOffset = start_;
        progress.endOffset = end_;
        progress.bytesRead = report_.bytesRead;
        progress.hits = report_.hits;
        progress.unreadableBytes = unreadable_.totalBytes();
        progress.elapsed = elapsed();
        try {
            options_.onProgress(progress);
        } catch (...) {
            log(LogLevel::Warning, "scan progress callback threw an exception");
        }
    }

    void log(LogLevel level, std::string_view message, std::initializer_list<diagnostics::LogField> fields = {}) const {
        if (options_.logger != nullptr) {
            options_.logger->log(level, kComponent, message, fields);
        }
    }

    const SignatureScanner& scanner_;
    storage::IStorageSource& source_;
    const HitSink& sink_;
    const ScanOptions& options_;

    std::uint32_t sectorSize_ = 0;
    std::uint64_t start_ = 0;
    std::uint64_t end_ = 0;
    std::uint64_t readLimit_ = 0;
    std::size_t reach_ = 0;
    // Per signature group: its offset and first-byte table (the hot loop avoids container accessors).
    std::vector<std::size_t> offsets_;
    std::vector<const bool*> tables_;

    std::vector<std::byte> buffer_;
    std::uint64_t dataStart_ = 0;
    std::size_t filled_ = 0;
    // Source offset of the next read: always dataStart_ + filled_ between reads.
    std::uint64_t readPos_ = 0;
    // Unreadable ranges that overlap the buffer (zeros there must not match).
    std::vector<storage::BadRegion> bufferUnreadable_;
    std::vector<storage::BadRegion> found_;
    storage::BadRegionMap unreadable_;
    std::uint64_t unreadableLogged_ = 0;
    std::vector<std::pair<std::size_t, std::size_t>> matches_;

    ScanReport report_;
    std::chrono::steady_clock::time_point started_;
    std::chrono::steady_clock::time_point lastProgress_;
};

Result<ScanReport> SignatureScanner::scan(storage::IStorageSource& source, const HitSink& sink,
                                          const ScanOptions& options) const {
    return Run(*this, source, sink, options).execute();
}

}  // namespace recovery::carving
