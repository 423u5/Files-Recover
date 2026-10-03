#include "evaluation/content_identity.hpp"

#include "recovery/crc32.hpp"

#include <algorithm>
#include <span>

namespace recovery::evaluation {

namespace {

constexpr std::size_t kChunk = carving::IContentReader::kMaxReadLength;

// CRC-32 of content bytes [begin, begin + length).
Result<std::uint32_t> crcOf(carving::IContentReader& content, std::uint64_t begin, std::uint64_t length) {
    std::uint32_t crc = 0;
    for (std::uint64_t done = 0; done < length;) {
        const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(length - done, kChunk));
        Result<std::span<const std::byte>> bytes = content.read(begin + done, chunk);
        if (!bytes.ok()) {
            return bytes.error();
        }
        crc = crc32Update(crc, *bytes);
        done += chunk;
    }
    return crc;
}

}  // namespace

Result<ContentIdentity> computeIdentity(carving::IContentReader& content, const IdentityOptions& options) {
    ContentIdentity identity;
    identity.size = content.size();
    const std::uint64_t window = std::min(identity.size, PreliminaryHash::kWindow);
    std::optional<std::uint32_t> head;
    if (options.sha256) {
        Sha256 hash;
        std::uint32_t crc = 0;
        for (std::uint64_t offset = 0; offset < identity.size;) {
            const auto chunk = static_cast<std::size_t>(std::min<std::uint64_t>(identity.size - offset, kChunk));
            Result<std::span<const std::byte>> bytes = content.read(offset, chunk);
            if (!bytes.ok()) {
                return bytes.error();
            }
            hash.update(*bytes);
            if (offset < window) {
                const auto take = static_cast<std::size_t>(std::min<std::uint64_t>(chunk, window - offset));
                crc = crc32Update(crc, bytes->first(take));
            }
            offset += chunk;
        }
        identity.sha256 = hash.finish();
        head = crc;
    }
    if (options.preliminary) {
        PreliminaryHash preliminary;
        preliminary.size = identity.size;
        if (head.has_value()) {
            preliminary.head = *head;
        } else {
            Result<std::uint32_t> crc = crcOf(content, 0, window);
            if (!crc.ok()) {
                return crc.error();
            }
            preliminary.head = *crc;
        }
        if (identity.size <= PreliminaryHash::kWindow) {
            preliminary.tail = preliminary.head;
        } else {
            Result<std::uint32_t> crc = crcOf(content, identity.size - window, window);
            if (!crc.ok()) {
                return crc.error();
            }
            preliminary.tail = *crc;
        }
        identity.preliminary = preliminary;
    }
    return identity;
}

std::optional<std::uint64_t> DuplicateIndex::add(const ContentIdentity& identity, std::uint64_t id) {
    if (identity.size == 0 || !identity.sha256.has_value()) {
        return std::nullopt;
    }
    const auto [at, inserted] = originals_.emplace(*identity.sha256, id);
    if (inserted) {
        return std::nullopt;
    }
    return at->second;
}

std::optional<std::uint64_t> DuplicateIndex::originalOf(const ContentIdentity& identity) const {
    if (identity.size == 0 || !identity.sha256.has_value()) {
        return std::nullopt;
    }
    const auto found = originals_.find(*identity.sha256);
    if (found == originals_.end()) {
        return std::nullopt;
    }
    return found->second;
}

}  // namespace recovery::evaluation
