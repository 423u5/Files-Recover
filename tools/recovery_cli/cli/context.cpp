#include "cli/context.hpp"

#include "cli/format.hpp"
#include "storage/destination_file.hpp"
#include "storage/destination_guard.hpp"

#include <cstddef>
#include <ostream>
#include <span>

namespace recovery::cli {

namespace {

// ERROR_FILE_NOT_FOUND.
constexpr std::uint32_t kFileNotFound = 2;

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
    std::string message(what);
    message += ": ";
    message += printable(describe(error));
    this->error(message);
    return ExitCode::Error;
}

Status Context::openLog(const std::filesystem::path& path, const storage::SourceInfo* source) {
    if (source != nullptr) {
        if (Status safe = storage::checkDestinationSafety(*source, path, diskResolver()); !safe.ok()) {
            return safe;
        }
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
