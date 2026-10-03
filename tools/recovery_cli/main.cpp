// RecoveryEngine command-line interface.
//
// Skeleton only: the commands are reserved but not wired to the engine yet
// (the full CLI is phase P18).

#include "recovery/version.hpp"

#include <array>
#include <iostream>
#include <string_view>

namespace {

constexpr int kExitSuccess = 0;
constexpr int kExitUsage = 1;
constexpr int kExitNotImplemented = 3;

constexpr std::string_view kUsage = R"(Usage: recovery <command> [options]

Commands:
  inspect    Show source, partition and filesystem information   (not yet available)
  image      Create a read-only image of a source drive           (not yet available)
  scan       Scan a source for recoverable files                  (not yet available)
  recover    Write recovered files to a destination               (not yet available)
  report     Export a recovery session report                     (not yet available)
  help       Show this help
  version    Show the engine version

Options:
  -h, --help       Show this help
  --version        Show the engine version
)";

constexpr std::array<std::string_view, 5> kPlannedCommands = {"inspect", "image", "scan", "recover", "report"};

bool isPlannedCommand(std::string_view command) {
    for (const std::string_view planned : kPlannedCommands) {
        if (planned == command) {
            return true;
        }
    }
    return false;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << kUsage;
        return kExitUsage;
    }

    const std::string_view command(argv[1]);

    if (command == "--version" || command == "version") {
        std::cout << recovery::kEngineName << ' ' << recovery::kEngineVersion << '\n';
        return kExitSuccess;
    }
    if (command == "--help" || command == "-h" || command == "help") {
        std::cout << kUsage;
        return kExitSuccess;
    }
    if (isPlannedCommand(command)) {
        std::cerr << "recovery: command '" << command << "' is not implemented in this build\n";
        return kExitNotImplemented;
    }

    std::cerr << "recovery: unknown command '" << command << "'\n\n" << kUsage;
    return kExitUsage;
}
