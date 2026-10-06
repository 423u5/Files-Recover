// The binary encoding of a session's record payloads (P16).
//
// Every value is written by one Encoder and read back by one Decoder, both
// driven by the same `fields` function per type: each lists the type's
// members once, so what is written is what is read. Encoding:
//
//   bool                 one byte, 0 or 1
//   unsigned integers    LEB128 (7 bits a byte, the last byte without the
//                        high bit); the shortest form only
//   signed integers      zigzag, then LEB128 (durations and times as counts)
//   enumerations         their value, as unsigned
//   strings, byte lists  the length, then the bytes
//   lists                the count, then the elements
//   optional values      a presence byte (0 or 1), then the value
//   GUIDs, digests       16 and 32 raw bytes
//   paths                UTF-8, as strings
//
// Decoding trusts nothing: a count or length must fit in the bytes left (an
// element takes at least one byte), an enumerator must be one of its type,
// a number must fit its type, candidates must keep their invariants, and a
// payload must be used up exactly. A failure stops the decoding (later reads
// return nothing) and is reported with where it happened.

#include "session/session_journal.hpp"

#include "recovery/text.hpp"

#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace recovery::session {

namespace {

// ---------------------------------------------------------------------------
// Type traits
// ---------------------------------------------------------------------------

template <class V, class T>
concept Of = std::same_as<std::remove_const_t<V>, T>;

template <class T>
struct IsVector : std::false_type {};
template <class T, class Allocator>
struct IsVector<std::vector<T, Allocator>> : std::true_type {};

template <class T>
struct IsOptional : std::false_type {};
template <class T>
struct IsOptional<std::optional<T>> : std::true_type {};

template <class T>
struct IsPair : std::false_type {};
template <class First, class Second>
struct IsPair<std::pair<First, Second>> : std::true_type {};

template <class T>
struct IsArray : std::false_type {};
template <class T, std::size_t N>
struct IsArray<std::array<T, N>> : std::true_type {};

template <class T>
struct IsStrong : std::false_type {};
template <class Tag, class Rep>
struct IsStrong<StrongValue<Tag, Rep>> : std::true_type {};

template <class T>
struct IsDuration : std::false_type {};
template <class Rep, class Period>
struct IsDuration<std::chrono::duration<Rep, Period>> : std::true_type {};

template <class T>
struct IsTimePoint : std::false_type {};
template <class Clock, class Duration>
struct IsTimePoint<std::chrono::time_point<Clock, Duration>> : std::true_type {};

// The last enumerator of every enumeration a payload holds: decoding refuses
// values beyond it. An enumeration without an entry here does not compile.
template <class E>
struct EnumLimit;

template <class E, E Last>
struct EnumLimitOf {
    static constexpr E last = Last;
};

// clang-format off
template <> struct EnumLimit<ErrorCode> : EnumLimitOf<ErrorCode, ErrorCode::InternalError> {};
template <> struct EnumLimit<storage::SourceType>
    : EnumLimitOf<storage::SourceType, storage::SourceType::Synthetic> {};
template <> struct EnumLimit<ScanMode> : EnumLimitOf<ScanMode, ScanMode::Deep> {};
template <> struct EnumLimit<scan::ScanStage> : EnumLimitOf<scan::ScanStage, scan::ScanStage::Completed> {};
template <> struct EnumLimit<partition::PartitionScheme>
    : EnumLimitOf<partition::PartitionScheme, partition::PartitionScheme::Gpt> {};
template <> struct EnumLimit<partition::PartitionIssueKind>
    : EnumLimitOf<partition::PartitionIssueKind, partition::PartitionIssueKind::GptUnavailable> {};
template <> struct EnumLimit<filesystem::FilesystemType>
    : EnumLimitOf<filesystem::FilesystemType, filesystem::FilesystemType::Ntfs> {};
template <> struct EnumLimit<filesystem::AllocationMethod>
    : EnumLimitOf<filesystem::AllocationMethod, filesystem::AllocationMethod::Resident> {};
template <> struct EnumLimit<filesystem::AllocationIssue>
    : EnumLimitOf<filesystem::AllocationIssue, filesystem::AllocationIssue::DataAttributeMissing> {};
template <> struct EnumLimit<filesystem::EntryState>
    : EnumLimitOf<filesystem::EntryState, filesystem::EntryState::Deleted> {};
template <> struct EnumLimit<filesystem::EntryIssue>
    : EnumLimitOf<filesystem::EntryIssue, filesystem::EntryIssue::DamagedRecord> {};
template <> struct EnumLimit<filesystem::ScanIssueKind>
    : EnumLimitOf<filesystem::ScanIssueKind, filesystem::ScanIssueKind::RecordUnreadable> {};
template <> struct EnumLimit<RecoveryMethod> : EnumLimitOf<RecoveryMethod, RecoveryMethod::Fragmented> {};
template <> struct EnumLimit<RegionKind> : EnumLimitOf<RegionKind, RegionKind::Missing> {};
template <> struct EnumLimit<LayoutEvidence> : EnumLimitOf<LayoutEvidence, LayoutEvidence::Guessed> {};
template <> struct EnumLimit<CandidateWarning>
    : EnumLimitOf<CandidateWarning, CandidateWarning::MetadataDamaged> {};
template <> struct EnumLimit<carving::ScanOutcome>
    : EnumLimitOf<carving::ScanOutcome, carving::ScanOutcome::HitLimitReached> {};
template <> struct EnumLimit<carving::ValidationStatus>
    : EnumLimitOf<carving::ValidationStatus, carving::ValidationStatus::Invalid> {};
template <> struct EnumLimit<carving::EndDetectionMethod>
    : EnumLimitOf<carving::EndDetectionMethod, carving::EndDetectionMethod::None> {};
template <> struct EnumLimit<carving::EndStatus> : EnumLimitOf<carving::EndStatus, carving::EndStatus::Unknown> {};
template <> struct EnumLimit<carving::ExtractionStrategy>
    : EnumLimitOf<carving::ExtractionStrategy, carving::ExtractionStrategy::Contiguous> {};
template <> struct EnumLimit<carving::CarveWarning>
    : EnumLimitOf<carving::CarveWarning, carving::CarveWarning::UnreadableData> {};
template <> struct EnumLimit<formats::mp4::FileStatus>
    : EnumLimitOf<formats::mp4::FileStatus, formats::mp4::FileStatus::Invalid> {};
template <> struct EnumLimit<formats::mp4::IssueKind>
    : EnumLimitOf<formats::mp4::IssueKind, formats::mp4::IssueKind::LimitExceeded> {};
template <> struct EnumLimit<formats::mp4::MediaKind>
    : EnumLimitOf<formats::mp4::MediaKind, formats::mp4::MediaKind::Neither> {};
template <> struct EnumLimit<formats::mp4::TrackKind>
    : EnumLimitOf<formats::mp4::TrackKind, formats::mp4::TrackKind::Other> {};
template <> struct EnumLimit<Mp4Warning> : EnumLimitOf<Mp4Warning, Mp4Warning::InsideActiveFile> {};
template <> struct EnumLimit<ReconstructionStatus>
    : EnumLimitOf<ReconstructionStatus, ReconstructionStatus::Unrecoverable> {};
template <> struct EnumLimit<SeedOrigin> : EnumLimitOf<SeedOrigin, SeedOrigin::Carving> {};
template <> struct EnumLimit<LayoutSource> : EnumLimitOf<LayoutSource, LayoutSource::SampleTables> {};
template <> struct EnumLimit<evaluation::EvaluationWarning>
    : EnumLimitOf<evaluation::EvaluationWarning, evaluation::EvaluationWarning::InsideActiveFile> {};
template <> struct EnumLimit<validation::LevelStatus>
    : EnumLimitOf<validation::LevelStatus, validation::LevelStatus::Unsupported> {};
template <> struct EnumLimit<validation::Coverage>
    : EnumLimitOf<validation::Coverage, validation::Coverage::Partial> {};
template <> struct EnumLimit<SessionState> : EnumLimitOf<SessionState, SessionState::Failed> {};
// clang-format on

template <class E>
constexpr std::uint64_t enumValue(E value) noexcept {
    return static_cast<std::uint64_t>(static_cast<std::underlying_type_t<E>>(value));
}

// ---------------------------------------------------------------------------
// Encoder
// ---------------------------------------------------------------------------

class Encoder {
public:
    static constexpr bool kDecoding = false;

