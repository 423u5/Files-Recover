// The validation levels (P14) with test formats, decoders and players: the
// order of the levels, which failures stop which levels, the overall status,
// a structural verdict known beforehand, limits, reader errors, and the media
// validator registry.

#include "validation/media_validator.hpp"
#include "validation/playability.hpp"
#include "validation/validation.hpp"

#include "carving/content_reader.hpp"
#include "support/carving_formats.hpp"
#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <array>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace recovery::validation {
namespace {

using carving::ValidationResult;
using carving::ValidationStatus;

LevelResult levelOf(LevelStatus status, std::string detail = "test") {
    LevelResult result;
    result.status = status;
    result.checker = "test checker";
    result.detail = std::move(detail);
    return result;
}

class FakeMedia final : public IMediaValidator {
public:
    explicit FakeMedia(std::string id, LevelResult result) : id_(std::move(id)), result_(std::move(result)) {}

    [[nodiscard]] std::string_view formatId() const noexcept override { return id_; }
    [[nodiscard]] Result<LevelResult> validate(carving::IContentReader& content,
                                               const MediaLimits&) const override {
        ++calls;
        if (content.size() > 0) {
            Result<std::span<const std::byte>> read = content.read(0, 1);
            if (!read.ok()) {
                return read.error();
            }
        }
        return result_;
    }

    mutable int calls = 0;

private:
    std::string id_;
    LevelResult result_;
};

class FakePlayer final : public IPlayabilityChecker {
public:
    explicit FakePlayer(LevelResult result) : result_(std::move(result)) {}

    [[nodiscard]] Result<LevelResult> check(carving::IContentReader&, std::string_view formatId,
                                            std::string_view extension) override {
        ++calls;
        lastFormat = std::string(formatId);
        lastExtension = std::string(extension);
        return result_;
    }

    int calls = 0;
    std::string lastFormat;
    std::string lastExtension;

private:
    LevelResult result_;
};

// A content reader whose reads fail (a cancelled scan, a failing source).
class FailingReader final : public carving::IContentReader {
public:
    [[nodiscard]] std::uint64_t size() const noexcept override { return 100; }
    [[nodiscard]] Result<std::span<const std::byte>> read(std::uint64_t, std::size_t) override {
        return makeError(ErrorCode::Cancelled, "cancelled");
    }
};

// A scripted format whose validator says `status` and counts its calls.
struct Fixture {
    explicit Fixture(ValidationStatus status, LevelResult media = levelOf(LevelStatus::Passed))
        : format(test::scriptedDescriptor("scripted", "SCR1")) {
        format.onValidate([this, status](carving::IContentReader& content) -> Result<ValidationResult> {
            ++structureCalls;
            Result<std::span<const std::byte>> read = content.read(0, 1);
            if (!read.ok()) {
                return read.error();
            }
            return ValidationResult{status, status == ValidationStatus::Valid ? content.size() : 3, "scripted verdict"};
        });
        auto decoder = std::make_shared<FakeMedia>("scripted", std::move(media));
        mediaDecoder = decoder.get();
        EXPECT_TRUE(registry.add(std::move(decoder)).ok());
    }

    Result<ValidationState> run(const ValidationOptions& options = {},
                                const std::optional<ValidationResult>& known = std::nullopt) {
        carving::MemoryContentReader content(data);
        return validateContent(content, &format, registry, options, known);
    }

