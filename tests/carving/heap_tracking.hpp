#pragma once

// Heap accounting for the carving test executable, which replaces the global
// operator new and delete (heap_tracking.cpp). Streaming tests use it to show
// that memory use does not grow with the size of the scanned image.
//
// Not available under AddressSanitizer, which supplies its own allocator.

#include <cstdint>

namespace recovery::test {

struct HeapStats {
    // Bytes allocated through operator new and not yet freed.
    std::int64_t current = 0;
    // Highest value of `current` since the last resetHeapPeak().
    std::int64_t peak = 0;
};

[[nodiscard]] bool heapTrackingAvailable() noexcept;
[[nodiscard]] HeapStats heapStats() noexcept;
// Sets the peak to the current value.
void resetHeapPeak() noexcept;

}  // namespace recovery::test
