#pragma once

// Formats that exist only in the tests. They exercise the carving framework
// without any real file format (those are P9 onwards), and they live outside
// src/ on purpose: registering them needs no change to the scanner or the
// carver.
//
//  * SizedFormat ("sized"): magic "SZD1", a 32-bit total length and a CRC-32
//    of the payload. End detection: SizeField. Validation: the CRC.
//  * MarkerFormat ("marker"): magic "<MK>", a version byte, a body of
//    non-zero bytes and the end marker "</MK>". End detection: EndMarker.
//  * BoxFormat ("box"): a sequence of boxes [size LE32][type], the first of
//    type "tbox" (so the signature is at offset 4), the last of type "tend".
//    End detection: StructureWalk.
//  * SyncFormat ("sync"): the masked signature FF Ex; no end information
//    (EndDetectionMethod::None), the length is always an estimate.
//  * ScriptedFormat: descriptor and behaviour supplied by the test, for
//    hostile or broken format modules.

#include "carving/file_format.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace recovery::test {

class SizedFormat final : public carving::IFileFormat, public carving::FormatValidator {
public:
    static constexpr std::size_t kHeaderSize = 12;

    explicit SizedFormat(std::uint64_t maximumSize = 1024 * 1024);

    const carving::FormatDescriptor& descriptor() const noexcept override { return descriptor_; }
    carving::HeaderCheck checkHeader(std::span<const std::byte> header) const override;
    Result<carving::EndDetection> findEnd(carving::IContentReader& content) const override;
    const carving::FormatValidator& validator() const noexcept override { return *this; }
    Result<carving::ValidationResult> validate(carving::IContentReader& content) const override;

private:
    carving::FormatDescriptor descriptor_;
};

// A SizedFormat file with `payloadSize` payload bytes (deterministic pattern).
[[nodiscard]] std::vector<std::byte> makeSizedFile(std::size_t payloadSize, std::uint64_t seed = 1);
// A SizedFormat file around `payload`, used as it is (for example another file embedded in it).
[[nodiscard]] std::vector<std::byte> makeSizedFileAround(const std::vector<std::byte>& payload);

class MarkerFormat final : public carving::IFileFormat, public carving::FormatValidator {
public:
    static constexpr std::size_t kHeaderSize = 5;  // "<MK>" + version

    explicit MarkerFormat(std::uint64_t maximumSize = 1024 * 1024);

    const carving::FormatDescriptor& descriptor() const noexcept override { return descriptor_; }
    carving::HeaderCheck checkHeader(std::span<const std::byte> header) const override;
    Result<carving::EndDetection> findEnd(carving::IContentReader& content) const override;
    const carving::FormatValidator& validator() const noexcept override { return *this; }
    Result<carving::ValidationResult> validate(carving::IContentReader& content) const override;

private:
    carving::FormatDescriptor descriptor_;
};

// A MarkerFormat file with `bodySize` body bytes: never zero, and never
// containing '<', so neither the magic nor the end marker occurs inside.
[[nodiscard]] std::vector<std::byte> makeMarkerFile(std::size_t bodySize, std::uint64_t seed = 2);
[[nodiscard]] std::vector<std::byte> markerEnd();

class BoxFormat final : public carving::IFileFormat, public carving::FormatValidator {
public:
    explicit BoxFormat(std::uint64_t maximumSize = 1024 * 1024);

    const carving::FormatDescriptor& descriptor() const noexcept override { return descriptor_; }
    carving::HeaderCheck checkHeader(std::span<const std::byte> header) const override;
    Result<carving::EndDetection> findEnd(carving::IContentReader& content) const override;
    const carving::FormatValidator& validator() const noexcept override { return *this; }
    Result<carving::ValidationResult> validate(carving::IContentReader& content) const override;

private:
    carving::FormatDescriptor descriptor_;
};

// A BoxFormat file: a "tbox" box with `firstPayload` bytes, one "data" box
// per entry of `dataPayloads`, and the closing "tend" box.
[[nodiscard]] std::vector<std::byte> makeBoxFile(std::size_t firstPayload, const std::vector<std::size_t>& dataPayloads,
                                                 std::uint64_t seed = 3);

class SyncFormat final : public carving::IFileFormat, public carving::FormatValidator {
public:
    static constexpr std::uint64_t kEstimate = 4096;

    SyncFormat();

    const carving::FormatDescriptor& descriptor() const noexcept override { return descriptor_; }
    carving::HeaderCheck checkHeader(std::span<const std::byte> header) const override;
    Result<carving::EndDetection> findEnd(carving::IContentReader& content) const override;
    const carving::FormatValidator& validator() const noexcept override { return *this; }
    Result<carving::ValidationResult> validate(carving::IContentReader& content) const override;

private:
    carving::FormatDescriptor descriptor_;
};

// A format whose every step is a function supplied by the test. Steps left
// empty accept the header, report Found at content.size(), and validate as Valid.
class ScriptedFormat final : public carving::IFileFormat, public carving::FormatValidator {
public:
    using HeaderStep = std::function<carving::HeaderCheck(std::span<const std::byte>)>;
    using EndStep = std::function<Result<carving::EndDetection>(carving::IContentReader&)>;
    using ValidateStep = std::function<Result<carving::ValidationResult>(carving::IContentReader&)>;

    explicit ScriptedFormat(carving::FormatDescriptor descriptor);

    ScriptedFormat& onHeader(HeaderStep step);
    ScriptedFormat& onEnd(EndStep step);
    ScriptedFormat& onValidate(ValidateStep step);

    const carving::FormatDescriptor& descriptor() const noexcept override { return descriptor_; }
    carving::HeaderCheck checkHeader(std::span<const std::byte> header) const override;
    Result<carving::EndDetection> findEnd(carving::IContentReader& content) const override;
    const carving::FormatValidator& validator() const noexcept override { return *this; }
    Result<carving::ValidationResult> validate(carving::IContentReader& content) const override;

private:
    carving::FormatDescriptor descriptor_;
    HeaderStep header_;
    EndStep end_;
    ValidateStep validate_;
};

// A valid descriptor for ScriptedFormat: id `id`, the text signature `magic`
// at `offset`, minimum size max(reach, 16), maximum size `maximumSize`.
[[nodiscard]] carving::FormatDescriptor scriptedDescriptor(std::string id, std::string_view magic,
                                                           std::uint64_t maximumSize = 64 * 1024,
                                                           std::uint32_t offset = 0);

[[nodiscard]] std::vector<std::byte> bytesOf(std::string_view text);

}  // namespace recovery::test
