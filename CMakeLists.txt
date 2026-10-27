cmake_minimum_required(VERSION 3.20)
project(wls861 VERSION 0.10.0 LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "M1 currently requires Linux journal and process semantics")
endif()
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang)$")
    message(FATAL_ERROR "M1 supports GCC or Clang with C++20")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(THREADS_PREFER_PTHREAD_FLAG TRUE)
find_package(Threads REQUIRED)
find_package(Boost 1.83 REQUIRED)
find_package(SQLite3 3.37 REQUIRED)
find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)
find_package(CURL 7.88 REQUIRED)
find_library(WLS_YAML_LIBRARY NAMES yaml libyaml-0.so.2 REQUIRED)

option(WLS_ENABLE_UBSAN "Enable undefined-behavior sanitizer" OFF)
option(WLS_ENABLE_ASAN "Enable address sanitizer" OFF)

add_library(wls_build_options INTERFACE)
target_compile_options(wls_build_options INTERFACE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
if(WLS_ENABLE_UBSAN)
    target_compile_options(wls_build_options INTERFACE
        -fsanitize=undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
    target_link_options(wls_build_options INTERFACE -fsanitize=undefined)
endif()
if(WLS_ENABLE_ASAN)
    target_compile_options(wls_build_options INTERFACE
        -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(wls_build_options INTERFACE -fsanitize=address)
endif()

add_library(wls_kernel STATIC
    src/task_engine.cpp
    src/config.cpp
    src/scheduler.cpp
    src/journal_posix.cpp)
target_include_directories(wls_kernel PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(wls_kernel PUBLIC wls_build_options Threads::Threads)

add_executable(wls861 src/main.cpp)
target_link_libraries(wls861 PRIVATE wls_kernel)

# M1 remains its original independent kernel. The S1 foundation has an explicit
# SQLite backend and external configuration; opening a Journal is never a
# migration path. Boost.JSON uses its documented single-TU library build.
add_library(wls_s1 STATIC
    src/boost_json.cpp
    src/runtime_types.cpp
    src/runtime_resources.cpp
    src/migrations.cpp
    src/capacity.cpp
    src/store.cpp
    src/config_loader.cpp
    src/model.cpp
    src/provider_profile.cpp
    src/protocol_codec.cpp
    src/sse_replay.cpp
    src/https_transport.cpp
    src/live_model.cpp
    src/management_context.cpp
    src/owned_capacity.cpp
    src/context.cpp
    src/structured_context.cpp
    src/structured_history.cpp
    src/worker_history.cpp
    src/schema.cpp
    src/offline_runtime.cpp
    src/event_runtime.cpp
    src/workspace_posix.cpp
    src/sandbox_posix.cpp
    src/process_evidence.cpp
    src/worker_tools.cpp
    src/delivery.cpp
    src/cleanup_posix.cpp
    src/cleanup_runtime.cpp)
target_include_directories(wls_s1 SYSTEM PUBLIC
    ${Boost_INCLUDE_DIRS}
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/valijson/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libyaml/include")
target_link_libraries(wls_s1 PUBLIC wls_kernel SQLite::SQLite3 OpenSSL::Crypto CURL::libcurl ${WLS_YAML_LIBRARY})
add_executable(wls src/cli.cpp)
target_link_libraries(wls PRIVATE wls_s1)

include(cmake/runtime_resources.cmake)
wls_install_runtime_resources(wls wls861)

include(CTest)
if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    find_program(WLS_TEST_OPENSSL openssl REQUIRED)
    execute_process(COMMAND "${Python3_EXECUTABLE}" -c "import jsonschema"
        RESULT_VARIABLE WLS_CONTRACT_DEPENDENCY_RESULT
        OUTPUT_QUIET ERROR_QUIET)
    if(NOT WLS_CONTRACT_DEPENDENCY_RESULT EQUAL 0)
        message(FATAL_ERROR
            "Contract tests need jsonschema. Install requirements-test.txt in a virtual environment and set Python3_EXECUTABLE to that environment's Python, or use -DBUILD_TESTING=OFF to build only the C++ runtime.")
    endif()
    foreach(suite IN ITEMS journal config scheduler task_engine)
        add_executable(${suite}_tests tests/${suite}_tests.cpp)
        target_link_libraries(${suite}_tests PRIVATE wls_kernel)
        target_include_directories(${suite}_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
        add_test(NAME ${suite} COMMAND ${suite}_tests)
        set_tests_properties(${suite} PROPERTIES TIMEOUT 90)
    endforeach()
    foreach(suite IN ITEMS runtime_types runtime_resources store event_store workflow_store acceptance_store config_loader model schema offline_runtime event_runtime workflow_runtime acceptance_runtime workspace sandbox worker_tools delivery cleanup cleanup_runtime audit24_reopen process_evidence audit24_reconcile audit24_capacity audit24_capacity_repro r1_history r1_structured r1_window r2_rework r2_recovery r2_amend r2a_mkdir allocation_recovery doctor_diagnostics protocol_codec sse_replay r1_management owned_capacity owned_capacity_runtime live_model_store)
        add_executable(${suite}_tests tests/${suite}_tests.cpp)
        target_link_libraries(${suite}_tests PRIVATE wls_s1)
        target_include_directories(${suite}_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
        target_compile_definitions(${suite}_tests PRIVATE WLS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
        add_test(NAME ${suite} COMMAND ${suite}_tests)
        set_tests_properties(${suite} PROPERTIES TIMEOUT 90)
    endforeach()
    set_tests_properties(owned_capacity_runtime PROPERTIES TIMEOUT 180)
    set_tests_properties(r1_management PROPERTIES TIMEOUT 300)
    foreach(suite IN ITEMS doctor_diagnostics protocol_replay)
        add_test(NAME ${suite}_cli
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_${suite}_cli.py"
                    "$<TARGET_FILE:wls>" "${CMAKE_CURRENT_SOURCE_DIR}")
        set_tests_properties(${suite}_cli PROPERTIES TIMEOUT 90)
    endforeach()
    # Local TLS fixture harness only: never installed or exposed by wls CLI.
    add_executable(https_transport_driver tests/https_transport_driver.cpp)
    target_link_libraries(https_transport_driver PRIVATE wls_s1)
    target_include_directories(https_transport_driver PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    add_test(NAME https_transport
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_https_transport.py"
                "$<TARGET_FILE:https_transport_driver>" "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(https_transport PROPERTIES TIMEOUT 180)
    add_executable(live_model_store_driver tests/live_model_store_driver.cpp)
    target_link_libraries(live_model_store_driver PRIVATE wls_s1)
    target_include_directories(live_model_store_driver PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    add_test(NAME live_model_store_tls
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_live_model_store.py"
                "$<TARGET_FILE:live_model_store_driver>" "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(live_model_store_tls PROPERTIES TIMEOUT 180)
    add_executable(live_workflow_driver tests/live_workflow_driver.cpp)
    target_link_libraries(live_workflow_driver PRIVATE wls_s1)
    target_include_directories(live_workflow_driver PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    add_test(NAME live_workflow
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_live_workflow.py"
                "$<TARGET_FILE:live_workflow_driver>" "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(live_workflow PROPERTIES TIMEOUT 300)
    add_test(NAME live_workflow_cli
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_live_workflow_cli.py"
                "$<TARGET_FILE:wls>" "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(live_workflow_cli PROPERTIES TIMEOUT 180)
    add_test(NAME runtime_resources_cli
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/runtime_resources_cli_tests.py"
                "$<TARGET_FILE:wls>" "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
    set_tests_properties(runtime_resources_cli PROPERTIES TIMEOUT 120)
    add_test(NAME r3_evaluation
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/r3_evaluation_tests.py"
                "$<TARGET_FILE:wls>" "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(r3_evaluation PROPERTIES TIMEOUT 180)
    add_test(NAME handoff_integrity
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/verify_handoff.py")
    add_test(NAME review_source_integrity
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/verify_review_source.py")
    add_test(NAME specification_contracts
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/contracts_tests.py")
    add_test(NAME demo_smoke
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/demo_smoke.py"
                "$<TARGET_FILE:wls861>")
    add_test(NAME s1_cli_smoke
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/s1_cli_smoke.py"
                "$<TARGET_FILE:wls>" "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(s1_cli_smoke PROPERTIES TIMEOUT 90)
    add_test(NAME s12_cli_smoke
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/s12_cli_smoke.py"
                "$<TARGET_FILE:wls>" "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(s12_cli_smoke PROPERTIES TIMEOUT 90)
    add_test(NAME s13_cli_smoke
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/s13_cli_smoke.py"
                "$<TARGET_FILE:wls>" "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(s13_cli_smoke PROPERTIES TIMEOUT 90)
    add_test(NAME s14_cli_smoke
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/s14_cli_smoke.py"
                "$<TARGET_FILE:wls>" "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(s14_cli_smoke PROPERTIES TIMEOUT 90)
    add_test(NAME s15_cli_smoke
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/s15_cli_smoke.py"
                "$<TARGET_FILE:wls>" "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(s15_cli_smoke PROPERTIES TIMEOUT 120)
    set_tests_properties(handoff_integrity review_source_integrity specification_contracts demo_smoke
        PROPERTIES TIMEOUT 60)
    set_tests_properties(cleanup_runtime PROPERTIES TIMEOUT 180)
    set_tests_properties(audit24_capacity_repro PROPERTIES TIMEOUT 300)
    add_test(NAME s16_cli_smoke
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/s16_cli_smoke.py"
                "$<TARGET_FILE:wls>" "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(s16_cli_smoke PROPERTIES TIMEOUT 120)
    add_test(NAME audit24_reconcile_cli_smoke
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/audit24_reconcile_cli_smoke.py"
            "$<TARGET_FILE:wls>" "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(audit24_reconcile_cli_smoke PROPERTIES TIMEOUT 120)
    add_test(NAME audit24_reopen_cli_smoke
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/audit24_reopen_cli_smoke.py"
            "$<TARGET_FILE:wls>" "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(audit24_reopen_cli_smoke PROPERTIES TIMEOUT 120)
    add_test(NAME review_cli_smoke
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/review_cli_smoke.py"
                "$<TARGET_FILE:wls>" "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(review_cli_smoke PROPERTIES TIMEOUT 120)
    add_test(NAME audit24_capacity_cli_smoke
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/audit24_capacity_cli_smoke.py"
                "$<TARGET_FILE:wls>" "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(audit24_capacity_cli_smoke PROPERTIES TIMEOUT 180)
    add_test(NAME r1_history_cli_smoke
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/r1_history_cli_smoke.py"
                "$<TARGET_FILE:wls>" "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(r1_history_cli_smoke PROPERTIES TIMEOUT 120)
    add_test(NAME r1_structured_cli_smoke
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/r1_structured_cli_smoke.py"
                "$<TARGET_FILE:wls>" "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(r1_structured_cli_smoke PROPERTIES TIMEOUT 120)
    add_test(NAME r1_window_cli_smoke
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/r1_window_cli_smoke.py"
                "$<TARGET_FILE:wls>" "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(r1_window_cli_smoke PROPERTIES TIMEOUT 180)
    add_test(NAME r2_rework_cli_smoke
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/r2_rework_cli_smoke.py"
                "$<TARGET_FILE:wls>" "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(r2_rework_cli_smoke PROPERTIES TIMEOUT 120)

    add_test(NAME r2_recovery_cli_smoke
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/r2_recovery_cli_smoke.py"
                "$<TARGET_FILE:wls>" "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(r2_recovery_cli_smoke PROPERTIES TIMEOUT 120)
    add_test(NAME r2_integration_cli_smoke
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/r2_integration_cli_smoke.py"
                "$<TARGET_FILE:wls>" "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(r2_integration_cli_smoke PROPERTIES TIMEOUT 120)
    add_test(NAME r2_amend_cli_smoke
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/r2_amend_cli_smoke.py"
                "$<TARGET_FILE:wls>" "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(r2_amend_cli_smoke PROPERTIES TIMEOUT 120)
    add_test(NAME r2a_mkdir_cli_smoke
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/r2a_mkdir_cli_smoke.py"
                "$<TARGET_FILE:wls>" "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(r2a_mkdir_cli_smoke PROPERTIES TIMEOUT 120)
    add_test(NAME allocation_recovery_cli
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_s1_allocation_recovery_cli.py"
                "$<TARGET_FILE:wls>" "${CMAKE_CURRENT_SOURCE_DIR}")
    set_tests_properties(allocation_recovery_cli PROPERTIES TIMEOUT 180)
endif()
