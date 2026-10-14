cmake_minimum_required(VERSION 3.25)

project(RecoveryEngine
    VERSION 0.1.0
    DESCRIPTION "Read-only file recovery engine for removable storage"
    LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "RecoveryEngine targets Windows 11 only.")
endif()
if(NOT MSVC)
    message(FATAL_ERROR "RecoveryEngine must be built with MSVC (cl.exe).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(RECOVERY_BUILD_TESTS "Build the test suites" ON)
option(RECOVERY_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(RECOVERY_ENABLE_ASAN "Build everything with AddressSanitizer" OFF)
option(RECOVERY_ENABLE_MSVC_ANALYZE "Run the MSVC code analyzer (/analyze) on engine targets" OFF)
# P20 security review: Control Flow Guard, CET compatibility, DLLs from System32 only.
option(RECOVERY_HARDENING "Build with the exploit mitigations (off only to measure their cost)" ON)
# P20: the libFuzzer targets of fuzz/ (preset msvc-fuzz). The engine libraries
# are instrumented for coverage, which only the fuzzers' runtime provides, so
# such a build has no tests and no tools.
option(RECOVERY_BUILD_FUZZERS "Build the fuzz targets (fuzz/) instead of the tests and tools" OFF)

include(cmake/RecoveryBuildOptions.cmake)

configure_file(
    ${PROJECT_SOURCE_DIR}/cmake/version.hpp.in
    ${PROJECT_BINARY_DIR}/generated/include/recovery/version.hpp
    @ONLY)

add_subdirectory(src)

if(RECOVERY_BUILD_FUZZERS)
    enable_testing()
    add_subdirectory(fuzz)
else()
    add_subdirectory(tools)
    if(RECOVERY_BUILD_TESTS)
        enable_testing()
        add_subdirectory(tests)
    endif()
endif()
