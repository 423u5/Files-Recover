// P20: the test corpus, end to end. Every scenario of tests/support/corpus is
// written as an image file, scanned deep and quick through the GUI-facing API,
// and every candidate recovered; then
//  - what each original file must give back is checked (corpus::Outcome);
//  - every candidate called Complete must hold an original's bytes or a part of
//    an original (an embedded thumbnail, the frames of a stream after damage):
//    nothing else may be presented as a whole file;
//  - every recovered file must have the SHA-256 its candidate reports, and
//    candidates with the same bytes must point at the first of them;
//  - the session's report must export;
//  - and every candidate, as one line, must match the scenario's recorded
//    baseline (tests/corpus/baseline/<scenario>.txt): the regression suite.
//    The baseline starts with the image's SHA-256, so it also shows that the
//    scenario is built the same, byte for byte, on every machine and run.
//
// After a deliberate change of the engine's results, set RECOVERY_CORPUS_UPDATE=1
// to write the baselines again, and review the difference.

#include "api/recovery_api.hpp"
#include "recovery/byte_order.hpp"
#include "recovery/sha256.hpp"
#include "recovery/text.hpp"
#include "support/corpus.hpp"
#include "support/json_reader.hpp"
#include "support/test_files.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#ifndef RECOVERY_CORPUS_BASELINE_DIR
#error "RECOVERY_CORPUS_BASELINE_DIR must name tests/corpus/baseline"
#endif

