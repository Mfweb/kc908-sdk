# Copyright (c) 2026 Mfweb
# SPDX-License-Identifier: MIT
"""Small, explicitly typed binding to the public ktos_sdk.h C ABI."""

import ctypes as ct
import ctypes.util
import json
import os
from pathlib import Path


class SDKError(RuntimeError):
    """An SDK operation failed; ``code`` is the original negative status."""

    def __init__(self, operation, code, detail):
        self.code = code
        super().__init__(f"{operation} failed ({code}): {detail}")


class SDK:
    def __init__(self, library_path=""):
        name = "kcsdr.dll" if os.name == "nt" else "libkcsdr.so"
        requested = library_path or os.environ.get("KTOS_SDK_LIBRARY")
        bundled = Path(__file__).resolve().parent / name
        path = requested or (str(bundled) if bundled.is_file() else None)
        path = path or ctypes.util.find_library("kcsdr") or name
        try:
            self.lib = ct.CDLL(str(path))  # KTOS_CALL is cdecl, including on Windows.
            signatures = {
                "ktos_open": ([ct.c_uint32, ct.POINTER(ct.c_void_p)], ct.c_int),
                "ktos_close": ([ct.c_void_p], None),
                "ktos_identity": ([ct.c_void_p, ct.c_void_p, ct.c_uint32], ct.c_int),
                "ktos_error": ([ct.c_void_p, ct.c_void_p, ct.c_uint32], ct.c_int),
                "ktos_iq_command": (
                    [ct.c_void_p, ct.c_uint32, ct.c_void_p, ct.c_uint32],
                    ct.c_int,
                ),
                "ktos_iq_read": (
                    [ct.c_void_p, ct.c_void_p, ct.c_uint32, ct.POINTER(ct.c_uint32)],
                    ct.c_int,
                ),
                "ktos_iq_write": (
                    [ct.c_void_p, ct.c_void_p, ct.c_uint32, ct.POINTER(ct.c_uint32)],
                    ct.c_int,
                ),
                "ktos_iq_stop": ([ct.c_void_p], ct.c_int),
            }
            for symbol, (args, result) in signatures.items():
                function = getattr(self.lib, symbol)
                function.argtypes = args
                function.restype = result
        except (OSError, AttributeError) as error:
            raise RuntimeError(
                f"Cannot load KTOS SDK {path!r}: {error}. Build host/sdk and set "
                "library_path or KTOS_SDK_LIBRARY to kcsdr.dll / libkcsdr.so. "
                "The vendor function-table-only library is not supported."
            ) from error

    def check(self, operation, status, handle):
        if status < 0:
            message = ct.create_string_buffer(512)
            self.lib.ktos_error(handle, message, len(message))
            raise SDKError(operation, status, message.value.decode("utf-8", errors="replace"))

    def open(self, index):
        handle = ct.c_void_p()
        status = self.lib.ktos_open(index, ct.byref(handle))
        self.check("open", status, None)
        try:
            message = ct.create_string_buffer(1024)
            self.check("identity", self.lib.ktos_identity(handle, message, len(message)), handle)
            if json.loads(message.value)["mode"] != "IQ":
                raise RuntimeError("Select Remote -> IQ mode on the KC908 before starting.")
        except BaseException:
            self.lib.ktos_close(handle)
            raise
        return handle

    def command(self, handle, opcode, payload=b""):
        buffer = ct.create_string_buffer(payload) if payload else None
        self.check(
            f"IQ command 0x{opcode:02x}",
            self.lib.ktos_iq_command(handle, opcode, buffer, len(payload)),
            handle,
        )

    def transfer(self, handle, data, transmit=False):
        actual = ct.c_uint32()
        function = self.lib.ktos_iq_write if transmit else self.lib.ktos_iq_read
        status = function(handle, data.ctypes.data, data.nbytes, ct.byref(actual))
        self.check("IQ write" if transmit else "IQ read", status, handle)
        if actual.value > data.nbytes or actual.value % 4:
            raise RuntimeError(f"SDK returned an invalid IQ byte count: {actual.value}")
        if transmit and actual.value != data.nbytes:
            raise RuntimeError(f"Short IQ write: {actual.value} of {data.nbytes} bytes")
        return actual.value // 4

    def close(self, handle):
        try:
            self.check("IQ stop", self.lib.ktos_iq_stop(handle), handle)
        finally:
            self.lib.ktos_close(handle)
