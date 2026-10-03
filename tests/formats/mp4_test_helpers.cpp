#include "mp4_test_helpers.hpp"

#include "format_test_helpers.hpp"

#include <algorithm>
#include <random>

namespace recovery::formats::testing {

namespace {

const char* statusName(mp4::FileStatus status) {
    switch (status) {
    case mp4::FileStatus::Valid:
        return "Valid";
    case mp4::FileStatus::Truncated:
        return "Truncated";
    case mp4::FileStatus::Invalid:
        break;
    }
    return "Invalid";
}

}  // namespace

mp4::Mp4File parseBytes(std::span<const std::byte> bytes, const mp4::ParseLimits& limits) {
    carving::MemoryContentReader content(bytes);
    Result<mp4::Mp4File> file = mp4::parseFile(content, limits);
    if (!file.ok()) {
        ADD_FAILURE() << "parseFile failed: " << recovery::describe(file.error());
        return {};
    }
    return std::move(file).value();
}

std::string describe(const mp4::Mp4File& file) {
    std::string text = std::string(statusName(file.status)) + ": " + file.detail;
    for (const mp4::Issue& issue : file.issues.recorded()) {
        text += "\n  " + issue.describe();
    }
    return text;
}

::testing::AssertionResult matchesBuilt(const mp4::Mp4File& file, const test::Mp4Built& built) {
    if (file.status != mp4::FileStatus::Valid) {
        return ::testing::AssertionFailure() << describe(file);
    }
    if (!file.movie.has_value()) {
        return ::testing::AssertionFailure() << "no movie";
    }
    const std::vector<mp4::Track>& tracks = file.movie->tracks;
    if (tracks.size() != built.samples.size()) {
        return ::testing::AssertionFailure() << tracks.size() << " tracks, built " << built.samples.size();
    }
    for (std::size_t t = 0; t < tracks.size(); ++t) {
        const mp4::Track& track = tracks[t];
        if (!track.sampleTableValid) {
            return ::testing::AssertionFailure() << "track " << t + 1 << ": sample table not valid";
        }
        if (track.chunks.size() != built.chunks[t].size()) {
            return ::testing::AssertionFailure() << "track " << t + 1 << ": " << track.chunks.size()
                                                 << " chunks, built " << built.chunks[t].size();
        }
        const std::vector<test::Mp4ChunkTruth> noRuns;
        const std::vector<test::Mp4ChunkTruth>& runs = t < built.runs.size() ? built.runs[t] : noRuns;
        if (track.fragmentRuns.size() != runs.size()) {
            return ::testing::AssertionFailure() << "track " << t + 1 << ": " << track.fragmentRuns.size()
                                                 << " fragment runs, built " << runs.size();
        }
        for (const bool run : {false, true}) {
            const std::vector<mp4::Chunk>& pieces = run ? track.fragmentRuns : track.chunks;
            const std::vector<test::Mp4ChunkTruth>& truths = run ? runs : built.chunks[t];
            for (std::size_t c = 0; c < pieces.size(); ++c) {
                const mp4::Chunk& chunk = pieces[c];
                const test::Mp4ChunkTruth& truth = truths[c];
                if (chunk.offset != truth.offset || chunk.size != truth.size ||
                    chunk.firstSample != truth.firstSample || chunk.sampleCount != truth.sampleCount ||
                    chunk.placement != mp4::ChunkPlacement::MediaData) {
                    return ::testing::AssertionFailure()
                           << "track " << t + 1 << (run ? " run " : " chunk ") << c + 1 << ": offset " << chunk.offset
                           << " size " << chunk.size << " samples " << chunk.firstSample << "+" << chunk.sampleCount
                           << ", built " << truth.offset << " " << truth.size << " " << truth.firstSample << "+"
                           << truth.sampleCount;
                }
            }
        }
        if (track.totalSamples() != built.samples[t].size()) {
            return ::testing::AssertionFailure() << "track " << t + 1 << ": " << track.totalSamples()
                                                 << " samples, built " << built.samples[t].size();
        }
        std::string mismatch;
        mp4::forEachSample(track, [&](std::uint32_t index, std::uint64_t offset, std::uint32_t size) {
            const test::Mp4Sample& truth = built.samples[t][index];
            if (mismatch.empty() && (offset != truth.offset || size != truth.size)) {
                mismatch = "track " + std::to_string(t + 1) + " sample " + std::to_string(index) + ": offset " +
                           std::to_string(offset) + " size " + std::to_string(size) + ", built " +
                           std::to_string(truth.offset) + " " + std::to_string(truth.size);
            }
        });
        if (!mismatch.empty()) {
            return ::testing::AssertionFailure() << mismatch;
        }
    }
    return ::testing::AssertionSuccess();
}

bool hasIssue(const mp4::Mp4File& file, std::string_view text) {
    return std::any_of(file.issues.recorded().begin(), file.issues.recorded().end(),
                       [&](const mp4::Issue& issue) { return issue.detail.find(text) != std::string::npos; });
}

ScatteredContentReader::ScatteredContentReader(std::vector<std::span<const std::byte>> pieces)
    : pieces_(std::move(pieces)) {
    for (const auto& piece : pieces_) {
        size_ += piece.size();
    }
}

Result<std::span<const std::byte>> ScatteredContentReader::read(std::uint64_t offset, std::size_t length) {
    if (length > kMaxReadLength || offset > size_ || length > size_ - offset) {
        return makeError(ErrorCode::InvalidInput, "read outside the scattered content");
    }
    ++reads_;
    buffer_.clear();
    std::uint64_t base = 0;
    for (const auto& piece : pieces_) {
        const std::uint64_t pieceEnd = base + piece.size();
        if (buffer_.size() < length && offset + buffer_.size() < pieceEnd) {
            const std::uint64_t from = offset + buffer_.size() - base;
            const std::uint64_t take = std::min<std::uint64_t>(piece.size() - from, length - buffer_.size());
            buffer_.insert(buffer_.end(), piece.begin() + static_cast<std::ptrdiff_t>(from),
                           piece.begin() + static_cast<std::ptrdiff_t>(from + take));
        }
        base = pieceEnd;
    }
    return std::span<const std::byte>(buffer_.data(), length);
}

int fuzzParser(const Bytes& file, int iterations, std::uint64_t seed) {
    std::mt19937_64 random(seed);
    const auto pick = [&](std::size_t bound) {
        return static_cast<std::size_t>(random() % std::max<std::size_t>(bound, 1));
    };
    int valid = 0;
    for (int i = 0; i < iterations; ++i) {
        Bytes damaged = file;
        switch (i % 4) {
        case 0:  // a few bytes replaced
            for (int n = 0; n <= static_cast<int>(pick(4)); ++n) {
                damaged[pick(damaged.size())] = static_cast<std::byte>(random() & 0xFF);
            }
            break;
        case 1:  // cut short, then damaged
            damaged.resize(pick(damaged.size()));
            if (!damaged.empty()) {
                damaged[pick(damaged.size())] = static_cast<std::byte>(random() & 0xFF);
            }
            break;
        case 2: {  // a size or count inflated: 0xFF bytes in a row
            const std::size_t at = pick(damaged.size());
            for (std::size_t k = at; k < std::min(damaged.size(), at + 4); ++k) {
                damaged[k] = std::byte{0xFF};
            }
            break;
        }
        default: {  // a run of zeros or noise
            const std::size_t at = pick(damaged.size());
            const std::size_t count = std::min(damaged.size() - at, pick(64) + 1);
            const Bytes junk = noise(count, seed + static_cast<std::uint64_t>(i));
            for (std::size_t k = 0; k < count; ++k) {
                damaged[at + k] = random() % 2 == 0 ? std::byte{0} : junk[k];
            }
            break;
        }
        }
        carving::MemoryContentReader content(damaged);
        Result<mp4::Mp4File> parsed = mp4::parseFile(content);
        if (!parsed.ok()) {
            ADD_FAILURE() << "iteration " << i << ": " << recovery::describe(parsed.error());
            return valid;
        }
        const mp4::Mp4File& result = *parsed;
        const std::uint64_t size = damaged.size();
        for (const mp4::BoxHeader& box : result.layout.boxes) {
            if (box.end() > size || box.size < box.headerSize) {
                ADD_FAILURE() << "iteration " << i << ": a top-level box outside the content";
                return valid;
            }
        }
        if (result.layout.stoppedAt > size) {
            ADD_FAILURE() << "iteration " << i << ": the scan stopped beyond the content";
            return valid;
        }
        if (result.movie.has_value()) {
            for (const mp4::Track& track : result.movie->tracks) {
                if (!track.chunks.empty() && !track.sampleTableValid) {
                    ADD_FAILURE() << "iteration " << i << ": chunks without a valid sample table";
                    return valid;
                }
                for (const bool run : {false, true}) {
                    for (const mp4::Chunk& chunk : run ? track.fragmentRuns : track.chunks) {
                        if ((chunk.placement == mp4::ChunkPlacement::BeyondData) != (chunk.end() > size)) {
                            ADD_FAILURE() << "iteration " << i << ": a placement that disagrees with the size";
                            return valid;
                        }
                    }
                }
                if (track.totalSamples() > 0xFFFFFFFFULL) {
                    ADD_FAILURE() << "iteration " << i << ": 2^32 samples or more in a track";
                    return valid;
                }
            }
        }
        if (result.issues.recorded().size() > mp4::IssueList::kMaxRecorded ||
            (result.status == mp4::FileStatus::Valid && !result.issues.empty())) {
            ADD_FAILURE() << "iteration " << i << ": " << describe(result);
            return valid;
        }
        if (result.status == mp4::FileStatus::Valid) {
            ++valid;
        }
    }
    return valid;
}

}  // namespace recovery::formats::testing
