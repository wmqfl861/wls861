cmake_minimum_required(VERSION 3.27)
project(wls_native_foundation VERSION 0.1.0 LANGUAGES CXX)

# Deliberately separate from the Linux production runtime. Never remove its
# platform guard to build this bounded format/algorithm slice.
if(NOT CMAKE_SYSTEM_NAME MATCHES "^(Windows|Linux)$")
    message(FATAL_ERROR "Native foundation supports Windows or Linux verification only")
endif()
if(WIN32 AND NOT MSVC)
    message(FATAL_ERROR "The initial Windows support set requires MSVC")
endif()
if(WIN32)
    # CMAKE_VS_PLATFORM_TOOLSET_VERSION is an MSBuild .props selector and
    # CMake may shorten a three-component directory version. Check the
    # explicitly requested toolset and actual compiler instead.
    file(TO_CMAKE_PATH "${CMAKE_CXX_COMPILER}" WLS_NATIVE_COMPILER_PATH)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/native-toolchain.txt"
        "generator=${CMAKE_GENERATOR}\nrequested_toolset=${CMAKE_GENERATOR_TOOLSET}\nprops_toolset=${CMAKE_VS_PLATFORM_TOOLSET_VERSION}\ncompiler=${WLS_NATIVE_COMPILER_PATH}\ncompiler_version=${CMAKE_CXX_COMPILER_VERSION}\nsdk=${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}\npointer_bytes=${CMAKE_SIZEOF_VOID_P}\n")
    if(NOT CMAKE_GENERATOR STREQUAL "Visual Studio 17 2022" OR
       NOT CMAKE_GENERATOR_TOOLSET STREQUAL "v143,host=x64,version=14.44.35207" OR
       NOT WLS_NATIVE_COMPILER_PATH MATCHES "/VC/Tools/MSVC/14\\.44\\.35207/bin/Hostx64/x64/cl\\.exe$" OR
       NOT CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION STREQUAL "10.0.26100.0" OR
       NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "Initial Windows build set: VS 2022 x64, MSVC 14.44.35207, SDK 10.0.26100.0")
    endif()
endif()
get_filename_component(WLS_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
find_package(Threads REQUIRED)
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/manifest.sha256" WLS_NATIVE_MANIFEST_SHA256)
string(STRIP "${WLS_NATIVE_MANIFEST_SHA256}" WLS_NATIVE_MANIFEST_SHA256)
configure_file(src/build_identity.hpp.in generated/build_identity.hpp @ONLY)

execute_process(COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/prepare_bundle.py"
    --source-root "${WLS_REPO_ROOT}"
    --destination "${CMAKE_CURRENT_BINARY_DIR}/captured-resources"
    --expected-manifest-sha256 "${WLS_NATIVE_MANIFEST_SHA256}"
    RESULT_VARIABLE WLS_NATIVE_BUNDLE_RESULT)
if(NOT WLS_NATIVE_BUNDLE_RESULT EQUAL 0)
    message(FATAL_ERROR "Native foundation public resource capture failed")
endif()
file(READ "${WLS_REPO_ROOT}/resources/runtime-manifest.json" WLS_NATIVE_MANIFEST)
string(JSON WLS_NATIVE_ASSET_COUNT LENGTH "${WLS_NATIVE_MANIFEST}" assets)
math(EXPR WLS_NATIVE_ASSET_LAST "${WLS_NATIVE_ASSET_COUNT} - 1")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${WLS_REPO_ROOT}/resources/runtime-manifest.json"
    "${WLS_REPO_ROOT}/src/runtime_resource_catalog.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/manifest.sha256"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/prepare_bundle.py")
foreach(index RANGE ${WLS_NATIVE_ASSET_LAST})
    string(JSON resource_path GET "${WLS_NATIVE_MANIFEST}" assets ${index} path)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${WLS_REPO_ROOT}/${resource_path}")
endforeach()

add_library(wls_native_foundation STATIC
    "${WLS_REPO_ROOT}/src/config.cpp"
    "${WLS_REPO_ROOT}/src/scheduler.cpp"
    src/foundation.cpp src/public_resources.cpp)
target_include_directories(wls_native_foundation PUBLIC include "${WLS_REPO_ROOT}/include")
target_include_directories(wls_native_foundation PRIVATE "${WLS_REPO_ROOT}/src"
    "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(wls_native_foundation PUBLIC Threads::Threads)
if(WIN32)
    target_sources(wls_native_foundation PRIVATE src/system_windows.cpp)
    target_compile_definitions(wls_native_foundation PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN
        UNICODE _UNICODE _WIN32_WINNT=0x0A00)
    target_compile_options(wls_native_foundation PUBLIC /W4 /WX /permissive- /utf-8 /EHsc)
    target_link_libraries(wls_native_foundation PRIVATE bcrypt advapi32)
else()
    find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)
    target_sources(wls_native_foundation PRIVATE src/system_linux.cpp)
    target_compile_options(wls_native_foundation PUBLIC
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
    target_link_libraries(wls_native_foundation PRIVATE OpenSSL::Crypto)
endif()

add_executable(wls-native-foundation src/main.cpp)
target_link_libraries(wls-native-foundation PRIVATE wls_native_foundation)
install(TARGETS wls-native-foundation RUNTIME DESTINATION bin)
install(DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/captured-resources/" DESTINATION share/wls861)

include(CTest)
if(BUILD_TESTING)
    foreach(suite IN ITEMS modules platform)
        add_executable(native_${suite}_tests tests/${suite}_tests.cpp)
        target_link_libraries(native_${suite}_tests PRIVATE wls_native_foundation)
        add_test(NAME native_${suite} COMMAND native_${suite}_tests
            "${CMAKE_CURRENT_BINARY_DIR}/captured-resources")
        set_tests_properties(native_${suite} PROPERTIES TIMEOUT 30)
    endforeach()
    add_test(NAME native_cli COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_native_cli.py"
        --executable "$<TARGET_FILE:wls-native-foundation>"
        --bundle "${CMAKE_CURRENT_BINARY_DIR}/captured-resources")
    set_tests_properties(native_cli PROPERTIES TIMEOUT 60)
    add_test(NAME native_bundle_capture COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_bundle_capture.py" --source-root "${WLS_REPO_ROOT}")
    set_tests_properties(native_bundle_capture PROPERTIES TIMEOUT 30)
endif()
