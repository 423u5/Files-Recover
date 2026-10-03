#include "recovery/text.hpp"

#include <format>

namespace recovery {

std::string toUtf8(const std::filesystem::path& path) {
    const std::u8string utf8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
}

std::string formatUtcTimestamp(std::chrono::system_clock::time_point time) {
    return std::format("{:%Y-%m-%dT%H:%M:%S}Z", std::chrono::floor<std::chrono::milliseconds>(time));
}

}  // namespace recovery
