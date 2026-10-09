#include "cli/progress.hpp"

#include "cli/format.hpp"
#include "report/utf8.hpp"

#include <ostream>

namespace recovery::cli {

namespace {

std::size_t codePoints(std::string_view text) noexcept {
    std::size_t count = 0;
    for (std::size_t i = 0; i < text.size();) {
        (void)report::decodeUtf8(text, i);
        ++count;
    }
    return count;
}

// The first `limit` code points of `text`.
std::string_view prefix(std::string_view text, std::size_t limit) noexcept {
    std::size_t i = 0;
    for (std::size_t count = 0; i < text.size() && count < limit; ++count) {
        (void)report::decodeUtf8(text, i);
    }
    return text.substr(0, i);
}

}  // namespace

ProgressDisplay::ProgressDisplay(std::ostream& err, ProgressStyle style, std::chrono::milliseconds lineInterval,
                                 std::size_t consoleWidth) noexcept
    : err_(err), style_(style), lineInterval_(lineInterval), width_(consoleWidth) {}

void ProgressDisplay::show(std::string_view line, bool force) {
    switch (style_) {
    case ProgressStyle::None:
        return;
    case ProgressStyle::Lines: {
        const auto now = std::chrono::steady_clock::now();
        if (!force && lastLine_.has_value() && now - *lastLine_ < lineInterval_) {
            return;
        }
        lastLine_ = now;
        err_ << line << '\n';
        err_.flush();
        return;
    }
    case ProgressStyle::Console: {
        // One column short of the width: a full line would wrap on some consoles.
        const std::string_view fitted = width_ > 1 ? prefix(line, width_ - 1) : line;
        const std::size_t length = codePoints(fitted);
        std::string text = "\r";
        text += fitted;
        if (length < shown_) {
            text.append(shown_ - length, ' ');
            text += '\r';
            text += fitted;
        }
        shown_ = length;
        err_ << text;
        err_.flush();
        return;
    }
    }
}

void ProgressDisplay::setStyle(ProgressStyle style) {
    clear();
    style_ = style;
}

void ProgressDisplay::clear() {
    if (style_ != ProgressStyle::Console || shown_ == 0) {
        return;
    }
    err_ << '\r' << std::string(shown_, ' ') << '\r';
    err_.flush();
    shown_ = 0;
}

std::string imagingLine(const imaging::ImagingProgress& progress) {
    std::string line = "Imaging  " + formatPercent(progress.bytesCompleted, progress.totalBytes) + "  " +
                       formatSize(progress.bytesCompleted) + " of " + formatSize(progress.totalBytes);
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(progress.elapsed).count();
    const std::uint64_t imagedNow = progress.bytesCompleted - std::min(progress.bytesCompleted, progress.resumedFrom);
    if (seconds > 0) {
        line += "  " + formatRate(imagedNow / static_cast<std::uint64_t>(seconds));
    }
    line += "  unreadable " + formatSize(progress.unreadableBytes);
    line += "  " + formatDuration(progress.elapsed);
    return line;
}

std::pair<std::size_t, std::size_t> stageNumber(scan::ScanStage stage, ScanMode mode) noexcept {
    std::size_t number = 0;
    std::size_t count = 0;
    for (scan::ScanStage at = scan::ScanStage::Volumes; at != scan::ScanStage::Completed;
         at = scan::nextStage(at, mode)) {
        ++count;
        if (at == stage) {
            number = count;
        }
    }
    return {number == 0 ? count : number, count};
}

std::string scanLine(const scan::ScanProgress& progress, ScanMode mode) {
    const auto [number, count] = stageNumber(progress.stage, mode);
    std::string line = "Scanning " + std::to_string(number) + "/" + std::to_string(count) + " " +
                       stageName(progress.stage);
    if (progress.paused) {
        line += "  (paused)";
    }
    const scan::ScanMetrics& metrics = progress.metrics;
    if (progress.stageTotal != 0) {
        line += "  " + formatPercent(progress.stageDone, progress.stageTotal);
        if (progress.stage == scan::ScanStage::SourcePass) {
            line += "  " + formatSize(progress.stageDone) + " of " + formatSize(progress.stageTotal);
        } else {
            line += "  " + formatCount(progress.stageDone) + " of " + formatCount(progress.stageTotal);
        }
    }
    if (metrics.scanSpeed != 0) {
        line += "  " + formatRate(metrics.scanSpeed);
    }
    line += "  " + formatCount(metrics.filesFound) + " files";
    if (mode == ScanMode::Deep) {
        line += ", " + formatCount(metrics.carves) + " carves";
    }
    if (metrics.candidates != 0) {
        line += ", " + formatCount(metrics.candidates) + " candidates";
    }
    if (metrics.unreadableBytes != 0) {
        line += "  unreadable " + formatSize(metrics.unreadableBytes);
    }
    line += "  " + formatDuration(metrics.elapsed);
    return line;
}

std::string recoveryLine(const scan::RecoveryJobProgress& progress) {
    const scan::RecoveryJobMetrics& metrics = progress.metrics;
    std::string line = "Recovering  " + formatCount(progress.done) + "/" + formatCount(progress.total) + " files  " +
                       formatSize(metrics.bytesRecovered);
    if (progress.paused) {
        line += "  (paused)";
    }
    if (metrics.speed != 0) {
        line += "  " + formatRate(metrics.speed);
    }
    if (metrics.failedFiles != 0) {
        line += "  " + formatCount(metrics.failedFiles) + " failed";
    }
    if (metrics.unreadableBytes != 0) {
        line += "  unreadable " + formatSize(metrics.unreadableBytes);
    }
    line += "  " + formatDuration(metrics.elapsed);
    return line;
}

}  // namespace recovery::cli
