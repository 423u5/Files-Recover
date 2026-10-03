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

include(cmake/RecoveryBuildOptions.cmake)

configure_file(
    ${PROJECT_SOURCE_DIR}/cmake/version.hpp.in
    ${PROJECT_BINARY_DIR}/generated/include/recovery/version.hpp
    @ONLY)

add_subdirectory(src)
add_subdirectory(tools)

if(RECOVERY_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
