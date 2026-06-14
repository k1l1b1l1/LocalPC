cmake_minimum_required(VERSION 3.16)
project(ego-offline VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Dependencies ──────────────────────────────────────────────────────────────

# nlohmann/json — prefer installed package, fall back to FetchContent
find_package(nlohmann_json 3.9 QUIET)
if (NOT nlohmann_json_FOUND)
    if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/include/nlohmann/json.hpp")
        add_library(nlohmann_json_vendor INTERFACE)
        target_include_directories(nlohmann_json_vendor INTERFACE
            "${CMAKE_CURRENT_SOURCE_DIR}/include")
        add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json_vendor)
    else()
        include(FetchContent)
        FetchContent_Declare(
            nlohmann_json
            URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
            URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
        )
        FetchContent_MakeAvailable(nlohmann_json)
    endif()
endif()

# ── Include paths ─────────────────────────────────────────────────────────────
include_directories(include)

# ── Shared headers-only library (protocol + checksum + types + config) ────────
add_library(ego_offline_proto INTERFACE)
target_include_directories(ego_offline_proto INTERFACE include)
target_compile_features(ego_offline_proto INTERFACE cxx_std_17)

# ── config ────────────────────────────────────────────────────────────────────
add_library(ego_offline_config STATIC
    src/config.cpp
)
target_include_directories(ego_offline_config PUBLIC include)
target_link_libraries(ego_offline_config PUBLIC ego_offline_proto)

# ── load ──────────────────────────────────────────────────────────────────────
add_library(ego_offline_load STATIC
    src/load/ego_log_reader.cpp
    src/load/ego_nav_sidecar_reader.cpp
    src/load/source_log_reader.cpp
    src/load/metadata_loader.cpp
)
target_include_directories(ego_offline_load PUBLIC include)
target_link_libraries(ego_offline_load
    PUBLIC  ego_offline_proto
    PRIVATE nlohmann_json::nlohmann_json
)

# ── parse ─────────────────────────────────────────────────────────────────────
add_library(ego_offline_parse STATIC
    src/parse/binary_parser.cpp
    src/parse/packet_demuxer.cpp
    src/parse/contract_demuxer.cpp
    src/parse/source_demuxer.cpp
)
target_include_directories(ego_offline_parse PUBLIC include)
target_link_libraries(ego_offline_parse PUBLIC ego_offline_proto ego_offline_load)

# ── validate ──────────────────────────────────────────────────────────────────
add_library(ego_offline_validate STATIC
    src/validate/log_validator.cpp
)
target_include_directories(ego_offline_validate PUBLIC include)
target_link_libraries(ego_offline_validate
    PUBLIC  ego_offline_parse ego_offline_config
)

# ── sync ──────────────────────────────────────────────────────────────────────
add_library(ego_offline_sync STATIC
    src/sync/time_sync.cpp
)
target_include_directories(ego_offline_sync PUBLIC include)
target_link_libraries(ego_offline_sync
    PUBLIC ego_offline_proto ego_offline_config
)

# ── align ─────────────────────────────────────────────────────────────────────
add_library(ego_offline_align STATIC
    src/align/time_aligner.cpp
)
target_include_directories(ego_offline_align PUBLIC include)
target_link_libraries(ego_offline_align PUBLIC ego_offline_proto ego_offline_config)

# ── scene ─────────────────────────────────────────────────────────────────────
add_library(ego_offline_scene STATIC
    src/scene/scene_geometry.cpp
)
target_include_directories(ego_offline_scene PUBLIC include)
target_link_libraries(ego_offline_scene PUBLIC ego_offline_align)

# ── mdf4 ──────────────────────────────────────────────────────────────────────
# ADR-001: native MDF4 v4.1 writer (no external library dependency)
add_library(ego_offline_mdf4 STATIC
    src/mdf4/mdf4_writer.cpp
    src/mdf4/mdf4_channel_builder.cpp
    src/mdf4/mdf4_lib_adapter.cpp
)
target_include_directories(ego_offline_mdf4 PUBLIC include)
target_link_libraries(ego_offline_mdf4 PUBLIC ego_offline_scene ego_offline_load)

# ── finalize ──────────────────────────────────────────────────────────────────
add_library(ego_offline_finalize STATIC
    src/finalize/scene_builder.cpp
)
target_include_directories(ego_offline_finalize PUBLIC include)
target_link_libraries(ego_offline_finalize PUBLIC ego_offline_scene ego_offline_parse)

