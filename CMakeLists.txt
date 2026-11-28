# Copyright (c) 2026 Mfweb
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.16)

project(
    ktos_sdk
    VERSION 1.0.6
    LANGUAGES C
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(WIN32)
    enable_language(RC)
elseif(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(
        FATAL_ERROR
            "Unsupported host platform '${CMAKE_SYSTEM_NAME}'. Supported transports: Windows D3XX and Linux libusb-1.0."
    )
endif()

math(EXPR SDK_VERSION_NUMBER
     "(${PROJECT_VERSION_MAJOR} << 16) | (${PROJECT_VERSION_MINOR} << 8) | ${PROJECT_VERSION_PATCH}"
)

# ------------------------------------------------------------------------------
# Platform-specific setup
# ------------------------------------------------------------------------------

set(SDK_CORE_SOURCES
    src/sdk.c
    src/legacy.c
    src/image.c
    src/vsa_gps.c
    src/drm.c
    src/ktos_protocol.c
    src/sha256.c
)

if(WIN32)
    # Windows VERSIONINFO resource
    set(HOST_VERSION "${PROJECT_VERSION}")
    set(HOST_VERSION_NUMERIC
        "${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},0"
    )
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

    set(SDK_TRANSPORT_SOURCES src/transport_windows.c "${CMAKE_CURRENT_BINARY_DIR}/version.rc")
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBUSB REQUIRED libusb-1.0)
    find_package(Threads REQUIRED)
    set(SDK_TRANSPORT_SOURCES src/transport_linux.c)
endif()

# ------------------------------------------------------------------------------
# Core shared library (kcsdr.dll on Windows, libkcsdr.so on Linux)
# ------------------------------------------------------------------------------

add_library(kcsdr SHARED ${SDK_CORE_SOURCES} ${SDK_TRANSPORT_SOURCES})

target_include_directories(
    kcsdr
    PUBLIC include
    PRIVATE src
)

target_compile_options(kcsdr PRIVATE "$<$<COMPILE_LANGUAGE:C>:-Wall;-Wextra;-Werror;-O2>")

target_compile_definitions(
    kcsdr PRIVATE KTOS_SDK_VERSION_NUMBER=${SDK_VERSION_NUMBER}
                  KTOS_SDK_VERSION_TEXT="${PROJECT_VERSION}"
)

if(WIN32)
    target_compile_definitions(kcsdr PRIVATE _WIN32_WINNT=0x0602)
    set_target_properties(kcsdr PROPERTIES PREFIX "")
    if(MINGW)
        target_link_options(kcsdr PRIVATE -static-libgcc -Wl,--kill-at)
    endif()
else()
    target_include_directories(kcsdr PRIVATE ${LIBUSB_INCLUDE_DIRS})
    target_compile_definitions(kcsdr PRIVATE _GNU_SOURCE)
    target_compile_options(kcsdr PRIVATE -fvisibility=hidden)
    target_link_libraries(kcsdr PRIVATE Threads::Threads ${CMAKE_DL_LIBS} m)
endif()

# ------------------------------------------------------------------------------
# Unit tests (using an in-tree FT601 emulator; never ship tests/ emulator libs)
# ------------------------------------------------------------------------------

include(CTest)

if(BUILD_TESTING)
    if(WIN32)
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
        set(FAKE_TRANSPORT_TARGET fake_d3xx)
    else()
        add_library(fake_libusb SHARED tests/fake_libusb.c src/ktos_protocol.c)
        target_include_directories(fake_libusb PRIVATE src ${LIBUSB_INCLUDE_DIRS})
        target_compile_definitions(fake_libusb PRIVATE _GNU_SOURCE)
        target_compile_options(fake_libusb PRIVATE -Wall -Wextra -Werror -O2)
        set_target_properties(
            fake_libusb
            PROPERTIES
                PREFIX "lib"
                OUTPUT_NAME "usb-1.0"
                SUFFIX ".so.0"
                NO_SONAME TRUE
                LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
        )
        set(FAKE_TRANSPORT_TARGET fake_libusb)
    endif()

    add_executable(test-sdk tests/test_sdk.c)
    target_link_libraries(test-sdk PRIVATE kcsdr)
    target_compile_options(test-sdk PRIVATE -UNDEBUG)
    set_target_properties(
        test-sdk
        PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )
    if(NOT WIN32)
        target_compile_definitions(test-sdk PRIVATE _GNU_SOURCE)
        set_target_properties(test-sdk PROPERTIES BUILD_RPATH "$ORIGIN")
    endif()
    add_dependencies(test-sdk ${FAKE_TRANSPORT_TARGET})
    add_custom_command(
        TARGET test-sdk
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:kcsdr>
                $<TARGET_FILE_DIR:test-sdk>
    )
    add_test(
        NAME sdk
        COMMAND test-sdk
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )

    add_executable(test-dynamic tests/test_dynamic.c)
    target_compile_options(test-dynamic PRIVATE -UNDEBUG)
    set_target_properties(
        test-dynamic
        PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )
    if(NOT WIN32)
        target_compile_definitions(test-dynamic PRIVATE _GNU_SOURCE)
        target_link_libraries(test-dynamic PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    endif()
    add_dependencies(test-dynamic test-sdk)
    add_test(
        NAME dynamic
        COMMAND test-dynamic
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )
endif()

# ------------------------------------------------------------------------------
# Optional GNU Radio 3.10 Python blocks (installed as the top-level kc908 package)
# ------------------------------------------------------------------------------

option(
    KTOS_ENABLE_GNURADIO
    "Build/install the KC908 GNU Radio 3.10 Python blocks"
    ON
)

if(KTOS_ENABLE_GNURADIO)
    add_subdirectory(gnuradio)
endif()
