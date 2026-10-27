# Program assets only. Site/system/function configuration and credentials are
# external inputs and are deliberately absent from this distribution.
function(wls_install_runtime_resources)
    if(NOT Python3_EXECUTABLE)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
    endif()
    set(WLS_RESOURCE_SOURCE_ROOT "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/..")
    get_filename_component(WLS_RESOURCE_SOURCE_ROOT "${WLS_RESOURCE_SOURCE_ROOT}" ABSOLUTE)
    set(WLS_RESOURCE_INSTALLER "${WLS_RESOURCE_SOURCE_ROOT}/scripts/install_runtime_resources.py")
    set(WLS_RESOURCE_BUILD_MANIFEST "${CMAKE_CURRENT_BINARY_DIR}/wls-runtime-manifest.json")
    configure_file("${WLS_RESOURCE_SOURCE_ROOT}/resources/runtime-manifest.json"
        "${WLS_RESOURCE_BUILD_MANIFEST}" COPYONLY)
    configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/install_runtime_resources.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/install_wls_runtime_resources.cmake" @ONLY)
    if(ARGN)
        install(TARGETS ${ARGN} RUNTIME DESTINATION bin COMPONENT wls-runtime)
    endif()
    install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/install_wls_runtime_resources.cmake"
        COMPONENT wls-runtime)
endfunction()
