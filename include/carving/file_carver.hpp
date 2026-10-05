#pragma once

// File carving: turns signature hits into file candidates by applying each
// hit's format: header detection, end detection, extraction and structural
// validation. A carve is never "find a header, copy until the next header":
// each format decides from its own structure where its files end and
// whether their bytes are consistent.

#include "carving/content_reader.hpp"
#include "carving/file_candidate.hpp"
#include "carving/file_format.hpp"
#include "carving/format_validator.hpp"
#include "carving/signature_scanner.hpp"
#include "recovery/result.hpp"
#include "storage/storage_source.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace recovery::carving {

enum class RejectionReason : std::uint8_t {
    // Fewer bytes than the format's minimum size (or the signature) are left
    // before the end of the source.
    TooLittleData,
    // The signature does not match at the hit, or the format rejected the header.
    HeaderRejected,
    // End detection gave a length below the format's minimum size.
    TooSmall,
    // End detection gave a length beyond the data it was given (a defect of
    // the format module; the candidate is not trusted).
    InvalidEnd,
    // The format module failed with an error of its own (not cancellation
    // and not a source failure).
    FormatFailed,
};

inline constexpr std::size_t kRejectionReasonCount = 5;

[[nodiscard]] std::string_view toString(RejectionReason reason) noexcept;

struct CarveRejection {
    RejectionReason reason = RejectionReason::HeaderRejected;
    // Why (for diagnostics; never file content).
    std::string detail;
};

// What a carve produced: a candidate, or why the hit was not one.
using CarveOutcome = std::variant<FileCandidate, CarveRejection>;

struct CarveOptions {
    // Scan settings for run(). Its reads (cancellation, known bad regions,
    // retries) and logger are also used for the carver's own reads and log
    // records, in carve() too.
    ScanOptions scan;
    // Run each format's validator on its candidates. Without validation no
    // candidate is trusted (see skipHitsInsideValidCandidates).
    bool validate = true;
    // run() skips hits that lie strictly inside a candidate whose end was
    // Found and that validated as Valid: an embedded thumbnail or resource
    // is part of that file. Hits inside any other candidate are carved,
    // except the hits of a self-synchronizing format inside an earlier
    // candidate of the same format (FormatDescriptor::selfSynchronizing),
    // which are skipped whatever that candidate's end and verdict. False
    // turns both rules off.
    bool skipHitsInsideValidCandidates = true;
    // Id of the first candidate; later candidates follow in carve order.
    std::uint64_t firstId = 1;
    // Cache size of each carve's content reader (SourceContentReader limits).
    std::size_t readCacheSize = SourceContentReader::kDefaultCacheSize;
};

// The rules by which run() skips hits (CarveOptions::skipHitsInsideValidCandidates),
// as a value (P15): what a scan driven hit by hit from outside, or resumed
// from a checkpoint, carries from one hit to the next to skip exactly the
// hits one run() skips.
struct CarveSkipState {
    struct Interval {
        std::uint64_t start = 0;
        std::uint64_t end = 0;

        // Holds `offset` strictly inside.
        [[nodiscard]] bool holds(std::uint64_t offset) const noexcept { return offset > start && offset < end; }
        friend bool operator==(const Interval&, const Interval&) = default;
    };

    // The candidate whose end was Found and that validated as Valid that
    // reaches furthest so far.
    Interval trusted;
    // Per format (by registry position): the candidate of a self-synchronizing
    // format that reaches furthest so far, whatever its verdict.
    std::vector<Interval> streams;

    // Whether run() skips `hit` given the candidates recorded so far.
    [[nodiscard]] bool skips(const SignatureHit& hit) const noexcept;
    // Records the candidate carved at `hit` (a hit with its format).
    void record(const SignatureHit& hit, const FileCandidate& candidate);

    friend bool operator==(const CarveSkipState&, const CarveSkipState&) = default;
};

struct CarveReport {
    ScanReport scan;
    std::uint64_t candidates = 0;
    // Candidates per ValidationStatus, and rejected hits per RejectionReason
    // (indexed by the enumerator's value; see count()).
    std::array<std::uint64_t, kValidationStatusCount> validation{};
    std::array<std::uint64_t, kRejectionReasonCount> rejected{};
    // Hits skipped because they lie inside a validated candidate.
    std::uint64_t skippedInsideCandidates = 0;
    // Source bytes read by end detection and validation, on top of scan.bytesRead.
    std::uint64_t carveBytesRead = 0;
    std::chrono::milliseconds elapsed{0};

    [[nodiscard]] std::uint64_t count(ValidationStatus status) const noexcept;
    [[nodiscard]] std::uint64_t count(RejectionReason reason) const noexcept;
    [[nodiscard]] std::uint64_t rejectedTotal() const noexcept;
};

// Receives candidates in non-decreasing source offset order. An error stops
// the run: Cancelled ends it as cancelled, any other error is returned.
using FileCandidateSink = std::function<Status(FileCandidate&& candidate)>;

// Carves files from a source.
//
// Each carve reads the would-be file through a SourceContentReader limited
// to the smaller of the format's maximum size and the rest of the source, so
// no format can read beyond either. Unreadable sectors are seen as zeros and
// recorded in the candidate (UnreadableData). The source is only read.
//
// Thread safety: none; one owner at a time. The source must stay open and
// outlive the carver, and the formats of the hits must outlive the calls.
class FileCarver {
public:
    FileCarver(storage::IStorageSource& source, CarveOptions options = {});

    // Carves the file `hit` points at. The hit may come from any scanner or
    // be made by hand. A rejection is an outcome, not an error. Fails with
    // InvalidInput for invalid options, a hit without a format, with an
    // invalid descriptor (validateDescriptor) or with a signature index out
    // of range, with Cancelled, and with the source's
    // error when a read fails for a reason other than an I/O error.
    [[nodiscard]] Result<CarveOutcome> carve(const SignatureHit& hit);

    // Scans with `scanner` (options.scan) and carves every hit, streaming:
    // each candidate goes to `sink` as soon as it is carved, and memory use
    // does not grow with the number of hits. Fails like scan() and carve(),
    // or with the sink's error.
    [[nodiscard]] Result<CarveReport> run(const SignatureScanner& scanner, const FileCandidateSink& sink);

    [[nodiscard]] const CarveOptions& options() const noexcept { return options_; }

private:
    [[nodiscard]] Status validateOptions() const;
    [[nodiscard]] Result<CarveOutcome> carveValidated(const SignatureHit& hit);
    void log(diagnostics::LogLevel level, std::string_view message,
             std::initializer_list<diagnostics::LogField> fields = {}) const;

    storage::IStorageSource* source_;
    CarveOptions options_;
    std::uint64_t nextId_;
    // Source bytes read by carves since the last run() started.
    std::uint64_t bytesRead_ = 0;
};

}  // namespace recovery::carving
