cmake_minimum_required(VERSION 3.20)
project(wls861 VERSION 0.1.1 LANGUAGES CXX)

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

include(CTest)
if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
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
    add_test(NAME handoff_integrity
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/verify_handoff.py")
    add_test(NAME review_source_integrity
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/verify_review_source.py")
    add_test(NAME specification_contracts
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/contracts_tests.py")
    add_test(NAME demo_smoke
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/demo_smoke.py"
                "$<TARGET_FILE:wls861>")
    set_tests_properties(handoff_integrity review_source_integrity specification_contracts demo_smoke
        PROPERTIES TIMEOUT 60)
endif()
