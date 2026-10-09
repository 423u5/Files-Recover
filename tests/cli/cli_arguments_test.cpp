// The command line (P18): options and their values, and the usage errors a
// person can fix: unknown commands and options, missing and malformed
// values, options that do not go together. A usage error exits with 1 and
// does nothing: no session, no file.

#include "cli/arguments.hpp"
#include "cli_test_support.hpp"
#include "recovery/version.hpp"
#include "support/test_files.hpp"

#include <gtest/gtest.h>

#include <array>
#include <limits>
#include <sstream>

namespace recovery::cli {
namespace {

using test::Cli;
using test::CliResult;

constexpr std::array kOptions = {
    OptionSpec{"--source", "", "SOURCE", "A source"},
    OptionSpec{"--resume", "", "", "Go on"},
    OptionSpec{"--help", "-h", "", "Help"},
};

std::vector<std::string> args(std::initializer_list<std::string_view> list) {
    std::vector<std::string> arguments;
    for (const std::string_view argument : list) {
        arguments.emplace_back(argument);
    }
    return arguments;
}

std::string errorOf(const Result<ParsedOptions>& parsed) {
    return parsed.ok() ? std::string() : parsed.error().message;
}

TEST(CliArgumentsTest, ValuesFlagsAndShortOptions) {
    Result<ParsedOptions> parsed = parseOptions(args({"--source", "a.img", "--resume"}), kOptions);
    ASSERT_TRUE(parsed.ok()) << errorOf(parsed);
    EXPECT_EQ(parsed->value("--source"), std::optional<std::string_view>("a.img"));
    EXPECT_TRUE(parsed->has("--resume"));
    EXPECT_FALSE(parsed->has("--help"));
    EXPECT_FALSE(parsed->value("--help").has_value());

    parsed = parseOptions(args({"--source=b.img", "-h"}), kOptions);
    ASSERT_TRUE(parsed.ok()) << errorOf(parsed);
    EXPECT_EQ(parsed->value("--source"), std::optional<std::string_view>("b.img"));
    EXPECT_TRUE(parsed->has("--help"));

    // A value may start with one dash, or hold '=' and spaces.
    parsed = parseOptions(args({"--source", "-x"}), kOptions);
    ASSERT_TRUE(parsed.ok()) << errorOf(parsed);
    EXPECT_EQ(parsed->value("--source"), std::optional<std::string_view>("-x"));
    parsed = parseOptions(args({"--source=a=b c"}), kOptions);
    ASSERT_TRUE(parsed.ok()) << errorOf(parsed);
    EXPECT_EQ(parsed->value("--source"), std::optional<std::string_view>("a=b c"));
    parsed = parseOptions(args({}), kOptions);
    ASSERT_TRUE(parsed.ok());
    EXPECT_TRUE(parsed->given().empty());
}

TEST(CliArgumentsTest, WhatTheParserRefuses) {
    const auto refused = [](std::initializer_list<std::string_view> list, std::string_view says) {
        const Result<ParsedOptions> parsed = parseOptions(args(list), kOptions);
        ASSERT_FALSE(parsed.ok());
        EXPECT_EQ(parsed.error().code, ErrorCode::InvalidInput);
        EXPECT_NE(parsed.error().message.find(says), std::string::npos) << parsed.error().message;
    };
    refused({"--nope"}, "unknown option '--nope'");
    refused({"-x"}, "unknown option '-x'");
    refused({"-h=1"}, "unknown option");
    refused({"a.img"}, "unexpected argument 'a.img'");
    refused({"-"}, "unexpected argument");
    refused({"--"}, "unexpected argument");
    refused({"--resume", "--resume"}, "--resume is given twice");
    refused({"--source", "a", "--source=b"}, "--source is given twice");
    refused({"--source"}, "--source needs a value (SOURCE)");
    refused({"--source", "--resume"}, "needs a value (SOURCE), not '--resume'");
    refused({"--resume=yes"}, "--resume takes no value");
    // What the user typed is printable in the message.
    refused({"--n\x1B[2Jope"}, "--n\\x1B[2Jope");
}

TEST(CliArgumentsTest, Numbers) {
    EXPECT_EQ(parseUnsigned("--n", "0", 0, 10).value(), 0u);
    EXPECT_EQ(parseUnsigned("--n", "10", 0, 10).value(), 10u);
    EXPECT_EQ(parseUnsigned("--n", "18446744073709551615", 0, std::numeric_limits<std::uint64_t>::max()).value(),
              std::numeric_limits<std::uint64_t>::max());
    for (const std::string_view bad : {"11", "-1", "+1", " 1", "1 ", "1.0", "", "0x10", "1e3",
                                        "18446744073709551616", "99999999999999999999999"}) {
        const Result<std::uint64_t> value = parseUnsigned("--n", bad, 0, 10);
        ASSERT_FALSE(value.ok()) << bad;
        EXPECT_NE(value.error().message.find("--n must be a whole number from 0 to 10"), std::string::npos);
    }
}

TEST(CliArgumentsTest, Sizes) {
    constexpr std::uint64_t kMax = std::uint64_t{4} << 30;
    EXPECT_EQ(parseSize("--b", "4096", 1, kMax).value(), 4096u);
    EXPECT_EQ(parseSize("--b", "4096b", 1, kMax).value(), 4096u);
    for (const std::string_view k : {"4k", "4K", "4kb", "4KB", "4KiB", "4kib"}) {
        EXPECT_EQ(parseSize("--b", k, 1, kMax).value(), 4096u) << k;
    }
    EXPECT_EQ(parseSize("--b", "1M", 1, kMax).value(), 1u << 20);
    EXPECT_EQ(parseSize("--b", "64MiB", 1, kMax).value(), 64u << 20);
    EXPECT_EQ(parseSize("--b", "2G", 1, kMax).value(), std::uint64_t{2} << 30);
    for (const std::string_view bad : {"", "M", "1.5M", "4kk", "1T", "-1", "5G", "99999999999999999999G", "1k"}) {
        const Result<std::uint64_t> value = parseSize("--b", bad, 4096, kMax);
        ASSERT_FALSE(value.ok()) << bad;
        EXPECT_NE(value.error().message.find("--b must be a size from 4.0 KiB to 4.0 GiB"), std::string::npos)
            << value.error().message;
    }
}

TEST(CliArgumentsTest, Choices) {
    constexpr std::array<std::string_view, 3> kKinds = {"image", "audio", "video"};
    EXPECT_EQ(parseChoice("--kind", "audio", kKinds).value(), "audio");
    const Result<std::string> wrong = parseChoice("--kind", "Audio", kKinds);
    ASSERT_FALSE(wrong.ok());
    EXPECT_NE(wrong.error().message.find("--kind must be image, audio or video, not 'Audio'"), std::string::npos)
        << wrong.error().message;

    const Result<std::vector<std::string>> list = parseChoices("--kind", "video,image", kKinds);
    ASSERT_TRUE(list.ok());
    EXPECT_EQ(*list, (std::vector<std::string>{"video", "image"}));
    for (const std::string_view bad : {"", "image,", ",image", "image,,video", "image,image", "music"}) {
        EXPECT_FALSE(parseChoices("--kind", bad, kKinds).ok()) << bad;
    }
}

TEST(CliArgumentsTest, CandidateIds) {
    const Result<IdSelection> one = parseIds("--ids", "4");
    ASSERT_TRUE(one.ok());
    EXPECT_TRUE(one->contains(4));
    EXPECT_FALSE(one->contains(3));
    EXPECT_FALSE(one->contains(5));
    EXPECT_EQ(one->last(), 4u);

    const Result<IdSelection> many = parseIds("--ids", "12,1,4-9");
    ASSERT_TRUE(many.ok());
    for (const std::uint64_t id : {1, 4, 5, 6, 7, 8, 9, 12}) {
        EXPECT_TRUE(many->contains(id)) << id;
    }
    for (const std::uint64_t id : {0, 2, 3, 10, 11, 13}) {
        EXPECT_FALSE(many->contains(id)) << id;
    }
    EXPECT_EQ(many->last(), 12u);

    // Overlapping and adjacent ranges merge.
    const Result<IdSelection> merged = parseIds("--ids", "5-9,1-6,10");
    ASSERT_TRUE(merged.ok());
    ASSERT_EQ(merged->ranges().size(), 1u);
    EXPECT_EQ(merged->ranges().front(), (std::pair<std::uint64_t, std::uint64_t>{1, 10}));

    // A huge range is two numbers, not a list of every id.
    const Result<IdSelection> huge = parseIds("--ids", "1-18446744073709551615");
    ASSERT_TRUE(huge.ok());
    EXPECT_TRUE(huge->contains(std::numeric_limits<std::uint64_t>::max()));

    for (const std::string_view bad : {"", "0", "3-1", "1-", "-5", "a", "1,,2", "1-2-3", "1;2", " 1", "0-4"}) {
        const Result<IdSelection> ids = parseIds("--ids", bad);
        ASSERT_FALSE(ids.ok()) << bad;
        EXPECT_NE(ids.error().message.find("such as 4 or 1,4-9,12"), std::string::npos) << ids.error().message;
    }
    std::string tooMany = "1";
    for (int i = 0; i < 10'000; ++i) {
        tooMany += ",1";
    }
    const Result<IdSelection> limited = parseIds("--ids", tooMany);
    ASSERT_FALSE(limited.ok());
    EXPECT_NE(limited.error().message.find("more than 10000 items"), std::string::npos);
}

TEST(CliArgumentsTest, SessionIds) {
    EXPECT_EQ(parseSessionId("--session", "20261008-101530-3fa94c2e").value(), "20261008-101530-3fa94c2e");
    EXPECT_EQ(parseSessionId("--session", "my session").value(), "my session");
    for (const std::string_view bad : {"", ".", "..", "a/b", "a\\b", "..\\x", "c:x", "a*", "a?", "a|b", "a<b", "x.",
                                       "x ", "a\x01"}) {
        const Result<std::string> id = parseSessionId("--session", bad);
        ASSERT_FALSE(id.ok()) << bad;
        EXPECT_NE(id.error().message.find("takes a session id"), std::string::npos);
    }
    EXPECT_FALSE(parseSessionId("--session", std::string(256, 'a')).ok());
}

TEST(CliArgumentsTest, HelpLinesAreWrappedAt100Columns) {
    const std::array options = {
        OptionSpec{"--condition", "", "LIST",
                   "Only files in these conditions: complete, unverified, corrupted, partial, unrecoverable, "
                   "ambiguous, and a few more words to wrap twice over the line width of the help"},
        OptionSpec{"--short", "", "", "x"},
        // No space to break at: cut where the line ends, nothing dropped.
        OptionSpec{"--unbroken", "", "",
                   "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
                   "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"},
    };
    const std::string text = describeOptions(options);
    std::istringstream lines(text);
    std::string words;
    for (std::string line; std::getline(lines, line);) {
        EXPECT_LE(line.size(), 100u) << line;
        words += line;
    }
    // Nothing is lost: every word is there, in order.
    for (const std::string_view word : {"conditions:", "ambiguous,", "twice", "help", "abcdefghijklmnopqrstuvwxyz"}) {
        EXPECT_NE(words.find(word), std::string::npos) << word;
    }
    std::string unbroken;
    for (const char c : words) {
        if (c >= 'a' && c <= 'z') {
            unbroken += c;
        }
    }
    std::string alphabets;
    for (int i = 0; i < 4; ++i) {
        alphabets += "abcdefghijklmnopqrstuvwxyz";
    }
    EXPECT_NE(unbroken.find(alphabets), std::string::npos);
}

class CliCommandLineTest : public ::testing::Test {
protected:
    // Nothing a usage error does may create this folder.
    [[nodiscard]] bool nothingWasDone() const { return !std::filesystem::exists(dir.path() / "sessions"); }