namespace recovery::test::corpus {
namespace {

using api::CandidateInfo;
using api::Condition;
using namespace std::chrono_literals;

constexpr std::chrono::milliseconds kWait = 20min;

std::string shaOf(std::span<const std::byte> bytes) {
    return sha256(bytes).hex();
}

// Recovered paths may be longer than MAX_PATH.
std::filesystem::path longPath(const std::filesystem::path& path) {
    const std::wstring native = std::filesystem::absolute(path).lexically_normal().native();
    if (native.starts_with(L"\\\\?\\")) {
        return native;
    }
    return std::filesystem::path(L"\\\\?\\" + native);
}

std::optional<Bytes> readLong(const std::filesystem::path& path) {
    std::ifstream in(longPath(path), std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Bytes bytes(content.size());
    std::memcpy(bytes.data(), content.data(), content.size());
    return bytes;
}

bool conditionMatches(Expected expected, Condition condition) {
    switch (expected) {
        case Expected::Complete:
            return condition == Condition::Complete;
        case Expected::Unverified:
            return condition == Condition::Unverified;
        case Expected::Any:
            return true;
    }
    return false;
}

struct ScanRun {
    std::string session;
    std::vector<CandidateInfo> candidates;
    std::filesystem::path destination;
    // Candidate id -> the recovered file's bytes.
    std::map<std::uint64_t, Bytes> recovered;
};

std::vector<CandidateInfo> allCandidates(api::RecoveryApi& recovery, const std::string& session) {
    std::vector<CandidateInfo> all;
    api::CandidateQuery query;
    query.count = 500;
    for (;;) {
        Result<api::CandidatePage> page = recovery.getCandidates(session, query);
        EXPECT_TRUE(page.ok()) << (page.ok() ? std::string() : describe(page.error()));
        if (!page.ok() || page->candidates.empty()) {
            return all;
        }
        all.insert(all.end(), page->candidates.begin(), page->candidates.end());
        query.first += page->candidates.size();
    }
}

// Scans `image`, recovers every candidate below `destination`, and reads the
// candidates (with their recovery state) and the recovered files back.
std::optional<ScanRun> scanAndRecover(api::RecoveryApi& recovery, const std::filesystem::path& image,
                                      api::ScanMode mode, const std::filesystem::path& destination) {
    api::ScanSettings settings;
    settings.mode = mode;
    Result<std::string> started = recovery.startScan(api::SourceRef::imageFile(image), settings);
    if (!started.ok()) {
        ADD_FAILURE() << "startScan: " << describe(started.error());
        return std::nullopt;
    }
    ScanRun run;
    run.session = *started;
    run.destination = destination;
    Result<api::Progress> scanned = recovery.waitForOperation(run.session, kWait);
    if (!scanned.ok() || scanned->state != api::OperationState::Completed) {
        ADD_FAILURE() << "the " << api::toString(mode) << " scan did not complete: "
                      << (scanned.ok() ? std::string(api::toString(scanned->state)) + " " +
                                             (scanned->error ? describe(*scanned->error) : std::string())
                                       : describe(scanned.error()));
        return std::nullopt;
    }
    run.candidates = allCandidates(recovery, run.session);
    api::RecoveryOptions options;
    options.destination = destination;
    Result<api::RecoveryStart> recovering = recovery.recoverAll(run.session, options);
    if (!recovering.ok()) {
        ADD_FAILURE() << "recoverAll: " << describe(recovering.error());
        return std::nullopt;
    }
    if (run.candidates.empty()) {
        // Nothing found, nothing to do (L199): no job, and no destination folder.
        EXPECT_TRUE(recovering->jobs.empty());
        EXPECT_FALSE(std::filesystem::exists(destination));
        return run;
    }
    if (!recovering->jobs.empty()) {
        Result<api::Progress> written = recovery.waitForOperation(run.session, kWait);
        if (!written.ok() || written->state != api::OperationState::Completed) {
            ADD_FAILURE() << "recovery did not complete: "
                          << (written.ok() ? std::string(api::toString(written->state)) : describe(written.error()));
            return std::nullopt;
        }
    }
    run.candidates = allCandidates(recovery, run.session);
    for (const CandidateInfo& candidate : run.candidates) {
        if (candidate.recovery == api::RecoveryState::Recovered) {
            std::optional<Bytes> bytes = readLong(candidate.recoveredFile);
            if (!bytes) {
                ADD_FAILURE() << "#" << candidate.id.value << ": cannot read " << toUtf8(candidate.recoveredFile);
                continue;
            }
            run.recovered.emplace(candidate.id.value, std::move(*bytes));
        }
    }
    return run;
}

// What the corpus knows of a candidate's bytes.
struct Origin {
    const Original* original = nullptr;
    bool part = false;  // a contiguous part of the original, not all of it
};

class Originals {
public:
    explicit Originals(const std::vector<Original>& originals) : originals_(&originals) {
        for (const Original& original : originals) {
            bySha_.emplace(shaOf(original.bytes), &original);
        }
    }

    [[nodiscard]] std::optional<Origin> find(const CandidateInfo& candidate, const Bytes* bytes) const {
        if (!candidate.sha256.empty()) {
            if (const auto it = bySha_.find(candidate.sha256); it != bySha_.end()) {
                return Origin{it->second, false};
            }
        }
        if (bytes == nullptr) {
            return std::nullopt;
        }
        for (const Original& original : *originals_) {
            if (bytes->size() <= original.bytes.size() &&
                std::search(original.bytes.begin(), original.bytes.end(), bytes->begin(), bytes->end()) !=
                    original.bytes.end()) {
                return Origin{&original, true};
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::string label(const Original& original) const {
        if (!original.path.empty()) {
            return original.path;
        }
        const auto index = static_cast<std::size_t>(&original - originals_->data());
        return "(unnamed original " + std::to_string(index + 1) + ")";
    }

private:
    const std::vector<Original>* originals_;
    std::multimap<std::string, const Original*> bySha_;
};

std::string reasonsOf(const CandidateInfo& candidate) {
    std::string out;
    for (const api::ConditionReason reason : candidate.reasons) {
        out += (out.empty() ? "" : ",") + std::string(api::toString(reason));
    }
    return out;
}

// One line per candidate: what the regression baseline records.
std::string line(const CandidateInfo& c, const ScanRun& run, const Originals& originals) {
    std::ostringstream out;
    out << "#" << c.id.value << " " << api::toString(c.foundBy) << " " << api::toString(c.condition);
    if (!c.reasons.empty()) {
        out << "(" << reasonsOf(c) << ")";
    }
    out << " " << api::toString(c.kind) << "/" << (c.format.empty() ? "-" : c.format);
    out << " \"" << (c.path.empty() ? c.name : c.path) << "\"";
    out << " size=" << c.size << "/" << c.expectedSize;
    if (c.deleted) {
        out << " deleted";
    }
    if (c.fragmented) {
        out << " fragmented";
    }
    out << " valid=" << api::toString(c.validation.result) << ":" << api::toString(c.validation.structural) << "/"
        << api::toString(c.validation.media);
    out << " sha=" << (c.sha256.empty() ? std::string("-") : c.sha256.substr(0, 16));
    if (c.duplicateOf) {
        out << " dup=#" << c.duplicateOf->value;
    }
    if (c.container) {
        out << " in=#" << c.container->value;
    }
    if (c.unreadableBytes != 0) {
        out << " unreadable=" << c.unreadableBytes;
    }
    out << " recovery=" << api::toString(c.recovery);
    if (c.recovery == api::RecoveryState::Recovered) {
        std::string relative = toUtf8(c.recoveredFile.lexically_relative(run.destination));
        std::replace(relative.begin(), relative.end(), '\\', '/');
        out << (c.recoveredComplete ? "" : "(incomplete)") << " \"" << relative << "\"";
    }
    const auto it = run.recovered.find(c.id.value);
    if (const std::optional<Origin> origin = originals.find(c, it == run.recovered.end() ? nullptr : &it->second)) {
        out << (origin->part ? " => part of " : " => ") << originals.label(*origin->original);
    }
    return out.str();
}

std::vector<std::string> lines(const ScanRun& run, const Originals& originals) {
    std::vector<std::string> out;
    for (const CandidateInfo& candidate : run.candidates) {
        out.push_back(line(candidate, run, originals));
    }
    return out;
}

// The checks every scan must pass, whatever the scenario.
void checkCandidates(const ScanRun& run, const Originals& originals, std::string_view mode) {
    std::map<std::string, std::vector<const CandidateInfo*>> bySha;
    for (const CandidateInfo& c : run.candidates) {
        SCOPED_TRACE(std::string(mode) + " #" + std::to_string(c.id.value) + " " + c.path + " " + c.name);
        const auto file = run.recovered.find(c.id.value);
        const Bytes* bytes = file == run.recovered.end() ? nullptr : &file->second;
        if (c.condition == Condition::Complete) {
            // Nothing else may be presented as a whole file.
            EXPECT_TRUE(originals.find(c, bytes).has_value())
                << "a Complete candidate holds bytes that are no original's, nor a part of one";
            EXPECT_EQ(c.recovery, api::RecoveryState::Recovered) << "a Complete candidate was not recovered";
            EXPECT_TRUE(c.recoveredComplete);
            EXPECT_EQ(c.unreadableBytes, 0u);
        }
        if (bytes != nullptr) {
            EXPECT_EQ(bytes->size(), c.size) << "the recovered file's size is not the candidate's";
            if (!c.sha256.empty()) {
                EXPECT_EQ(shaOf(*bytes), c.sha256) << "the recovered file's SHA-256 is not the candidate's";
            }
        }
        if (c.duplicateOf) {
            EXPECT_LT(c.duplicateOf->value, c.id.value);
        }
        if (!c.sha256.empty()) {
            bySha[c.sha256].push_back(&c);
        }
    }
    // Candidates with the same bytes: each one after the first points at an earlier one with them.
    std::map<std::uint64_t, std::string> shaOfId;
    for (const CandidateInfo& c : run.candidates) {
        shaOfId[c.id.value] = c.sha256;
    }
    for (const auto& [digest, group] : bySha) {
        for (std::size_t i = 1; i < group.size(); ++i) {
            const CandidateInfo& c = *group[i];
            if (c.size == 0) {
                continue;
            }
            EXPECT_TRUE(c.duplicateOf.has_value())
                << mode << " #" << c.id.value << " has the bytes of #" << group.front()->id.value
                << " but is not marked as its duplicate";
            if (c.duplicateOf) {
                EXPECT_EQ(shaOfId[c.duplicateOf->value], digest) << mode << " #" << c.id.value;
            }
        }
    }
}

std::vector<const CandidateInfo*> atPath(const ScanRun& run, const std::string& path) {
    std::vector<const CandidateInfo*> found;
    for (const CandidateInfo& c : run.candidates) {
        if (!path.empty() && c.path == path) {
            found.push_back(&c);
        }
    }
    return found;
}

std::string listOf(const std::vector<const CandidateInfo*>& candidates) {
    std::string out;
    for (const CandidateInfo* c : candidates) {
        out += " #" + std::to_string(c->id.value) + ":" + std::string(api::toString(c->condition));
    }
    return out.empty() ? " none" : out;
}

// A candidate holding the original's bytes whole, in the expected condition,
// recovered byte for byte; at its path when `path` is set.
bool holdsWhole(const ScanRun& run, const Original& original, const std::string& path) {
    const std::string digest = shaOf(original.bytes);
    for (const CandidateInfo& c : run.candidates) {
        if (c.sha256 != digest || !conditionMatches(original.condition, c.condition)) {
            continue;
        }
        if (!path.empty() && c.path != path) {
            continue;
        }
        const auto file = run.recovered.find(c.id.value);
        if (file != run.recovered.end() && file->second == original.bytes) {
            return true;
        }
    }
    return false;
}

// The members stored (method 0) in a ZIP archive, by its central directory,
// and those of ZIP archives stored in them (depth levels down). A scan finds
// an image in an archive only where it is stored: compressed members hold no
// image signature. Archives this does not read (ZIP64) give nothing.
void storedMembers(std::span<const std::byte> zip, int depth, std::vector<Bytes>& out) {
    constexpr std::size_t kEndSize = 22;
    if (zip.size() < kEndSize) {
        return;
    }
    std::size_t end = zip.size() - kEndSize + 1;
    std::optional<std::size_t> found;
    while (end-- > 0 && zip.size() - end <= kEndSize + 0xFFFF) {
        if (loadLe32(zip, end) == 0x06054B50U) {
            found = end;
            break;
        }
    }
    if (!found) {
        return;
    }
    const std::uint16_t entries = loadLe16(zip, *found + 10);
    std::size_t at = loadLe32(zip, *found + 16);
    for (std::uint16_t i = 0; i < entries; ++i) {
        if (at + 46 > zip.size() || loadLe32(zip, at) != 0x02014B50U) {
            return;
        }
        const std::uint16_t method = loadLe16(zip, at + 10);
        const std::uint32_t packed = loadLe32(zip, at + 20);
        const std::uint32_t size = loadLe32(zip, at + 24);
        const std::size_t name = loadLe16(zip, at + 28);
        const std::size_t extra = loadLe16(zip, at + 30);
        const std::size_t comment = loadLe16(zip, at + 32);
        const std::size_t local = loadLe32(zip, at + 42);
        at += 46 + name + extra + comment;
        if (method != 0 || packed != size || local + 30 > zip.size() || loadLe32(zip, local) != 0x04034B50U) {
            continue;
        }
        const std::size_t data = local + 30 + loadLe16(zip, local + 26) + loadLe16(zip, local + 28);
        if (data > zip.size() || size > zip.size() - data) {
            continue;
        }
        const std::span<const std::byte> member = zip.subspan(data, size);
        out.emplace_back(member.begin(), member.end());
        if (depth > 0 && size >= 4 && loadLe32(member, 0) == 0x04034B50U) {
            storedMembers(member, depth - 1, out);
        }
    }
}

// A recovered Complete archive candidate holds `bytes` as a stored member (or
// as a member of an archive stored in it).
bool heldByRecoveredArchive(const ScanRun& run, const Bytes& bytes) {
    for (const CandidateInfo& c : run.candidates) {
        if (c.format != "zip" || c.condition != Condition::Complete) {
            continue;
        }
        const auto file = run.recovered.find(c.id.value);
        if (file == run.recovered.end()) {
            continue;
        }
        std::vector<Bytes> members;
        storedMembers(file->second, 3, members);
        if (std::find(members.begin(), members.end(), bytes) != members.end()) {
            return true;
        }
    }
    return false;
}

void checkOriginals(const ScanRun& run, const std::vector<Original>& originals, bool deep) {
    for (const Original& original : originals) {
        SCOPED_TRACE(std::string(deep ? "deep" : "quick") + " scan, original " +
                     (original.path.empty() ? std::string("(no name)") : original.path) + " (" +
                     std::string(toString(original.outcome)) + "): " + original.note);
        const std::vector<const CandidateInfo*> named = atPath(run, original.path);
        const bool anyCompleteAtPath = std::any_of(named.begin(), named.end(), [](const CandidateInfo* c) {
            return c->condition == Condition::Complete;
        });
        switch (original.outcome) {
            case Outcome::Named:
                if (deep || !original.deepOnly) {
                    EXPECT_TRUE(holdsWhole(run, original, original.path))
                        << "no candidate at its path holds its bytes in condition " << toString(original.condition)
                        << "; at its path:" << listOf(named);
                } else {
                    EXPECT_FALSE(named.empty()) << "no candidate at its path";
                }
                break;
            case Outcome::Carved:
                if (deep) {
                    EXPECT_TRUE(holdsWhole(run, original, {})) << "no candidate holds its bytes whole";
                }
                break;
            case Outcome::NamedDamaged:
                EXPECT_FALSE(named.empty()) << "no candidate at its path";
                EXPECT_FALSE(anyCompleteAtPath) << "a candidate at its path is called Complete:" << listOf(named);
                break;
            case Outcome::NamedDamagedCarved:
                EXPECT_FALSE(named.empty()) << "no candidate at its path";
                EXPECT_FALSE(anyCompleteAtPath) << "a candidate at its path is called Complete:" << listOf(named);
                if (deep) {
                    EXPECT_TRUE(holdsWhole(run, original, {})) << "carving did not give its bytes back whole";
                }
                break;
            case Outcome::InsideArchive:
                if (deep) {
                    const std::string digest = shaOf(original.bytes);
                    EXPECT_FALSE(std::any_of(run.candidates.begin(), run.candidates.end(),
                                             [&](const CandidateInfo& c) { return c.sha256 == digest; }))
                        << "a candidate of its own: a needless copy of what an archive holds";
                    EXPECT_TRUE(heldByRecoveredArchive(run, original.bytes))
                        << "no recovered Complete archive holds it as a stored member";
                }
                break;
            case Outcome::Lost: {
                EXPECT_FALSE(anyCompleteAtPath) << "a candidate at its path is called Complete:" << listOf(named);
                const std::string digest = shaOf(original.bytes);
                EXPECT_FALSE(std::any_of(run.candidates.begin(), run.candidates.end(),
                                         [&](const CandidateInfo& c) { return c.sha256 == digest; }))
                    << "a candidate holds the bytes of a file that is lost";
                break;
            }
        }
    }
}

// Every recovered file is a candidate's, and nothing else is left in the folder.
void checkDestination(const ScanRun& run) {
    std::set<std::filesystem::path> expected;
    for (const CandidateInfo& c : run.candidates) {
        if (c.recovery == api::RecoveryState::Recovered) {
            expected.insert(c.recoveredFile.lexically_normal());
        }
    }
    std::size_t files = 0;
    std::error_code ec;
    if (!std::filesystem::exists(longPath(run.destination), ec)) {
        EXPECT_TRUE(expected.empty()) << "candidates were recovered, but there is no destination folder";
        return;
    }
    for (std::filesystem::recursive_directory_iterator it(longPath(run.destination), ec), end; !ec && it != end;
         it.increment(ec)) {
        if (it->is_regular_file()) {
            ++files;
        }
    }
    EXPECT_FALSE(ec) << ec.message();
    EXPECT_EQ(files, expected.size()) << "the destination holds files no candidate was recovered to";
}

std::filesystem::path baselinePath(const std::string& scenario) {
    return std::filesystem::path(RECOVERY_CORPUS_BASELINE_DIR) / (scenario + ".txt");
}

bool updating() {
    wchar_t* value = nullptr;
    std::size_t length = 0;
    if (_wdupenv_s(&value, &length, L"RECOVERY_CORPUS_UPDATE") != 0 || value == nullptr) {
        return false;
    }
    const bool update = std::wstring_view(value) == L"1";
    std::free(value);
    return update;
}

std::vector<std::string> readLines(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<std::string> out;
    std::string text;
    while (std::getline(in, text)) {
        if (!text.empty() && text.back() == '\r') {
            text.pop_back();
        }
        out.push_back(text);
    }
    return out;
}

void compareWithBaseline(const std::string& scenario, const std::vector<std::string>& actual) {
    const std::filesystem::path path = baselinePath(scenario);
    if (updating()) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        for (const std::string& text : actual) {
            out << text << '\n';
        }
        ASSERT_TRUE(out.good()) << "cannot write " << toUtf8(path);
        GTEST_SKIP() << "baseline written: " << toUtf8(path);
    }
    ASSERT_TRUE(std::filesystem::exists(path))
        << "no baseline " << toUtf8(path) << ": run with RECOVERY_CORPUS_UPDATE=1 and review it";
    const std::vector<std::string> expected = readLines(path);
    if (expected == actual) {
        return;
    }
    std::ostringstream difference;
    const std::size_t count = std::max(expected.size(), actual.size());
    std::size_t shown = 0;
    for (std::size_t i = 0; i < count && shown < 40; ++i) {
        const std::string* e = i < expected.size() ? &expected[i] : nullptr;
        const std::string* a = i < actual.size() ? &actual[i] : nullptr;
        if (e != nullptr && a != nullptr && *e == *a) {
            continue;
        }
        if (e != nullptr) {
            difference << "- " << *e << "\n";
        }
        if (a != nullptr) {
            difference << "+ " << *a << "\n";
        }
        ++shown;
    }
    ADD_FAILURE() << "the results differ from the baseline " << toUtf8(path)
                  << " (RECOVERY_CORPUS_UPDATE=1 writes them, after review):\n"
                  << difference.str();
}

std::vector<std::string> scenarioNames() {
    std::vector<std::string> names;
    for (const Scenario& scenario : scenarios()) {
        names.push_back(scenario.name);
    }
    return names;
}

class CorpusTest : public ::testing::TestWithParam<std::string> {};

TEST_P(CorpusTest, RecoversWhatItMustAndMatchesTheBaseline) {
    const Scenario* scenario = findScenario(GetParam());
    ASSERT_NE(scenario, nullptr);
    const Image image = generate(*scenario);
    TempDir folder;
    ASSERT_TRUE(write(folder.path(), *scenario, image, false).ok());
    const std::filesystem::path imagePath = folder / (scenario->name + ".img");

    api::ApiOptions options;
    options.sessionsRoot = folder / "sessions";
    Result<std::unique_ptr<api::RecoveryApi>> created = api::RecoveryApi::create(options);
    ASSERT_TRUE(created.ok()) << describe(created.error());
    api::RecoveryApi& recovery = **created;

    const std::optional<ScanRun> deep = scanAndRecover(recovery, imagePath, api::ScanMode::Deep, folder / "deep");
    ASSERT_TRUE(deep.has_value());
    const std::optional<ScanRun> quick = scanAndRecover(recovery, imagePath, api::ScanMode::Quick, folder / "quick");
    ASSERT_TRUE(quick.has_value());

    const Originals originals(image.originals);
    checkCandidates(*deep, originals, "deep");
    checkCandidates(*quick, originals, "quick");
    checkOriginals(*deep, image.originals, true);
    checkOriginals(*quick, image.originals, false);
    checkDestination(*deep);
    checkDestination(*quick);

    // The report exports, as JSON a program can read and as text.
    const std::filesystem::path json = folder / "report.json";
    ASSERT_TRUE(recovery.exportReport(deep->session, json).ok());
    {
        std::ifstream in(json, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::string error;
        EXPECT_TRUE(json::parse(text, &error).has_value()) << error;
    }
    api::ReportOptions textReport;
    textReport.format = api::ReportFormat::Text;
    textReport.details = true;
    EXPECT_TRUE(recovery.exportReport(deep->session, folder / "report.txt", textReport).ok());

    std::vector<std::string> baseline;
    baseline.push_back("# corpus scenario " + scenario->name + ": " + scenario->summary);
    baseline.push_back("# image " + std::to_string(image.bytes.size()) + " bytes, sha256 " +
                       shaOf(image.bytes).substr(0, 16) + ", " + std::to_string(image.originals.size()) + " originals");
    baseline.push_back("# deep scan: " + std::to_string(deep->candidates.size()) + " candidates");
    for (std::string& text : lines(*deep, originals)) {
        baseline.push_back(std::move(text));
    }
    baseline.push_back("# quick scan: " + std::to_string(quick->candidates.size()) + " candidates");
    for (std::string& text : lines(*quick, originals)) {
        baseline.push_back(std::move(text));
    }
    if (::testing::Test::HasFailure()) {
        // Show what the engine did, for the failures above.
        std::string all;
        for (const std::string& text : baseline) {
            all += text + "\n";
        }
        ADD_FAILURE() << "what the scans found:\n" << all;
        return;
    }
    compareWithBaseline(scenario->name, baseline);
}

INSTANTIATE_TEST_SUITE_P(Scenarios, CorpusTest, ::testing::ValuesIn(scenarioNames()),
                         [](const ::testing::TestParamInfo<std::string>& info) {
                             std::string name = info.param;
                             std::replace(name.begin(), name.end(), '-', '_');
                             return name;
                         });

TEST(CorpusCoverageTest, EveryCategoryOfThePlanHasAScenario) {
    std::set<std::string> covered;
    for (const Scenario& scenario : scenarios()) {
        for (const std::string& category : scenario.categories) {
            EXPECT_NE(std::find(categories().begin(), categories().end(), category), categories().end())
                << scenario.name << ": unknown category " << category;
            covered.insert(category);
        }
    }
    for (const std::string& category : categories()) {
        EXPECT_TRUE(covered.contains(category)) << "no scenario covers " << category;
    }
}

TEST(CorpusCoverageTest, EveryBaselineBelongsToAScenario) {
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(RECOVERY_CORPUS_BASELINE_DIR, ec)) {
        const std::string stem = toUtf8(entry.path().stem());
        EXPECT_NE(findScenario(stem), nullptr) << "a baseline without a scenario: " << toUtf8(entry.path());
    }
}

}  // namespace
}  // namespace recovery::test::corpus
