#pragma once

// Reading source ranges for carving without letting a bad sector stop the
// scan (private to recovery_carving).

#include "carving/content_reader.hpp"
#include "recovery/result.hpp"
#include "storage/bad_region.hpp"
#include "storage/storage_source.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace recovery::carving::detail {

// Reads source bytes [offset, offset + buffer.size()) into `buffer`, which
// must lie inside the source and hold at most storage::kMaxReadSize bytes.
//
// Bytes inside options.knownBadRegions are not read. A read that fails with
// an I/O error is repeated sector by sector, each sector up to
// 1 + options.sectorRetryCount times. Bytes that are not read or still fail
// are zero-filled, and their ranges are appended to `unreadable` in
// increasing order.
//
// Fails with Cancelled when cancellation is requested, and with the error of
// a read that fails for a reason other than an I/O error.
[[nodiscard]] Status fillFromSource(storage::IStorageSource& source, std::uint64_t offset,
                                    std::span<std::byte> buffer, const SourceReadOptions& options,
                                    std::vector<storage::BadRegion>& unreadable);

[[nodiscard]] Error cancelledError();

}  // namespace recovery::carving::detail
