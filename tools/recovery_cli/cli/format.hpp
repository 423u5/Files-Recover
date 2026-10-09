#pragma once

// Text for people: the reports' formatting (report/text_format.hpp: sizes,
// counts, times, sources, stages, tables, strings from untrusted disks made
// safe to print), and command lines to type.

#include "report/text_format.hpp"

#include <string>
#include <string_view>

namespace recovery::cli {

using report::displayPath;
using report::formatBytes;
using report::formatCount;
using report::formatDuration;
using report::formatIsoTime;
using report::formatIsoTimestamp;
using report::formatPercent;
using report::formatRate;
using report::formatSize;
using report::formatTime;
using report::formatTimestamp;
using report::pathFromUtf8;
using report::printable;
using report::stageName;
using report::Table;

// One argument of a command line to type, quoted as the Windows C runtime
// reads it back (CommandLineToArgvW rules) when it holds spaces or quotes.
[[nodiscard]] std::string quoteArgument(std::string_view argument);

}  // namespace recovery::cli