    test::ScriptedFormat format;
    MediaValidatorRegistry registry;
    FakeMedia* mediaDecoder = nullptr;
    int structureCalls = 0;
    std::vector<std::byte> data = test::bytesOf("SCR1 some content");
};

TEST(ValidationLevelsTest, LevelsRunInOrderAndSayWhatRan) {
    Fixture fixture(ValidationStatus::Valid);
    FakePlayer player(levelOf(LevelStatus::Passed, "played"));
    ValidationOptions options;
    options.playability = &player;
    Result<ValidationState> state = fixture.run(options);
    RECOVERY_ASSERT_OK(state);
    EXPECT_EQ(state->structural.status, LevelStatus::Passed);
    EXPECT_EQ(state->structural.checker, "scripted structure");
    EXPECT_EQ(state->structural.detail, "scripted verdict");
    EXPECT_FALSE(state->structural.offset.has_value());
    EXPECT_EQ(state->media.status, LevelStatus::Passed);
    EXPECT_EQ(state->playability.detail, "played");
    EXPECT_EQ(player.lastFormat, "scripted");
    EXPECT_EQ(player.lastExtension, fixture.format.descriptor().extension);
    EXPECT_EQ(state->status(), ValidationStatus::Valid);
    EXPECT_EQ(state->deepestPassed(), ValidationLevel::Playability);
    EXPECT_EQ(fixture.structureCalls, 1);
    EXPECT_EQ(fixture.mediaDecoder->calls, 1);
    EXPECT_EQ(player.calls, 1);
}

TEST(ValidationLevelsTest, AFailedStructureStopsTheLaterLevels) {
    Fixture fixture(ValidationStatus::Invalid);
    FakePlayer player(levelOf(LevelStatus::Passed));
    ValidationOptions options;
    options.playability = &player;
    Result<ValidationState> state = fixture.run(options);
    RECOVERY_ASSERT_OK(state);
    EXPECT_EQ(state->structural.status, LevelStatus::Failed);
    EXPECT_EQ(state->structural.offset, 3U);
    EXPECT_EQ(state->media.status, LevelStatus::NotRun);
    EXPECT_EQ(state->media.detail, "the structure failed");
    EXPECT_EQ(state->playability.status, LevelStatus::NotRun);
    EXPECT_EQ(state->status(), ValidationStatus::Invalid);
    EXPECT_FALSE(state->deepestPassed().has_value());
    EXPECT_EQ(fixture.mediaDecoder->calls, 0);
    EXPECT_EQ(player.calls, 0);
}

TEST(ValidationLevelsTest, ATruncatedStructureIsStillDecodedButNotPlayed) {
    // The media decoders read what is there; a platform decoder cannot play
    // a file to its end when the end is missing (and its failure would hide
    // that the file is truncated).
    Fixture fixture(ValidationStatus::Truncated);
    FakePlayer player(levelOf(LevelStatus::Passed));
    ValidationOptions options;
    options.playability = &player;
    Result<ValidationState> state = fixture.run(options);
    RECOVERY_ASSERT_OK(state);
    EXPECT_EQ(state->structural.status, LevelStatus::Truncated);
    EXPECT_EQ(state->media.status, LevelStatus::Passed);
    EXPECT_EQ(state->playability.status, LevelStatus::NotRun);
    EXPECT_EQ(state->playability.detail, "the content is cut short");
    EXPECT_EQ(player.calls, 0);
    EXPECT_EQ(state->status(), ValidationStatus::Truncated);
}

TEST(ValidationLevelsTest, TruncatedMediaStopPlayability) {
    Fixture fixture(ValidationStatus::Valid, levelOf(LevelStatus::Truncated, "the data ends inside frame 3"));
    FakePlayer player(levelOf(LevelStatus::Passed));
    ValidationOptions options;
    options.playability = &player;
    Result<ValidationState> state = fixture.run(options);
    RECOVERY_ASSERT_OK(state);
    EXPECT_EQ(state->playability.status, LevelStatus::NotRun);
    EXPECT_EQ(state->playability.detail, "the content is cut short");
    EXPECT_EQ(player.calls, 0);
    EXPECT_EQ(state->status(), ValidationStatus::Truncated);
}

TEST(ValidationLevelsTest, FailedMediaStopsPlayability) {
    Fixture fixture(ValidationStatus::Valid, levelOf(LevelStatus::Failed, "bad code"));
    FakePlayer player(levelOf(LevelStatus::Passed));
    ValidationOptions options;
    options.playability = &player;
    Result<ValidationState> state = fixture.run(options);
    RECOVERY_ASSERT_OK(state);
    EXPECT_EQ(state->media.status, LevelStatus::Failed);
    EXPECT_EQ(state->playability.status, LevelStatus::NotRun);
    EXPECT_EQ(state->playability.detail, "the media failed");
    EXPECT_EQ(player.calls, 0);
    EXPECT_EQ(state->status(), ValidationStatus::Invalid);
    EXPECT_EQ(state->deepestPassed(), ValidationLevel::Structural);
}

TEST(ValidationLevelsTest, UnsupportedAndNotApplicableMediaLeaveAValidStructureValid) {
    for (const LevelStatus media : {LevelStatus::Unsupported, LevelStatus::NotApplicable}) {
        Fixture fixture(ValidationStatus::Valid, levelOf(media));
        FakePlayer player(levelOf(LevelStatus::Passed));
        ValidationOptions options;
        options.playability = &player;
        Result<ValidationState> state = fixture.run(options);
        RECOVERY_ASSERT_OK(state);
        EXPECT_EQ(state->status(), ValidationStatus::Valid);
        EXPECT_EQ(player.calls, 1) << "playability still runs";
    }
}

TEST(ValidationLevelsTest, WithoutAFormatNothingIsValidated) {
    MediaValidatorRegistry registry;
    const std::vector<std::byte> data = test::bytesOf("anything");
    carving::MemoryContentReader content(data);
    Result<ValidationState> state = validateContent(content, nullptr, registry, {});
    RECOVERY_ASSERT_OK(state);
    EXPECT_EQ(state->structural.status, LevelStatus::Unsupported);
    EXPECT_EQ(state->media.status, LevelStatus::NotRun);
    EXPECT_EQ(state->playability.status, LevelStatus::NotRun);
    EXPECT_EQ(state->status(), ValidationStatus::NotValidated);
}

TEST(ValidationLevelsTest, MediaCanBeTurnedOffAndFormatsWithoutADecoderAreUnsupported) {
    Fixture fixture(ValidationStatus::Valid);
    ValidationOptions options;
    options.media = false;
    Result<ValidationState> off = fixture.run(options);
    RECOVERY_ASSERT_OK(off);
    EXPECT_EQ(off->media.status, LevelStatus::NotRun);
    EXPECT_EQ(off->media.detail, "not requested");
    EXPECT_EQ(off->playability.detail, "not requested");
    EXPECT_EQ(fixture.mediaDecoder->calls, 0);

    test::ScriptedFormat other(test::scriptedDescriptor("other", "OTH1"));
    const std::vector<std::byte> data = test::bytesOf("OTH1 data");
    carving::MemoryContentReader content(data);
    Result<ValidationState> state = validateContent(content, &other, fixture.registry, {});
    RECOVERY_ASSERT_OK(state);
    EXPECT_EQ(state->structural.status, LevelStatus::Passed);
    EXPECT_EQ(state->media.status, LevelStatus::Unsupported);
    EXPECT_EQ(state->media.detail, "no media decoder for other");
    EXPECT_EQ(state->status(), ValidationStatus::Valid);
}

TEST(ValidationLevelsTest, AKnownStructuralVerdictIsUsedInsteadOfValidatingAgain) {
    Fixture fixture(ValidationStatus::Valid);
    Result<ValidationState> state =
        fixture.run({}, ValidationResult{ValidationStatus::Truncated, 7, "known before"});
    RECOVERY_ASSERT_OK(state);
    EXPECT_EQ(fixture.structureCalls, 0);
    EXPECT_EQ(state->structural.status, LevelStatus::Truncated);
    EXPECT_EQ(state->structural.offset, 7U);
    EXPECT_EQ(state->structural.detail, "known before");
    // NotValidated is not a verdict: the validator runs.
    Result<ValidationState> again = fixture.run({}, ValidationResult{});
    RECOVERY_ASSERT_OK(again);
    EXPECT_EQ(fixture.structureCalls, 1);
    EXPECT_EQ(again->structural.status, LevelStatus::Passed);
}

TEST(ValidationLevelsTest, TheOverallStatusFollowsFixedRules) {
    struct Case {
        LevelStatus structural;
        LevelStatus media;
        LevelStatus playability;
        ValidationStatus expected;
    };
    const std::vector<Case> cases = {
        {LevelStatus::Passed, LevelStatus::Passed, LevelStatus::Passed, ValidationStatus::Valid},
        {LevelStatus::Passed, LevelStatus::NotRun, LevelStatus::NotRun, ValidationStatus::Valid},
        {LevelStatus::Passed, LevelStatus::Unsupported, LevelStatus::Unsupported, ValidationStatus::Valid},
        {LevelStatus::Passed, LevelStatus::NotApplicable, LevelStatus::Passed, ValidationStatus::Valid},
        {LevelStatus::Passed, LevelStatus::Truncated, LevelStatus::NotRun, ValidationStatus::Truncated},
        {LevelStatus::Truncated, LevelStatus::Passed, LevelStatus::NotRun, ValidationStatus::Truncated},
        {LevelStatus::Truncated, LevelStatus::Failed, LevelStatus::NotRun, ValidationStatus::Invalid},
        {LevelStatus::Passed, LevelStatus::Passed, LevelStatus::Failed, ValidationStatus::Invalid},
        {LevelStatus::Failed, LevelStatus::NotRun, LevelStatus::NotRun, ValidationStatus::Invalid},
        {LevelStatus::Unsupported, LevelStatus::NotRun, LevelStatus::NotRun, ValidationStatus::NotValidated},
        {LevelStatus::NotRun, LevelStatus::Passed, LevelStatus::Passed, ValidationStatus::NotValidated},
    };
    for (const Case& c : cases) {
        ValidationState state;
        state.structural.status = c.structural;
        state.media.status = c.media;
        state.playability.status = c.playability;
        EXPECT_EQ(state.status(), c.expected)
            << toString(c.structural) << "/" << toString(c.media) << "/" << toString(c.playability);
        EXPECT_EQ(&state.level(ValidationLevel::Media), &state.media);
    }
}

TEST(ValidationLevelsTest, ReaderErrorsArePassedOnAndLimitsChecked) {
    Fixture fixture(ValidationStatus::Valid);
    FailingReader failing;
    Result<ValidationState> state = validateContent(failing, &fixture.format, fixture.registry, {});
    RECOVERY_EXPECT_ERROR(state, ErrorCode::Cancelled);
    // A decoder's reader error too.
    Result<ValidationState> media = validateContent(failing, &fixture.format, fixture.registry, {},
                                                    ValidationResult{ValidationStatus::Valid, 100, ""});
    RECOVERY_EXPECT_ERROR(media, ErrorCode::Cancelled);

    ValidationOptions options;
    options.limits.maxMemory = 0;
    RECOVERY_EXPECT_ERROR(fixture.run(options), ErrorCode::InvalidInput);
    options.limits = {};
    options.limits.maxDecodedBytes = 0;
    RECOVERY_EXPECT_ERROR(fixture.run(options), ErrorCode::InvalidInput);
    options.limits = {};
    options.limits.mp4.maxBoxes = 0;
    RECOVERY_EXPECT_ERROR(fixture.run(options), ErrorCode::InvalidInput);
    RECOVERY_EXPECT_OK(validate(MediaLimits{}));
}

TEST(ValidationLevelsTest, StructuralLevelsFromCarvingVerdicts) {
    EXPECT_EQ(structuralLevel({ValidationStatus::NotValidated, 0, ""}, "x").status, LevelStatus::NotRun);
    const LevelResult invalid = structuralLevel({ValidationStatus::Invalid, 12, "bad"}, "jpeg");
    EXPECT_EQ(invalid.status, LevelStatus::Failed);
    EXPECT_EQ(invalid.offset, 12U);
    EXPECT_EQ(invalid.checker, "jpeg structure");
    EXPECT_EQ(structuralLevel({ValidationStatus::Truncated, 5, ""}, "x").offset, 5U);
}

TEST(ValidationNamesTest, EveryValueHasAName) {
    EXPECT_EQ(toString(ValidationLevel::Structural), "structural");
    EXPECT_EQ(toString(ValidationLevel::Media), "media");
    EXPECT_EQ(toString(ValidationLevel::Playability), "playability");
    EXPECT_EQ(toString(LevelStatus::NotRun), "not run");
    EXPECT_EQ(toString(LevelStatus::Passed), "passed");
    EXPECT_EQ(toString(LevelStatus::Truncated), "truncated");
    EXPECT_EQ(toString(LevelStatus::Failed), "failed");
    EXPECT_EQ(toString(LevelStatus::NotApplicable), "not applicable");
    EXPECT_EQ(toString(LevelStatus::Unsupported), "unsupported");
    EXPECT_EQ(toString(Coverage::Full), "full");
    EXPECT_EQ(toString(Coverage::Partial), "partial");
}

TEST(MediaValidatorRegistryTest, AddFindAndDuplicates) {
    MediaValidatorRegistry registry;
    RECOVERY_EXPECT_ERROR(registry.add(nullptr), ErrorCode::InvalidInput);
    RECOVERY_EXPECT_OK(registry.add(std::make_shared<FakeMedia>("a", levelOf(LevelStatus::Passed))));
    RECOVERY_EXPECT_ERROR(registry.add(std::make_shared<FakeMedia>("a", levelOf(LevelStatus::Passed))),
                          ErrorCode::InvalidInput);
    EXPECT_NE(registry.find("a"), nullptr);
    EXPECT_EQ(registry.find("b"), nullptr);
    EXPECT_EQ(registry.size(), 1U);
}

TEST(MediaValidatorRegistryTest, TheEngineDecodersRegisterOnce) {
    MediaValidatorRegistry registry;
    RECOVERY_ASSERT_OK(registerMediaValidators(registry));
    EXPECT_EQ(registry.size(), mediaValidators().size());
    RECOVERY_EXPECT_ERROR(registerMediaValidators(registry), ErrorCode::InvalidInput);
    EXPECT_EQ(registry.size(), mediaValidators().size()) << "a failed registration changes nothing";
}

}  // namespace
}  // namespace recovery::validation
