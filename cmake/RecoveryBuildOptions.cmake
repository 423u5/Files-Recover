# Compiler configuration shared by every RecoveryEngine target.
#
# Engine targets link `recovery_build_options` PRIVATELY so that warning and
# analysis flags never leak into third-party code (GoogleTest).

if(RECOVERY_ENABLE_ASAN)
    # Applied globally: every object in the process must agree on ASan
    # instrumentation (MSVC enforces this through /failifmismatch).
    add_compile_options(/fsanitize=address /Zi)
    # /RTC run-time checks are incompatible with AddressSanitizer.
    foreach(flags_var CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS)
        string(REGEX REPLACE "/RTC[1csu]+" "" ${flags_var} "${${flags_var}}")
    endforeach()
endif()

add_library(recovery_build_options INTERFACE)

target_compile_options(recovery_build_options INTERFACE
    /W4
    /permissive-
    /Zc:__cplusplus
    /Zc:preprocessor
    /Zc:inline
    /utf-8
    /EHsc
    /sdl
    # Headers included with <...> are third-party or system headers.
    /external:anglebrackets
    /external:W0
    # Off-by-default warnings that catch real defects in binary parsing code.
    /w14242 # conversion, possible loss of data
    /w14254 # larger bit field assigned to smaller bit field
    /w14263 # member function does not override base virtual
    /w14265 # class has virtual functions but destructor is not virtual
    /w14287 # unsigned/negative constant mismatch
    /w14296 # expression is always true/false
    /w14311 # pointer truncation
    /w14545 /w14546 /w14547 /w14549 # suspicious comma / operator usage
    /w14555 # expression has no effect
    /w14640 # thread-unsafe static member initialization
    /w14826 # sign-extending conversion
    /w14905 /w14906 # string literal cast
    /w14928 # illegal copy-initialization
)

target_compile_definitions(recovery_build_options INTERFACE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00 # Windows 10/11 API surface
    WINVER=0x0A00
)

if(RECOVERY_WARNINGS_AS_ERRORS)
    target_compile_options(recovery_build_options INTERFACE /WX)
endif()

# Engine libraries and tools additionally get static analysis. Test code is
# excluded: GoogleTest's assertion macros trip analyzer rules such as C6326.
add_library(recovery_engine_options INTERFACE)
target_link_libraries(recovery_engine_options INTERFACE recovery_build_options)
if(RECOVERY_ENABLE_MSVC_ANALYZE)
    target_compile_options(recovery_engine_options INTERFACE /analyze /analyze:external-)
endif()