    template <class T>
    void operator()(const T& value) {
        if constexpr (std::is_same_v<T, bool>) {
            bytes_.push_back(value ? std::byte{1} : std::byte{0});
        } else if constexpr (std::is_enum_v<T>) {
            static_assert(enumValue(EnumLimit<T>::last) < std::numeric_limits<std::uint64_t>::max());
            varint(enumValue(value));
        } else if constexpr (std::is_integral_v<T> && std::is_unsigned_v<T>) {
            varint(value);
        } else if constexpr (std::is_integral_v<T>) {
            const auto wide = static_cast<std::int64_t>(value);
            varint((static_cast<std::uint64_t>(wide) << 1) ^ static_cast<std::uint64_t>(wide >> 63));
        } else if constexpr (std::is_same_v<T, std::string>) {
            varint(value.size());
            raw(std::as_bytes(std::span(value.data(), value.size())));
        } else if constexpr (std::is_same_v<T, std::vector<std::byte>>) {
            varint(value.size());
            raw(value);
        } else if constexpr (IsVector<T>::value) {
            varint(value.size());
            for (const auto& item : value) {
                (*this)(item);
            }
        } else if constexpr (IsOptional<T>::value) {
            (*this)(value.has_value());
            if (value.has_value()) {
                (*this)(*value);
            }
        } else if constexpr (IsPair<T>::value) {
            (*this)(value.first);
            (*this)(value.second);
        } else if constexpr (IsArray<T>::value) {
            for (const auto& item : value) {
                (*this)(item);
            }
        } else if constexpr (IsStrong<T>::value) {
            (*this)(value.value());
        } else if constexpr (IsDuration<T>::value) {
            (*this)(static_cast<std::int64_t>(value.count()));
        } else if constexpr (IsTimePoint<T>::value) {
            (*this)(value.time_since_epoch());
        } else if constexpr (std::is_same_v<T, partition::Guid>) {
            raw(value.bytes());
        } else if constexpr (std::is_same_v<T, formats::mp4::FourCc>) {
            varint(value.value());
        } else if constexpr (std::is_same_v<T, Sha256Digest>) {
            raw(std::as_bytes(std::span(value.bytes())));
        } else if constexpr (std::is_same_v<T, std::filesystem::path>) {
            (*this)(toUtf8(value));
        } else {
            fields(*this, value);
        }
    }

