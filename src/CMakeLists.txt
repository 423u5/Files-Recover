# ---------------------------------------------------------------------------
# recovery_core: error/result types, configuration, cancellation, logging,
# checksums and SHA-256.
# ---------------------------------------------------------------------------
add_library(recovery_core STATIC
    recovery/byte_order.cpp
    recovery/config.cpp
    recovery/crc32.cpp
    recovery/error.cpp
    recovery/result.cpp
    recovery/sha256.cpp
    recovery/text.cpp
    recovery/unicode.cpp
    diagnostics/logger.cpp
)
add_library(recovery::core ALIAS recovery_core)
target_include_directories(recovery_core PUBLIC
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_BINARY_DIR}/generated/include
)
target_link_libraries(recovery_core PRIVATE recovery_engine_options)

# ---------------------------------------------------------------------------
# recovery_storage: read-only storage sources. All Win32 code is confined to
# storage/windows/.
# ---------------------------------------------------------------------------
add_library(recovery_storage STATIC
    storage/bad_region.cpp
    storage/destination_guard.cpp
    storage/device_backed_source.cpp
    storage/disk_image_source.cpp
    storage/physical_disk_source.cpp
    storage/storage_source.cpp
    storage/windows/win32.hpp
    storage/windows/windows_destination_file.cpp
    storage/windows/windows_device_io.cpp
    storage/windows/windows_disk_resolver.cpp
)
add_library(recovery::storage ALIAS recovery_storage)
target_link_libraries(recovery_storage
    PUBLIC recovery::core
    PRIVATE recovery_engine_options
)

# ---------------------------------------------------------------------------
# recovery_imaging: forensic-style imaging of a source to a destination file.
# ---------------------------------------------------------------------------
add_library(recovery_imaging STATIC
    imaging/image_metadata.cpp
    imaging/image_writer.cpp
)
add_library(recovery::imaging ALIAS recovery_imaging)
target_link_libraries(recovery_imaging
    PUBLIC recovery::storage
    PRIVATE recovery_engine_options
)

# ---------------------------------------------------------------------------
# recovery_partition: MBR/GPT detection and bounded partition views.
# ---------------------------------------------------------------------------
add_library(recovery_partition STATIC
    partition/guid.cpp
    partition/partition_source.cpp
    partition/partition_table.cpp
)
add_library(recovery::partition ALIAS recovery_partition)
target_link_libraries(recovery_partition
    PUBLIC recovery::storage
    PRIVATE recovery_engine_options
)

# ---------------------------------------------------------------------------
# recovery_filesystem: filesystem-independent model and filesystem modules.
# ---------------------------------------------------------------------------
add_library(recovery_filesystem STATIC
    filesystem/filesystem.cpp
    filesystem/fat32/directory_parser.cpp
    filesystem/fat32/directory_parser.hpp
    filesystem/fat32/fat_table.cpp
    filesystem/fat32/fat_table.hpp
    filesystem/fat32/fat32_boot_sector.cpp
    filesystem/fat32/fat32_filesystem.cpp
    filesystem/fat32/fat32_names.cpp
    filesystem/exfat/allocation_bitmap.cpp
    filesystem/exfat/allocation_bitmap.hpp
    filesystem/exfat/directory_parser.cpp
    filesystem/exfat/directory_parser.hpp
    filesystem/exfat/exfat_boot_sector.cpp
    filesystem/exfat/exfat_filesystem.cpp
    filesystem/exfat/exfat_names.cpp
    filesystem/exfat/fat_table.cpp
    filesystem/exfat/fat_table.hpp
    filesystem/exfat/paged_region.cpp
    filesystem/exfat/paged_region.hpp
    filesystem/ntfs/cluster_bitmap.cpp
    filesystem/ntfs/cluster_bitmap.hpp
    filesystem/ntfs/ntfs_boot_sector.cpp
    filesystem/ntfs/ntfs_filesystem.cpp
    filesystem/ntfs/ntfs_record.cpp
)
add_library(recovery::filesystem ALIAS recovery_filesystem)
target_link_libraries(recovery_filesystem
    PUBLIC recovery::storage
    PRIVATE recovery_engine_options
)

# ---------------------------------------------------------------------------
# recovery_candidates: recovery candidates, filesystem-based recovery (FAT32,
# exFAT, NTFS), reconstruction of candidate data, and recovery output.
# ---------------------------------------------------------------------------
add_library(recovery_candidates STATIC
    recovery/candidate_reader.cpp
    recovery/filesystem_recovery.cpp
    recovery/ntfs_recovery.cpp
    recovery/output_names.cpp
    recovery/recovery_candidate.cpp
    recovery/recovery_writer.cpp
)
add_library(recovery::candidates ALIAS recovery_candidates)
target_link_libraries(recovery_candidates
    PUBLIC recovery::filesystem
    PRIVATE recovery::partition recovery_engine_options
)

# ---------------------------------------------------------------------------
# recovery_carving: the format-independent carving framework: signatures,
# signature scanning, file carving, format validators and the format registry.
# Formats plug in through carving/file_format.hpp (see recovery_formats).
# ---------------------------------------------------------------------------
add_library(recovery_carving STATIC
    carving/content_reader.cpp
    carving/file_candidate.cpp
    carving/file_carver.cpp
    carving/file_format.cpp
    carving/file_signature.cpp
    carving/format_registry.cpp
    carving/format_validator.cpp
    carving/signature_scanner.cpp
    carving/source_reads.cpp
    carving/source_reads.hpp
)
add_library(recovery::carving ALIAS recovery_carving)
target_link_libraries(recovery_carving
    PUBLIC recovery::storage
    PRIVATE recovery_engine_options
)

