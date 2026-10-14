add_subdirectory(recovery_cli)

# Development tools built on the test support code (tests/support).
if(RECOVERY_BUILD_TESTS)
    add_subdirectory(test_image_generator)
    add_subdirectory(benchmark)
endif()
