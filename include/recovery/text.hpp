#pragma once

#include <chrono>
#include <filesystem>
#include <string>

namespace recovery {

// UTF-8 representation of a path, for logs, reports and metadata files.
[[nodiscard]] std::string toUtf8(const std::filesystem::path& path);

// ISO-8601 UTC timestamp with millisecond precision, e.g. "2026-09-19T08:15:30.125Z".
[[nodiscard]] std::string formatUtcTimestamp(std::chrono::system_clock::time_point time);

}  // namespace recovery