# ---------------------------------------------------------------------------
# recovery_formats: carvable file formats that plug into recovery_carving.
# Images (P9): JPEG, PNG, WEBP, GIF and BMP. Audio (P10): MP3, WAV, M4A and
# raw AAC (ADTS). The ISO base media file format (MP4) parser (P11). Video
# (P12): MP4, and the MP4 analysis that MP4 recovery builds on.
# ---------------------------------------------------------------------------
add_library(recovery_formats STATIC
    formats/aac_format.cpp
    formats/audio_formats.cpp
    formats/audio_tags.cpp
    formats/audio_tags.hpp
    formats/bmp_format.cpp
    formats/crc16.cpp
    formats/crc16.hpp
    formats/gif_format.cpp
    formats/image_formats.cpp
    formats/iso_walk.cpp
    formats/iso_walk.hpp
    formats/jpeg_format.cpp
    formats/m4a_format.cpp
    formats/mp3_format.cpp
    formats/mp4_analysis.cpp
    formats/mp4_box.cpp
    formats/mp4_format.cpp
    formats/mp4_parser.cpp
    formats/png_format.cpp
    formats/structure_walk.cpp
    formats/structure_walk.hpp
    formats/video_formats.cpp
    formats/wav_format.cpp
    formats/webp_format.cpp
)
add_library(recovery::formats ALIAS recovery_formats)
target_link_libraries(recovery_formats
    PUBLIC recovery::carving
    PRIVATE recovery_engine_options
)

# ---------------------------------------------------------------------------
# recovery_mp4: MP4 recovery (P12) from filesystem candidates and carving
# together (FILESYSTEM, CARVING and HYBRID candidates), and the content
# reader over a candidate's regions it validates filesystem candidates with.
# ---------------------------------------------------------------------------
add_library(recovery_mp4 STATIC
    recovery/candidate_content.cpp
    recovery/mp4_recovery.cpp
)
add_library(recovery::mp4 ALIAS recovery_mp4)
target_link_libraries(recovery_mp4
    PUBLIC recovery::candidates recovery::formats
    PRIVATE recovery_engine_options
)

# ---------------------------------------------------------------------------
# recovery_fragments: fragment reconstruction (P13): files whose layout no
# metadata records (deleted FAT32 and exFAT files, carves that break),
# reconstructed from layout hypotheses that each format validates, ranked by
# the evidence of the allocation and of other files, with MP4 placed by its
# sample tables.
# ---------------------------------------------------------------------------
add_library(recovery_fragments STATIC
    recovery/fragment_evidence.cpp
    recovery/fragment_evidence.hpp
    recovery/fragment_mp4.cpp
    recovery/fragment_recovery.cpp
    recovery/fragment_search.cpp
    recovery/fragment_search.hpp
)
add_library(recovery::fragments ALIAS recovery_fragments)
target_link_libraries(recovery_fragments
    PUBLIC recovery::mp4
    PRIVATE recovery_engine_options
)

# ---------------------------------------------------------------------------
# recovery_validation: validation levels (P14): structural (the formats'
# validators), media (the engine's own deterministic decoders: Huffman,
# deflate, LZW, RLE, VP8L, and the headers of lossy codecs) and playability
# (through a platform checker, recovery_playability on Windows).
# ---------------------------------------------------------------------------
add_library(recovery_validation STATIC
    validation/aac_media.cpp
    validation/aac_syntax.cpp
    validation/aac_syntax.hpp
    validation/audio_media.cpp
    validation/avc_syntax.cpp
    validation/avc_syntax.hpp
    validation/bit_reader.hpp
    validation/bmp_media.cpp
    validation/content_bytes.cpp
    validation/content_bytes.hpp
    validation/gif_media.cpp
    validation/hevc_syntax.cpp
    validation/hevc_syntax.hpp
    validation/inflate.cpp
    validation/inflate.hpp
    validation/jpeg_media.cpp
    validation/media_decoders.hpp
    validation/media_registry.cpp
    validation/mp4_media.cpp
    validation/nal_units.cpp
    validation/nal_units.hpp
    validation/png_media.cpp
    validation/prefix_code.cpp
    validation/prefix_code.hpp
    validation/validation.cpp
    validation/vp8_header.cpp
    validation/vp8_header.hpp
    validation/vp8l_decoder.cpp
    validation/vp8l_decoder.hpp
    validation/webp_media.cpp
)
add_library(recovery::validation ALIAS recovery_validation)
target_link_libraries(recovery_validation
    PUBLIC recovery::formats
    PRIVATE recovery_engine_options
)

# ---------------------------------------------------------------------------
# recovery_playability: the playability level on Windows (P14): the Windows
# Imaging Component and Media Foundation decode whole files. Like
# storage/windows/, validation/windows/ is a place where <windows.h> may be
# included; nothing else in validation/ includes it.
# ---------------------------------------------------------------------------
add_library(recovery_playability STATIC
    validation/windows/windows_playability.cpp
)
add_library(recovery::playability ALIAS recovery_playability)
target_link_libraries(recovery_playability
    PUBLIC recovery::validation
    PRIVATE recovery_engine_options mfplat mfreadwrite mfuuid ole32 windowscodecs
)

# ---------------------------------------------------------------------------
# recovery_evaluation: candidate evaluation (P14): the candidates of every
# stage merged into one per file, validated at every level, identified by
# SHA-256, with duplicates and explainable evidence.
# ---------------------------------------------------------------------------
add_library(recovery_evaluation STATIC
    evaluation/candidate_evaluation.cpp
    evaluation/content_identity.cpp
    evaluation/evaluated_candidate.cpp
)
add_library(recovery::evaluation ALIAS recovery_evaluation)
target_link_libraries(recovery_evaluation
    PUBLIC recovery::validation recovery::mp4 recovery::fragments
    PRIVATE recovery_engine_options
)
