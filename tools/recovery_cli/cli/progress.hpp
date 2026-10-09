#pragma once

// Progress on the error stream: one line redrawn in place on a console, a
// line now and then in a file or a pipe, nothing with --quiet; and the lines
// for imaging, scans and recovery jobs.

#include "cli/cli.hpp"
#include "imaging/image_writer.hpp"
#include "recovery/config.hpp"
#include "report/text_format.hpp"
#include "scan/recovery_job.hpp"
#include "scan/scan_coordinator.hpp"

#include <chrono>
#include <cstddef>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>

namespace recovery::cli {

class ProgressDisplay {
public:
    ProgressDisplay(std::ostream& err, ProgressStyle style, std::chrono::milliseconds lineInterval,
                    std::size_t consoleWidth) noexcept;

    // Shows `line`: on a console at once (in place of the line before), in a
    // file or pipe when `force` is set or the line interval has passed.
    void show(std::string_view line, bool force);
    // Removes the line from the console, so that other text can be printed.
    void clear();

    [[nodiscard]] ProgressStyle style() const noexcept { return style_; }
    // Changes the style (--quiet: None); the line shown is removed first.
    void setStyle(ProgressStyle style);

private:
    std::ostream& err_;
    ProgressStyle style_;
    std::chrono::milliseconds lineInterval_;
    std::size_t width_;
    // Console: the length of the line shown (code points).
    std::size_t shown_ = 0;
    std::optional<std::chrono::steady_clock::time_point> lastLine_;
};

// "Imaging  45.2%  0.9 GiB of 2.0 GiB  21.0 MiB/s  unreadable 0 B  0:01:23"
[[nodiscard]] std::string imagingLine(const imaging::ImagingProgress& progress);
// "Scanning 4/7 source pass  45.2%  0.9 GiB of 2.0 GiB  85.0 MiB/s  12 files, 30 carves  0:01:23"
[[nodiscard]] std::string scanLine(const scan::ScanProgress& progress, ScanMode mode);
// "Recovering  45/120 files  12.3 MiB  3.2 MiB/s  1 failed  0:00:12"
[[nodiscard]] std::string recoveryLine(const scan::RecoveryJobProgress& progress);

// The stage's number in a scan of `mode` (1-based) and the number of stages.
[[nodiscard]] std::pair<std::size_t, std::size_t> stageNumber(scan::ScanStage stage, ScanMode mode) noexcept;
// "source pass"
using report::stageName;

}  // namespace recovery::cli
