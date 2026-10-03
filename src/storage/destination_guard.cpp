#include "storage/destination_guard.hpp"

#include "recovery/text.hpp"

#include <algorithm>
#include <cwctype>
#include <string>

namespace recovery::storage {

namespace {

bool equalIgnoreCase(const std::wstring& a, const std::wstring& b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                      [](wchar_t x, wchar_t y) { return std::towlower(x) == std::towlower(y); });
}

// Windows paths are case-insensitive; compare normalized absolute forms.
bool samePathLexically(const std::filesystem::path& a, const std::filesystem::path& b) {
    std::error_code ec;
    const auto absA = std::filesystem::absolute(a, ec).lexically_normal();
    if (ec) {
        return false;
    }
    const auto absB = std::filesystem::absolute(b, ec).lexically_normal();
    if (ec) {
        return false;
    }
    return equalIgnoreCase(absA.native(), absB.native());
}

std::filesystem::path nearestExistingAncestor(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::path current = std::filesystem::absolute(path, ec);
    if (ec) {
        return {};
    }
    // Bounded by the number of path components.
    while (!current.empty()) {
        if (std::filesystem::exists(current, ec)) {
            return current;
        }
        const std::filesystem::path parent = current.parent_path();
        if (parent == current) {
            break;
        }
        current = parent;
    }
    return {};
}

}  // namespace

Status checkDestinationSafety(const SourceInfo& source, const std::filesystem::path& destination,
                              const DiskResolver& resolver) {
    if (destination.empty()) {
        return makeError(ErrorCode::InvalidInput, "destination path is empty");
    }
    if (isDeviceNamespacePath(destination)) {
        return makeError(ErrorCode::DestinationError,
                         "destination must be a regular file path, not a device: " + toUtf8(destination));
    }

    switch (source.type) {
    case SourceType::DiskImage: {
        if (samePathLexically(source.path, destination)) {
            return makeError(ErrorCode::DestinationError, "destination is the source image itself");
        }
        std::error_code ec;
        if (std::filesystem::exists(destination, ec) && std::filesystem::equivalent(source.path, destination, ec)) {
            return makeError(ErrorCode::DestinationError, "destination refers to the source image file");
        }
        return success();
    }
    case SourceType::PhysicalDisk: {
        if (!source.diskNumber.has_value()) {
            return makeError(ErrorCode::InternalError, "physical source has no disk number");
        }
        if (!resolver) {
            return makeError(ErrorCode::DestinationError, "no disk resolver available to verify the destination");
        }
        const std::filesystem::path existing = nearestExistingAncestor(destination);
        if (existing.empty()) {
            return makeError(ErrorCode::DestinationError,
                             "destination location does not exist: " + toUtf8(destination));
        }
        const Result<std::vector<std::uint32_t>> disks = resolver(existing);
        if (!disks.ok()) {
            return makeError(ErrorCode::DestinationError,
                             "cannot verify that the destination is on a different disk than the source: " +
                                 disks.error().message,
                             disks.error().systemErrorCode);
        }
        const auto& numbers = disks.value();
        if (std::find(numbers.begin(), numbers.end(), *source.diskNumber) != numbers.end()) {
            return makeError(ErrorCode::DestinationError,
                             "destination is on the source disk (PhysicalDrive" +
                                 std::to_string(*source.diskNumber) +
                                 "); writing there could overwrite recoverable data");
        }
        return success();
    }
    case SourceType::Synthetic:
        return success();
    }
    return makeError(ErrorCode::InternalError, "unknown source type");
}

}  // namespace recovery::storage
