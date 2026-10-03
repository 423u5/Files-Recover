#include "content_bytes.hpp"

#include <algorithm>
#include <utility>

namespace recovery::validation::detail {

ContentBytes::ContentBytes(carving::IContentReader& content, std::uint64_t begin, std::uint64_t end)
    : ContentBytes(content, std::vector<Range>{Range{begin, end}}) {}

ContentBytes::ContentBytes(carving::IContentReader& content, std::vector<Range> ranges) : content_(&content) {
    const std::uint64_t size = content.size();
    for (Range range : ranges) {
        range.end = std::min(range.end, size);
        if (range.begin < range.end) {
            ranges_.push_back(range);
        }
    }
    bufferOffset_ = ranges_.empty() ? 0 : ranges_.front().begin;
}

bool ContentBytes::refill() {
    if (error_.has_value()) {
        return false;
    }
    std::uint64_t next = bufferOffset_ + buffer_.size();
    while (range_ < ranges_.size() && next >= ranges_[range_].end) {
        ++range_;
        if (range_ < ranges_.size()) {
            next = ranges_[range_].begin;
        }
    }
    buffer_.clear();
    index_ = 0;
    bufferOffset_ = next;
    if (range_ >= ranges_.size()) {
        return false;
    }
    const auto length =
        static_cast<std::size_t>(std::min<std::uint64_t>(kBufferSize, ranges_[range_].end - next));
    Result<std::span<const std::byte>> read = content_->read(next, length);
    if (!read.ok()) {
        error_ = read.error();
        return false;
    }
    buffer_.assign(read->begin(), read->end());
    return true;
}

bool ContentBytes::skip(std::uint64_t count) {
    const std::size_t inBuffer = buffer_.size() - index_;
    if (count <= inBuffer) {
        index_ += static_cast<std::size_t>(count);
        return true;
    }
    std::uint64_t left = count - inBuffer;
    std::uint64_t next = bufferOffset_ + buffer_.size();
    buffer_.clear();
    index_ = 0;
    while (range_ < ranges_.size()) {
        next = std::max(next, ranges_[range_].begin);
        const std::uint64_t inRange = ranges_[range_].end - next;
        if (left <= inRange) {
            bufferOffset_ = next + left;
            return true;
        }
        left -= inRange;
        ++range_;
        if (range_ < ranges_.size()) {
            next = ranges_[range_].begin;
        }
    }
    bufferOffset_ = ranges_.empty() ? 0 : ranges_.back().end;
    return false;
}

std::uint64_t ContentBytes::position() const noexcept {
    std::uint64_t next = bufferOffset_ + index_;
    if (index_ < buffer_.size()) {
        return next;
    }
    std::size_t range = range_;
    while (range < ranges_.size() && next >= ranges_[range].end) {
        ++range;
        if (range < ranges_.size()) {
            next = ranges_[range].begin;
        }
    }
    return next;
}

std::uint64_t ContentBytes::remaining() const noexcept {
    if (range_ >= ranges_.size()) {
        return 0;
    }
    const std::uint64_t next = std::max(bufferOffset_ + buffer_.size(), ranges_[range_].begin);
    std::uint64_t left = (buffer_.size() - index_) + (ranges_[range_].end > next ? ranges_[range_].end - next : 0);
    for (std::size_t r = range_ + 1; r < ranges_.size(); ++r) {
        left += ranges_[r].end - ranges_[r].begin;
    }
    return left;
}

}  // namespace recovery::validation::detail
