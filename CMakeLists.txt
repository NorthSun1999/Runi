cmake_minimum_required(VERSION 3.24)

project(runi VERSION 0.1.0 LANGUAGES CXX)

option(RUNI_BUILD_TESTS "Build Runi tests" ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(cmake/CompilerWarnings.cmake)

add_library(runi_core STATIC
    src/agent_loop.cpp
    src/action_parser.cpp
    src/checkpoint.cpp
    src/config.cpp
    src/context_manager.cpp
    src/evaluation.cpp
    src/http_client.cpp
    src/memory.cpp
    src/model_client.cpp
    src/process_runner.cpp
    src/prompt_prefix.cpp
    src/providers.cpp
    src/run_store.cpp
    src/runtime.cpp
    src/security.cpp
    src/session.cpp
    src/session_store.cpp
    src/task_state.cpp
    src/tool_executor.cpp
    src/tools.cpp
    src/workspace.cpp
    src/core/error.cpp
    src/core/json_codec.cpp
    src/core/json_value.cpp
    src/core/sha256.cpp
    src/core/text.cpp
    src/core/time.cpp
)
target_include_directories(runi_core PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(runi_core PUBLIC cxx_std_20)
runi_enable_warnings(runi_core)
if(WIN32)
    target_link_libraries(runi_core PUBLIC winhttp)
endif()

add_executable(runi apps/runi_cli/main.cpp)
target_link_libraries(runi PRIVATE runi_core)
runi_enable_warnings(runi)

add_executable(runi_eval apps/runi_eval/main.cpp)
target_link_libraries(runi_eval PRIVATE runi_core)
runi_enable_warnings(runi_eval)

if(MINGW)
    target_link_options(runi PRIVATE -static -static-libgcc -static-libstdc++)
    target_link_options(runi_eval PRIVATE -static -static-libgcc -static-libstdc++)
endif()

include(CTest)
if(BUILD_TESTING AND RUNI_BUILD_TESTS)
    add_executable(runi_unit_tests tests/unit/test_main.cpp)
    target_link_libraries(runi_unit_tests PRIVATE runi_core)
    target_compile_definitions(runi_unit_tests PRIVATE RUNI_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    runi_enable_warnings(runi_unit_tests)
    if(MINGW)
        target_link_options(runi_unit_tests PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
    add_test(NAME runi.unit COMMAND runi_unit_tests)
    add_test(NAME runi.cli_help COMMAND runi --help)
    add_test(NAME runi.cli_help_equals COMMAND runi --cwd=. --help)
    add_test(
        NAME runi.benchmark
        COMMAND runi_eval
            ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/coding_tasks.json
            ${CMAKE_CURRENT_BINARY_DIR}/evaluation/benchmark-v1.json
            ${CMAKE_CURRENT_BINARY_DIR}/evaluation/workspaces
    )
    set_tests_properties(runi.benchmark PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()
