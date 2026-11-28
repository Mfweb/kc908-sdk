# Copyright (c) 2026 Mfweb
# SPDX-License-Identifier: MIT

include(GNUInstallDirs)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# The Python blocks are only useful with a GNU Radio 3.10 runtime and numpy.
execute_process(
    COMMAND "${Python3_EXECUTABLE}" -c [=[
import sys

try:
    import numpy
    import gnuradio.gr as gr
except Exception as error:
    sys.stderr.write(
        "KC908 GNU Radio blocks require importable gnuradio.gr and numpy: %s\n"
        % error
    )
    sys.exit(1)

version = gr.version()
parts = []
for part in version.split("."):
    if not part.isdigit():
        break
    parts.append(int(part))
if len(parts) < 2 or parts[0] != 3 or parts[1] < 10:
    sys.stderr.write(
        "KC908 GNU Radio blocks require GNU Radio >= 3.10 and < 4 (found %s)\n"
        % version
    )
    sys.exit(1)
print(version)
]=]
    RESULT_VARIABLE KTOS_GNURADIO_CHECK_RESULT
    OUTPUT_VARIABLE KTOS_GNURADIO_VERSION
    ERROR_VARIABLE KTOS_GNURADIO_CHECK_ERROR
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT KTOS_GNURADIO_CHECK_RESULT EQUAL 0)
    message(FATAL_ERROR "${KTOS_GNURADIO_CHECK_ERROR}")
endif()
message(STATUS "KC908 GNU Radio runtime: ${KTOS_GNURADIO_VERSION}")

# Compute the interpreter's purelib for the requested install prefix.  sysconfig
# returns an absolute path already rooted at the prefix, so it must be used
# directly (prefixing it again would duplicate the install path).
execute_process(
    COMMAND "${Python3_EXECUTABLE}" -c [=[
import os
import sys
import sysconfig

prefix = os.path.normpath(sys.argv[1])
scheme = sysconfig.get_default_scheme()
variables = {
    "base": prefix,
    "platbase": prefix,
    "installed_base": prefix,
    "installed_platbase": prefix,
}
purelib = os.path.normpath(
    sysconfig.get_path("purelib", scheme=scheme, vars=variables)
)

# Debian/Ubuntu's posix_local scheme bakes an extra "local" component into the
# path.  Use the system dist-packages scheme for /usr and remove the duplicated
# component for other prefixes, while preserving the dist-packages layout.
if scheme == "posix_local":
    if prefix == "/usr" and "deb_system" in sysconfig.get_scheme_names():
        purelib = os.path.normpath(
            sysconfig.get_path("purelib", scheme="deb_system", vars=variables)
        )
    elif prefix != "/usr":
        local_prefix = os.path.join(prefix, "local")
        if purelib == local_prefix or purelib.startswith(local_prefix + os.sep):
            purelib = os.path.join(prefix, os.path.relpath(purelib, local_prefix))
print(purelib)
]=]
    "${CMAKE_INSTALL_PREFIX}"
    RESULT_VARIABLE KTOS_GNURADIO_PURELIB_RESULT
    OUTPUT_VARIABLE KTOS_GNURADIO_PURELIB
    ERROR_VARIABLE KTOS_GNURADIO_PURELIB_ERROR
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT KTOS_GNURADIO_PURELIB_RESULT EQUAL 0)
    message(FATAL_ERROR "${KTOS_GNURADIO_PURELIB_ERROR}")
endif()

set(
    KTOS_GNURADIO_PYTHON_DIR
    "${KTOS_GNURADIO_PURELIB}"
    CACHE PATH
    "Directory receiving the top-level kc908 Python package and kcsdr library"
)
set(
    KTOS_GNURADIO_GRC_DIR
    "${CMAKE_INSTALL_DATADIR}/gnuradio/grc/blocks"
    CACHE PATH
    "Directory receiving the KC908 GNU Radio GRC block definitions"
)

install(
    DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/python/kc908"
    DESTINATION "${KTOS_GNURADIO_PYTHON_DIR}"
    FILES_MATCHING
    PATTERN "*.py"
    PATTERN "__pycache__" EXCLUDE
    PATTERN "*.pyc" EXCLUDE
)

# The ctypes binding looks for kcsdr inside the kc908 package.  The in-tree
# FT601 emulator used by the SDK unit tests is deliberately not installed.
install(
    TARGETS kcsdr
    RUNTIME DESTINATION "${KTOS_GNURADIO_PYTHON_DIR}/kc908"
    LIBRARY DESTINATION "${KTOS_GNURADIO_PYTHON_DIR}/kc908"
)

install(
    FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/grc/kc908_source.block.yml"
        "${CMAKE_CURRENT_SOURCE_DIR}/grc/kc908_sink.block.yml"
    DESTINATION "${KTOS_GNURADIO_GRC_DIR}"
)

if(BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/qa_kc908.py")
    add_test(
        NAME gnuradio_qa
        COMMAND
            "${CMAKE_COMMAND}" -E env
            "PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}/python"
            "KTOS_SDK_LIBRARY=$<TARGET_FILE_DIR:test-sdk>/$<TARGET_FILE_NAME:kcsdr>"
            "PYTHONDONTWRITEBYTECODE=1"
            "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/qa_kc908.py"
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )
    set_tests_properties(gnuradio_qa PROPERTIES TIMEOUT 45)
    add_test(
        NAME gnuradio_grc_qa
        COMMAND
            "${CMAKE_COMMAND}" -E env
            "PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}/python"
            "KTOS_SDK_LIBRARY=$<TARGET_FILE_DIR:test-sdk>/$<TARGET_FILE_NAME:kcsdr>"
            "PYTHONDONTWRITEBYTECODE=1"
            "XDG_CACHE_HOME=${CMAKE_CURRENT_BINARY_DIR}/cache"
            "XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/config"
            "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/qa_grc.py"
            "${CMAKE_CURRENT_BINARY_DIR}/generated"
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/tests"
    )
    set_tests_properties(gnuradio_grc_qa PROPERTIES TIMEOUT 45)
endif()
