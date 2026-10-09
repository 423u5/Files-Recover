#pragma once

// The program's platform glue (Windows): the console's streams, Ctrl+C, the
// command line as UTF-8, and where sessions are kept by default. Only
// windows/console.cpp includes <windows.h>.

#include "cli/interrupt.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace recovery::cli::console {

// For its lifetime:
//  * the standard output and error streams write UTF-8 text to a console as
//    the characters it holds (WriteConsoleW), whatever the console's code
//    page; streams redirected to a file or a pipe get the UTF-8 bytes;
//  * Ctrl+C and Ctrl+Break go to `interrupt`: the first stops the command
//    cleanly, a second one ends the process at once (the default handler);
//    closing the console window stops the command and gives it a few seconds
//    to record that before Windows ends the process.
// `interrupt` must outlive every console event: give it static storage.
class ConsoleSession {
public:
    explicit ConsoleSession(Interrupt& interrupt);
    ~ConsoleSession();
    ConsoleSession(const ConsoleSession&) = delete;
    ConsoleSession& operator=(const ConsoleSession&) = delete;
    ConsoleSession(ConsoleSession&&) = delete;
    ConsoleSession& operator=(ConsoleSession&&) = delete;

    // The error stream is a console (progress is redrawn in place there).
    [[nodiscard]] bool errorIsConsole() const noexcept;
    // The width of the console's window in characters (0: not a console).
    [[nodiscard]] std::size_t consoleWidth() const noexcept;

    struct State;

private:
    std::unique_ptr<State> state_;
};

// The arguments after the program's name, as UTF-8 (unpaired surrogates,
// which UTF-8 cannot hold, become U+FFFD).
[[nodiscard]] std::vector<std::string> utf8Arguments(int argc, wchar_t** argv);

// %LOCALAPPDATA%\RecoveryEngine\Sessions, when %LOCALAPPDATA% is set.
[[nodiscard]] std::optional<std::filesystem::path> defaultSessionsRoot();

}  // namespace recovery::cli::console
