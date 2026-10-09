// Candidate details and previews through the API (P19): media metadata read
// from the source on demand, preview bytes whole and piece by piece,
// requests out of range, a source that is gone, and what recovery jobs did.

#include "api_test_support.hpp"

#include "support/test_macros.hpp"

#include <gtest/gtest.h>

namespace recovery::api::test {
namespace {

class ApiDetailsTest : public ::testing::Test {
protected:
    void SetUp() override {
        id_ = world_.startCardScan();
        ASSERT_EQ(world_.waitIdle(id_).state, OperationState::Completed);
    }

    [[nodiscard]] CandidateDetails details(std::string_view name, bool readMedia = true) {
        const Result<CandidateDetails> found =
            world_.api().getCandidateDetails(id_, candidateNamed(world_.api(), id_, name).id, readMedia);
        EXPECT_TRUE(found.ok()) << (found.ok() ? std::string() : describe(found.error()));
        return found.ok() ? *found : CandidateDetails{};
    }

    [[nodiscard]] static std::optional<std::size_t> contentPreview(const CandidateDetails& details) {
        if (!details.media.has_value()) {
            return std::nullopt;
        }
        for (std::size_t i = 0; i < details.media->previews.size(); ++i) {
            if (details.media->previews[i].kind == PreviewKind::Content) {
                return i;
            }
        }
        return std::nullopt;
    }

