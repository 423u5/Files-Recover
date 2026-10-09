#include "cli/cli.hpp"

#include "cli/commands.hpp"
#include "cli/format.hpp"
#include "recovery/config.hpp"
#include "recovery/version.hpp"

#include <array>
#include <bit>
#include <functional>
#include <ostream>

namespace recovery::cli {

namespace {

std::array<std::reference_wrapper<const CommandSpec>, 5> allCommands() {
    return {std::cref(inspectCommand()), std::cref(imageCommand()), std::cref(scanCommand()),
            std::cref(recoverCommand()), std::cref(reportCommand())};
}

const CommandSpec* findCommand(std::string_view name) {
    for (const CommandSpec& command : allCommands()) {
        if (command.name == name) {
            return &command;
        }
    }
    return nullptr;
}

std::string commandHelp(const CommandSpec& command) {
    std::string text = "Usage: ";
    text += command.usage;
    text += "\n\n";
    text += command.description;
    text += "\n\nOptions:\n";
    text += describeOptions(command.options);
    return text;
}

}  // namespace

std::string usageText() {
    std::string text = "Usage: recovery <command> [options]\n\nCommands:\n";
    for (const CommandSpec& command : allCommands()) {
        text += "  ";
        text += command.name;
        text.append(10 - command.name.size(), ' ');
        text += command.summary;
        text += '\n';
    }
    text +=
        "  help      Show the help of a command (recovery help scan)\n"
        "  version   Show the engine's version\n"
        "\n"
        "A SOURCE is a disk image file or a physical disk (\\\\.\\PhysicalDriveN; reading a disk needs\n"
        "administrator rights). The source is only ever read. Sessions are kept in\n"
        "%LOCALAPPDATA%\\RecoveryEngine\\Sessions unless --sessions-dir names another folder.\n"
        "\n"
        "Exit codes: 0 done, 1 wrong command line, 2 failed, 3 stopped (Ctrl+C), 4 done but not\n"
        "everything could be read or written.\n"
        "\n"
        "Example:\n"
        "  recovery inspect --source card.img\n"
        "  recovery scan --source card.img --mode deep\n"
        "  recovery recover --session <id> --output D:\\Recovered\n"
        "  recovery report --session <id>\n";
    return text;
}

Result<std::uint32_t> parseSectorSize(std::string_view option, std::string_view text) {
    Result<std::uint64_t> value = parseUnsigned(option, text, storage::kMinSectorSize, storage::kMaxSectorSize);
    if (!value.ok()) {
        return value.error();
    }
    if (!std::has_single_bit(*value)) {
        return makeError(ErrorCode::InvalidInput,
                         std::string(option) + " must be a power of two (512, 1024, 2048, 4096, ...), not " +
                             std::string(text));
    }
    return static_cast<std::uint32_t>(*value);
}

Result<std::uint32_t> parseWorkers(std::string_view option, std::string_view text) {
    Result<std::uint64_t> value = parseUnsigned(option, text, 1, EngineConfig::kMaxWorkerThreads);
    if (!value.ok()) {
        return value.error();
    }
    return static_cast<std::uint32_t>(*value);
}

ExitCode usageError(Context& context, const Error& error) {
    context.error(error.message);
    context.err() << "Run 'recovery help " << context.command() << "' for its options.\n";
    return ExitCode::Usage;
}

bool openLogOption(const ParsedOptions& options, const storage::SourceInfo* source, Context& context) {
    const std::optional<std::string_view> path = options.value(kLogOption.name);
    if (!path.has_value()) {
        return true;
    }
    Result<std::filesystem::path> file = pathFromUtf8(*path);
    if (!file.ok()) {
        (void)context.fail("cannot use the log file", file.error());
        return false;
    }
    if (Status opened = context.openLog(*file, source); !opened.ok()) {
        (void)context.fail("cannot open the log file '" + displayPath(*file) + "'", opened.error());
        return false;
    }
    return true;
}

ExitCode run(std::span<const std::string> arguments, Environment& environment) {
    if (environment.out == nullptr || environment.err == nullptr) {
        return ExitCode::Error;
    }
    std::ostream& out = *environment.out;
    std::ostream& err = *environment.err;
    if (arguments.empty()) {
        err << usageText();
        return ExitCode::Usage;
    }
    const std::string& first = arguments.front();
    if (first == "--version" || first == "version") {
        if (arguments.size() > 1) {
            err << "recovery: '" << first << "' takes no arguments\n";
            return ExitCode::Usage;
        }
        out << kEngineName << ' ' << kEngineVersion << '\n';
        return ExitCode::Success;
    }
    if (first == "--help" || first == "-h" || first == "help") {
        if (arguments.size() == 1) {
            out << usageText();
            return ExitCode::Success;
        }
        const CommandSpec* command = arguments.size() == 2 ? findCommand(arguments[1]) : nullptr;
        if (command == nullptr) {
            err << "recovery: no help for '" << printable(arguments[1]) << "'\n\n" << usageText();
            return ExitCode::Usage;
        }
        out << commandHelp(*command);
        return ExitCode::Success;
    }
    const CommandSpec* command = findCommand(first);
    if (command == nullptr) {
        err << "recovery: unknown command '" << printable(first) << "'\n\n" << usageText();
        return ExitCode::Usage;
    }

    Context context(environment, std::string(command->name));
    Result<ParsedOptions> parsed = parseOptions(arguments.subspan(1), command->options);
    if (!parsed.ok()) {
        return usageError(context, parsed.error());
    }
    if (parsed->has(kHelpOption.name)) {
        out << commandHelp(*command);
        return ExitCode::Success;
    }
    context.setQuiet(parsed->has(kQuietOption.name));
    const ExitCode code = command->run(*parsed, context);
    context.progress().clear();
    out.flush();
    err.flush();
    return code;
}

}  // namespace recovery::cli
