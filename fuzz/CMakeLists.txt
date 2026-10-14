# Coverage-guided fuzz targets (P20), built with RECOVERY_BUILD_FUZZERS
# (preset msvc-fuzz): MSVC's libFuzzer (/fsanitize=fuzzer) with
# AddressSanitizer, the engine libraries instrumented for coverage
# (cmake/RecoveryBuildOptions.cmake). Each target takes its input as the
# untrusted data the engine reads from a disk. docs/testing/testing.md
# ("Fuzzing") says how to make seeds and run them.
add_library(recovery_fuzz_support STATIC
    fuzz_support.cpp
    fuzz_support.hpp
)
target_link_libraries(recovery_fuzz_support
    PUBLIC recovery::scan recovery::metadata recovery::session recovery::imaging
    PRIVATE recovery_build_options
)

set(RECOVERY_FUZZ_TARGETS
    partition
    filesystem
    formats
    media
    mp4
    metadata
    journal
    imgmeta
    carving
    scan
)

foreach(name IN LISTS RECOVERY_FUZZ_TARGETS)
    add_executable(fuzz_${name} fuzz_${name}.cpp)
    target_link_libraries(fuzz_${name} PRIVATE recovery_fuzz_support recovery_build_options)
    target_compile_options(fuzz_${name} PRIVATE /fsanitize=fuzzer)

    # A short run from nothing: the harness works and finds nothing at once.
    # A whole scan per input is slow: fewer runs.
    set(runs 2000)
    if(name STREQUAL "scan")
        set(runs 200)
    endif()
    add_test(NAME fuzz.${name}.smoke COMMAND fuzz_${name} -runs=${runs} -seed=1 -max_len=4096)
    set_tests_properties(fuzz.${name}.smoke PROPERTIES LABELS fuzz)
    # Every input that once failed a target (fuzz/regressions/<name>) runs again.
    set(regressions ${CMAKE_CURRENT_SOURCE_DIR}/regressions/${name})
    if(EXISTS ${regressions})
        add_test(NAME fuzz.${name}.regressions COMMAND fuzz_${name} -runs=0 ${regressions})
        set_tests_properties(fuzz.${name}.regressions PROPERTIES LABELS fuzz)
    endif()
endforeach()
