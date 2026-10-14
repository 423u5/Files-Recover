# The engine's performance on this computer (P20), for development: built
# with the tests, as its inputs come from their builders (tests/support).
add_executable(recovery_benchmark main.cpp)
target_link_libraries(recovery_benchmark PRIVATE
    recovery_test_support
    recovery::scan
    recovery::imaging
    recovery::validation
    recovery_build_options
)
