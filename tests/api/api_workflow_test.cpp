// P19's acceptance: a user interface implements the whole workflow through
// the GUI-facing API alone (gui_workflow.cpp, compiled with nothing but the
// API's public headers on its include path), on an image file and on a
// physical disk; and those headers include nothing of the engine.

#include "api_test_support.hpp"
#include "gui_workflow.hpp"

#include "support/json_reader.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <regex>
#include <set>
#include <sstream>

namespace recovery::api::test {
namespace {

std::string stepsOf(const gui::WorkflowResult& result) {
    std::string text;
    for (const std::string& step : result.steps) {
        text += "  " + step + "\n";
    }
    return text + "error: " + result.error;
}

// The files recovered, by name, must be the card's originals byte for byte.
void expectOriginals(const gui::WorkflowResult& result, const std::filesystem::path& destination) {
    const std::map<std::string, Bytes> files = ::recovery::test::filesBelow(destination);
    EXPECT_EQ(files.size(), result.recovered);
    for (const auto& [name, bytes] : ::recovery::test::cardOriginals()) {
        const auto found = files.find(name);
        ASSERT_NE(found, files.end()) << name << " was not recovered";
        EXPECT_EQ(found->second, bytes) << name << " differs from the original";
    }
}

TEST(ApiWorkflowTest, AUserInterfaceRecoversAnImageThroughTheApiAlone) {
    ApiWorld world;
    const std::filesystem::path destination = world.folder() / "Recovered";
    const std::filesystem::path report = world.folder() / "report.json";
    const gui::WorkflowResult result = gui::runWorkflow(world.api(), world.cardSource(), destination, report);
    ASSERT_TRUE(result.error.empty()) << stepsOf(result);
    EXPECT_EQ(result.volumes, 1u);
    EXPECT_EQ(result.candidates, expectedCard().size());
    EXPECT_EQ(result.recovered, expectedCard().size());
    EXPECT_GT(result.previews, 0u);
    expectOriginals(result, destination);

    // The report is a document any JSON reader takes, and lists every file.
    std::ifstream in(report, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    std::string error;
    const std::optional<::recovery::test::json::Value> document = ::recovery::test::json::parse(text.str(), &error);
    ASSERT_TRUE(document.has_value()) << error;
    EXPECT_EQ((*document)["format"].string(), "recovery-session-report");
    EXPECT_EQ((*document)["candidates"].size(), expectedCard().size());
    EXPECT_EQ((*document)["jobs"].size(), 1u);
}

TEST(ApiWorkflowTest, AUserInterfaceRecoversAPhysicalDiskThroughTheApiAlone) {
    ApiWorld world;
    // The card itself, as a simulated physical disk.
    const std::uint32_t card = world.addDisk(::recovery::test::readFile(world.cardImage()));
    storage::AttachedDisk listed;
    listed.number = card;
    listed.sizeBytes = std::filesystem::file_size(world.cardImage());
    listed.logicalSectorSize = 512;
    listed.vendor = "Simulated";
    listed.product = "Card Reader";
    listed.removable = true;
    listed.bus = "USB";
    world.hooks().diskLister = [listed] { return Result<std::vector<storage::AttachedDisk>>(std::vector{listed}); };
    const std::filesystem::path destination = world.folder() / "FromDisk";
    const gui::WorkflowResult result =
        gui::runWorkflow(world.api(), SourceRef::physicalDisk(card), destination, world.folder() / "disk.json");
    ASSERT_TRUE(result.error.empty()) << stepsOf(result);
    EXPECT_EQ(result.disks, 1u);
    EXPECT_EQ(result.candidates, expectedCard().size());
    expectOriginals(result, destination);
}

TEST(ApiWorkflowTest, TheGuiFacingHeadersIncludeNothingOfTheEngine) {
    // Besides the standard library: each other and the engine's structured
    // errors. (The workflow above is compiled with nothing else on its
    // include path; this says so in a test's words.)
    const std::set<std::string> allowed = {"api/api_types.hpp", "recovery/error.hpp", "recovery/result.hpp"};
    const std::regex quoted(R"re(^\s*#\s*include\s*"([^"]+)")re");
    for (const char* name : {"api/recovery_api.hpp", "api/api_types.hpp"}) {
        std::ifstream header(std::filesystem::path(RECOVERY_SOURCE_DIR) / "include" / name);
        ASSERT_TRUE(header.is_open()) << name;
        std::string line;
        while (std::getline(header, line)) {
            std::smatch match;
            if (std::regex_search(line, match, quoted)) {
                EXPECT_TRUE(allowed.contains(match[1].str())) << name << " includes " << match[1].str();
            }
        }
    }
}

}  // namespace
}  // namespace recovery::api::test
