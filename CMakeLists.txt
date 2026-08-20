cmake_minimum_required(VERSION 3.24)

project(runi VERSION 0.2.0 LANGUAGES CXX)

option(RUNI_BUILD_TESTS "Build Runi tests" ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(cmake/CompilerWarnings.cmake)
if(MINGW)
    get_filename_component(RUNI_COMPILER_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    get_filename_component(RUNI_TOOLCHAIN_ROOT "${RUNI_COMPILER_BIN_DIR}" DIRECTORY)
    find_path(SQLite3_INCLUDE_DIR NAMES sqlite3.h HINTS "${RUNI_TOOLCHAIN_ROOT}/include" NO_DEFAULT_PATH)
    find_library(SQLite3_LIBRARY NAMES sqlite3 HINTS "${RUNI_TOOLCHAIN_ROOT}/lib" NO_DEFAULT_PATH)
endif()
find_package(SQLite3 REQUIRED)

add_library(runi_core STATIC
    src/agent/action_parser.cpp
    src/agent/agent_loop.cpp
    src/agent/prompt_prefix.cpp
    src/agent/runtime.cpp
    src/context/checkpoint.cpp
    src/context/context_manager.cpp
    src/context/memory.cpp
    src/core/error.cpp
    src/core/json_codec.cpp
    src/core/json_value.cpp
    src/core/sha256.cpp
    src/core/text.cpp
    src/core/time.cpp
    src/evaluation/evaluation.cpp
    src/model/config.cpp
    src/model/http_client.cpp
    src/model/model_client.cpp
    src/model/providers.cpp
    src/orchestration/execution.cpp
    src/orchestration/multi_agent.cpp
    src/service/agent_service.cpp
    src/service/service.cpp
    src/state/context_cache.cpp
    src/state/run_store.cpp
    src/state/session.cpp
    src/state/session_store.cpp
    src/state/sqlite_session_store.cpp
    src/state/state_store.cpp
    src/state/task_state.cpp
    src/tool/process_runner.cpp
    src/tool/security.cpp
    src/tool/tool_executor.cpp
    src/tool/tools.cpp
    src/tool/workspace.cpp
)
target_include_directories(runi_core PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(runi_core PUBLIC cxx_std_20)
target_link_libraries(runi_core PUBLIC SQLite::SQLite3)
runi_enable_warnings(runi_core)
if(WIN32)
    target_link_libraries(runi_core PUBLIC winhttp ws2_32)
endif()

add_executable(runi apps/runi_cli/main.cpp)
target_link_libraries(runi PRIVATE runi_core)
runi_enable_warnings(runi)

add_executable(runi_eval apps/runi_eval/main.cpp)
target_link_libraries(runi_eval PRIVATE runi_core)
runi_enable_warnings(runi_eval)

add_executable(runi_server apps/runi_server/main.cpp)
target_link_libraries(runi_server PRIVATE runi_core)
runi_enable_warnings(runi_server)

if(MINGW)
    target_link_options(runi PRIVATE -static -static-libgcc -static-libstdc++)
    target_link_options(runi_eval PRIVATE -static -static-libgcc -static-libstdc++)
    target_link_options(runi_server PRIVATE -static -static-libgcc -static-libstdc++)
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
    add_executable(runi_runtime_tests tests/runtime/test_runtime.cpp)
    target_link_libraries(runi_runtime_tests PRIVATE runi_core)
    target_compile_definitions(runi_runtime_tests PRIVATE RUNI_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    runi_enable_warnings(runi_runtime_tests)
    if(MINGW)
        target_link_options(runi_runtime_tests PRIVATE -static -static-libgcc -static-libstdc++)
    endif()
    add_test(NAME runi.unit COMMAND runi_unit_tests)
    add_test(NAME runi.runtime COMMAND runi_runtime_tests)
    add_test(NAME runi.cli_help COMMAND runi --help)
    add_test(NAME runi.cli_help_equals COMMAND runi --cwd=. --help)
    add_test(NAME runi.server_help COMMAND runi_server --help)
    add_test(
        NAME runi.benchmark
        COMMAND runi_eval
            ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/coding_tasks.json
            ${CMAKE_CURRENT_BINARY_DIR}/evaluation/benchmark-v1.json
            ${CMAKE_CURRENT_BINARY_DIR}/evaluation/workspaces
    )
    set_tests_properties(runi.benchmark PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(
        NAME runi.context_ablation
        COMMAND runi_eval
            context-ablation
            ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/context_ablation.json
            ${CMAKE_CURRENT_BINARY_DIR}/evaluation/context-ablation-v1.json
            ${CMAKE_CURRENT_BINARY_DIR}/evaluation/context-workspaces
    )
    set_tests_properties(runi.context_ablation PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()