    ::recovery::test::TempDir dir;
    Cli cli{dir.path() / "sessions"};
};

TEST_F(CliCommandLineTest, NoArgumentsShowsTheUsage) {
    const CliResult result = cli.run({});
    EXPECT_EQ(result.code, ExitCode::Usage) << result;
    EXPECT_TRUE(result.out.empty());
    EXPECT_NE(result.err.find("Usage: recovery <command> [options]"), std::string::npos);
}

TEST_F(CliCommandLineTest, HelpAndVersion) {
    for (const std::string_view help : {"--help", "-h", "help"}) {
        const CliResult result = cli.run({std::string(help)});
        EXPECT_EQ(result.code, ExitCode::Success) << result;
        for (const std::string_view command : {"inspect", "image", "scan", "recover", "report"}) {
            EXPECT_NE(result.out.find("  " + std::string(command) + " "), std::string::npos) << command;
        }
        EXPECT_NE(result.out.find("Exit codes: 0 done, 1 wrong command line, 2 failed, 3 stopped"),
                  std::string::npos);
    }
    for (const std::string_view command : {"inspect", "image", "scan", "recover", "report"}) {
        const CliResult help = cli.run({"help", std::string(command)});
        EXPECT_EQ(help.code, ExitCode::Success) << help;
        EXPECT_TRUE(help.out.starts_with("Usage: recovery " + std::string(command))) << help;
        EXPECT_NE(help.out.find("Options:"), std::string::npos);
        // The same help as --help.
        EXPECT_EQ(cli.run({std::string(command), "--help"}).out, help.out);
        EXPECT_EQ(cli.run({std::string(command), "-h", "--frobnicate"}).code, ExitCode::Usage);
    }
    const CliResult unknown = cli.run({"help", "nope"});
    EXPECT_EQ(unknown.code, ExitCode::Usage) << unknown;
    EXPECT_NE(unknown.err.find("no help for 'nope'"), std::string::npos);

    for (const std::string_view version : {"version", "--version"}) {
        const CliResult result = cli.run({std::string(version)});
        EXPECT_EQ(result.code, ExitCode::Success) << result;
        EXPECT_EQ(result.out, std::string(kEngineName) + " " + std::string(kEngineVersion) + "\n");
    }
    EXPECT_EQ(cli.run({"version", "extra"}).code, ExitCode::Usage);
    EXPECT_TRUE(nothingWasDone());
}

TEST_F(CliCommandLineTest, UnknownCommand) {
    const CliResult result = cli.run({"frobnicate", "--source", "x"});
    EXPECT_EQ(result.code, ExitCode::Usage) << result;
    EXPECT_NE(result.err.find("recovery: unknown command 'frobnicate'"), std::string::npos);
    EXPECT_NE(result.err.find("Usage: recovery <command>"), std::string::npos);
}

TEST_F(CliCommandLineTest, EveryCommandRefusesUnknownOptionsAndArguments) {
    for (const std::string_view command : {"inspect", "image", "scan", "recover", "report"}) {
        for (const std::vector<std::string>& line :
             {std::vector<std::string>{std::string(command), "--frobnicate"},
              std::vector<std::string>{std::string(command), "stray"}}) {
            const CliResult result = cli.run(line);
            EXPECT_EQ(result.code, ExitCode::Usage) << result;
            EXPECT_TRUE(result.err.starts_with("recovery " + std::string(command) + ": error: ")) << result;
            EXPECT_NE(result.err.find("Run 'recovery help " + std::string(command) + "' for its options."),
                      std::string::npos)
                << result;
            EXPECT_TRUE(result.out.empty());
        }
    }
    EXPECT_TRUE(nothingWasDone());
}

TEST_F(CliCommandLineTest, RequiredOptions) {
    const auto usage = [&](std::vector<std::string> line, std::string_view says) {
        const CliResult result = cli.run(std::move(line));
        EXPECT_EQ(result.code, ExitCode::Usage) << result;
        EXPECT_NE(result.err.find(says), std::string::npos) << result;
    };
    usage({"inspect"}, "--source is required");
    usage({"image", "--source", "a.img"}, "--source and --output are required");
    usage({"image", "--output", "b.img"}, "--source and --output are required");
    usage({"scan"}, "give --source for a new scan, or --session to resume one");
    usage({"scan", "--source", "a.img", "--session", "x"}, "give --source for a new scan, or --session");
    usage({"recover", "--session", "x"}, "--session and --output are required");
    usage({"recover", "--output", "out"}, "--session and --output are required");
    EXPECT_TRUE(nothingWasDone());
}

TEST_F(CliCommandLineTest, MalformedValues) {
    const auto usage = [&](std::vector<std::string> line, std::string_view says) {
        const CliResult result = cli.run(std::move(line));
        EXPECT_EQ(result.code, ExitCode::Usage) << result;
        EXPECT_NE(result.err.find(says), std::string::npos) << result;
    };
    usage({"scan", "--source", "a.img", "--mode", "fast"}, "--mode must be quick or deep, not 'fast'");
    usage({"scan", "--source", "a.img", "--workers", "0"}, "--workers must be a whole number from 1 to 64");
    usage({"scan", "--source", "a.img", "--workers", "65"}, "--workers must be a whole number from 1 to 64");
    usage({"scan", "--source", "a.img", "--workers", "two"}, "--workers must be a whole number");
    usage({"scan", "--source", "a.img", "--block-size", "3"}, "--block-size must be a size from 4.0 KiB");
    usage({"scan", "--source", "a.img", "--block-size", "128M"}, "--block-size must be a size");
    usage({"scan", "--source", "a.img", "--sector-size", "1000"}, "--sector-size must be a power of two");
    usage({"scan", "--source", "a.img", "--sector-size", "256"}, "--sector-size must be a whole number from 512");
    usage({"scan", "--source", "a.img", "--alignment", "0"}, "--alignment must be a whole number from 1");
    usage({"scan", "--source", "a.img", "--max-hits", "0"}, "--max-hits must be a whole number from 1");
    usage({"scan", "--source", "a.img", "--sector-retries", "17"}, "--sector-retries must be a whole number");
    usage({"scan", "--source", "a.img", "--mode", "quick", "--no-carving"}, "--no-carving is for deep scans");
    usage({"scan", "--source", "a.img", "--mode", "quick", "--alignment", "512"}, "--alignment is for deep scans");
    usage({"scan", "--session", "x", "--mode", "deep"}, "--mode cannot be used with --session");
    usage({"scan", "--session", "x", "--playability"}, "--playability cannot be used with --session");
    usage({"scan", "--session", "../x"}, "--session takes a session id");
    usage({"image", "--source", "a.img", "--output", "b.img", "--retries", "17"},
          "--retries must be a whole number from 0 to 16");
    usage({"image", "--source", "a.img", "--output", ""}, "--output needs a file name");
    usage({"recover", "--session", "..", "--output", "out"}, "--session takes a session id");
    usage({"recover", "--session", "x", "--output", "out", "--ids", "0"}, "--ids takes candidate ids");
    usage({"recover", "--session", "x", "--output", "out", "--kind", "music"},
          "--kind takes a comma-separated list of image, audio, video or other");
    usage({"recover", "--session", "x", "--output", "out", "--condition", "good"},
          "--condition takes a comma-separated list");
    usage({"recover", "--session", "x", "--output", ""}, "--output needs a folder");
    usage({"report", "--format", "xml"}, "--format must be text or json, not 'xml'");
    usage({"report", "--details"}, "--details needs --session");
    usage({"report", "--session", "x", "--format", "json", "--details"}, "--details is for text reports");
    usage({"inspect", "--source", "\\\\.\\C:"}, "is a device the engine does not read");
    usage({"inspect", "--source", "\\\\?\\Volume{01234567-89ab-cdef-0123-456789abcdef}"},
          "is a device the engine does not read");
    usage({"inspect", "--source", "\\\\.\\PhysicalDrive4096"}, "is not a physical disk: give \\\\.\\PhysicalDriveN, "
                                                                "N from 0 to 1023");
    usage({"inspect", "--source", "\\\\.\\physicaldrive01"}, "is not a physical disk");
    usage({"inspect", "--source", "\\\\.\\PhysicalDrive"}, "is not a physical disk");
    usage({"inspect", "--source", ""}, "--source needs a disk image file or a physical disk");
    EXPECT_TRUE(nothingWasDone());
}

}  // namespace
}  // namespace recovery::cli
