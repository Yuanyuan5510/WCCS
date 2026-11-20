cmake_minimum_required(VERSION 3.15)
project(wccs VERSION 1.0.0 LANGUAGES C CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(WCCS_BUILD_TESTS    "Build the C core test matrix"        ON)
option(WCCS_BUILD_SHARED   "Build the shared library"            ON)
option(WCCS_BUILD_CPP_TEST "Build the C++ binding vector test"   ON)

set(WCCS_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")

# ---- static library (self-contained, no DLL needed) --------------------
add_library(wccs_static STATIC src/wccs.c)
add_library(wccs::static ALIAS wccs_static)
target_include_directories(wccs_static PUBLIC
    $<BUILD_INTERFACE:${WCCS_INCLUDE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_definitions(wccs_static PUBLIC WCCS_STATIC)
set_target_properties(wccs_static PROPERTIES
    OUTPUT_NAME wccs_static
    POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(wccs_static PRIVATE /W4)
else()
    target_compile_options(wccs_static PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---- shared library (the C ABI artifact shipped to all bindings) ------
if(WCCS_BUILD_SHARED)
    add_library(wccs SHARED src/wccs.c)
    add_library(wccs::wccs ALIAS wccs)
    target_include_directories(wccs PUBLIC
        $<BUILD_INTERFACE:${WCCS_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    target_compile_definitions(wccs PRIVATE WCCS_BUILD_DLL)
    if(WIN32 AND CMAKE_C_COMPILER_ID STREQUAL "GNU")
        # Produce a self-contained DLL (no libgcc/libwinpthread dependency).
        target_link_options(wccs PRIVATE -static-libgcc -static)
    endif()
    set_target_properties(wccs PROPERTIES
        OUTPUT_NAME wccs
        C_VISIBILITY_PRESET hidden
        WINDOWS_EXPORT_ALL_SYMBOLS OFF)
    if(MSVC)
        target_compile_options(wccs PRIVATE /W4)
    else()
        target_compile_options(wccs PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

# ---- tests (section 5.1 of work.md) -----------------------------------
if(WCCS_BUILD_TESTS)
    enable_testing()
    add_executable(wccs_test_core   tests/test_core.c   tests/sha256.c)
    add_executable(wccs_test_vector tests/test_vector.c tests/sha256.c)
    target_link_libraries(wccs_test_core   PRIVATE wccs_static)
    target_link_libraries(wccs_test_vector PRIVATE wccs_static)
    add_test(NAME core_matrix  COMMAND wccs_test_core)
    add_test(NAME test_vector  COMMAND wccs_test_vector)

    if(WCCS_BUILD_CPP_TEST)
        add_executable(wccs_cpp_vector bindings/cpp/test/vector.cpp)
        target_link_libraries(wccs_cpp_vector PRIVATE wccs_static)
        target_include_directories(wccs_cpp_vector PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/bindings/cpp/include")
        set_target_properties(wccs_cpp_vector PROPERTIES
            CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
        add_test(NAME cpp_vector COMMAND wccs_cpp_vector)
    endif()
endif()

# ---- install / packaging ----------------------------------------------
set(WCCS_INSTALL_TARGETS wccs_static)
if(WCCS_BUILD_SHARED)
    list(APPEND WCCS_INSTALL_TARGETS wccs)
endif()
install(TARGETS ${WCCS_INSTALL_TARGETS}
    EXPORT wccsTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/wccs.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

configure_package_config_file(
    cmake/wccs-config.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/wccs-config.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/wccs)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/wccs-config-version.cmake"
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/wccs-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/wccs-config-version.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/wccs)
install(EXPORT wccsTargets NAMESPACE wccs::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/wccs)

configure_file(wccs.pc.in "${CMAKE_CURRENT_BINARY_DIR}/wccs.pc" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/wccs.pc"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
