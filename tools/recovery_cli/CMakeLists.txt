# ---------------------------------------------------------------------------
# recovery_cli_lib: the recovery command-line interface (P18) as a library, so
# that tests run it in-process. It uses the engine's public headers only: the
# engine libraries export include/ alone, never src/. Its report and text
# formatting are the recovery_report library's (shared with the API, P19).
# ---------------------------------------------------------------------------
add_library(recovery_cli_lib STATIC
    cli/arguments.cpp
    cli/arguments.hpp
    cli/cli.cpp
    cli/cli.hpp
    cli/command_image.cpp
    cli/command_inspect.cpp
    cli/command_recover.cpp
    cli/command_report.cpp
    cli/command_scan.cpp
    cli/commands.hpp
    cli/context.cpp
    cli/context.hpp
    cli/format.cpp
    cli/format.hpp
    cli/interrupt.cpp
    cli/interrupt.hpp
    cli/progress.cpp
    cli/progress.hpp
    cli/sessions.cpp
    cli/sessions.hpp
    cli/sources.cpp
    cli/sources.hpp
)
target_include_directories(recovery_cli_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(recovery_cli_lib
    PUBLIC recovery::report recovery::imaging recovery::playability
    PRIVATE recovery_engine_options
)

# ---------------------------------------------------------------------------
# recovery (recovery_cli): the program around the library: wide arguments as
# UTF-8, the console's streams and Ctrl+C. windows/ is the only place of the
# CLI where <windows.h> may be included.
# ---------------------------------------------------------------------------
add_executable(recovery_cli
    console.hpp
    main.cpp
    windows/console.cpp
)
set_target_properties(recovery_cli PROPERTIES OUTPUT_NAME recovery)
target_link_libraries(recovery_cli PRIVATE recovery_cli_lib recovery_engine_options)
