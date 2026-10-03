#include "partition/guid.hpp"

#include <algorithm>
#include <format>

namespace recovery::partition {

Guid Guid::fromDisk(std::span<const std::byte> bytes) noexcept {
    Guid guid;
    if (bytes.size() >= guid.bytes_.size()) {
        std::copy_n(bytes.begin(), guid.bytes_.size(), guid.bytes_.begin());
    }
    return guid;
}

std::string Guid::toString() const {
    const auto b = [this](std::size_t i) { return static_cast<unsigned>(bytes_[i]); };
    return std::format("{:02X}{:02X}{:02X}{:02X}-{:02X}{:02X}-{:02X}{:02X}-{:02X}{:02X}-{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}",
                       b(3), b(2), b(1), b(0), b(5), b(4), b(7), b(6), b(8), b(9), b(10), b(11), b(12), b(13), b(14),
                       b(15));
}

}  // namespace recovery::partition