    [[nodiscard]] std::vector<std::byte> take() && { return std::move(bytes_); }

private:
    void varint(std::uint64_t value) {
        while (value >= 0x80) {
            bytes_.push_back(static_cast<std::byte>((value & 0x7F) | 0x80));
            value >>= 7;
        }
        bytes_.push_back(static_cast<std::byte>(value));
    }

    void raw(std::span<const std::byte> data) { bytes_.insert(bytes_.end(), data.begin(), data.end()); }

    std::vector<std::byte> bytes_;
};

// ---------------------------------------------------------------------------
// Decoder
// ---------------------------------------------------------------------------

class Decoder {
public:
    static constexpr bool kDecoding = true;

    explicit Decoder(std::span<const std::byte> bytes) noexcept : bytes_(bytes) {}

    template <class T>
    void operator()(T& value) {
        if (failed()) {
            return;
        }
        if constexpr (std::is_same_v<T, bool>) {
            const std::uint8_t flag = byte();
            if (flag > 1) {
                fail("a flag that is neither 0 nor 1");
                return;
            }
            value = flag == 1;
        } else if constexpr (std::is_enum_v<T>) {
            const std::uint64_t number = varint();
            if (number > enumValue(EnumLimit<T>::last)) {
                fail("a value out of the range of its kind (" + std::to_string(number) + ")");
                return;
            }
            value = static_cast<T>(static_cast<std::underlying_type_t<T>>(number));
        } else if constexpr (std::is_integral_v<T> && std::is_unsigned_v<T>) {
            const std::uint64_t number = varint();
            if (number > std::numeric_limits<T>::max()) {
                fail("a number too large for its field (" + std::to_string(number) + ")");
                return;
            }
            value = static_cast<T>(number);
        } else if constexpr (std::is_integral_v<T>) {
            const std::uint64_t zigzag = varint();
            const auto number = static_cast<std::int64_t>((zigzag >> 1) ^ (std::uint64_t{0} - (zigzag & 1)));
            if (number < std::numeric_limits<T>::min() || number > std::numeric_limits<T>::max()) {
                fail("a number out of the range of its field");
                return;
            }
            value = static_cast<T>(number);
        } else if constexpr (std::is_same_v<T, std::string>) {
            const std::span<const std::byte> text = take(length());
            value.assign(reinterpret_cast<const char*>(text.data()), text.size());
        } else if constexpr (std::is_same_v<T, std::vector<std::byte>>) {
            const std::span<const std::byte> data = take(length());
            value.assign(data.begin(), data.end());
        } else if constexpr (IsVector<T>::value) {
            // Every element takes at least one byte, so the count is bounded
            // by the bytes left; the list grows with what is decoded.
            const std::uint64_t count = length();
            value.clear();
            for (std::uint64_t i = 0; i < count && !failed(); ++i) {
                (*this)(value.emplace_back());
            }
        } else if constexpr (IsOptional<T>::value) {
            bool present = false;
            (*this)(present);
            value.reset();
            if (present && !failed()) {
                (*this)(value.emplace());
            }
        } else if constexpr (IsPair<T>::value) {
            (*this)(value.first);
            (*this)(value.second);
        } else if constexpr (IsArray<T>::value) {
            for (auto& item : value) {
                (*this)(item);
            }
        } else if constexpr (IsStrong<T>::value) {
            typename T::RepType rep{};
            (*this)(rep);
            value = T{rep};
        } else if constexpr (IsDuration<T>::value) {
            std::int64_t count = 0;
            (*this)(count);
            value = T{static_cast<typename T::rep>(count)};
        } else if constexpr (IsTimePoint<T>::value) {
            typename T::duration since{};
            (*this)(since);
            value = T{since};
        } else if constexpr (std::is_same_v<T, partition::Guid>) {
            const std::span<const std::byte> data = take(16);
            if (!failed()) {
                value = partition::Guid::fromDisk(data);
            }
        } else if constexpr (std::is_same_v<T, formats::mp4::FourCc>) {
            std::uint32_t code = 0;
            (*this)(code);
            value = formats::mp4::FourCc{code};
        } else if constexpr (std::is_same_v<T, Sha256Digest>) {
            const std::span<const std::byte> data = take(Sha256Digest::kSize);
            std::array<std::uint8_t, Sha256Digest::kSize> digest{};
            for (std::size_t i = 0; i < data.size(); ++i) {
                digest[i] = static_cast<std::uint8_t>(data[i]);
            }
            value = Sha256Digest{digest};
        } else if constexpr (std::is_same_v<T, std::filesystem::path>) {
            std::string text;
            (*this)(text);
            if (failed()) {
                return;
            }
            try {
                value = std::filesystem::path(std::u8string(text.begin(), text.end()));
            } catch (const std::exception&) {
                fail("a path that is not UTF-8");
            }
        } else {
            fields(*this, value);
        }
    }

    // Fails the decoding with the reason of a failed check.
    void check(const Status& status) {
        if (!status.ok() && !failed()) {
            fail(status.error().message);
        }
    }

    [[nodiscard]] bool failed() const noexcept { return error_.has_value(); }
    [[nodiscard]] const std::string& error() const noexcept { return *error_; }
    [[nodiscard]] std::size_t position() const noexcept { return position_; }
    // Ends the decoding: every byte must have been used.
    void finish() {
        if (!failed() && position_ != bytes_.size()) {
            fail(std::to_string(bytes_.size() - position_) + " bytes left over");
        }
    }

private:
    void fail(std::string reason) {
        if (!error_.has_value()) {
            error_ = std::move(reason) + " at byte " + std::to_string(position_);
        }
    }

