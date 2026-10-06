#include "recovery/recovery_writer.hpp"

#include "recovery/output_names.hpp"
#include "recovery/text.hpp"
#include "storage/destination_file.hpp"

#include <cwctype>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace recovery {

namespace {

using diagnostics::field;
using diagnostics::LogLevel;

constexpr std::string_view kComponent = "recovery";

// The \\?\ form of an absolute, normalised path, so that paths longer than
// MAX_PATH work. Such paths are not normalised again by Windows, which is
// why every component written below it comes from safeFileName.
std::filesystem::path withLongPathPrefix(const std::filesystem::path& absolute) {
    const std::wstring& text = absolute.native();
    if (text.starts_with(L"\\\\?\\")) {
        return absolute;
    }
    if (text.size() >= 3 && std::iswalpha(text[0]) != 0 && text[1] == L':' && text[2] == L'\\') {
        return std::filesystem::path(L"\\\\?\\" + text);
    }
    if (text.starts_with(L"\\\\") && text.size() > 2 && text[2] != L'.' && text[2] != L'?') {
        return std::filesystem::path(L"\\\\?\\UNC\\" + text.substr(2));
    }
    return absolute;
}

// A directory that is neither a symbolic link nor a junction.
bool isPlainDirectory(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::symlink_status(path, ec).type() == std::filesystem::file_type::directory && !ec;
}

// Something (file, directory, link) already holds this name.
bool nameTaken(const std::filesystem::path& path) {
    std::error_code ec;
    const std::filesystem::file_type type = std::filesystem::symlink_status(path, ec).type();
    return type != std::filesystem::file_type::not_found && type != std::filesystem::file_type::none;
}

// The directory part of a candidate's original path. The name is removed as
// a whole, since a damaged name may itself contain '/'.
std::string_view parentPathOf(const RecoveryCandidate& candidate) {
    const std::string_view path = candidate.filesystemEvidence.path;
    const std::string_view name = candidate.filename;
    if (path.size() > name.size() && path.ends_with(name) && path[path.size() - name.size() - 1] == '/') {
        return path.substr(0, path.size() - name.size() - 1);
    }
    const std::size_t slash = path.rfind('/');
    return slash == std::string_view::npos ? std::string_view{} : path.substr(0, slash);
}

}  // namespace

RecoveryWriter::RecoveryWriter(storage::IStorageSource& source, std::filesystem::path destination,
                               std::filesystem::path ioRoot, RecoveryWriterOptions options)
    : source_(&source),
      sourceInfo_(source.getInfo()),
      destination_(std::move(destination)),
      ioRoot_(std::move(ioRoot)),
      options_(std::move(options)) {}

Result<RecoveryWriter> RecoveryWriter::create(storage::IStorageSource& source, std::filesystem::path destination,
                                              RecoveryWriterOptions options) {
    if (!source.isOpen()) {
        return makeError(ErrorCode::InvalidInput, "recovery source is not open");
    }
    if (destination.empty()) {
        return makeError(ErrorCode::InvalidInput, "destination path is empty");
    }
    if (storage::isDeviceNamespacePath(destination)) {
        return makeError(ErrorCode::DestinationError, "refusing to write to device path '" + toUtf8(destination) + "'");
    }
    std::error_code ec;
    std::filesystem::path absolute = std::filesystem::absolute(destination, ec).lexically_normal();
    if (ec) {
        return makeError(ErrorCode::DestinationError, "cannot resolve destination '" + toUtf8(destination) + "'",
                         static_cast<std::uint32_t>(ec.value()));
    }
    if (!absolute.has_filename() && absolute.has_relative_path()) {
        absolute = absolute.parent_path();  // drop a trailing separator
    }
    if (!options.diskResolver) {
        options.diskResolver = storage::makePlatformDiskResolver();
    }

    const storage::SourceInfo info = source.getInfo();
    // Checked before anything is created, and again once the directory exists.
    if (Status safe = storage::checkDestinationSafety(info, absolute, options.diskResolver); !safe.ok()) {
        return safe.error();
    }
    const std::filesystem::path ioRoot = withLongPathPrefix(absolute);
    std::filesystem::create_directories(ioRoot, ec);
    if (ec) {
        return makeError(ErrorCode::DestinationError, "cannot create destination '" + toUtf8(absolute) + "'",
                         static_cast<std::uint32_t>(ec.value()));
    }
    if (!isPlainDirectory(ioRoot)) {
        return makeError(ErrorCode::DestinationError,
                         "destination '" + toUtf8(absolute) + "' is not a plain directory (a file or a link)");
    }
    if (Status safe = storage::checkDestinationSafety(info, absolute, options.diskResolver); !safe.ok()) {
        return safe.error();
    }
    return RecoveryWriter(source, std::move(absolute), ioRoot, std::move(options));
}

Status RecoveryWriter::checkSafety(const std::filesystem::path& path) const {
    return storage::checkDestinationSafety(sourceInfo_, path, options_.diskResolver);
}

