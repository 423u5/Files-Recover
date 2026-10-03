#include "format_test_helpers.hpp"

#include "carving/format_registry.hpp"
#include "carving/signature_scanner.hpp"
#include "formats/audio_formats.hpp"
#include "formats/image_formats.hpp"
#include "formats/video_formats.hpp"
#include "support/test_files.hpp"

#include <algorithm>
#include <cstdlib>
#include <random>

namespace recovery::formats::testing {

using carving::EndDetection;
using carving::EndStatus;
using carving::HeaderCheck;
using carving::IFileFormat;
using carving::MemoryContentReader;
using carving::ValidationResult;
using carving::ValidationStatus;

HeaderCheck headerOf(const IFileFormat& format, std::span<const std::byte> data) {
    return format.checkHeader(data.first(std::min<std::size_t>(data.size(), format.descriptor().headerSize)));
}

EndDetection endOf(const IFileFormat& format, std::span<const std::byte> data) {
    MemoryContentReader content(data);
    Result<EndDetection> end = format.findEnd(content);
    EXPECT_TRUE(end.ok()) << describe(end.error());
    return end.ok() ? *end : EndDetection{};
}

ValidationResult verdictOf(const IFileFormat& format, std::span<const std::byte> data) {
    MemoryContentReader content(data);
    Result<ValidationResult> result = format.validator().validate(content);
    EXPECT_TRUE(result.ok()) << describe(result.error());
    return result.ok() ? *result : ValidationResult{};
}

std::string describe(const EndDetection& end) {
    return std::string(toString(end.status)) + " at " + std::to_string(end.length) + " (" + end.detail + ")";
}

std::string describe(const ValidationResult& result) {
    return std::string(toString(result.status)) + ", " + std::to_string(result.validBytes) + " valid bytes (" +
           result.detail + ")";
}

::testing::AssertionResult isIntact(const IFileFormat& format, std::span<const std::byte> file) {
    const auto& signatures = format.descriptor().signatures;
    const bool signed_ = std::any_of(signatures.begin(), signatures.end(), [&](const carving::FileSignature& s) {
        return file.size() >= s.reach() && s.matches(file.subspan(s.offset));
    });
    if (!signed_) {
        return ::testing::AssertionFailure() << "no signature of the format at the start";
    }
    const HeaderCheck header = headerOf(format, file);
    if (!header.plausible) {
        return ::testing::AssertionFailure() << "header rejected: " << header.reason;
    }
    const EndDetection end = endOf(format, file);
    if (end.status != EndStatus::Found || end.length != file.size()) {
        return ::testing::AssertionFailure() << "end of " << file.size() << " bytes: " << describe(end);
    }
    // More data after the file does not move its end.
    const Bytes more = concat({file, noise(3000, file.size())});
    const EndDetection followed = endOf(format, more);
    if (followed.status != EndStatus::Found || followed.length != file.size()) {
        return ::testing::AssertionFailure() << "end with data after the file: " << describe(followed);
    }
    const ValidationResult verdict = verdictOf(format, file);
    if (verdict.status != ValidationStatus::Valid || verdict.validBytes != file.size()) {
        return ::testing::AssertionFailure() << "validation of " << file.size() << " bytes: " << describe(verdict);
    }
    return ::testing::AssertionSuccess() << describe(verdict);
}

::testing::AssertionResult prefixesAreTruncated(const IFileFormat& format, std::span<const std::byte> file,
                                                std::span<const std::size_t> completeAt, std::size_t rejectedBelow) {
    std::vector<std::size_t> lengths;
    for (std::size_t length = 0; length < file.size(); ++length) {
        if (length < 4096 || length % 97 == 0 || file.size() - length <= 64) {
            lengths.push_back(length);
        }
    }
    for (const std::size_t length : completeAt) {
        const std::span<const std::byte> part = file.first(length);
        const EndDetection end = endOf(format, part);
        const ValidationResult verdict = verdictOf(format, part);
        if (end.status != EndStatus::Found || end.length != length || verdict.status != ValidationStatus::Valid) {
            return ::testing::AssertionFailure() << "the first " << length << " bytes are a complete file, but: "
                                                 << describe(end) << "; " << describe(verdict);
        }
    }
    for (const std::size_t length : lengths) {
        if (std::find(completeAt.begin(), completeAt.end(), length) != completeAt.end()) {
            continue;
        }
        const std::span<const std::byte> part = file.first(length);
        const EndDetection end = endOf(format, part);
        if (length < rejectedBelow && end.status == EndStatus::Broken && end.length == 0) {
            if (verdictOf(format, part).status == ValidationStatus::Valid) {
                return ::testing::AssertionFailure() << "the first " << length << " bytes are rejected but valid";
            }
            continue;
        }
        if (end.status != EndStatus::Truncated || end.length != length) {
            return ::testing::AssertionFailure() << "end of the first " << length << " bytes: " << describe(end);
        }
        const ValidationResult verdict = verdictOf(format, part);
        if (verdict.status != ValidationStatus::Truncated || verdict.validBytes > length) {
            return ::testing::AssertionFailure() << "validation of the first " << length
                                                 << " bytes: " << describe(verdict);
        }
    }
    return ::testing::AssertionSuccess();
}

::testing::AssertionResult isInvalid(const IFileFormat& format, std::span<const std::byte> file,
                                     std::optional<std::uint64_t> validBytes) {
    const ValidationResult verdict = verdictOf(format, file);
    if (verdict.status != ValidationStatus::Invalid) {
        return ::testing::AssertionFailure() << "validation: " << describe(verdict);
    }
    if (validBytes.has_value() && verdict.validBytes > *validBytes) {
        return ::testing::AssertionFailure() << "more than " << *validBytes << " valid bytes: " << describe(verdict);
    }
    return ::testing::AssertionSuccess() << describe(verdict);
}

Bytes concat(std::initializer_list<std::span<const std::byte>> parts) {
    Bytes out;
    for (const std::span<const std::byte> part : parts) {
        out.insert(out.end(), part.begin(), part.end());
    }
    return out;
}

Bytes overwritten(Bytes file, std::size_t offset, std::initializer_list<std::uint8_t> bytes) {
    for (const std::uint8_t byte : bytes) {
        file.at(offset++) = static_cast<std::byte>(byte);
    }
    return file;
}

Bytes overwritten(Bytes file, std::size_t offset, std::span<const std::byte> bytes) {
    for (const std::byte byte : bytes) {
        file.at(offset++) = byte;
    }
    return file;
}

Bytes inserted(const Bytes& file, std::size_t offset, std::span<const std::byte> bytes) {
    Bytes out(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(offset));
    out.insert(out.end(), bytes.begin(), bytes.end());
    out.insert(out.end(), file.begin() + static_cast<std::ptrdiff_t>(offset), file.end());
    return out;
}

Bytes erased(const Bytes& file, std::size_t offset, std::size_t count) {
    Bytes out = file;
    out.erase(out.begin() + static_cast<std::ptrdiff_t>(offset),
              out.begin() + static_cast<std::ptrdiff_t>(offset + count));
    return out;
}

Bytes prefix(const Bytes& file, std::size_t length) {
    return Bytes(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(length));
}

Bytes quietNoise(std::size_t size, std::uint64_t seed) {
    Bytes bytes = test::makePattern(size, seed);
    for (std::byte& byte : bytes) {
        switch (static_cast<std::uint8_t>(byte)) {
        case 0xFF:
        case 0x89:
        case 'R':
        case 'G':
        case 'B':
            byte = static_cast<std::byte>(static_cast<std::uint8_t>(byte) - 1);
            break;
        default:
            break;
        }
    }
    return bytes;
}

Bytes quietAudioNoise(std::size_t size, std::uint64_t seed) {
    Bytes bytes = test::makePattern(size, seed);
    for (std::byte& byte : bytes) {
        switch (static_cast<std::uint8_t>(byte)) {
        case 0xFF:
        case 'I':
        case 'R':
        case 'f':
            byte = static_cast<std::byte>(static_cast<std::uint8_t>(byte) - 1);
            break;
        default:
            break;
        }
    }
    return bytes;
}

Bytes noise(std::size_t size, std::uint64_t seed) {
    return test::makePattern(size, seed);
}

void fuzz(const IFileFormat& format, const Bytes& file, int iterations, std::uint64_t seed) {
    std::mt19937_64 random(seed);
    const auto pick = [&](std::size_t bound) {
        return static_cast<std::size_t>(random() % std::max<std::size_t>(bound, 1));
    };
    for (int i = 0; i < iterations; ++i) {
        Bytes damaged = file;
        switch (i % 4) {
        case 0:  // a few bytes replaced
            for (int n = 0; n <= static_cast<int>(pick(4)); ++n) {
                damaged[pick(damaged.size())] = static_cast<std::byte>(random() & 0xFF);
            }
            break;
        case 1:  // cut short, then damaged
            damaged.resize(pick(damaged.size()));
            if (!damaged.empty()) {
                damaged[pick(damaged.size())] = static_cast<std::byte>(random() & 0xFF);
            }
            break;
        case 2: {  // a length-like field inflated: 0xFF bytes in a row
            const std::size_t at = pick(damaged.size());
            for (std::size_t k = at; k < std::min(damaged.size(), at + 4); ++k) {
                damaged[k] = std::byte{0xFF};
            }
            break;
        }
        default: {  // a run of zeros or noise
            const std::size_t at = pick(damaged.size());
            const std::size_t count = std::min(damaged.size() - at, pick(64) + 1);
            const Bytes junk = noise(count, seed + static_cast<std::uint64_t>(i));
            for (std::size_t k = 0; k < count; ++k) {
                damaged[at + k] = random() % 2 == 0 ? std::byte{0} : junk[k];
            }
            break;
        }
        }
        (void)headerOf(format, damaged);
        const EndDetection end = endOf(format, damaged);
        ASSERT_LE(end.length, damaged.size()) << "iteration " << i << ": " << describe(end);
        const ValidationResult verdict = verdictOf(format, damaged);
        ASSERT_LE(verdict.validBytes, damaged.size()) << "iteration " << i << ": " << describe(verdict);
        ASSERT_NE(verdict.status, ValidationStatus::NotValidated);
    }
}

Carved carveImages(storage::IStorageSource& source, carving::CarveOptions options) {
    return carve(source, Formats::Images, std::move(options));
}

Carved carveAudio(storage::IStorageSource& source, carving::CarveOptions options) {
    return carve(source, Formats::Audio, std::move(options));
}

Carved carve(storage::IStorageSource& source, Formats formats, carving::CarveOptions options) {
    Carved carved;
    carving::FormatRegistry registry;
    if (formats == Formats::Images || formats == Formats::All || formats == Formats::Everything) {
        EXPECT_TRUE(registerImageFormats(registry).ok());
    }
    if (formats == Formats::Audio || formats == Formats::All || formats == Formats::Everything) {
        EXPECT_TRUE(registerAudioFormats(registry).ok());
    }
    if (formats == Formats::Video || formats == Formats::Everything) {
        EXPECT_TRUE(registerVideoFormats(registry).ok());
    }
    Result<carving::SignatureScanner> scanner = carving::SignatureScanner::create(registry);
    EXPECT_TRUE(scanner.ok());
    if (!scanner.ok()) {
        return carved;
    }
    carving::FileCarver carver(source, std::move(options));
    Result<carving::CarveReport> report = carver.run(*scanner, [&](carving::FileCandidate&& candidate) {
        const Status valid = carving::validateFileCandidate(candidate);
        EXPECT_TRUE(valid.ok()) << describe(valid.error());
        carved.candidates.push_back(std::move(candidate));
        return success();
    });
    EXPECT_TRUE(report.ok()) << describe(report.error());
    if (report.ok()) {
        carved.report = std::move(*report);
    }
    return carved;
}

Bytes carvedBytes(storage::IStorageSource& source, const carving::FileCandidate& candidate) {
    Bytes bytes(static_cast<std::size_t>(candidate.length));
    for (const carving::CarvedExtent& extent : candidate.extents) {
        const std::span<std::byte> target(bytes.data() + extent.fileOffset, static_cast<std::size_t>(extent.length));
        const Status read = source.readExact(ByteOffset{extent.sourceOffset}, target);
        EXPECT_TRUE(read.ok()) << describe(read.error());
    }
    return bytes;
}

std::string describe(const carving::FileCandidate& candidate) {
    std::string text = "#" + std::to_string(candidate.id.value()) + " " + candidate.formatId + " @" +
                       std::to_string(candidate.sourceOffset) + " +" + std::to_string(candidate.length) + " end " +
                       std::string(toString(candidate.end.status)) + " (" + candidate.end.detail + ") validation " +
                       describe(candidate.validation);
    for (const carving::CarveWarning warning : candidate.warnings) {
        text += " " + std::string(toString(warning));
    }
    return text;
}

std::optional<std::filesystem::path> directoryFromEnvironment(const wchar_t* name, bool mustExist) {
    wchar_t* value = nullptr;
    std::size_t length = 0;
    if (_wdupenv_s(&value, &length, name) != 0 || value == nullptr) {
        return std::nullopt;
    }
    const std::filesystem::path path(value);
    std::free(value);
    std::error_code ec;
    if (mustExist && !std::filesystem::is_directory(path, ec)) {
        return std::nullopt;
    }
    return path;
}

}  // namespace recovery::formats::testing