    [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - position_; }

    [[nodiscard]] std::uint8_t byte() {
        if (failed() || remaining() == 0) {
            fail("the data ends early");
            return 0;
        }
        return static_cast<std::uint8_t>(bytes_[position_++]);
    }

    [[nodiscard]] std::uint64_t varint() {
        std::uint64_t value = 0;
        for (unsigned shift = 0; shift < 64; shift += 7) {
            const std::uint8_t piece = byte();
            if (failed()) {
                return 0;
            }
            if (shift == 63 && piece > 1) {
                fail("a number beyond 64 bits");
                return 0;
            }
            value |= static_cast<std::uint64_t>(piece & 0x7F) << shift;
            if ((piece & 0x80) == 0) {
                if (piece == 0 && shift != 0) {
                    fail("a number not in its shortest form");
                    return 0;
                }
                return value;
            }
        }
        fail("a number beyond 64 bits");
        return 0;
    }

    // A count or a length: at most the bytes left.
    [[nodiscard]] std::uint64_t length() {
        const std::uint64_t value = varint();
        if (!failed() && value > remaining()) {
            fail("a length of " + std::to_string(value) + " with " + std::to_string(remaining()) + " bytes left");
            return 0;
        }
        return failed() ? 0 : value;
    }

    [[nodiscard]] std::span<const std::byte> take(std::uint64_t count) {
        if (failed()) {
            return {};
        }
        if (count > remaining()) {
            fail("the data ends early");
            return {};
        }
        const std::span<const std::byte> part = bytes_.subspan(position_, static_cast<std::size_t>(count));
        position_ += static_cast<std::size_t>(count);
        return part;
    }

