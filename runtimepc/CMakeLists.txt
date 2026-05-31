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

add_library(ego_runtime_lib STATIC
    src/chunk_writer.cpp
    src/config.cpp
    src/contract_frame_io.cpp
    src/contract_tcp_client.cpp
    src/control_ipc.cpp
    src/error_log.cpp
    src/runtime_service.cpp
    src/session_integrity.cpp
    src/session_manager.cpp
    src/offline_trigger.cpp
    src/shutdown.cpp
    src/storage_monitor.cpp
    src/version.cpp
)
target_include_directories(ego_runtime_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ego_runtime_lib PUBLIC ego_protocol)
target_compile_definitions(ego_runtime_lib PUBLIC EGO_RUNTIME_GIT_HASH="${EGO_RUNTIME_GIT_HASH}")

if(WIN32)
    target_compile_definitions(ego_runtime_lib PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(ego_runtime_lib PRIVATE ws2_32)
endif()

add_executable(ego-runtime src/main.cpp)
target_link_libraries(ego-runtime PRIVATE ego_runtime_lib)

if(EGO_RUNTIME_BUILD_TESTS)
    add_executable(ego_runtime_tests tests/test_main.cpp)
    target_link_libraries(ego_runtime_tests PRIVATE ego_runtime_lib)
    if(WIN32)
        target_link_libraries(ego_runtime_tests PRIVATE ws2_32)
    endif()
    enable_testing()
    add_test(NAME ego_runtime_tests COMMAND ego_runtime_tests)
endif()

install(TARGETS ego-runtime RUNTIME DESTINATION bin)
install(FILES config/board.yaml DESTINATION etc/ego-runtime RENAME config.yaml)
install(FILES systemd/ego-runtime.service DESTINATION lib/systemd/system)
