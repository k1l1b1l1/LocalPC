cmake_minimum_required(VERSION 3.16)
project(ego_runtime VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(EGO_V1_DIR "${CMAKE_SOURCE_DIR}/../common/ego_v1" CACHE PATH "vendored ego-contract headers")

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

option(EGO_RUNTIME_BUILD_TESTS "Build unit tests" ON)

find_package(Protobuf REQUIRED)
if(NOT Protobuf_PROTOC_EXECUTABLE)
    message(FATAL_ERROR "protoc not found. Install: sudo apt install -y protobuf-compiler")
endif()

find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE EGO_RUNTIME_GIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
endif()
if(NOT EGO_RUNTIME_GIT_HASH)
    set(EGO_RUNTIME_GIT_HASH "unknown")
endif()

add_library(ego_protocol INTERFACE)
target_include_directories(ego_protocol INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    "${EGO_V1_DIR}")

# Vendored .proto from ego-contract v1.3; generated at build time with system protoc.
set(EGO_PROTO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ego-contract/proto")
set(EGO_PROTO_FILES
    "${EGO_PROTO_ROOT}/ego/v1/ego_common.proto"
    "${EGO_PROTO_ROOT}/ego/v1/ego_config.proto"
    "${EGO_PROTO_ROOT}/ego/v1/ego_control.proto"
    "${EGO_PROTO_ROOT}/ego/v1/ego_data.proto"
)

foreach(_proto IN LISTS EGO_PROTO_FILES)
    if(NOT EXISTS "${_proto}")
        message(FATAL_ERROR
            "Missing ${_proto}. Run: git pull origin contract-v1.3")
    endif()
endforeach()

set(PROTOBUF_IMPORT_DIRS "${EGO_PROTO_ROOT}")
set(PROTOBUF_GENERATE_CPP_APPEND_PATH OFF)
protobuf_generate_cpp(EGO_PROTO_SRCS EGO_PROTO_HDRS ${EGO_PROTO_FILES})

if(NOT EGO_PROTO_SRCS)
    message(FATAL_ERROR
        "protobuf_generate_cpp produced no sources. "
        "Install protobuf-compiler and re-run cmake.")
endif()

add_library(ego_proto STATIC ${EGO_PROTO_SRCS} ${EGO_PROTO_HDRS})
target_include_directories(ego_proto PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(ego_proto PUBLIC protobuf::libprotobuf)

add_library(ego_runtime_lib STATIC
    src/chunk_writer.cpp
    src/config.cpp
    src/contract_control_client.cpp
    src/contract_frame_io.cpp
    src/contract_tcp_client.cpp
    src/control_ipc.cpp
    src/error_log.cpp
    src/nav_provider.cpp
    src/nav_sidecar_writer.cpp
    src/runtime_service.cpp
    src/session_checkpoint.cpp
    src/session_integrity.cpp
    src/session_manager.cpp
    src/offline_trigger.cpp
    src/shutdown.cpp
    src/storage_monitor.cpp
    src/version.cpp
)
target_include_directories(ego_runtime_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ego_runtime_lib PUBLIC ego_protocol ego_proto)
target_compile_definitions(ego_runtime_lib PUBLIC EGO_RUNTIME_GIT_HASH="${EGO_RUNTIME_GIT_HASH}")

if(WIN32)
    target_compile_definitions(ego_runtime_lib PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(ego_runtime_lib PRIVATE ws2_32)
endif()

add_executable(ego-runtime src/main.cpp)
target_link_libraries(ego-runtime PRIVATE ego_runtime_lib)

add_executable(localpc-finalize src/localpc_finalize_main.cpp)

if(EGO_RUNTIME_BUILD_TESTS)
    add_executable(ego_runtime_tests tests/test_main.cpp)
    target_link_libraries(ego_runtime_tests PRIVATE ego_runtime_lib)
    if(WIN32)
        target_link_libraries(ego_runtime_tests PRIVATE ws2_32)
    endif()
    enable_testing()
    add_test(NAME ego_runtime_tests COMMAND ego_runtime_tests)
endif()

install(TARGETS ego-runtime localpc-finalize RUNTIME DESTINATION bin)
install(FILES config/board.yaml DESTINATION etc/ego-runtime RENAME config.yaml)
install(FILES systemd/ego-runtime.service DESTINATION lib/systemd/system)
