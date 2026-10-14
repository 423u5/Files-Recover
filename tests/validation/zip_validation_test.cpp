// ZIP at the validation levels: the structure decodes every member and
// checks its CRC-32, so the media level has nothing of its own to decode; an
// archive the structure cannot check is Unsupported, never Valid.

#include "recovery/byte_order.hpp"
#include "support/test_macros.hpp"
#include "support/zip_samples.hpp"
#include "validation/media_test_helpers.hpp"

#include <gtest/gtest.h>

#include <string>

namespace recovery::validation {
namespace {

using carving::ValidationStatus;
using testing::Bytes;
using testing::validated;

Bytes vector(std::string_view name) {
    return test::zip_samples::named(name).data();
}

TEST(ZipValidationTest, AnIntactArchivePassesItsStructureAndHasNoMediaOfItsOwn) {
    const ValidationState state = validated("zip", vector("docx"));
    EXPECT_EQ(state.structural.status, LevelStatus::Passed) << testing::describe(state);
    EXPECT_EQ(state.structural.checker, "zip structure");
    EXPECT_EQ(state.media.status, LevelStatus::NotApplicable);
    EXPECT_EQ(state.media.checker, "zip members");
    EXPECT_EQ(state.status(), ValidationStatus::Valid);
    EXPECT_EQ(state.deepestPassed(), ValidationLevel::Structural);
}

TEST(ZipValidationTest, AnArchiveTheStructureCannotCheckIsUnsupported) {
    const ValidationState state = validated("zip", vector("lzma"));
    EXPECT_EQ(state.structural.status, LevelStatus::Unsupported) << testing::describe(state);
    EXPECT_NE(state.structural.detail.find("uses LZMA"), std::string::npos) << state.structural.detail;
    EXPECT_EQ(state.status(), ValidationStatus::NotValidated);
}

TEST(ZipValidationTest, ADamagedArchiveFailsItsStructure) {
    Bytes data = vector("stored");
    data[data.size() / 3] ^= std::byte{0x20};
    const ValidationState state = validated("zip", data);
    EXPECT_EQ(state.structural.status, LevelStatus::Failed) << testing::describe(state);
    EXPECT_EQ(state.media.status, LevelStatus::NotRun);
    EXPECT_EQ(state.status(), ValidationStatus::Invalid);
}

}  // namespace
}  // namespace recovery::validation