    std::span<const std::byte> bytes_;
    std::size_t position_ = 0;
    std::optional<std::string> error_;
};

// ---------------------------------------------------------------------------
// The members of every type, once each
// ---------------------------------------------------------------------------

template <class A, Of<Error> V>
void fields(A& a, V& v) {
    a(v.code);
    a(v.message);
    a(v.systemErrorCode);
}

template <class A, Of<storage::BadRegion> V>
void fields(A& a, V& v) {
    a(v.offset);
    a(v.length);
    a(v.errorCode);
    if constexpr (A::kDecoding) {
        if (v.length == 0 || v.offset > std::numeric_limits<std::uint64_t>::max() - v.length) {
            a.check(makeError(ErrorCode::InvalidFormat, "an empty or overflowing unreadable region"));
        }
    }
}

// Partitions.

template <class A, Of<partition::FilesystemCandidates> V>
void fields(A& a, V& v) {
    a(v.fat);
    a(v.exfat);
    a(v.ntfs);
}

template <class A, Of<partition::PartitionIssue> V>
void fields(A& a, V& v) {
    a(v.kind);
    a(v.partitionIndex);
    a(v.detail);
}

template <class A, Of<partition::Partition> V>
void fields(A& a, V& v) {
    a(v.index);
    a(v.firstLba);
    a(v.sectorCount);
    a(v.offset);
    a(v.size);
    a(v.truncated);
    a(v.mbrType);
    a(v.bootable);
    a(v.logical);
    a(v.typeGuid);
    a(v.uniqueGuid);
    a(v.attributes);
    a(v.name);
    a(v.typeName);
    a(v.candidates);
}

template <class A, Of<partition::GptInfo> V>
void fields(A& a, V& v) {
    a(v.diskGuid);
    a(v.headerLba);
    a(v.firstUsableLba);
    a(v.lastUsableLba);
    a(v.entryCount);
    a(v.entrySize);
    a(v.primaryValid);
    a(v.backupValid);
}

template <class A, Of<partition::PartitionTable> V>
void fields(A& a, V& v) {
    a(v.scheme);
    a(v.sectorSize);
    a(v.deviceSectors);
    a(v.partitions);
    a(v.gpt);
    a(v.issues);
}

// Filesystems and their candidates.

template <class A, Of<filesystem::FilesystemInfo> V>
void fields(A& a, V& v) {
    a(v.type);
    a(v.label);
    a(v.serialNumber);
    a(v.bytesPerSector);
    a(v.clusterSize);
    a(v.clusterCount);
    a(v.firstCluster);
    a(v.volumeSize);
    a(v.dataOffset);
    a(v.warnings);
}

template <class A, Of<filesystem::ScanIssue> V>
void fields(A& a, V& v) {
    a(v.kind);
    a(v.path);
    a(v.detail);
}

template <class A, Of<filesystem::Timestamp> V>
void fields(A& a, V& v) {
    a(v.time);
    a(v.local);
}

template <class A, Of<filesystem::EntryAttributes> V>
void fields(A& a, V& v) {
    a(v.readOnly);
    a(v.hidden);
    a(v.system);
    a(v.archive);
}

template <class A, Of<SourceRegion> V>
void fields(A& a, V& v) {
    a(v.fileOffset);
    a(v.length);
    a(v.kind);
    a(v.sourceOffset);
    a(v.reallocated);
}

template <class A, Of<AllocationInfo> V>
void fields(A& a, V& v) {
    a(v.method);
    a(v.layout);
    a(v.firstCluster);
    a(v.clusterCount);
    a(v.clusterSize);
    a(v.issues);
}

template <class A, Of<FragmentationInfo> V>
void fields(A& a, V& v) {
    a(v.fragmentCount);
    a(v.known);
}

template <class A, Of<FilesystemEvidence> V>
void fields(A& a, V& v) {
    a(v.type);
    a(v.volumeOffset);
    a(v.metadataOffset);
    a(v.path);
    a(v.shortName);
    a(v.state);
    a(v.parentDeleted);
    a(v.validDataLength);
    a(v.created);
    a(v.modified);
    a(v.accessed);
    a(v.attributes);
    a(v.entryIssues);
    a(v.allocation);
}

template <class A, Of<RecoveryCandidate> V>
void fields(A& a, V& v) {
    a(v.id);
    a(v.method);
    a(v.filename);
    a(v.extension);
    a(v.expectedSize);
    a(v.sourceRegions);
    a(v.embeddedData);
    a(v.fragmentation);
    a(v.filesystemEvidence);
    a(v.warnings);
    if constexpr (A::kDecoding) {
        if (!a.failed()) {
            a.check(validateCandidate(v));
        }
    }
}

template <class A, Of<CandidateScan> V>
void fields(A& a, V& v) {
    a(v.filesystemInfo);
    a(v.volumeOffset);
    a(v.candidates);
    a(v.issues);
    a(v.directories);
    a(v.systemFiles);
    a(v.complete);
}

// Carving.

template <class A, Of<carving::CarvedExtent> V>
void fields(A& a, V& v) {
    a(v.fileOffset);
    a(v.sourceOffset);
    a(v.length);
}

template <class A, Of<carving::SignatureEvidence> V>
void fields(A& a, V& v) {
    a(v.signatureIndex);
    a(v.signatureName);
    a(v.matchOffset);
}

template <class A, Of<carving::EndEvidence> V>
void fields(A& a, V& v) {
    a(v.method);
    a(v.status);
    a(v.detail);
    a(v.available);
}

template <class A, Of<carving::ValidationResult> V>
void fields(A& a, V& v) {
    a(v.status);
    a(v.validBytes);
    a(v.detail);
}

template <class A, Of<carving::FileCandidate> V>
void fields(A& a, V& v) {
    a(v.id);
    a(v.formatId);
    a(v.extension);
    a(v.sourceOffset);
    a(v.length);
    a(v.extraction);
    a(v.extents);
    a(v.signature);
    a(v.end);
    a(v.validation);
    a(v.unreadableRegions);
    a(v.warnings);
    if constexpr (A::kDecoding) {
        if (!a.failed()) {
            a.check(carving::validateFileCandidate(v));
        }
    }
}

template <class A, Of<carving::ScanReport> V>
void fields(A& a, V& v) {
    a(v.outcome);
    a(v.startOffset);
    a(v.endOffset);
    a(v.nextOffset);
    a(v.bytesRead);
    a(v.hits);
    a(v.hitsPerFormat);
    a(v.unreadableBytes);
    a(v.unreadableRegions);
    a(v.elapsed);
}

template <class A, Of<carving::CarveReport> V>
void fields(A& a, V& v) {
    a(v.scan);
    a(v.candidates);
    a(v.validation);
    a(v.rejected);
    a(v.skippedInsideCandidates);
    a(v.carveBytesRead);
    a(v.elapsed);
}

template <class A, Of<carving::CarveSkipState::Interval> V>
void fields(A& a, V& v) {
    a(v.start);
    a(v.end);
}

template <class A, Of<carving::CarveSkipState> V>
void fields(A& a, V& v) {
    a(v.trusted);
    a(v.streams);
}

// MP4.

template <class A, Of<formats::mp4::Issue> V>
void fields(A& a, V& v) {
    a(v.kind);
    a(v.offset);
    a(v.path);
    a(v.detail);
}

template <class A, Of<formats::mp4::MediaExtent> V>
void fields(A& a, V& v) {
    a(v.begin);
    a(v.end);
    a(v.samples);
    a(v.bytes);
}

template <class A, Of<Mp4TrackEvidence> V>
void fields(A& a, V& v) {
    a(v.number);
    a(v.kind);
    a(v.codec);
    a(v.width);
    a(v.height);
    a(v.channels);
    a(v.sampleRate);
    a(v.duration);
    a(v.timescale);
    a(v.samples);
    a(v.sampleBytes);
    a(v.samplesBeyondData);
    a(v.samplesMissing);
    a(v.samplesUnreadable);
    a(v.samplesReallocated);
    a(v.samplesIntact);
    a(v.samplesFramed);
    a(v.samplesMisframed);
}

template <class A, Of<Mp4Structure> V>
void fields(A& a, V& v) {
    a(v.status);
    a(v.detail);
    a(v.issues);
    a(v.issueCount);
    a(v.kind);
    a(v.kindReason);
    a(v.majorBrand);
    a(v.moovOffset);
    a(v.moovSize);
    a(v.moovFoundBySearch);
    a(v.moovBeforeMediaData);
    a(v.mediaDataBoxes);
    a(v.movieFragments);
    a(v.media);
    a(v.structureEnd);
    a(v.dataSize);
    a(v.tracks);
}

template <class A, Of<Mp4Allocation> V>
void fields(A& a, V& v) {
    a(v.filesystem);
    a(v.volumeOffset);
    a(v.clusters);
    a(v.freeClusters);
    a(v.allocatedClusters);
    a(v.otherClusters);
    a(v.complete);
    a(v.activeFiles);
    a(v.insideActiveFile);
}

template <class A, Of<Mp4Candidate> V>
void fields(A& a, V& v) {
    a(v.data);
    a(v.filesystemCandidate);
    a(v.recordedSize);
    a(v.carving);
    a(v.structure);
    a(v.allocation);
    a(v.warnings);
}

template <class A, Of<Mp4PendingCandidate> V>
void fields(A& a, V& v) {
    a(v.candidate);
    a(v.volume);
    a(v.examined);
    a(v.start);
}

template <class A, Of<Mp4Examination> V>
void fields(A& a, V& v) {
    a(v.volume);
    a(v.index);
    a(v.analysed);
    a(v.video);
    a(v.pending);
}

template <class A, Of<Mp4RecoveryReport> V>
void fields(A& a, V& v) {
    a(v.filesystemExamined);
    a(v.filesystemMp4);
    a(v.scan);
    a(v.carved);
    a(v.carvesRejected);
    a(v.hitsSkipped);
    a(v.carvesMerged);
    a(v.filesystem);
    a(v.carving);
    a(v.hybrid);
    a(v.valid);
    a(v.truncated);
    a(v.invalid);
    a(v.elapsed);
}

template <class A, Of<Mp4StepsChanges> V>
void fields(A& a, V& v) {
    a(v.pending);
    a(v.carved);
    a(v.trustedStart);
    a(v.trustedEnd);
    a(v.nextCarveId);
    a(v.report);
}

// Fragment reconstruction.

template <class A, Of<ClusterRun> V>
void fields(A& a, V& v) {
    a(v.firstCluster);
    a(v.count);
}

template <class A, Of<HypothesisEvidence> V>
void fields(A& a, V& v) {
    a(v.fragments);
    a(v.clusters);
    a(v.allocatedClusters);
    a(v.claimedClusters);
    a(v.placedBytes);
    a(v.missingBytes);
    a(v.unreadableBytes);
    a(v.dataChecked);
}

template <class A, Of<ReconstructionHypothesis> V>
void fields(A& a, V& v) {
    a(v.data);
    a(v.clusters);
    a(v.source);
    a(v.validation);
    a(v.evidence);
    a(v.mp4);
}

template <class A, Of<SearchStats> V>
void fields(A& a, V& v) {
    a(v.layoutsValidated);
    a(v.sampleProbes);
    a(v.bytesRead);
    a(v.complete);
    a(v.limit);
}

template <class A, Of<FragmentCandidate> V>
void fields(A& a, V& v) {
    a(v.id);
    a(v.status);
    a(v.reason);
    a(v.origin);
    a(v.formatId);
    a(v.name);
    a(v.path);
    a(v.filesystemCandidate);
    a(v.recordedSize);
    a(v.carve);
    a(v.filesystem);
    a(v.volumeOffset);
    a(v.clusterSize);
    a(v.hypotheses);
    a(v.tied);
    a(v.search);
}

template <class A, Of<FragmentRecoveryReport> V>
void fields(A& a, V& v) {
    a(v.filesystemSeeds);
    a(v.filesystemSkipped);
    a(v.scan);
    a(v.carved);
    a(v.carvingSeeds);
    a(v.carvesValid);
    a(v.complete);
    a(v.partial);
    a(v.corrupted);
    a(v.ambiguous);
    a(v.unrecoverable);
    a(v.searchesLimited);
    a(v.layoutsValidated);
    a(v.elapsed);
}

template <class A, Of<FragmentSeedExamination> V>
void fields(A& a, V& v) {
    a(v.volume);
    a(v.index);
    a(v.seed);
    a(v.skipped);
    a(v.formatId);
    a(v.unrecoverable);
}

template <class A, Of<FragmentPassEvent> V>
void fields(A& a, V& v) {
    a(v.fileOffset);
    a(v.formatId);
    a(v.signatureIndex);
    a(v.carve);
}

// Validation and evaluation.

template <class A, Of<validation::LevelResult> V>
void fields(A& a, V& v) {
    a(v.status);
    a(v.checker);
    a(v.coverage);
    a(v.offset);
    a(v.detail);
}

template <class A, Of<validation::ValidationState> V>
void fields(A& a, V& v) {
    a(v.structural);
    a(v.media);
    a(v.playability);
}

template <class A, Of<evaluation::PreliminaryHash> V>
void fields(A& a, V& v) {
    a(v.size);
    a(v.head);
    a(v.tail);
}

template <class A, Of<evaluation::ContentIdentity> V>
void fields(A& a, V& v) {
    a(v.size);
    a(v.sha256);
    a(v.preliminary);
}

template <class A, Of<evaluation::FragmentEvidence> V>
void fields(A& a, V& v) {
    a(v.fragmentCandidate);
    a(v.status);
    a(v.reason);
    a(v.origin);
    a(v.source);
    a(v.clusters);
    a(v.evidence);
    a(v.hypotheses);
    a(v.tied);
    a(v.alternative);
    a(v.search);
}

template <class A, Of<evaluation::AllocationEvidence> V>
void fields(A& a, V& v) {
    a(v.filesystem);
    a(v.volumeOffset);
    a(v.clusters);
    a(v.freeClusters);
    a(v.allocatedClusters);
    a(v.otherClusters);
    a(v.complete);
    a(v.activeFiles);
    a(v.insideActiveFile);
}

template <class A, Of<evaluation::EvaluatedCandidate> V>
void fields(A& a, V& v) {
    a(v.id);
    a(v.formatId);
    a(v.data);
    a(v.filesystemCandidate);
    a(v.mp4Candidate);
    a(v.carve);
    a(v.otherCarves);
    a(v.mp4);
    a(v.mp4Warnings);
    a(v.fragments);
    a(v.allocation);
    a(v.container);
    a(v.validation);
    a(v.identity);
    a(v.unreadableBytes);
    a(v.duplicateOf);
    a(v.warnings);
}

template <class A, Of<evaluation::EvaluationReport> V>
void fields(A& a, V& v) {
    a(v.filesystem);
    a(v.carving);
    a(v.hybrid);
    a(v.fragmented);
    a(v.valid);
    a(v.truncated);
    a(v.invalid);
    a(v.notValidated);
    a(v.duplicates);
    a(v.carvesMerged);
    a(v.carvesCompeting);
    a(v.superseded);
    a(v.alternatives);
    a(v.bytesHashed);
    a(v.elapsed);
}

// Scans.

template <class A, Of<scan::ScanIdentity> V>
void fields(A& a, V& v) {
    a(v.checkpointVersion);
    a(v.engineVersion);
    a(v.sourceType);
    a(v.sourcePath);
    a(v.sourceSize);
    a(v.sectorSize);
    a(v.mode);
    a(v.configuration);
    a(v.formats);
    a(v.mediaValidators);
}

template <class A, Of<scan::VolumeRecord> V>
void fields(A& a, V& v) {
    a(v.offset);
    a(v.size);
    a(v.partition);
    a(v.scanned);
    a(v.filesystem);
    a(v.candidates);
    a(v.error);
}

template <class A, Of<scan::PassState> V>
void fields(A& a, V& v) {
    a(v.position);
    a(v.scan);
    a(v.carving);
    a(v.skip);
    a(v.nextCarveId);
}

template <class A, Of<scan::ScanMetrics> V>
void fields(A& a, V& v) {
    a(v.sourceSize);
    a(v.bytesScanned);
    a(v.bytesRead);
    a(v.scanSpeed);
    a(v.filesFound);
    a(v.carves);
    a(v.mp4Candidates);
    a(v.fragmentCandidates);
    a(v.candidates);
    a(v.validationFailures);
    a(v.duplicates);
    a(v.unreadableBytes);
    a(v.elapsed);
}

template <class A, Of<scan::ScanUpdate> V>
void fields(A& a, V& v) {
    a(v.sequence);
    a(v.stage);
    a(v.stageComplete);
    a(v.identity);
    a(v.partitionTable);
    a(v.volumePlan);
    a(v.volumes);
    a(v.mp4Examinations);
    a(v.seedExaminations);
    a(v.pass);
    a(v.carves);
    a(v.mp4Changes);
    a(v.fragmentEvents);
    a(v.mp4Candidates);
    a(v.mp4Report);
    a(v.fragmentCandidates);
    a(v.fragmentReport);
    a(v.candidates);
    a(v.evaluationReport);
    a(v.unreadable);
    a(v.metrics);
}

// Every setting of a scan's configuration but the playability checker, which
// is not stored (SessionCreatedRecord::playability says whether there is one).
template <class A, Of<scan::ScanConfiguration> V>
void fields(A& a, V& v) {
    a(v.mode);
    a(v.carving);
    a(v.mp4);
    a(v.fragments);
    a(v.includeActive);
    a(v.includeDeleted);
    a(v.alignment);
    a(v.maxHits);
    a(v.sectorRetryCount);
    a(v.knownBadRegions);
    a(v.media);
    a(v.sha256);
    a(v.preliminaryHash);
    if constexpr (A::kDecoding) {
        v.playability = nullptr;
    }
}

// Recovery jobs.

template <class A, Of<ReconstructionReport> V>
void fields(A& a, V& v) {
    a(v.expectedSize);
    a(v.outputSize);
    a(v.storedBytes);
    a(v.embeddedBytes);
    a(v.zeroBytes);
    a(v.missingBytes);
    a(v.unreadableBytes);
    a(v.outsideSourceBytes);
    a(v.reallocatedBytes);
    a(v.unreadableRegions);
}

template <class A, Of<RecoveredFile> V>
void fields(A& a, V& v) {
    a(v.candidate);
    a(v.path);
    a(v.report);
}

template <class A, Of<scan::RecoveredItem> V>
void fields(A& a, V& v) {
    a(v.candidate);
    a(v.file);
    a(v.error);
}

template <class A, Of<scan::RecoveryJobMetrics> V>
void fields(A& a, V& v) {
    a(v.files);
    a(v.recoveredFiles);
    a(v.failedFiles);
    a(v.bytesRecovered);
    a(v.unreadableBytes);
    a(v.bytesRead);
    a(v.speed);
    a(v.elapsed);
}

template <class A, Of<scan::RecoveryJobUpdate> V>
void fields(A& a, V& v) {
    a(v.sequence);
    a(v.destination);
    a(v.items);
    a(v.complete);
    a(v.metrics);
}

// The session's own records.

template <class A, Of<SourceFingerprint> V>
void fields(A& a, V& v) {
    a(v.digest);
    a(v.bytes);
    a(v.unreadableBytes);
}

template <class A, Of<SessionSource> V>
void fields(A& a, V& v) {
    a(v.type);
    a(v.path);
    a(v.size);
    a(v.sectorSize);
    a(v.physicalSectorSize);
    a(v.diskNumber);
    a(v.vendor);
    a(v.product);
    a(v.removable);
    a(v.fingerprint);
}

template <class A, Of<SessionCreatedRecord> V>
void fields(A& a, V& v) {
    a(v.id);
    a(v.engineVersion);
    a(v.source);
    a(v.configuration);
    a(v.playability);
}

template <class A, Of<ScanStateRecord> V>
void fields(A& a, V& v) {
    a(v.state);
    a(v.engineVersion);
    a(v.error);
    a(v.stage);
    a(v.metrics);
}

template <class A, Of<JobCreatedRecord> V>
void fields(A& a, V& v) {
    a(v.job);
    a(v.destination);
    a(v.candidates);
}

template <class A, Of<JobStateRecord> V>
void fields(A& a, V& v) {
    a(v.job);
    a(v.state);
    a(v.engineVersion);
    a(v.error);
    a(v.metrics);
}

template <class A, Of<JobUpdateRecord> V>
void fields(A& a, V& v) {
    a(v.job);
    a(v.update);
}

template <class A, Of<FileStartedRecord> V>
void fields(A& a, V& v) {
    a(v.job);
    a(v.candidate);
    a(v.path);
}

template <class A, Of<DamageRecord> V>
void fields(A& a, V& v) {
    a(v.offset);
    a(v.bytesDropped);
    a(v.recordsDropped);
    a(v.reason);
    a(v.backup);
}

// ---------------------------------------------------------------------------

template <class T>
std::vector<std::byte> encodeValue(const T& value) {
    Encoder encoder;
    encoder(value);
    return std::move(encoder).take();
}

template <class T>
Result<RecordPayload> decodeValue(RecordType type, std::span<const std::byte> bytes) {
    Decoder decoder(bytes);
    T value{};
    decoder(value);
    decoder.finish();
    if (decoder.failed()) {
        return makeError(ErrorCode::InvalidFormat,
                         "session: a '" + std::string(toString(type)) + "' record that does not decode: " +
                             decoder.error() + " of " + std::to_string(bytes.size()));
    }
    return RecordPayload{std::move(value)};
}

}  // namespace

