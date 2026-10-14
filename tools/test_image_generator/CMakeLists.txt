# The test corpus as files (P20), for development and testing only. It is
# built with the tests: the scenarios use their volume and media builders
# (tests/support, library recovery_corpus).
add_executable(test_image_generator main.cpp fuzz_seeds.cpp fuzz_seeds.hpp)
target_link_libraries(test_image_generator PRIVATE recovery_corpus recovery::session recovery::validation
                                                   recovery_build_options)
