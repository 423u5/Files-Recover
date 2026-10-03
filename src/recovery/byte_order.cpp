#include "recovery/byte_order.hpp"

#include <cstdio>
#include <cstdlib>

namespace recovery::detail {

void failedBoundsCheck(std::size_t offset, std::size_t width, std::size_t size) noexcept {
    std::fprintf(stderr, "RecoveryEngine: structure field out of bounds (offset %zu, width %zu, size %zu)\n", offset,
                 width, size);
    std::fflush(stderr);
    std::abort();
}

}  // namespace recovery::detail