std::string_view toString(RecordType type) noexcept {
    switch (type) {
    case RecordType::SessionCreated:
        return "session-created";
    case RecordType::ScanState:
        return "scan-state";
    case RecordType::ScanUpdate:
        return "scan-update";
    case RecordType::JobCreated:
        return "job-created";
    case RecordType::JobState:
        return "job-state";
    case RecordType::JobUpdate:
        return "job-update";
    case RecordType::FileStarted:
        return "file-started";
    case RecordType::Damage:
        return "damage";
    }
    return "unknown";
}

RecordType typeOf(const RecordPayload& payload) noexcept {
    struct Visitor {
        RecordType operator()(const SessionCreatedRecord&) const noexcept { return RecordType::SessionCreated; }
        RecordType operator()(const ScanStateRecord&) const noexcept { return RecordType::ScanState; }
        RecordType operator()(const scan::ScanUpdate&) const noexcept { return RecordType::ScanUpdate; }
        RecordType operator()(const JobCreatedRecord&) const noexcept { return RecordType::JobCreated; }
        RecordType operator()(const JobStateRecord&) const noexcept { return RecordType::JobState; }
        RecordType operator()(const JobUpdateRecord&) const noexcept { return RecordType::JobUpdate; }
        RecordType operator()(const FileStartedRecord&) const noexcept { return RecordType::FileStarted; }
        RecordType operator()(const DamageRecord&) const noexcept { return RecordType::Damage; }
    };
    return std::visit(Visitor{}, payload);
}

