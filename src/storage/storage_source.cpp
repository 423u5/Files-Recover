#include "storage/storage_source.hpp"

#include "recovery/checked_math.hpp"

#include <bit>
#include <cwctype>
#include <string>

namespace recovery::storage {

namespace {

bool startsWithIgnoreCase(std::wstring_view text, std::wstring_view prefix) noexcept {
    if (text.size() < prefix.size()) {
        return false;
    }
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (std::towupper(text[i]) != std::towupper(prefix[i])) {
            return false;
        }
    }
    return true;
}

bool isSeparator(wchar_t c) noexcept {
    return c == L'\\' || c == L'/';
}

std::string describeRange(const ReadResult& result) {
    return "at offset " + std::to_string(result.offset) + " (" + std::to_string(result.requestedBytes) +
           " bytes requested)";
}

}  // namespace

std::string_view toString(SourceType type) noexcept {
    switch (type) {
    case SourceType::PhysicalDisk:
        return "PhysicalDisk";
    case SourceType::DiskImage:
        return "DiskImage";
    case SourceType::Synthetic:
        return "Synthetic";
    }
    return "Unknown";
}

std::optional<SourceType> parseSourceType(std::string_view text) noexcept {
    if (text == "PhysicalDisk") {
        return SourceType::PhysicalDisk;
    }
    if (text == "DiskImage") {
        return SourceType::DiskImage;
    }
    if (text == "Synthetic") {
        return SourceType::Synthetic;
    }
    return std::nullopt;
}

std::string_view toString(ReadStatus status) noexcept {
    switch (status) {
    case ReadStatus::Success:
        return "Success";
    case ReadStatus::PartialRead:
        return "PartialRead";
    case ReadStatus::OutOfRange:
        return "OutOfRange";
    case ReadStatus::InvalidArgument:
        return "InvalidArgument";
    case ReadStatus::NotOpen:
        return "NotOpen";
    case ReadStatus::IoError:
        return "IoError";
    case ReadStatus::InternalError:
        return "InternalError";
    }
    return "Unknown";
}

Error toError(const ReadResult& result) {
    switch (result.status) {
    case ReadStatus::Success:
        return makeError(ErrorCode::InternalError, "toError() called for a successful read");
    case ReadStatus::PartialRead:
        return makeError(ErrorCode::IoError,
                         "partial read: " + std::to_string(result.bytesRead) + " bytes returned " +
                             describeRange(result),
                         result.systemErrorCode);
    case ReadStatus::OutOfRange:
        return makeError(ErrorCode::InvalidInput, "read outside the source " + describeRange(result));
    case ReadStatus::InvalidArgument:
        return makeError(ErrorCode::InvalidInput, "invalid read request " + describeRange(result));
    case ReadStatus::NotOpen:
        return makeError(ErrorCode::InvalidInput, "source is not open");
    case ReadStatus::IoError:
        return makeError(ErrorCode::IoError, "device read failed " + describeRange(result), result.systemErrorCode);
    case ReadStatus::InternalError:
        return makeError(ErrorCode::InternalError, "internal read failure " + describeRange(result),
                         result.systemErrorCode);
    }
    return makeError(ErrorCode::InternalError, "unknown read status");
}

bool isValidSectorSize(std::uint64_t sectorSize) noexcept {
    return sectorSize >= kMinSectorSize && sectorSize <= kMaxSectorSize && std::has_single_bit(sectorSize);
}

bool isDeviceNamespacePath(const std::filesystem::path& path) {
    const std::wstring_view text = path.native();
    if (text.size() >= 4 && isSeparator(text[0]) && isSeparator(text[1]) && text[2] == L'.' &&
        isSeparator(text[3])) {
        return true;  // \\.\X
    }
    if (startsWithIgnoreCase(text, L"\\??\\")) {
        return true;  // NT object-manager path
    }
    if (text.size() >= 4 && isSeparator(text[0]) && isSeparator(text[1]) && text[2] == L'?' &&
        isSeparator(text[3])) {
        // \\?\ long-path prefix: only \\?\C:\... and \\?\UNC\... name files.
        const std::wstring_view rest = text.substr(4);
        const bool driveLetter = rest.size() >= 3 && std::iswalpha(rest[0]) != 0 && rest[1] == L':' &&
                                 isSeparator(rest[2]);
        const bool unc = rest.size() >= 4 && startsWithIgnoreCase(rest, L"UNC") && isSeparator(rest[3]);
        return !(driveLetter || unc);
    }
    return false;
}

ReadResult IStorageSource::read(ByteOffset offset, std::span<std::byte> buffer) {
    ReadResult result;
    result.requestedBytes = buffer.size();
    result.offset = offset.value();

    if (!isOpen()) {
        result.status = ReadStatus::NotOpen;
        return result;
    }
    if (buffer.size() > kMaxReadSize || offset.value() > kMaxAddressableOffset) {
        result.status = ReadStatus::InvalidArgument;
        return result;
    }
    if (!rangeWithin<std::uint64_t>(offset.value(), buffer.size(), size())) {
        result.status = ReadStatus::OutOfRange;
        return result;
    }
    if (buffer.empty()) {
        result.status = ReadStatus::Success;
        return result;
    }

    ReadResult inner = readValidated(offset.value(), buffer);

    // Never trust the implementation to describe the request correctly.
    inner.requestedBytes = buffer.size();
    inner.offset = offset.value();
    if (inner.bytesRead > buffer.size()) {
        inner.status = ReadStatus::InternalError;
        inner.bytesRead = 0;
        return inner;
    }
    if (inner.status == ReadStatus::Success && inner.bytesRead != buffer.size()) {
        inner.status = ReadStatus::PartialRead;
    }
    return inner;
}

Status IStorageSource::readExact(ByteOffset offset, std::span<std::byte> buffer) {
    const ReadResult result = read(offset, buffer);
    if (result.ok()) {
        return success();
    }
    return toError(result);
}

ReadResult IStorageSource::readSectors(SectorNumber first, SectorCount count, std::span<std::byte> buffer) {
    ReadResult result;
    if (!isOpen()) {
        result.status = ReadStatus::NotOpen;
        return result;
    }

    const std::uint64_t sector = sectorSize();
    const std::optional<std::uint64_t> byteCount = checkedMul(count.value(), sector);
    const std::optional<std::uint64_t> byteOffset = checkedMul(first.value(), sector);
    result.offset = byteOffset.value_or(0);
    if (!byteCount.has_value() || !byteOffset.has_value()) {
        result.status = ReadStatus::InvalidArgument;
        return result;
    }
    if (*byteCount > buffer.size()) {
        result.requestedBytes = buffer.size();
        result.status = ReadStatus::InvalidArgument;
        return result;
    }
    return read(ByteOffset{*byteOffset}, buffer.first(static_cast<std::size_t>(*byteCount)));
}

}  // namespace recovery::storage