Result<std::filesystem::path> RecoveryWriter::directoryFor(const RecoveryCandidate& candidate) {
    if (!options_.preserveDirectories) {
        return std::filesystem::path{};
    }
    const std::string_view parent = parentPathOf(candidate);
    if (const auto known = directories_.find(std::string(parent)); known != directories_.end()) {
        return known->second;
    }

    std::filesystem::path relative;
    std::string key;
    std::size_t start = 0;
    while (start <= parent.size()) {
        std::size_t slash = parent.find('/', start);
        if (slash == std::string_view::npos) {
            slash = parent.size();
        }
        const std::string_view component = parent.substr(start, slash - start);
        start = slash + 1;
        if (component.empty()) {
            continue;
        }
        key += '/';
        key += component;
        if (const auto known = directories_.find(key); known != directories_.end()) {
            relative = known->second;
            continue;
        }

        const std::wstring safe = safeFileName(component);
        std::optional<std::filesystem::path> placed;
        for (std::uint32_t attempt = 0; attempt < RecoveryWriterOptions::kMaxNameAttempts && !placed; ++attempt) {
            const std::filesystem::path candidatePath = relative / numberedName(safe, attempt);
            const std::filesystem::path io = ioRoot_ / candidatePath;
            std::error_code ec;
            const bool created = std::filesystem::create_directory(io, ec);
            if (!created && !isPlainDirectory(io)) {
                if (nameTaken(io)) {
                    continue;  // a file or a link holds the name
                }
                return makeError(ErrorCode::DestinationError,
                                 "cannot create directory '" + toUtf8(destination_ / candidatePath) + "'",
                                 static_cast<std::uint32_t>(ec.value()));
            }
            // An existing plain directory (from an earlier recovery, or an
            // original name that maps to the same safe name) is shared; files
            // in it still never overwrite each other.
            if (Status safeDirectory = checkSafety(destination_ / candidatePath); !safeDirectory.ok()) {
                return safeDirectory.error();
            }
            placed = candidatePath;
        }
        if (!placed) {
            return makeError(ErrorCode::DestinationError,
                             "no free name for directory '" + std::string(component) + "'");
        }
        relative = *placed;
        directories_.emplace(key, relative);
    }
    return relative;
}

Result<RecoveredFile> RecoveryWriter::recover(const RecoveryCandidate& candidate) {
    return recover(candidate, FileCreatedCallback{});
}

Result<RecoveredFile> RecoveryWriter::recover(const RecoveryCandidate& candidate,
                                              const FileCreatedCallback& onCreated) {
    if (Status valid = validateCandidate(candidate); !valid.ok()) {
        return valid.error();
    }
    if (candidate.expectedSize > 0 && candidate.bytes(RegionKind::Missing) == candidate.expectedSize) {
        return makeError(ErrorCode::InvalidInput,
                         "candidate " + std::to_string(candidate.id.value()) + " has no located data to recover");
    }
    Result<std::filesystem::path> directory = directoryFor(candidate);
    if (!directory.ok()) {
        return directory.error();
    }

    const std::wstring safe = safeFileName(candidate.filename);
    storage::DestinationFile file;
    std::filesystem::path relative;
    for (std::uint32_t attempt = 0; attempt < RecoveryWriterOptions::kMaxNameAttempts; ++attempt) {
        relative = *directory / numberedName(safe, attempt);
        Result<storage::DestinationFile> opened =
            storage::DestinationFile::open(ioRoot_ / relative, storage::DestinationFile::OpenMode::CreateNew);
        if (opened.ok()) {
            file = std::move(opened).value();
            break;
        }
        if (!nameTaken(ioRoot_ / relative)) {
            return opened.error();
        }
    }
    if (!file.isOpen()) {
        return makeError(ErrorCode::DestinationError, "no free name for '" + toUtf8(destination_ / relative) + "'");
    }
    const std::filesystem::path ioPath = ioRoot_ / relative;
    std::filesystem::path finalPath = destination_ / relative;

    // A failed file is removed again: it was created by this call, so nothing else can be lost.
    const auto discard = [&](const Error& error) -> Error {
        file.close();
        std::error_code ignored;
        std::filesystem::remove(ioPath, ignored);
        if (options_.logger != nullptr) {
            options_.logger->log(LogLevel::Warning, kComponent, "recovering file failed",
                                 {field("candidate", candidate.id.value()), field("path", toUtf8(finalPath)),
                                  field("error", describe(error))});
        }
        return error;
    };

    if (onCreated) {
        if (Status told = onCreated(finalPath); !told.ok()) {
            return discard(told.error());
        }
    }

    const CandidateSink sink = [&file](std::uint64_t offset, std::span<const std::byte> data) {
        return file.writeAt(offset, data);
    };
    Result<ReconstructionReport> report =
        reconstructCandidate(*source_, candidate, sink, options_.reconstruction);
    if (!report.ok()) {
        return discard(report.error());
    }
    // Extends the file over trailing zeros (sparse holes, data beyond the valid data length).
    if (Status sized = file.truncate(report->outputSize); !sized.ok()) {
        return discard(sized.error());
    }
    if (Status flushed = file.flush(); !flushed.ok()) {
        return discard(flushed.error());
    }
    const Result<std::uint64_t> written = file.size();
    if (!written.ok()) {
        return discard(written.error());
    }
    if (*written != report->outputSize) {
        return discard(makeError(ErrorCode::DestinationError, "recovered file has " + std::to_string(*written) +
                                                                  " bytes, expected " +
                                                                  std::to_string(report->outputSize)));
    }
    file.close();

    if (options_.logger != nullptr) {
        options_.logger->log(
            LogLevel::Info, kComponent, "recovered file",
            {field("candidate", candidate.id.value()), field("path", toUtf8(finalPath)),
             field("expected_size", report->expectedSize), field("size", report->outputSize),
             field("missing_bytes", report->missingBytes), field("unreadable_bytes", report->unreadableBytes),
             field("reallocated_bytes", report->reallocatedBytes)});
    }
    return RecoveredFile{candidate.id, std::move(finalPath), std::move(report).value()};
}

}  // namespace recovery
