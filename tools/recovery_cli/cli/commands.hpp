#pragma once

// The commands: what each is called, its help, its options and what runs it.

#include "cli/arguments.hpp"
#include "cli/cli.hpp"
#include "cli/context.hpp"

#include <span>
#include <string_view>

namespace recovery::cli {

struct CommandSpec {
    std::string_view name;
    // One line for the general help.
    std::string_view summary;
    // The command's usage lines and what it does.
    std::string_view usage;
    std::string_view description;
    std::span<const OptionSpec> options;
    ExitCode (*run)(const ParsedOptions& options, Context& context);
};

// Options every command takes.
inline constexpr OptionSpec kHelpOption{"--help", "-h", "", "Show this help"};
inline constexpr OptionSpec kQuietOption{"--quiet", "-q", "", "No progress and no notes on the error stream"};
inline constexpr OptionSpec kLogOption{"--log", "", "FILE", "Append the engine's log to FILE (not on the source)"};

[[nodiscard]] const CommandSpec& inspectCommand();
[[nodiscard]] const CommandSpec& imageCommand();
[[nodiscard]] const CommandSpec& scanCommand();
[[nodiscard]] const CommandSpec& recoverCommand();
[[nodiscard]] const CommandSpec& reportCommand();

// --sector-size: a power of two from 512 to 65536 (a usage error otherwise).
[[nodiscard]] Result<std::uint32_t> parseSectorSize(std::string_view option, std::string_view text);
// --workers: 1 to EngineConfig::kMaxWorkerThreads.
[[nodiscard]] Result<std::uint32_t> parseWorkers(std::string_view option, std::string_view text);
// A usage error found in an option's value: reported with a pointer to the
// command's help. Returns ExitCode::Usage.
[[nodiscard]] ExitCode usageError(Context& context, const Error& error);
// Opens --log when given, checked against `source` (none: nothing to protect).
// Reports a failure and returns false.
[[nodiscard]] bool openLogOption(const ParsedOptions& options, const storage::SourceInfo* source, Context& context);

}  // namespace recovery::cli
