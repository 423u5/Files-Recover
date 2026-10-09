// RecoveryEngine command-line interface (P18). The commands are in cli/
// (the recovery_cli_lib library); this is the program around them: the
// command line as UTF-8, the console, Ctrl+C.

#include "cli/cli.hpp"
#include "console.hpp"

#include <iostream>
#include <string>
#include <vector>

int wmain(int argc, wchar_t* argv[]) {
    using namespace recovery::cli;
    // Static: a console handler may still run while the program ends.
    static Interrupt interrupt;
    const console::ConsoleSession console(interrupt);

    Environment environment;
    environment.out = &std::cout;
    environment.err = &std::cerr;
    environment.progress = console.errorIsConsole() ? ProgressStyle::Console : ProgressStyle::Lines;
    environment.consoleWidth = console.consoleWidth();
    environment.interrupt = &interrupt;
    environment.defaultSessionsRoot = console::defaultSessionsRoot();

    const std::vector<std::string> arguments = console::utf8Arguments(argc, argv);
    const ExitCode code = run(arguments, environment);
    interrupt.finish();
    return static_cast<int>(code);
}
