// Format registration: descriptors are checked against every rule before a
// format is accepted, ids are unique, lookups and order, and scanners take
// a snapshot of the registry.

#include "carving/format_registry.hpp"

#include "carving/signature_scanner.hpp"
#include "support/carving_formats.hpp"
#include "support/memory_source.hpp"
#include "support/test_macros.hpp"

#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace recovery::carving {
namespace {

using test::scriptedDescriptor;
using test::ScriptedFormat;

std::shared_ptr<const IFileFormat> scripted(FormatDescriptor descriptor) {
    return std::make_shared<ScriptedFormat>(std::move(descriptor));
}

TEST(FormatRegistryTest, KeepsFormatsInRegistrationOrder) {
    FormatRegistry registry;
    EXPECT_TRUE(registry.empty());
    auto sized = std::make_shared<test::SizedFormat>();
    auto marker = std::make_shared<test::MarkerFormat>();
    auto box = std::make_shared<test::BoxFormat>();
    RECOVERY_ASSERT_OK(registry.add(sized));
    RECOVERY_ASSERT_OK(registry.add(marker));
    RECOVERY_ASSERT_OK(registry.add(box));

    ASSERT_EQ(registry.size(), 3u);
    EXPECT_EQ(registry.formats()[0], sized);
    EXPECT_EQ(registry.formats()[1], marker);
    EXPECT_EQ(registry.formats()[2], box);
    EXPECT_EQ(registry.find("marker"), marker.get());
    EXPECT_EQ(registry.indexOf("box"), 2u);
    EXPECT_EQ(registry.find("jpeg"), nullptr);
    EXPECT_FALSE(registry.indexOf("jpeg").has_value());
}

TEST(FormatRegistryTest, EveryTestFormatHasAValidDescriptor) {
    for (const std::shared_ptr<const IFileFormat>& format :
         std::vector<std::shared_ptr<const IFileFormat>>{
             std::make_shared<test::SizedFormat>(), std::make_shared<test::MarkerFormat>(),
             std::make_shared<test::BoxFormat>(), std::make_shared<test::SyncFormat>()}) {
        RECOVERY_EXPECT_OK(validateDescriptor(format->descriptor()));
    }
}

TEST(FormatRegistryTest, RejectsNullAndDuplicateFormats) {
    FormatRegistry registry;
    RECOVERY_EXPECT_ERROR(registry.add(nullptr), ErrorCode::InvalidInput);
    RECOVERY_ASSERT_OK(registry.add(std::make_shared<test::SizedFormat>()));
    // Same id, different format: refused, and the registry keeps the first.
    RECOVERY_EXPECT_ERROR(registry.add(scripted(scriptedDescriptor("sized", "OTHR"))), ErrorCode::InvalidInput);
    ASSERT_EQ(registry.size(), 1u);
    EXPECT_EQ(registry.formats()[0]->descriptor().name, "Sized test format");
}

struct DescriptorCase {
    std::string name;
    std::function<void(FormatDescriptor&)> change;
};

TEST(FormatRegistryTest, RejectsDescriptorsBreakingARule) {
    const std::vector<DescriptorCase> cases = {
        {"empty id", [](FormatDescriptor& d) { d.id.clear(); }},
        {"upper-case id", [](FormatDescriptor& d) { d.id = "JPEG"; }},
        {"id with a space", [](FormatDescriptor& d) { d.id = "my format"; }},
        {"id with a dot", [](FormatDescriptor& d) { d.id = "a.b"; }},
        {"id too long", [](FormatDescriptor& d) { d.id = std::string(FormatDescriptor::kMaxIdLength + 1, 'a'); }},
        {"no name", [](FormatDescriptor& d) { d.name.clear(); }},
        {"no extension", [](FormatDescriptor& d) { d.extension.clear(); }},
        {"extension with a dot", [](FormatDescriptor& d) { d.extension = ".jpg"; }},
        {"upper-case extension", [](FormatDescriptor& d) { d.extension = "JPG"; }},
        {"extension with a separator", [](FormatDescriptor& d) { d.extension = "a/b"; }},
        {"extension too long",
         [](FormatDescriptor& d) { d.extension = std::string(FormatDescriptor::kMaxExtensionLength + 1, 'a'); }},
        {"no signatures", [](FormatDescriptor& d) { d.signatures.clear(); }},
        {"too many signatures",
         [](FormatDescriptor& d) {
             d.signatures.assign(FormatDescriptor::kMaxSignatures + 1, textSignature("many", "ab"));
         }},
        {"invalid signature", [](FormatDescriptor& d) { d.signatures.push_back(textSignature("one byte", "a")); }},
        {"minimum below the signature reach", [](FormatDescriptor& d) { d.minimumSize = 3; }},
        {"maximum below the minimum", [](FormatDescriptor& d) { d.maximumSize = d.minimumSize - 1; }},
        {"maximum above the limit", [](FormatDescriptor& d) { d.maximumSize = FormatDescriptor::kMaxMaximumSize + 1; }},
        {"header below the signature reach", [](FormatDescriptor& d) { d.headerSize = 3; }},
        {"header above the limit", [](FormatDescriptor& d) { d.headerSize = FormatDescriptor::kMaxHeaderSize + 1; }},
        {"unknown end detection", [](FormatDescriptor& d) { d.endDetection = static_cast<EndDetectionMethod>(99); }},
        {"unknown extraction", [](FormatDescriptor& d) { d.extraction = static_cast<ExtractionStrategy>(99); }},
    };
    for (const DescriptorCase& test : cases) {
        SCOPED_TRACE(test.name);
        FormatDescriptor descriptor = scriptedDescriptor("fmt", "MAGC");
        RECOVERY_ASSERT_OK(validateDescriptor(descriptor));
        test.change(descriptor);
        RECOVERY_EXPECT_ERROR(validateDescriptor(descriptor), ErrorCode::InvalidInput);
        FormatRegistry registry;
        RECOVERY_EXPECT_ERROR(registry.add(scripted(descriptor)), ErrorCode::InvalidInput);
        EXPECT_TRUE(registry.empty());
    }
}

TEST(FormatRegistryTest, AcceptsDescriptorsAtTheLimits) {
    FormatDescriptor descriptor = scriptedDescriptor(std::string(FormatDescriptor::kMaxIdLength, 'z'), "MAGC");
    descriptor.extension = std::string(FormatDescriptor::kMaxExtensionLength, 'x');
    descriptor.maximumSize = FormatDescriptor::kMaxMaximumSize;
    descriptor.headerSize = FormatDescriptor::kMaxHeaderSize;
    descriptor.signatures.assign(FormatDescriptor::kMaxSignatures, textSignature("many", "ab"));
    descriptor.signatures.front() = textSignature("far", "ab", FileSignature::kMaxOffset);
    descriptor.minimumSize = FileSignature::kMaxOffset + 2;
    descriptor.endDetection = EndDetectionMethod::None;
    FormatRegistry registry;
    RECOVERY_EXPECT_OK(registry.add(scripted(descriptor)));
    // minimum == maximum is allowed: files of one fixed size.
    FormatDescriptor fixed = scriptedDescriptor("fixed", "FIXD");
    fixed.maximumSize = fixed.minimumSize;
    RECOVERY_EXPECT_OK(registry.add(scripted(fixed)));
}

TEST(FormatRegistryTest, ScannerNeedsAtLeastOneFormat) {
    const FormatRegistry registry;
    RECOVERY_EXPECT_ERROR(SignatureScanner::create(registry), ErrorCode::InvalidInput);
}

TEST(FormatRegistryTest, ScannerKeepsTheFormatsItWasCreatedWith) {
    auto registry = std::make_unique<FormatRegistry>();
    RECOVERY_ASSERT_OK(registry->add(std::make_shared<test::SizedFormat>()));
    Result<SignatureScanner> scanner = SignatureScanner::create(*registry);
    RECOVERY_ASSERT_OK(scanner);
    // Added after the scanner was created: not scanned for.
    RECOVERY_ASSERT_OK(registry->add(std::make_shared<test::MarkerFormat>()));
    ASSERT_EQ(scanner->formats().size(), 1u);

    std::vector<std::byte> data(4096);
    const std::vector<std::byte> sized = test::makeSizedFile(20);
    const std::vector<std::byte> marker = test::makeMarkerFile(20);
    std::copy(sized.begin(), sized.end(), data.begin() + 100);
    std::copy(marker.begin(), marker.end(), data.begin() + 1000);
    // The registry may even be gone: the scanner shares ownership of its formats.
    registry.reset();

    test::MemoryStorageSource source(std::move(data));
    RECOVERY_ASSERT_OK(source.open());
    std::vector<SignatureHit> hits;
    Result<ScanReport> report = scanner->scan(source, [&](const SignatureHit& hit) {
        hits.push_back(hit);
        return success();
    });
    RECOVERY_ASSERT_OK(report);
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0].fileOffset, 100u);
    EXPECT_EQ(hits[0].format->descriptor().id, "sized");
}

}  // namespace
}  // namespace recovery::carving