# ── reports ───────────────────────────────────────────────────────────────────
add_library(ego_offline_reports STATIC
    src/reports/report_builder.cpp
)
target_include_directories(ego_offline_reports PUBLIC include)
target_link_libraries(ego_offline_reports
    PUBLIC ego_offline_validate ego_offline_sync ego_offline_finalize
)

# ── upload (S3) ─────────────────────────────────────────────────────────────
add_library(ego_offline_upload STATIC
    src/upload/s3_client.cpp
    src/upload/s3_uploader.cpp
)
target_include_directories(ego_offline_upload PUBLIC include)
target_link_libraries(ego_offline_upload PUBLIC ego_offline_config)
if (WIN32)
    target_link_libraries(ego_offline_upload PRIVATE bcrypt)
endif()

# ── pipeline orchestrator ────────────────────────────────────────────────────
add_library(ego_offline_pipeline STATIC
    src/pipeline/pipeline.cpp
)
target_include_directories(ego_offline_pipeline PUBLIC include)
target_link_libraries(ego_offline_pipeline
    PUBLIC
        ego_offline_config
        ego_offline_load
        ego_offline_parse
        ego_offline_validate
        ego_offline_sync
        ego_offline_align
        ego_offline_scene
        ego_offline_mdf4
        ego_offline_finalize
        ego_offline_reports
        ego_offline_upload
)

# ── Main executable ───────────────────────────────────────────────────────────
add_executable(ego-offline src/main.cpp)
target_link_libraries(ego-offline PRIVATE ego_offline_pipeline)

# ── Tests ─────────────────────────────────────────────────────────────────────
enable_testing()

add_executable(test_binary_parser tests/test_binary_parser.cpp)
target_include_directories(test_binary_parser PRIVATE include)
target_link_libraries(test_binary_parser PRIVATE ego_offline_parse)
add_test(NAME binary_parser COMMAND test_binary_parser)

add_executable(test_valid_ranges tests/test_valid_ranges.cpp)
target_include_directories(test_valid_ranges PRIVATE include)
target_link_libraries(test_valid_ranges PRIVATE ego_offline_validate)
add_test(NAME valid_ranges COMMAND test_valid_ranges)

add_executable(test_sync tests/test_sync.cpp)
target_include_directories(test_sync PRIVATE include)
target_link_libraries(test_sync PRIVATE ego_offline_sync)
add_test(NAME sync COMMAND test_sync)

add_executable(test_segment_classifier tests/test_segment_classifier.cpp)
target_include_directories(test_segment_classifier PRIVATE include)
target_link_libraries(test_segment_classifier PRIVATE ego_offline_finalize)
add_test(NAME segment_classifier COMMAND test_segment_classifier)

add_executable(test_config_yaml tests/test_config_yaml.cpp)
target_include_directories(test_config_yaml PRIVATE include)
target_link_libraries(test_config_yaml PRIVATE ego_offline_config)
add_test(NAME config_yaml COMMAND test_config_yaml WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(test_ego_nav_sidecar_reader tests/test_ego_nav_sidecar_reader.cpp)
target_include_directories(test_ego_nav_sidecar_reader PRIVATE include)
target_link_libraries(test_ego_nav_sidecar_reader PRIVATE ego_offline_load)
add_test(NAME ego_nav_sidecar_reader COMMAND test_ego_nav_sidecar_reader WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(test_mdf4_writer tests/test_mdf4_writer.cpp)
target_include_directories(test_mdf4_writer PRIVATE include)
target_link_libraries(test_mdf4_writer PRIVATE ego_offline_mdf4)
add_test(NAME mdf4_writer COMMAND test_mdf4_writer WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(test_e2e_runtime_session tests/test_e2e_runtime_session.cpp)
target_include_directories(test_e2e_runtime_session PRIVATE include)
target_link_libraries(test_e2e_runtime_session PRIVATE ego_offline_pipeline)
add_test(NAME e2e_runtime_session COMMAND test_e2e_runtime_session WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# ── Compiler warnings ─────────────────────────────────────────────────────────
if (MSVC)
    add_compile_options(/W3 /WX-)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ── Install ───────────────────────────────────────────────────────────────────
install(TARGETS ego-offline DESTINATION bin)
install(DIRECTORY include/ego_offline DESTINATION include)
