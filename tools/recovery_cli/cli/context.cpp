#include "cli/context.hpp"

#include "cli/format.hpp"
#include "recovery/text.hpp"
#include "storage/destination_file.hpp"
#include "storage/destination_guard.hpp"

#include <windows.h>

#include <cstddef>
#include <ostream>
#include <span>
#include <system_error>

namespace recovery::cli {

namespace {

// ERROR_FILE_NOT_FOUND.
constexpr std::uint32_t kFileNotFound = 2;

// Windows' own words for a system error code ("The system cannot find the
// path specified"), without the final period; empty when it has none.
std::string systemErrorText(std::uint32_t code) {
    wchar_t* buffer = nullptr;
    const DWORD length = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
        reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    if (length == 0 || buffer == nullptr) {
        return {};
    }
    std::wstring text(buffer, length);
    ::LocalFree(buffer);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ' || text.back() == L'.')) {
        text.pop_back();
    }
    return toUtf8(std::filesystem::path(text));
}

// Appends formatted records to a file on the destination side.
class FileLogSink final : public diagnostics::ILogSink {
public:
    FileLogSink(storage::DestinationFile file, std::uint64_t end) : file_(std::move(file)), end_(end) {}

    // Called with the logger's lock held. A record that cannot be written is
    // dropped: logging never stops a command.
    void write(const diagnostics::LogRecord& record) override {
        std::string line = diagnostics::formatRecord(record);
        line += '\n';
        const auto bytes = std::as_bytes(std::span(line.data(), line.size()));
        if (file_.writeAt(end_, bytes).ok()) {
            end_ += bytes.size();
        }
    }

private:
    storage::DestinationFile file_;
    std::uint64_t end_;
};

}  // namespace

Context::Context(Environment& environment, std::string command)
    : environment_(environment),
      command_(std::move(command)),
      progress_(*environment.err, environment.progress, environment.lineInterval, environment.consoleWidth) {}

Context::~Context() {
    progress_.clear();
}

void Context::setQuiet(bool quiet) {
    quiet_ = quiet;
    if (quiet) {
        progress_.setStyle(ProgressStyle::None);
    }
}

void Context::error(std::string_view message) {
    progress_.clear();
    err() << "recovery " << command_ << ": error: " << message << '\n';
    err().flush();
}

void Context::warning(std::string_view message) {
    progress_.clear();
    err() << "recovery " << command_ << ": warning: " << message << '\n';
    err().flush();
}

void Context::note(std::string_view message) {
    if (quiet_) {
        return;
    }
    progress_.clear();
    err() << message << '\n';
    err().flush();
}

ExitCode Context::fail(std::string_view what, const Error& error) {
    std::string described = describe(error);
    // "(system error 3)" -> "(system error 3: The system cannot find the path specified)".
    if (error.systemErrorCode != 0 && described.ends_with(')')) {
        if (const std::string text = systemErrorText(error.systemErrorCode); !text.empty()) {
            described.pop_back();
            described += ": " + text + ")";
        }
    }
    std::string message(what);
    message += ": ";
    message += printable(described);
    this->error(message);
    return ExitCode::Error;
}

Status Context::prepareFolderFor(const std::filesystem::path& path, const storage::SourceInfo* source) {
    if (source != nullptr) {
        if (Status safe = storage::checkDestinationSafety(*source, path, diskResolver()); !safe.ok()) {
            return safe;
        }
    }
    const std::filesystem::path folder = path.parent_path();
    std::error_code ec;
    if (folder.empty() || std::filesystem::is_directory(folder, ec)) {
        return success();
    }
    std::filesystem::create_directories(folder, ec);
    if (ec) {
        // A Windows error code goes with the error; a portable one is told in words.
        const bool windows = ec.category() == std::system_category();
        return makeError(ErrorCode::DestinationError,
                         "cannot create the folder '" + toUtf8(folder) + "'" + (windows ? "" : ": " + ec.message()),
                         windows ? static_cast<std::uint32_t>(ec.value()) : 0);
    }
    return success();
}

Status Context::openLog(const std::filesystem::path& path, const storage::SourceInfo* source) {
    if (Status ready = prepareFolderFor(path, source); !ready.ok()) {
        return ready;
    }
    Result<storage::DestinationFile> file =
        storage::DestinationFile::open(path, storage::DestinationFile::OpenMode::OpenExisting);
    if (!file.ok() && file.error().systemErrorCode == kFileNotFound) {
        file = storage::DestinationFile::open(path, storage::DestinationFile::OpenMode::CreateNew);
    }
    if (!file.ok()) {
        return file.error();
    }
    Result<std::uint64_t> size = file->size();
    if (!size.ok()) {
        return size.error();
    }
    auto logger = std::make_unique<diagnostics::Logger>(diagnostics::LogLevel::Info);
    logger->addSink(std::make_shared<FileLogSink>(std::move(file).value(), *size));
    logger_ = std::move(logger);
    return success();
}

bool Context::interrupted() const noexcept {
    return environment_.interrupt != nullptr && environment_.interrupt->requested();
}

bool Context::reportProgress(const ProgressEvent& event) {
    if (environment_.onProgress) {
        environment_.onProgress(event);
    }
    return interrupted();
}

storage::DiskResolver Context::diskResolver() const {
    return environment_.diskResolver ? environment_.diskResolver : storage::makePlatformDiskResolver();
}

}  // namespace recovery::cli
