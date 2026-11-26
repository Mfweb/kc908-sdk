# Copyright (c) 2026 Mfweb
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.16)

project(
    ktos_sdk
    VERSION 1.0.5
    LANGUAGES C RC
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT WIN32)
    message(FATAL_ERROR "This transport uses Windows D3XX. The wire codec is portable C.")
endif()

# ------------------------------------------------------------------------------
# Windows VERSIONINFO resource
# ------------------------------------------------------------------------------

set(HOST_VERSION "${PROJECT_VERSION}")
set(HOST_VERSION_NUMERIC
    "${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},0")
set(HOST_FILE_TYPE VFT_DLL)
set(HOST_DESCRIPTION "KC908 control and IQ SDK")
set(HOST_INTERNAL_NAME kcsdr)
set(HOST_FILENAME kcsdr.dll)
set(HOST_PRODUCT_NAME "KTOS SDK")

configure_file(resources/version.rc.in version.rc @ONLY)

if(MINGW)
    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        set(resource_target pe-i386)
    else()
        set(resource_target pe-x86-64)
    endif()
    set_source_files_properties(
        "${CMAKE_CURRENT_BINARY_DIR}/version.rc"
        PROPERTIES COMPILE_OPTIONS "--target=${resource_target}"
    )
endif()

# ------------------------------------------------------------------------------
# Core shared library (kcsdr.dll)
# ------------------------------------------------------------------------------

add_library(
    kcsdr SHARED
    src/sdk.c
    src/transport_windows.c
    src/legacy.c
    src/image.c
    src/vsa_gps.c
    src/drm.c
    src/ktos_protocol.c
    src/sha256.c
    "${CMAKE_CURRENT_BINARY_DIR}/version.rc"
)

target_include_directories(
    kcsdr
    PUBLIC include
    PRIVATE src
)

target_compile_options(
    kcsdr PRIVATE "$<$<COMPILE_LANGUAGE:C>:-Wall;-Wextra;-Werror;-O2>"
)

math(EXPR SDK_VERSION_NUMBER
     "(${PROJECT_VERSION_MAJOR} << 16) | (${PROJECT_VERSION_MINOR} << 8) | ${PROJECT_VERSION_PATCH}")

target_compile_definitions(
    kcsdr PRIVATE
    _WIN32_WINNT=0x0602
    KTOS_SDK_VERSION_NUMBER=${SDK_VERSION_NUMBER}
    KTOS_SDK_VERSION_TEXT="${PROJECT_VERSION}"
)

set_target_properties(kcsdr PROPERTIES PREFIX "")

if(MINGW)
    target_link_options(kcsdr PRIVATE -static-libgcc -Wl,--kill-at)
endif()

# ------------------------------------------------------------------------------
# Unit tests (using an in-tree FT601 emulator DLL; never ship tests/FTD3XX.dll)
# ------------------------------------------------------------------------------

include(CTest)

if(BUILD_TESTING)
    add_library(fake_d3xx SHARED tests/fake_d3xx.c src/ktos_protocol.c)
    target_include_directories(fake_d3xx PRIVATE src)
    target_compile_definitions(fake_d3xx PRIVATE _WIN32_WINNT=0x0602)
    set_target_properties(
        fake_d3xx
        PROPERTIES
            PREFIX ""
            OUTPUT_NAME FTD3XX
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )
    if(MINGW)
        target_link_options(fake_d3xx PRIVATE -static-libgcc -Wl,--kill-at)
    endif()

    add_executable(test-sdk tests/test_sdk.c)
    target_link_libraries(test-sdk PRIVATE kcsdr)
    target_compile_options(test-sdk PRIVATE -UNDEBUG)
    set_target_properties(
        test-sdk
        PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )
    add_dependencies(test-sdk fake_d3xx)
    add_custom_command(
        TARGET test-sdk
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:kcsdr> $<TARGET_FILE_DIR:test-sdk>
    )
    add_test(NAME sdk COMMAND test-sdk)

    add_executable(test-dynamic tests/test_dynamic.c)
    target_compile_options(test-dynamic PRIVATE -UNDEBUG)
    set_target_properties(
        test-dynamic
        PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )
    add_dependencies(test-dynamic test-sdk)
    add_test(NAME dynamic COMMAND test-dynamic)
endif()
