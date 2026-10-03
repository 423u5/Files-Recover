#pragma once

// Sequential reading for the media decoders: the bytes of one or more content
// ranges in order (one range for most formats; the IDAT payloads of a PNG, the
// sub-blocks of a GIF image), through a buffer of their own, so the decoders
// can read byte by byte without a reader call each.
//
// A read error is kept and ends the bytes: a decoder that runs out of data
// checks error() to tell a failing reader (Cancelled, a source failure) from
// the end of the file.

#include "carving/content_reader.hpp"
#include "recovery/error.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace recovery::validation::detail {

class ContentBytes {
public:
    struct Range {
        std::uint64_t begin = 0;
        std::uint64_t end = 0;
    };

    static constexpr std::size_t kBufferSize = 64 * 1024;

    // [begin, end) of the content, clipped to its size.
    ContentBytes(carving::IContentReader& content, std::uint64_t begin, std::uint64_t end);
    // The ranges in this order (each clipped to the content's size).
    ContentBytes(carving::IContentReader& content, std::vector<Range> ranges);

    // The next byte; false at the end of the ranges or after a read error.
    bool next(std::uint8_t& value) {
        if (index_ == buffer_.size() && !refill()) {
            return false;
        }
        value = static_cast<std::uint8_t>(buffer_[index_++]);
        return true;
    }
    // The byte next() would return, without taking it.
    bool peek(std::uint8_t& value) {
        if (index_ == buffer_.size() && !refill()) {
            return false;
        }
        value = static_cast<std::uint8_t>(buffer_[index_]);
        return true;
    }
    // Skips `count` bytes; false when the data ends (or fails) first.
    bool skip(std::uint64_t count);

    // Content offset of the next byte; the end of the last range when all
    // were read.
    [[nodiscard]] std::uint64_t position() const noexcept;
    // Bytes left in the ranges.
    [[nodiscard]] std::uint64_t remaining() const noexcept;
    [[nodiscard]] const std::optional<Error>& error() const noexcept { return error_; }

private:
    bool refill();

    carving::IContentReader* content_;
    std::vector<Range> ranges_;
    // The range being read, and the content offset the buffer starts at.
    std::size_t range_ = 0;
    std::uint64_t bufferOffset_ = 0;
    std::vector<std::byte> buffer_;
    std::size_t index_ = 0;
    std::optional<Error> error_;
};

}  // namespace recovery::validation::detail