std::vector<std::byte> encodePayload(const RecordPayload& payload) {
    return std::visit([](const auto& value) { return encodeValue(value); }, payload);
}

std::vector<std::byte> encodePayload(const SessionCreatedRecord& payload) {
    return encodeValue(payload);
}

std::vector<std::byte> encodePayload(const ScanStateRecord& payload) {
    return encodeValue(payload);
}

std::vector<std::byte> encodePayload(const scan::ScanUpdate& payload) {
    return encodeValue(payload);
}

std::vector<std::byte> encodePayload(const JobCreatedRecord& payload) {
    return encodeValue(payload);
}

std::vector<std::byte> encodePayload(const JobStateRecord& payload) {
    return encodeValue(payload);
}

std::vector<std::byte> encodePayload(const JobUpdateRecord& payload) {
    return encodeValue(payload);
}

std::vector<std::byte> encodePayload(const FileStartedRecord& payload) {
    return encodeValue(payload);
}

std::vector<std::byte> encodePayload(const DamageRecord& payload) {
    return encodeValue(payload);
}

Result<RecordPayload> decodePayload(RecordType type, std::span<const std::byte> bytes) {
    switch (type) {
    case RecordType::SessionCreated:
        return decodeValue<SessionCreatedRecord>(type, bytes);
    case RecordType::ScanState:
        return decodeValue<ScanStateRecord>(type, bytes);
    case RecordType::ScanUpdate:
        return decodeValue<scan::ScanUpdate>(type, bytes);
    case RecordType::JobCreated:
        return decodeValue<JobCreatedRecord>(type, bytes);
    case RecordType::JobState:
        return decodeValue<JobStateRecord>(type, bytes);
    case RecordType::JobUpdate:
        return decodeValue<JobUpdateRecord>(type, bytes);
    case RecordType::FileStarted:
        return decodeValue<FileStartedRecord>(type, bytes);
    case RecordType::Damage:
        return decodeValue<DamageRecord>(type, bytes);
    }
    return makeError(ErrorCode::InvalidInput,
                     "session: no record type " + std::to_string(static_cast<unsigned>(type)));
}

}  // namespace recovery::session
