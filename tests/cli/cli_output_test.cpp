// The CLI's output (P18): strings from untrusted disks made safe to print,
// sizes, counts, times and durations, command lines to type, tables, JSON
// that a strict reader takes; and the interrupt that Ctrl+C goes through.

#include "cli/format.hpp"
#include "cli/interrupt.hpp"
#include "cli_test_support.hpp"
#include "report/json_writer.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <limits>
#include <sstream>
#include <thread>

namespace recovery::cli {
namespace {

using namespace std::chrono_literals;

// U+FFFD in UTF-8.
constexpr std::string_view kReplacement = "\xEF\xBF\xBD";

TEST(CliOutputTest, PrintableKeepsOrdinaryText) {
    EXPECT_EQ(printable("photo.jpg"), "photo.jpg");
    EXPECT_EQ(printable("/DCIM/100MEDIA/IMG 0001.JPG"), "/DCIM/100MEDIA/IMG 0001.JPG");
    // German, Japanese and an emoji (four bytes).
    const std::string text = "Gr\xC3\xBC\xC3\x9F" "e \xE6\x97\xA5\xE6\x9C\xAC \xF0\x9F\x8E\x89";
    EXPECT_EQ(printable(text), text);
    EXPECT_EQ(printable(""), "");
}

TEST(CliOutputTest, PrintableEscapesControlCharacters) {
    // An escape sequence that would turn the terminal red, and one that clears it.
    EXPECT_EQ(printable("a\x1B[31mred"), "a\\x1B[31mred");
    EXPECT_EQ(printable("line\nbreak\ttab\rreturn"), "line\\x0Abreak\\x09tab\\x0Dreturn");
    EXPECT_EQ(printable(std::string_view("nul\0byte", 8)), "nul\\x00byte");
    EXPECT_EQ(printable("\x7F"), "\\x7F");
    // C1 controls: U+009B is the one-byte CSI of some terminals.
    EXPECT_EQ(printable("\xC2\x9B" "2J"), "\\u{009B}2J");
    EXPECT_EQ(printable("\xC2\x85"), "\\u{0085}");
}

TEST(CliOutputTest, PrintableShowsCharactersThatReorderText) {
    // "evil<RLO>gpj.exe" shows as "evilexe.jpg" on a terminal that honours the override.
    EXPECT_EQ(printable("evil\xE2\x80\xAE" "gpj.exe"), "evil\\u{202E}gpj.exe");
    EXPECT_EQ(printable("\xE2\x81\xA6x\xE2\x81\xA9"), "\\u{2066}x\\u{2069}");
    EXPECT_EQ(printable("\xE2\x80\x8F"), "\\u{200F}");
    EXPECT_EQ(printable("\xD8\x9C"), "\\u{061C}");
    EXPECT_EQ(printable("a\xE2\x80\xA8" "b\xE2\x80\xA9"), "a\\u{2028}b\\u{2029}");
    // Joiners are kept: emoji sequences and scripts use them.
    EXPECT_EQ(printable("\xE2\x80\x8D"), "\xE2\x80\x8D");
}

TEST(CliOutputTest, PrintableReplacesInvalidUtf8) {
    const auto replaced = [](std::size_t count) {
        std::string text;
        for (std::size_t i = 0; i < count; ++i) {
            text += kReplacement;
        }
        return text;
    };
    EXPECT_EQ(printable("\x80"), replaced(1));
    EXPECT_EQ(printable("\xFF\xFE"), replaced(2));
    // An overlong '/', a surrogate, a value beyond U+10FFFF.
    EXPECT_EQ(printable("\xC0\xAF"), replaced(2));
    EXPECT_EQ(printable("\xED\xA0\x80"), replaced(3));
    EXPECT_EQ(printable("\xF4\x90\x80\x80"), replaced(4));
    // Cut short, at the end and before other text.
    EXPECT_EQ(printable("ab\xE6\x97"), "ab" + replaced(2));
    EXPECT_EQ(printable("\xE6\x97" "x"), replaced(2) + "x");
}

TEST(CliOutputTest, Sizes) {
    EXPECT_EQ(formatSize(0), "0 B");
    EXPECT_EQ(formatSize(1023), "1023 B");
    EXPECT_EQ(formatSize(1024), "1.0 KiB");
    EXPECT_EQ(formatSize(1536), "1.5 KiB");
    EXPECT_EQ(formatSize(2 * 1024 * 1024), "2.0 MiB");
    EXPECT_EQ(formatSize(std::uint64_t{5} * 1024 * 1024 * 1024 / 2), "2.5 GiB");
    EXPECT_EQ(formatSize(std::numeric_limits<std::uint64_t>::max()), "16.0 EiB");
    EXPECT_EQ(formatBytes(1), "1 byte");
    EXPECT_EQ(formatBytes(512), "512 bytes");
    EXPECT_EQ(formatBytes(2'097'152), "2,097,152 bytes (2.0 MiB)");
    EXPECT_EQ(formatRate(1536), "1.5 KiB/s");
}

TEST(CliOutputTest, Counts) {
    EXPECT_EQ(formatCount(0), "0");
    EXPECT_EQ(formatCount(999), "999");
    EXPECT_EQ(formatCount(1000), "1,000");
    EXPECT_EQ(formatCount(12'345), "12,345");
    EXPECT_EQ(formatCount(123'456), "123,456");
    EXPECT_EQ(formatCount(1'234'567), "1,234,567");
    EXPECT_EQ(formatCount(std::numeric_limits<std::uint64_t>::max()), "18,446,744,073,709,551,615");
}

TEST(CliOutputTest, DurationsAndShares) {
    EXPECT_EQ(formatDuration(0ms), "0:00:00");
    EXPECT_EQ(formatDuration(999ms), "0:00:00");
    EXPECT_EQ(formatDuration(61s), "0:01:01");
    EXPECT_EQ(formatDuration(3h + 2min + 1s), "3:02:01");
    EXPECT_EQ(formatDuration(100h), "100:00:00");
    EXPECT_EQ(formatDuration(-5s), "0:00:00");
    EXPECT_EQ(formatPercent(1, 3), "33.3%");
    EXPECT_EQ(formatPercent(3, 3), "100.0%");
    EXPECT_EQ(formatPercent(7, 5), "100.0%");
    EXPECT_EQ(formatPercent(5, 0), "");
}

TEST(CliOutputTest, Times) {
    using namespace std::chrono;
    const session::SessionTime time = sys_days{2026y / 10 / 8} + 10h + 15min + 30s + 125ms;
    EXPECT_EQ(formatTime(time), "2026-10-08 10:15:30 UTC");
    EXPECT_EQ(formatIsoTime(time), "2026-10-08T10:15:30.125Z");
    EXPECT_EQ(formatTimestamp(filesystem::Timestamp{time, true}), "2026-10-08 10:15:30 (local time)");
    EXPECT_EQ(formatTimestamp(filesystem::Timestamp{time, false}), "2026-10-08 10:15:30 UTC");
    EXPECT_EQ(formatIsoTimestamp(filesystem::Timestamp{time, true}), "2026-10-08T10:15:30.125");
    EXPECT_EQ(formatIsoTimestamp(filesystem::Timestamp{time, false}), "2026-10-08T10:15:30.125Z");
    // NTFS times go back to 1601.
    const session::SessionTime early = sys_days{1601y / 1 / 1} + 1ms;
    EXPECT_EQ(formatTime(early), "1601-01-01 00:00:00 UTC");
    EXPECT_EQ(formatIsoTimestamp(filesystem::Timestamp{early, false}), "1601-01-01T00:00:00.001Z");
}

TEST(CliOutputTest, QuoteArgument) {
    EXPECT_EQ(quoteArgument("plain"), "plain");
    EXPECT_EQ(quoteArgument("D:\\Recovered"), "D:\\Recovered");
    EXPECT_EQ(quoteArgument(""), "\"\"");
    EXPECT_EQ(quoteArgument("D:\\My Files"), "\"D:\\My Files\"");
    // A backslash before the closing quote is doubled, or it would escape it.
    EXPECT_EQ(quoteArgument("D:\\My Files\\"), "\"D:\\My Files\\\\\"");
    EXPECT_EQ(quoteArgument("say \"hi\""), "\"say \\\"hi\\\"\"");
    EXPECT_EQ(quoteArgument("a\\\"b"), "\"a\\\\\\\"b\"");
}

TEST(CliOutputTest, PathsFromUtf8) {
    const Result<std::filesystem::path> path = pathFromUtf8("D:\\Gr\xC3\xBC\xC3\x9F" "e\\\xE6\x97\xA5");
    ASSERT_TRUE(path.ok());
    EXPECT_EQ(path->native(), L"D:\\Gr\u00FC\u00DFe\\\u65E5");
    EXPECT_FALSE(pathFromUtf8("bad \xFF").ok());
}

TEST(CliOutputTest, TablesAlignColumns) {
    Table table({{"ID", true}, {"Name"}, {"Size", true}});
    table.addRow({"1", "a.jpg", "12 B"});
    table.addRow({"10", "long-name.png", "1.5 KiB"});
    // Widths count code points: "Grüße" is five columns wide, not seven bytes.
    table.addRow({"2", "Gr\xC3\xBC\xC3\x9F" "e", "3 B"});
    std::ostringstream out;
    table.print(out, "  ");
    const std::string expected = "  ID  Name" + std::string(14, ' ') + "Size\n" +
                                 "   1  a.jpg" + std::string(13, ' ') + "12 B\n" +
                                 "  10  long-name.png  1.5 KiB\n" +
                                 "   2  Gr\xC3\xBC\xC3\x9F" "e" + std::string(14, ' ') + "3 B\n";
    EXPECT_EQ(out.str(), expected);
}

TEST(CliOutputTest, JsonWriterWritesWhatAStrictReaderReads) {
    static constexpr char kText[] = "quote \" backslash \\ newline \n tab \t bell \x07 nul \0 end";
    report::JsonWriter writer;
    writer.beginObject();
    writer.field("text", std::string_view(kText, sizeof(kText) - 1));
    writer.field("unicode", "Gr\xC3\xBC\xC3\x9F" "e \xE6\x97\xA5 \xF0\x9F\x8E\x89");
    writer.field("invalid", "bad \xFF byte");
    writer.field("separators", "\xE2\x80\xA8\xE2\x80\xA9");
    writer.field("big", std::numeric_limits<std::uint64_t>::max());
    writer.field("negative", std::int64_t{-5});
    writer.field("yes", true);
    writer.field("literal", "a C string");
    writer.nullField("nothing");
    writer.field("unset", std::optional<std::uint32_t>{});
    writer.field("set", std::optional<std::uint32_t>{7});
    writer.key("emptyArray");
    writer.beginArray();
    writer.endArray();
    writer.key("emptyObject");
    writer.beginObject();
    writer.endObject();
    writer.key("nested");
    writer.beginArray();
    writer.beginObject();
    writer.field("a", 1);
    writer.endObject();
    writer.number(std::uint64_t{2});
    writer.string("three");
    writer.endArray();
    writer.endObject();
    const std::string text = writer.finish();

    std::string error;
    const std::optional<test::json::Value> value = test::json::parse(text, &error);
    ASSERT_TRUE(value.has_value()) << error << "\n" << text;
    const test::json::Value& root = *value;
    EXPECT_EQ(root["text"].string(), std::string(kText, sizeof(kText) - 1));
    EXPECT_EQ(root["unicode"].string(), "Gr\xC3\xBC\xC3\x9F" "e \xE6\x97\xA5 \xF0\x9F\x8E\x89");
    EXPECT_EQ(root["invalid"].string(), "bad " + std::string(kReplacement) + " byte");
    EXPECT_EQ(root["separators"].string(), "\xE2\x80\xA8\xE2\x80\xA9");
    EXPECT_EQ(root["big"].u64(), std::numeric_limits<std::uint64_t>::max());
    EXPECT_TRUE(root["yes"].boolean());
    EXPECT_EQ(root["literal"].string(), "a C string");
    EXPECT_TRUE(root["nothing"].isNull());
    EXPECT_TRUE(root["unset"].isNull());
    EXPECT_EQ(root["set"].u64(), 7u);
    EXPECT_EQ(root["emptyArray"].size(), 0u);
    EXPECT_EQ(root["emptyObject"].size(), 0u);
    EXPECT_EQ(root["nested"].size(), 3u);
    EXPECT_EQ(root["nested"][0]["a"].u64(), 1u);
    EXPECT_EQ(root["nested"][2].string(), "three");
    // Controls and the separators are escaped; the document is indented and ends with a newline.
    EXPECT_NE(text.find("\\u0007"), std::string::npos);
    EXPECT_NE(text.find("\\u0000"), std::string::npos);
    EXPECT_NE(text.find("\\u2028\\u2029"), std::string::npos);
    EXPECT_NE(text.find("\"negative\": -5"), std::string::npos);
    EXPECT_TRUE(text.starts_with("{\n  \"text\": \""));
    EXPECT_TRUE(text.ends_with("}\n"));
}

TEST(CliOutputTest, TheTestsJsonReaderIsStrict) {
    for (const std::string_view bad :
         {"", "{", "[1,2", "{\"a\":1,}", "[1,]", "{\"a\" 1}", "{\"a\":1}x", "\"\x01\"", "{\"a\":1,\"a\":2}",
          "\"\\ud800\"", "\"\\udc00x\"", "\"\\x\"", "01", "1.", "-", "1e", "tru", "\"\xFF\"", "\"open"}) {
        std::string error;
        EXPECT_FALSE(test::json::parse(bad, &error).has_value()) << bad;
        EXPECT_FALSE(error.empty()) << bad;
    }
    const std::optional<test::json::Value> good =
        test::json::parse(" {\"a\": [1, -2.5e3, true, null, \"\\u00fc\\ud83c\\udf89\"]} ");
    ASSERT_TRUE(good.has_value());
    EXPECT_EQ((*good)["a"][4].string(), "\xC3\xBC\xF0\x9F\x8E\x89");
}

TEST(CliInterruptTest, TheFirstRequestIsHandledAndStopsTheCommand) {
    Interrupt interrupt;
    int calls = 0;
    const Interrupt::Scope scope(&interrupt, [&] { ++calls; });
    EXPECT_FALSE(interrupt.requested());
    EXPECT_TRUE(interrupt.request());
    EXPECT_EQ(calls, 1);
    EXPECT_TRUE(interrupt.requested());
    // A second Ctrl+C is not handled: the console handler lets Windows end the process.
    EXPECT_FALSE(interrupt.request());
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(interrupt.requests(), 2u);
}

TEST(CliInterruptTest, ARequestBeforeTheCommandsActionIsNotLost) {
    Interrupt interrupt;
    EXPECT_TRUE(interrupt.request());
    int calls = 0;
    const Interrupt::Scope scope(&interrupt, [&] { ++calls; });
    EXPECT_EQ(calls, 1);
}

TEST(CliInterruptTest, ScopesNestAndRestoreTheActionBefore) {
    Interrupt interrupt;
    int outer = 0;
    int inner = 0;
    const Interrupt::Scope first(&interrupt, [&] { ++outer; });
    {
        const Interrupt::Scope second(&interrupt, [&] { ++inner; });
    }
    EXPECT_TRUE(interrupt.request());
    EXPECT_EQ(outer, 1);
    EXPECT_EQ(inner, 0);
}

TEST(CliInterruptTest, TheScopeWaitsForAnActionUnderWay) {
    Interrupt interrupt;
    std::atomic<bool> started{false};
    std::atomic<bool> finished{false};
    std::thread handler;
    {
        const Interrupt::Scope scope(&interrupt, [&] {
            started = true;
            std::this_thread::sleep_for(50ms);
            finished = true;
        });
        handler = std::thread([&] { (void)interrupt.request(); });
        while (!started) {
            std::this_thread::yield();
        }
    }
    // The action referred to the scope's objects: it was done before the scope ended.
    EXPECT_TRUE(finished);
    handler.join();
}

TEST(CliInterruptTest, WaitingForTheCommandToFinish) {
    Interrupt interrupt;
    EXPECT_FALSE(interrupt.waitFinished(0ms));
    std::thread command([&] { interrupt.finish(); });
    command.join();
    EXPECT_TRUE(interrupt.waitFinished(0ms));
}

TEST(CliInterruptTest, NoInterruptIsHarmless) {
    int calls = 0;
    {
        const Interrupt::Scope scope(nullptr, [&] { ++calls; });
    }
    EXPECT_EQ(calls, 0);
}

}  // namespace
}  // namespace recovery::cli