    ApiWorld world_;
    std::string id_;
};

TEST_F(ApiDetailsTest, APhotosDetailsAndItsContentPreview) {
    const CandidateDetails photo = details("PHOTO.JPG");
    EXPECT_EQ(photo.candidate.name, "PHOTO.JPG");
    EXPECT_FALSE(photo.explanation.empty());
    EXPECT_TRUE(photo.recoveries.empty());
    ASSERT_TRUE(photo.media.has_value()) << (photo.mediaError ? describe(*photo.mediaError) : "");
    EXPECT_EQ(photo.media->kind, MediaKind::Image);
    EXPECT_EQ(photo.media->format, "jpeg");
    ASSERT_TRUE(photo.media->image.has_value());
    EXPECT_EQ(photo.media->image->width, 128u);
    EXPECT_EQ(photo.media->image->height, 96u);
    const std::optional<std::size_t> content = contentPreview(photo);
    ASSERT_TRUE(content.has_value());
    const PreviewInfo& preview = photo.media->previews[*content];
    EXPECT_EQ(preview.format, "jpeg");
    EXPECT_EQ(preview.mediaType, "image/jpeg");
    EXPECT_EQ(preview.size, photo.candidate.size);

    const Bytes& original = ::recovery::test::cardOriginals().at("PHOTO.JPG");
    const Result<std::vector<std::byte>> whole = world_.api().readPreview(id_, photo.candidate.id, *content);
    RECOVERY_ASSERT_OK(whole);
    EXPECT_EQ(*whole, original);

    // Piece by piece, as a player streams a large preview.
    Bytes pieces;
    for (std::uint64_t offset = 0;;) {
        const Result<std::vector<std::byte>> piece =
            world_.api().readPreview(id_, photo.candidate.id, *content, offset, 1000);
        RECOVERY_ASSERT_OK(piece);
        if (piece->empty()) {
            break;
        }
        EXPECT_LE(piece->size(), 1000u);
        pieces.insert(pieces.end(), piece->begin(), piece->end());
        offset += piece->size();
    }
    EXPECT_EQ(pieces, original);
}

TEST_F(ApiDetailsTest, AudioAndVideoDetails) {
    const CandidateDetails sound = details("SOUND.WAV");
    ASSERT_TRUE(sound.media.has_value());
    EXPECT_EQ(sound.media->kind, MediaKind::Audio);
    ASSERT_TRUE(sound.media->audio.has_value());
    EXPECT_EQ(sound.media->audio->codec, "pcm");
    EXPECT_GT(sound.media->audio->sampleRate, 0u);
    EXPECT_TRUE(sound.media->duration.has_value());

    const CandidateDetails clip = details("CLIP.MP4");
    ASSERT_TRUE(clip.media.has_value());
    EXPECT_EQ(clip.media->kind, MediaKind::Video);
    ASSERT_TRUE(clip.media->movie.has_value());
    EXPECT_FALSE(clip.media->movie->tracks.empty());
}

TEST_F(ApiDetailsTest, RequestsOutOfRangeAreRefused) {
    const CandidateDetails photo = details("PHOTO.JPG");
    const std::optional<std::size_t> content = contentPreview(photo);
    ASSERT_TRUE(content.has_value());
    const CandidateId id = photo.candidate.id;
    RECOVERY_EXPECT_ERROR(world_.api().getCandidateDetails(id_, CandidateId{0}), ErrorCode::InvalidInput);
    RECOVERY_EXPECT_ERROR(world_.api().getCandidateDetails(id_, CandidateId{999}), ErrorCode::InvalidInput);
    RECOVERY_EXPECT_ERROR(world_.api().readPreview(id_, CandidateId{999}, 0), ErrorCode::InvalidInput);
    RECOVERY_EXPECT_ERROR(world_.api().readPreview(id_, id, photo.media->previews.size()), ErrorCode::InvalidInput);
    RECOVERY_EXPECT_ERROR(world_.api().readPreview(id_, id, *content, photo.candidate.size + 1),
                          ErrorCode::InvalidInput);
    const Result<std::vector<std::byte>> end = world_.api().readPreview(id_, id, *content, photo.candidate.size);
    RECOVERY_ASSERT_OK(end);
    EXPECT_TRUE(end->empty());
    const Result<std::vector<std::byte>> none = world_.api().readPreview(id_, id, *content, 0, 0);
    RECOVERY_ASSERT_OK(none);
    EXPECT_TRUE(none->empty());
}

TEST_F(ApiDetailsTest, WithoutMediaNothingIsReadFromTheSource) {
    const CandidateDetails photo = details("PHOTO.JPG", false);
    EXPECT_FALSE(photo.media.has_value());
    EXPECT_FALSE(photo.mediaError.has_value());
    EXPECT_FALSE(photo.explanation.empty());
}

TEST_F(ApiDetailsTest, WhenTheSourceIsGoneTheDetailsSaySo) {
    std::filesystem::rename(world_.cardImage(), world_.folder() / "moved.img");
    const CandidateDetails photo = details("PHOTO.JPG");
    EXPECT_EQ(photo.candidate.name, "PHOTO.JPG");
    EXPECT_FALSE(photo.media.has_value());
    ASSERT_TRUE(photo.mediaError.has_value());
    EXPECT_NE(photo.mediaError->message.find("attached and unchanged"), std::string::npos)
        << describe(*photo.mediaError);
    EXPECT_FALSE(world_.api().readPreview(id_, photo.candidate.id, 0).ok());

    // Back where it was, it reads again.
    std::filesystem::rename(world_.folder() / "moved.img", world_.cardImage());
    EXPECT_TRUE(details("PHOTO.JPG").media.has_value());
}

TEST_F(ApiDetailsTest, TheDetailsTellWhatRecoveryJobsDid) {
    const CandidateInfo photo = candidateNamed(world_.api(), id_, "PHOTO.JPG");
    RecoveryOptions options;
    options.destination = world_.folder() / "out";
    RECOVERY_ASSERT_OK(world_.api().recoverCandidate(id_, photo.id, options));
    ASSERT_EQ(world_.waitIdle(id_).state, OperationState::Completed);
    const CandidateDetails recovered = details("PHOTO.JPG", false);
    ASSERT_EQ(recovered.recoveries.size(), 1u);
    const RecoveryRecord& record = recovered.recoveries.front();
    EXPECT_EQ(record.state, RecoveryState::Recovered);
    EXPECT_EQ(record.job, 1u);
    EXPECT_TRUE(record.complete);
    EXPECT_EQ(record.size, photo.size);
    EXPECT_EQ(record.missingBytes, 0u);
    EXPECT_EQ(::recovery::test::readFile(record.file), ::recovery::test::cardOriginals().at("PHOTO.JPG"));
    EXPECT_EQ(recovered.candidate.recovery, RecoveryState::Recovered);
    EXPECT_EQ(recovered.candidate.recoveredFile, record.file);
}

}  // namespace
}  // namespace recovery::api::test
