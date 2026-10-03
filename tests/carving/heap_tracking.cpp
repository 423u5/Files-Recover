// Replaces the global operator new and delete of this test executable to
// count heap bytes. Each block carries its size in a header in front of the
// memory handed out. Only the unaligned forms are replaced; the aligned forms
// keep their default (untracked) implementation, which never mixes with
// these. Nothing is replaced under AddressSanitizer.

#include "heap_tracking.hpp"

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>

namespace recovery::test {

namespace {

#if !defined(__SANITIZE_ADDRESS__)

std::atomic<std::int64_t> currentBytes{0};
std::atomic<std::int64_t> peakBytes{0};

// Keeps the memory after the header aligned like malloc's.
constexpr std::size_t kHeader = alignof(std::max_align_t) > sizeof(std::size_t) ? alignof(std::max_align_t)
                                                                                 : sizeof(std::size_t);

void* allocate(std::size_t size) noexcept {
    if (size > static_cast<std::size_t>(PTRDIFF_MAX) - kHeader) {
        return nullptr;
    }
    auto* block = static_cast<unsigned char*>(std::malloc(size + kHeader));
    if (block == nullptr) {
        return nullptr;
    }
    *reinterpret_cast<std::size_t*>(block) = size;
    const std::int64_t now = currentBytes.fetch_add(static_cast<std::int64_t>(size)) + static_cast<std::int64_t>(size);
    std::int64_t peak = peakBytes.load();
    while (now > peak && !peakBytes.compare_exchange_weak(peak, now)) {
    }
    return block + kHeader;
}

void release(void* memory) noexcept {
    if (memory == nullptr) {
        return;
    }
    unsigned char* block = static_cast<unsigned char*>(memory) - kHeader;
    currentBytes.fetch_sub(static_cast<std::int64_t>(*reinterpret_cast<std::size_t*>(block)));
    std::free(block);
}

#endif

}  // namespace

bool heapTrackingAvailable() noexcept {
#if defined(__SANITIZE_ADDRESS__)
    return false;
#else
    return true;
#endif
}

HeapStats heapStats() noexcept {
#if defined(__SANITIZE_ADDRESS__)
    return {};
#else
    return HeapStats{currentBytes.load(), peakBytes.load()};
#endif
}

void resetHeapPeak() noexcept {
#if !defined(__SANITIZE_ADDRESS__)
    peakBytes.store(currentBytes.load());
#endif
}

}  // namespace recovery::test

#if !defined(__SANITIZE_ADDRESS__)

void* operator new(std::size_t size) {
    void* memory = recovery::test::allocate(size == 0 ? 1 : size);
    if (memory == nullptr) {
        throw std::bad_alloc();
    }
    return memory;
}

void* operator new[](std::size_t size) {
    return ::operator new(size);
}

void* operator new(std::size_t size, const std::nothrow_t& /*tag*/) noexcept {
    return recovery::test::allocate(size == 0 ? 1 : size);
}

void* operator new[](std::size_t size, const std::nothrow_t& /*tag*/) noexcept {
    return recovery::test::allocate(size == 0 ? 1 : size);
}

void operator delete(void* memory) noexcept {
    recovery::test::release(memory);
}

void operator delete[](void* memory) noexcept {
    recovery::test::release(memory);
}

void operator delete(void* memory, std::size_t /*size*/) noexcept {
    recovery::test::release(memory);
}

void operator delete[](void* memory, std::size_t /*size*/) noexcept {
    recovery::test::release(memory);
}

void operator delete(void* memory, const std::nothrow_t& /*tag*/) noexcept {
    recovery::test::release(memory);
}

void operator delete[](void* memory, const std::nothrow_t& /*tag*/) noexcept {
    recovery::test::release(memory);
}

#endif
