include(FetchContent)

# GoogleTest v1.15.2, pinned by commit for reproducible builds.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG b514bdc898e2951020cbdca1304b75f5950d1f59 # v1.15.2
    SYSTEM
)
FetchContent_MakeAvailable(googletest)
include(GoogleTest)

# ---------------------------------------------------------------------------
# Shared test doubles: simulated devices and fault-injecting sources.
# ---------------------------------------------------------------------------
add_library(recovery_test_support STATIC
    support/audio_builders.cpp
    support/audio_samples.cpp
    support/candidate_helpers.cpp
    support/carving_formats.cpp
    support/exfat_builder.cpp
    support/fake_device.cpp
    support/fat32_builder.cpp
    support/image_builders.cpp
    support/image_samples.cpp
    support/memory_source.cpp
    support/mp4_builders.cpp
    support/mp4_samples.cpp
    support/ntfs_builder.cpp
    support/partition_builder.cpp
    support/test_files.cpp
    support/virtual_source.cpp
)
target_include_directories(recovery_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(recovery_test_support
    PUBLIC recovery::partition recovery::filesystem recovery::candidates recovery::carving recovery::formats
           recovery::mp4 recovery::fragments GTest::gtest
    PRIVATE recovery_build_options
)

# ---------------------------------------------------------------------------
# Unit tests: no real devices; only in-memory sources and mocks.
# ---------------------------------------------------------------------------
add_executable(recovery_unit_tests
    unit/core/cancellation_test.cpp
    unit/core/checked_math_test.cpp
    unit/core/config_test.cpp
    unit/core/encoding_test.cpp
    unit/core/error_test.cpp
    unit/core/result_test.cpp
    unit/core/sha256_test.cpp
    unit/core/text_test.cpp
    unit/diagnostics/logger_test.cpp
    unit/imaging/image_metadata_test.cpp
    unit/partition/partition_source_test.cpp
    unit/partition/partition_table_test.cpp
    unit/recovery/candidate_content_test.cpp
    unit/recovery/candidate_reader_test.cpp
    unit/recovery/output_names_test.cpp
    unit/storage/bad_region_test.cpp
    unit/storage/destination_guard_test.cpp
    unit/storage/physical_disk_source_test.cpp
    unit/storage/storage_source_test.cpp
)
target_link_libraries(recovery_unit_tests PRIVATE
    recovery::imaging
    recovery_test_support
    GTest::gtest_main
    recovery_build_options
)
gtest_discover_tests(recovery_unit_tests
    PROPERTIES LABELS unit
    DISCOVERY_TIMEOUT 60
)

# ---------------------------------------------------------------------------
# Integration tests: real files in a temporary directory, through the Windows
# platform layer. Never opens a physical drive.
# ---------------------------------------------------------------------------
add_executable(recovery_integration_tests
    integration/imaging/image_writer_test.cpp
    integration/storage/destination_file_test.cpp
    integration/storage/disk_resolver_test.cpp
    integration/storage/disk_image_source_test.cpp
    integration/storage/source_contract_test.cpp
)
target_link_libraries(recovery_integration_tests PRIVATE
    recovery::imaging
    recovery_test_support
    GTest::gtest_main
    recovery_build_options
)
gtest_discover_tests(recovery_integration_tests
    PROPERTIES LABELS integration
    DISCOVERY_TIMEOUT 60
)

# ---------------------------------------------------------------------------
# Filesystem tests: generated in-memory FAT32, exFAT and NTFS volumes (normal,
# deleted, fragmented, Unicode, different geometries).
# ---------------------------------------------------------------------------
add_executable(recovery_filesystem_tests
    filesystem/exfat_boot_sector_test.cpp
    filesystem/exfat_filesystem_test.cpp
    filesystem/exfat_names_test.cpp
    filesystem/exfat_reference_image_test.cpp
    filesystem/fat32_boot_sector_test.cpp
    filesystem/fat32_filesystem_test.cpp
    filesystem/fat32_names_test.cpp
    filesystem/fat32_reference_image_test.cpp
    filesystem/ntfs_boot_sector_test.cpp
    filesystem/ntfs_filesystem_test.cpp
    filesystem/ntfs_record_test.cpp
    filesystem/ntfs_reference_image_test.cpp
)
target_link_libraries(recovery_filesystem_tests PRIVATE
    recovery_test_support
    GTest::gtest_main
    recovery_build_options
)
gtest_discover_tests(recovery_filesystem_tests
    PROPERTIES LABELS filesystem
    DISCOVERY_TIMEOUT 60
)

# ---------------------------------------------------------------------------
# Corruption tests: damaged metadata, loops, cross-links and fuzzing.
# ---------------------------------------------------------------------------
add_executable(recovery_corruption_tests
    corruption/exfat_corruption_test.cpp
    corruption/fat32_corruption_test.cpp
    corruption/filesystem_recovery_corruption_test.cpp
    corruption/ntfs_corruption_test.cpp
)
target_link_libraries(recovery_corruption_tests PRIVATE
    recovery_test_support
    GTest::gtest_main
    recovery_build_options
)
gtest_discover_tests(recovery_corruption_tests
    PROPERTIES LABELS corruption
    DISCOVERY_TIMEOUT 60
)

# ---------------------------------------------------------------------------
# Recovery tests: candidates from generated FAT32, exFAT and NTFS volumes,
# reconstructed data compared with the originals, and recovery output written
# to temporary directories. MP4 recovery (P12) from filesystem and carving
# evidence together. Fragment reconstruction (P13).
# ---------------------------------------------------------------------------
add_executable(recovery_recovery_tests
    recovery/filesystem_recovery_test.cpp
    recovery/fragment_recovery_test.cpp
    recovery/mp4_recovery_test.cpp
    recovery/recovery_reference_image_test.cpp
    recovery/recovery_writer_test.cpp
)
target_link_libraries(recovery_recovery_tests PRIVATE
    recovery_test_support
    GTest::gtest_main
    recovery_build_options
)
gtest_discover_tests(recovery_recovery_tests
    PROPERTIES LABELS recovery
    DISCOVERY_TIMEOUT 60
)

# ---------------------------------------------------------------------------
# Carving tests: the format-independent carving framework with test-only
# formats: signatures, registration, scanning, candidate generation, boundary
# checks, cancellation and large virtual images (streaming, bounded memory).
# heap_tracking.cpp replaces the global operator new of this executable only.
# ---------------------------------------------------------------------------
add_executable(recovery_carving_tests
    carving/carving_streaming_test.cpp
    carving/content_reader_test.cpp
    carving/file_carver_test.cpp
    carving/file_signature_test.cpp
    carving/format_registry_test.cpp
    carving/heap_tracking.cpp
    carving/heap_tracking.hpp
    carving/signature_scanner_test.cpp
)
target_link_libraries(recovery_carving_tests PRIVATE
    recovery_test_support
    GTest::gtest_main
    recovery_build_options
)
gtest_discover_tests(recovery_carving_tests
    PROPERTIES LABELS carving
    DISCOVERY_TIMEOUT 60
)

# ---------------------------------------------------------------------------
# Format tests: the image formats (P9) and the audio formats (P10) on files
# written by the test builders and by independent encoders: intact files,
# truncation at every position, damaged headers, segments, chunks, frames and
# boxes, false signatures, fragmentation, mutation fuzzing, and carving whole
# images with every format registered. The MP4 parser (P11) on builder files
# of every layout and on files from FFmpeg, GPAC and Media Foundation.
# ---------------------------------------------------------------------------
add_executable(recovery_formats_tests
    formats/aac_format_test.cpp
    formats/audio_carving_test.cpp
    formats/audio_reference_test.cpp
    formats/bmp_format_test.cpp
    formats/format_test_helpers.cpp
    formats/format_test_helpers.hpp
    formats/gif_format_test.cpp
    formats/image_carving_test.cpp
    formats/image_reference_test.cpp
    formats/jpeg_format_test.cpp
    formats/m4a_format_test.cpp
    formats/mp3_format_test.cpp
    formats/mp4_analysis_test.cpp
    formats/mp4_box_test.cpp
    formats/mp4_format_test.cpp
    formats/mp4_fragment_test.cpp
    formats/mp4_parser_test.cpp
    formats/mp4_reference_test.cpp
    formats/mp4_test_helpers.cpp
    formats/mp4_test_helpers.hpp
    formats/png_format_test.cpp
    formats/video_carving_test.cpp
    formats/wav_format_test.cpp
    formats/webp_format_test.cpp
)
target_link_libraries(recovery_formats_tests PRIVATE
    recovery_test_support
    GTest::gtest_main
    recovery_build_options
)
gtest_discover_tests(recovery_formats_tests
    PROPERTIES LABELS formats
    DISCOVERY_TIMEOUT 60
)

# ---------------------------------------------------------------------------
# Validation tests (P14): the validation levels, and the media decoders on
# builder files, files of independent encoders, vectors that break one rule
# each, damage the structure cannot see, truncation, limits and fuzzing.
# ---------------------------------------------------------------------------
add_executable(recovery_validation_tests
    formats/format_test_helpers.cpp
    formats/format_test_helpers.hpp
    validation/aac_media_test.cpp
    validation/aac_media_vectors.cpp
    validation/aac_media_vectors.hpp
    validation/audio_media_test.cpp
    validation/bmp_media_test.cpp
    validation/gif_media_test.cpp
    validation/jpeg_media_test.cpp
    validation/media_test_helpers.cpp
    validation/media_test_helpers.hpp
    validation/mp4_media_test.cpp
    validation/playability_test.cpp
    validation/png_media_test.cpp
    validation/png_media_vectors.cpp
    validation/png_media_vectors.hpp
    validation/validation_test.cpp
    validation/video_media_vectors.cpp
    validation/video_media_vectors.hpp
    validation/webp_media_test.cpp
    validation/webp_media_vectors.cpp
    validation/webp_media_vectors.hpp
)
target_link_libraries(recovery_validation_tests PRIVATE
    recovery::playability
    recovery::validation
    recovery_test_support
    GTest::gtest_main
    recovery_build_options
)
gtest_discover_tests(recovery_validation_tests
    PROPERTIES LABELS validation
    DISCOVERY_TIMEOUT 60
)

# ---------------------------------------------------------------------------
# Evaluation tests (P14): content identity, and candidate evaluation on
# generated FAT32 cards with carves, MP4 candidates and reconstructions.
# ---------------------------------------------------------------------------
add_executable(recovery_evaluation_tests
    evaluation/candidate_evaluation_test.cpp
    evaluation/content_identity_test.cpp
)
target_link_libraries(recovery_evaluation_tests PRIVATE
    recovery::evaluation
    recovery_test_support
    GTest::gtest_main
    recovery_build_options
)
gtest_discover_tests(recovery_evaluation_tests
    PROPERTIES LABELS evaluation
    DISCOVERY_TIMEOUT 60
)

# ---------------------------------------------------------------------------
# CLI smoke tests.
# ---------------------------------------------------------------------------
add_test(NAME cli.version COMMAND recovery_cli --version)
set_tests_properties(cli.version PROPERTIES
    PASS_REGULAR_EXPRESSION "RecoveryEngine ${PROJECT_VERSION}"
    LABELS cli)

add_test(NAME cli.help COMMAND recovery_cli --help)
set_tests_properties(cli.help PROPERTIES
    PASS_REGULAR_EXPRESSION "Usage: recovery"
    LABELS cli)

add_test(NAME cli.no_arguments_fails COMMAND recovery_cli)
set_tests_properties(cli.no_arguments_fails PROPERTIES WILL_FAIL TRUE LABELS cli)

add_test(NAME cli.unknown_command_fails COMMAND recovery_cli frobnicate)
set_tests_properties(cli.unknown_command_fails PROPERTIES WILL_FAIL TRUE LABELS cli)

add_test(NAME cli.unimplemented_command_fails COMMAND recovery_cli scan)
set_tests_properties(cli.unimplemented_command_fails PROPERTIES WILL_FAIL TRUE LABELS cli)
