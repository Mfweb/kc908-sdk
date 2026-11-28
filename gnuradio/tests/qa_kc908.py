# Copyright (c) 2026 Mfweb
# SPDX-License-Identifier: MIT
"""QA for the KC908 GNU Radio source/sink blocks.

The emulator tests use the in-tree fake FT601 transport that backs
``host/sdk/tests/test_sdk.c``.  They are skipped unless ``KTOS_SDK_LIBRARY``
points at an emulator build, i.e. a ``libkcsdr.so`` with the fake
``libusb-1.0.so.0`` beside it.  No physical device is opened.
"""

import ctypes as ct
import errno
import os
import struct
import threading
import time
import unittest
from contextlib import contextmanager
from unittest import mock

import numpy as np
from gnuradio import blocks, gr

import kc908
from kc908 import _sdk
from kc908 import blocks as kc908_blocks


EMULATOR_LIBRARY = os.environ.get("KTOS_SDK_LIBRARY")


# ---------------------------------------------------------------------------
# Environment and flowgraph helpers
# ---------------------------------------------------------------------------


def _restore_env(name, value):
    if value is None:
        os.environ.pop(name, None)
    else:
        os.environ[name] = value


@contextmanager
def emulator_env(mode=1, failure=None):
    """Set the fake-transport controls and always restore the old values."""
    old_mode = os.environ.get("KTOS_TEST_MODE")
    old_failure = os.environ.get("KTOS_TEST_FAILURE")
    os.environ["KTOS_TEST_MODE"] = str(mode)
    if failure is None:
        os.environ.pop("KTOS_TEST_FAILURE", None)
    else:
        os.environ["KTOS_TEST_FAILURE"] = str(failure)
    try:
        yield
    finally:
        _restore_env("KTOS_TEST_MODE", old_mode)
        _restore_env("KTOS_TEST_FAILURE", old_failure)


def _run_flowgraph(tb):
    tb.start()
    try:
        tb.wait()
    finally:
        try:
            tb.stop()
        except Exception:
            pass
        try:
            tb.wait()
        except Exception:
            pass


# ---------------------------------------------------------------------------
# Fake SDK used to observe block behaviour without the C library
# ---------------------------------------------------------------------------


class FakeSDK:
    """Stand-in for ``kc908.blocks.SDK`` with deterministic transfer results."""

    instances = []

    def __init__(self, library_path=""):
        self.library_path = library_path
        self.handle = object()
        self.opens = []
        self.closes = []
        self.commands = []
        self.transfers = []
        self.open_error = None
        self.command_errors = {}
        self.transfer_error = None
        self.rx_count = None
        self.rx_values = None
        self.transfer_gate = None
        self.entered = threading.Event()
        FakeSDK.instances.append(self)

    def open(self, index):
        self.opens.append(index)
        if self.open_error is not None:
            raise self.open_error
        return self.handle

    def command(self, handle, opcode, payload=b""):
        self.commands.append((opcode, bytes(payload)))
        error = self.command_errors.get(opcode)
        if error is not None:
            raise error

    def transfer(self, handle, data, transmit=False):
        self.transfers.append((transmit, data.copy()))
        if self.transfer_gate is not None:
            self.entered.set()
            if not self.transfer_gate.wait(1.0):
                raise AssertionError("fake transfer gate timed out")
        if self.transfer_error is not None:
            raise self.transfer_error
        if transmit:
            return len(data)
        count = len(data) if self.rx_count is None else self.rx_count
        if self.rx_values is not None:
            data[:count] = self.rx_values[:count]
        return count

    def close(self, handle):
        self.closes.append(handle)


class ExplodingSDK:
    """Proves constructors and pre-start setters never load the SDK."""

    def __init__(self, *args, **kwargs):
        raise AssertionError("SDK was constructed too early")


class CountingSDK:
    """Wrapper around the real SDK that records lifecycle and TX/RX data."""

    instances = []

    def __init__(self, library_path=""):
        self.inner = _sdk.SDK(library_path)
        self.library_path = library_path
        self.opens = []
        self.closes = []
        self.commands = []
        self.transfers = []
        CountingSDK.instances.append(self)

    def open(self, index):
        self.opens.append(index)
        return self.inner.open(index)

    def command(self, handle, opcode, payload=b""):
        self.commands.append((opcode, bytes(payload)))
        return self.inner.command(handle, opcode, payload)

    def transfer(self, handle, data, transmit=False):
        # Copy before the real read overwrites data; for TX the data is final.
        self.transfers.append((transmit, data.copy()))
        return self.inner.transfer(handle, data, transmit)

    def close(self, handle):
        self.closes.append(handle)
        return self.inner.close(handle)


# ---------------------------------------------------------------------------
# Fake ctypes library for direct SDK binding tests
# ---------------------------------------------------------------------------


class FakeLib:
    def __init__(self, actual=0, status=0, stop_status=0):
        self.actual = actual if callable(actual) else (lambda _n: actual)
        self.status = status
        self.stop_status = stop_status
        self.calls = []

    def ktos_iq_read(self, handle, data, nbytes, actual_ptr):
        actual_ptr._obj.value = int(self.actual(nbytes))
        self.calls.append(("read", nbytes))
        return self.status

    def ktos_iq_write(self, handle, data, nbytes, actual_ptr):
        actual_ptr._obj.value = int(self.actual(nbytes))
        self.calls.append(("write", nbytes))
        return self.status

    def ktos_iq_command(self, handle, opcode, data, nbytes):
        payload = ct.string_at(data, nbytes) if data else b""
        self.calls.append(("command", opcode, payload, nbytes))
        return self.status

    def ktos_iq_stop(self, handle):
        self.calls.append(("stop",))
        return self.stop_status

    def ktos_close(self, handle):
        self.calls.append(("close",))
        return 0

    def ktos_error(self, handle, text, capacity):
        text.value = b"fake error"
        return self.status if self.status < 0 else 0


class TestBlockConstructionAndBounds(unittest.TestCase):
    def setUp(self):
        FakeSDK.instances.clear()

    def test_constructor_does_not_load_or_open(self):
        with mock.patch.object(kc908_blocks, "SDK", ExplodingSDK):
            src = kc908.source()
            snk = kc908.sink()
        self.assertEqual(FakeSDK.instances, [])
        self.assertIsNone(src._handle)
        self.assertIsNone(snk._handle)
        self.assertEqual(src.work([], [np.empty(4, dtype=np.complex64)]), -1)
        self.assertEqual(snk.work([np.empty(4, dtype=np.complex64)], []), -1)
        self.assertTrue(src.stop())
        self.assertTrue(snk.stop())

    def test_valid_bounds_construct_without_sdk(self):
        with mock.patch.object(kc908_blocks, "SDK", ExplodingSDK):
            src = kc908.source(
                device_index=0xFFFFFFFF,
                sample_rate=2_000_000,
                center_freq=0,
                port=1,
                attenuation=0,
                if_gain=0,
                amp=-1,
                buffer_samples=262144,
            )
            snk = kc908.sink(
                device_index=0xFFFFFFFF,
                sample_rate=40_000_000,
                center_freq=0x7FFFFFFFFFFFFFFF,
                port=2,
                attenuation=31,
                if_gain=255,
                buffer_samples=262144,
            )
            src.set_sample_rate(40_000_000)
            src.set_center_freq(0x7FFFFFFFFFFFFFFF)
            src.set_port(2)
            src.set_attenuation(31)
            src.set_if_gain(255)
            src.set_amp(1)
            snk.set_sample_rate(2_000_000)
            snk.set_center_freq(0)
            snk.set_port(1)
            snk.set_attenuation(0)
            snk.set_if_gain(0)
        self.assertEqual(FakeSDK.instances, [])
        self.assertTrue(src.stop())
        self.assertTrue(snk.stop())

    def test_invalid_constructor_parameters(self):
        cases = [
            ("source", {"device_index": -1}),
            ("source", {"device_index": 0x100000000}),
            ("source", {"sample_rate": 1_999_999}),
            ("source", {"sample_rate": 40_000_001}),
            ("source", {"sample_rate": 1.5}),
            ("source", {"center_freq": -1}),
            ("source", {"center_freq": 0x8000000000000000}),
            ("source", {"port": 0}),
            ("source", {"port": 3}),
            ("source", {"port": "2"}),
            ("source", {"attenuation": -1}),
            ("source", {"attenuation": 32}),
            ("source", {"if_gain": -1}),
            ("source", {"if_gain": 256}),
            ("source", {"amp": -2}),
            ("source", {"amp": 2}),
            ("source", {"buffer_samples": 0}),
            ("source", {"buffer_samples": 262145}),
            ("source", {"buffer_samples": 1.5}),
            ("sink", {"device_index": 0x100000000}),
            ("sink", {"sample_rate": 1_999_999}),
            ("sink", {"sample_rate": 40_000_001}),
            ("sink", {"center_freq": -1}),
            ("sink", {"center_freq": 0x8000000000000000}),
            ("sink", {"port": 0}),
            ("sink", {"port": 3}),
            ("sink", {"attenuation": 32}),
            ("sink", {"if_gain": 256}),
            ("sink", {"buffer_samples": 0}),
            ("sink", {"buffer_samples": 262145}),
        ]
        with mock.patch.object(kc908_blocks, "SDK", ExplodingSDK):
            for kind, kwargs in cases:
                with self.subTest(kind=kind, kwargs=kwargs):
                    with self.assertRaises(ValueError):
                        getattr(kc908, kind)(**kwargs)
        self.assertEqual(FakeSDK.instances, [])

    def test_invalid_setter_parameters(self):
        src = kc908.source()
        snk = kc908.sink()
        with mock.patch.object(kc908_blocks, "SDK", ExplodingSDK):
            for setter, value in (
                ("set_sample_rate", 1_999_999),
                ("set_sample_rate", 40_000_001),
                ("set_sample_rate", 1.5),
                ("set_center_freq", -1),
                ("set_center_freq", 0x8000000000000000),
                ("set_port", 0),
                ("set_port", 3),
                ("set_attenuation", 32),
                ("set_if_gain", 256),
                ("set_amp", -2),
                ("set_amp", 2),
            ):
                with self.subTest(setter=setter, value=value):
                    with self.assertRaises(ValueError):
                        getattr(src, setter)(value)
            for setter, value in (
                ("set_sample_rate", 1_999_999),
                ("set_center_freq", -1),
                ("set_port", 0),
                ("set_attenuation", 32),
                ("set_if_gain", 256),
            ):
                with self.subTest(sink_setter=setter, value=value):
                    with self.assertRaises(ValueError):
                        getattr(snk, setter)(value)
            with self.assertRaises(ValueError):
                snk.set_amp(0)
        self.assertEqual(FakeSDK.instances, [])
        self.assertTrue(src.stop())
        self.assertTrue(snk.stop())


class TestCommandsAndPayloads(unittest.TestCase):
    def setUp(self):
        FakeSDK.instances.clear()

    def test_source_start_commands_and_runtime_setters(self):
        with mock.patch.object(kc908_blocks, "SDK", FakeSDK):
            src = kc908.source(
                sample_rate=2_000_000,
                center_freq=100_000_000,
                port=2,
                attenuation=0,
                if_gain=0,
                amp=0,
            )
            self.assertEqual(FakeSDK.instances, [])
            self.assertTrue(src.start())
            self.assertTrue(src.start())  # idempotent; no second open
            fake = FakeSDK.instances[0]
            self.assertEqual(fake.opens, [0])
            self.assertEqual(
                [opcode for opcode, _ in fake.commands],
                [1, 2, 3, 4, 5, 8, 6],
            )
            payloads = dict(fake.commands)
            self.assertEqual(payloads[1], b"\x01")  # physical port 2 -> 1
            self.assertEqual(payloads[2], struct.pack("<Q", 100_000_000))
            self.assertEqual(payloads[3], b"\x00")
            self.assertEqual(payloads[4], b"\x00")
            self.assertEqual(payloads[5], struct.pack("<I", 2_000_000))
            self.assertEqual(payloads[8], b"\x00")
            self.assertEqual(payloads[6], b"")

            src.set_port(1)
            src.set_port(2)
            src.set_center_freq(0x0123456789ABCDEF)
            src.set_attenuation(31)
            src.set_if_gain(255)
            src.set_sample_rate(40_000_000)
            src.set_amp(-1)
            src.set_amp(1)
            src.set_amp(0)
            runtime = fake.commands[len(payloads):]
            self.assertEqual(
                [opcode for opcode, _ in runtime],
                [1, 1, 2, 3, 4, 5, 8, 8, 8],
            )
            self.assertEqual(runtime[0][1], b"\x00")  # physical port 1 -> 0
            self.assertEqual(runtime[1][1], b"\x01")  # physical port 2 -> 1
            self.assertEqual(
                runtime[2][1], struct.pack("<Q", 0x0123456789ABCDEF)
            )
            self.assertEqual(runtime[3][1], b"\x1f")
            self.assertEqual(runtime[4][1], b"\xff")
            self.assertEqual(runtime[5][1], struct.pack("<I", 40_000_000))
            self.assertEqual(runtime[6][1], b"\xff")  # signed amp -1
            self.assertEqual(runtime[7][1], b"\x01")
            self.assertEqual(runtime[8][1], b"\x00")

            self.assertTrue(src.stop())
            self.assertTrue(src.stop())  # idempotent; no second close
            self.assertEqual(len(fake.closes), 1)
            self.assertIsNone(src._handle)

    def test_sink_start_commands_and_runtime_setters(self):
        with mock.patch.object(kc908_blocks, "SDK", FakeSDK):
            snk = kc908.sink(
                sample_rate=2_000_000,
                center_freq=100_000_000,
                port=1,
                attenuation=31,
                if_gain=0,
            )
            self.assertEqual(FakeSDK.instances, [])
            self.assertTrue(snk.start())
            fake = FakeSDK.instances[0]
            self.assertEqual(fake.opens, [0])
            self.assertEqual(
                [opcode for opcode, _ in fake.commands],
                [0x81, 0x82, 0x83, 0x84, 0x85, 0x86],
            )
            payloads = dict(fake.commands)
            self.assertEqual(payloads[0x81], b"\x00")  # physical port 1 -> 0
            self.assertEqual(payloads[0x82], struct.pack("<Q", 100_000_000))
            self.assertEqual(payloads[0x83], b"\x1f")
            self.assertEqual(payloads[0x84], b"\x00")
            self.assertEqual(payloads[0x85], struct.pack("<I", 2_000_000))
            self.assertEqual(payloads[0x86], b"")

            snk.set_port(2)
            snk.set_port(1)
            snk.set_center_freq(0x0123456789ABCDEF)
            snk.set_sample_rate(40_000_000)
            snk.set_attenuation(0)
            snk.set_if_gain(255)
            with self.assertRaises(ValueError):
                snk.set_amp(0)
            runtime = fake.commands[len(payloads):]
            self.assertEqual(
                [opcode for opcode, _ in runtime],
                [0x81, 0x81, 0x82, 0x85, 0x83, 0x84],
            )
            self.assertEqual(runtime[0][1], b"\x01")  # physical port 2 -> 1
            self.assertEqual(runtime[1][1], b"\x00")  # physical port 1 -> 0
            self.assertEqual(
                runtime[2][1], struct.pack("<Q", 0x0123456789ABCDEF)
            )
            self.assertEqual(runtime[3][1], struct.pack("<I", 40_000_000))
            self.assertEqual(runtime[4][1], b"\x00")
            self.assertEqual(runtime[5][1], b"\xff")

            self.assertTrue(snk.stop())
            self.assertTrue(snk.stop())
            self.assertEqual(len(fake.closes), 1)
            self.assertIsNone(snk._handle)


class TestWorkConversions(unittest.TestCase):
    def setUp(self):
        FakeSDK.instances.clear()

    def _started_source(self, **kwargs):
        src = kc908.source(**kwargs)
        src.start()
        return src, FakeSDK.instances[-1]

    def _started_sink(self, **kwargs):
        snk = kc908.sink(**kwargs)
        snk.start()
        return snk, FakeSDK.instances[-1]

    def test_sink_exact_tx_bytes_and_saturation(self):
        samples = np.array(
            [
                complex(-1.0, 1.0),
                complex(np.nan, -np.inf),
                complex(np.inf, 0.5),
                complex(-0.5, -0.50001),
                complex(0.49999, -0.49999),
                complex(0.0, -0.0),
                complex(2.0, -2.0),
                complex(1.0, -1.0),
            ],
            dtype=np.complex64,
        )
        expected = np.array(
            [
                [-32768, 32767],
                [0, -32768],
                [32767, 16384],
                [-16384, -16384],
                [16384, -16384],
                [0, 0],
                [32767, -32768],
                [32767, -32768],
            ],
            dtype="<i2",
        )
        with mock.patch.object(kc908_blocks, "SDK", FakeSDK):
            snk = kc908.sink(buffer_samples=16)
            try:
                self.assertTrue(snk.start())
                fake = FakeSDK.instances[-1]
                count = snk.work([samples], [])
                self.assertEqual(count, len(samples))
                self.assertEqual(len(fake.transfers), 1)
                transmit, captured = fake.transfers[0]
                self.assertTrue(transmit)
                self.assertEqual(captured.dtype, np.dtype("<i2"))
                self.assertEqual(captured.shape, (len(samples), 2))
                np.testing.assert_array_equal(captured, expected)
                self.assertEqual(
                    captured[:3].tobytes(),
                    b"\x00\x80\xff\x7f"
                    b"\x00\x00\x00\x80"
                    b"\xff\x7f\x00\x40",
                )
            finally:
                snk.stop()
            self.assertEqual(len(fake.closes), 1)
            self.assertIsNone(snk._handle)
            self.assertTrue(snk.stop())
            self.assertEqual(len(fake.closes), 1)

    def test_source_partial_rx_count_is_not_dropped(self):
        values = np.array(
            [
                [100, -200],
                [-32768, 32767],
                [1, -1],
                [2, -2],
                [3, -3],
                [4, -4],
                [5, -5],
                [6, -6],
                [7, -7],
                [8, -8],
            ],
            dtype="<i2",
        )
        with mock.patch.object(kc908_blocks, "SDK", FakeSDK):
            src = kc908.source(buffer_samples=10)
            try:
                self.assertTrue(src.start())
                fake = FakeSDK.instances[-1]
                fake.rx_values = values
                fake.rx_count = 3
                sentinel = complex(123.0, -456.0)
                out = np.full(10, sentinel, dtype=np.complex64)
                count = src.work([], [out])
                self.assertEqual(count, 3)
                self.assertEqual(len(fake.transfers), 1)
                transmit, requested = fake.transfers[0]
                self.assertFalse(transmit)
                self.assertEqual(requested.shape, (10, 2))
                expected = values[:3].astype(np.float32) / np.float32(32768.0)
                np.testing.assert_array_equal(out[:3].real, expected[:, 0])
                np.testing.assert_array_equal(out[:3].imag, expected[:, 1])
                np.testing.assert_array_equal(out[3:], np.full(7, sentinel))
            finally:
                src.stop()
            self.assertEqual(len(fake.closes), 1)
            self.assertIsNone(src._handle)

    def test_source_transfer_error_cleans_up(self):
        with mock.patch.object(kc908_blocks, "SDK", FakeSDK):
            src, fake = self._started_source(buffer_samples=8)
            error = _sdk.SDKError("IQ read", -5, "fatal transfer")
            fake.transfer_error = error
            try:
                with self.assertRaises(_sdk.SDKError) as caught:
                    src.work([], [np.empty(8, dtype=np.complex64)])
                self.assertIs(caught.exception, error)
                self.assertEqual(caught.exception.code, -5)
                self.assertIsNone(src._handle)
                self.assertEqual(len(fake.closes), 1)
            finally:
                src.stop()
            self.assertEqual(len(fake.closes), 1)
            self.assertIsNone(src._handle)

    def test_sink_transfer_error_cleans_up(self):
        with mock.patch.object(kc908_blocks, "SDK", FakeSDK):
            snk, fake = self._started_sink(buffer_samples=8)
            error = _sdk.SDKError("IQ write", -5, "fatal transfer")
            fake.transfer_error = error
            try:
                with self.assertRaises(_sdk.SDKError) as caught:
                    snk.work([np.zeros(8, dtype=np.complex64)], [])
                self.assertIs(caught.exception, error)
                self.assertIsNone(snk._handle)
                self.assertEqual(len(fake.closes), 1)
            finally:
                snk.stop()
            self.assertEqual(len(fake.closes), 1)
            self.assertIsNone(snk._handle)

    def test_source_start_error_cleans_up(self):
        fake = FakeSDK()
        error = _sdk.SDKError("IQ command 0x06", -6, "start failed")
        fake.command_errors[6] = error
        with mock.patch.object(kc908_blocks, "SDK", lambda *a, **k: fake):
            src = kc908.source()
            try:
                with self.assertRaises(_sdk.SDKError) as caught:
                    src.start()
                self.assertIs(caught.exception, error)
                self.assertIsNone(src._handle)
                self.assertEqual(len(fake.closes), 1)
            finally:
                src.stop()
            self.assertEqual(len(fake.closes), 1)
            self.assertIsNone(src._handle)

    def test_sink_start_error_cleans_up(self):
        fake = FakeSDK()
        error = _sdk.SDKError("IQ command 0x86", -6, "start failed")
        fake.command_errors[0x86] = error
        with mock.patch.object(kc908_blocks, "SDK", lambda *a, **k: fake):
            snk = kc908.sink()
            try:
                with self.assertRaises(_sdk.SDKError) as caught:
                    snk.start()
                self.assertIs(caught.exception, error)
                self.assertIsNone(snk._handle)
                self.assertEqual(len(fake.closes), 1)
            finally:
                snk.stop()
            self.assertEqual(len(fake.closes), 1)
            self.assertIsNone(snk._handle)

    def test_source_setter_error_cleans_up(self):
        with mock.patch.object(kc908_blocks, "SDK", FakeSDK):
            src, fake = self._started_source()
            error = _sdk.SDKError("IQ command 0x05", -7, "setter failed")
            fake.command_errors[5] = error
            try:
                with self.assertRaises(_sdk.SDKError) as caught:
                    src.set_sample_rate(3_000_000)
                self.assertIs(caught.exception, error)
                self.assertIsNone(src._handle)
                self.assertEqual(len(fake.closes), 1)
            finally:
                src.stop()
            self.assertEqual(len(fake.closes), 1)
            self.assertIsNone(src._handle)

    def test_sink_setter_error_cleans_up(self):
        with mock.patch.object(kc908_blocks, "SDK", FakeSDK):
            snk, fake = self._started_sink()
            error = _sdk.SDKError("IQ command 0x85", -7, "setter failed")
            fake.command_errors[0x85] = error
            try:
                with self.assertRaises(_sdk.SDKError) as caught:
                    snk.set_sample_rate(3_000_000)
                self.assertIs(caught.exception, error)
                self.assertIsNone(snk._handle)
                self.assertEqual(len(fake.closes), 1)
            finally:
                snk.stop()
            self.assertEqual(len(fake.closes), 1)
            self.assertIsNone(snk._handle)

    def test_concurrent_work_and_stop_is_bounded(self):
        with mock.patch.object(kc908_blocks, "SDK", FakeSDK):
            src, fake = self._started_source(buffer_samples=8)
            fake.rx_values = np.arange(16, dtype="<i2").reshape(8, 2)
            fake.rx_count = 8
            fake.transfer_gate = threading.Event()
            fake.entered.clear()
            results = []

            def do_work():
                out = np.empty(8, dtype=np.complex64)
                results.append(src.work([], [out]))

            worker = threading.Thread(target=do_work)
            stopper = threading.Thread(target=src.stop)
            try:
                worker.start()
                self.assertTrue(fake.entered.wait(1.0))
                stopper.start()
                time.sleep(0.02)
                fake.transfer_gate.set()
                worker.join(2.0)
                stopper.join(2.0)
                self.assertFalse(worker.is_alive())
                self.assertFalse(stopper.is_alive())
                self.assertEqual(results, [8])
                self.assertEqual(len(fake.closes), 1)
                self.assertIsNone(src._handle)
            finally:
                fake.transfer_gate.set()
                if worker.is_alive():
                    worker.join(2.0)
                if stopper.is_alive():
                    stopper.join(2.0)
                src.stop()
            self.assertEqual(len(fake.closes), 1)

            stoppers = [threading.Thread(target=src.stop) for _ in range(2)]
            for thread in stoppers:
                thread.start()
            for thread in stoppers:
                thread.join(2.0)
                self.assertFalse(thread.is_alive())
            self.assertEqual(len(fake.closes), 1)


class TestSDKBinding(unittest.TestCase):
    def _sdk_with_lib(self, lib):
        sdk = object.__new__(_sdk.SDK)
        sdk.lib = lib
        return sdk

    def test_transfer_short_write_and_invalid_counts(self):
        data = np.zeros((4, 2), dtype="<i2")
        sdk = self._sdk_with_lib(FakeLib(actual=lambda n: n - 4))
        with self.assertRaisesRegex(RuntimeError, "Short IQ write: 12 of 16"):
            sdk.transfer(object(), data, transmit=True)

        sdk = self._sdk_with_lib(FakeLib(actual=lambda n: n + 4))
        with self.assertRaisesRegex(RuntimeError, "invalid IQ byte count: 20"):
            sdk.transfer(object(), data, transmit=True)

        sdk = self._sdk_with_lib(FakeLib(actual=lambda n: n - 1))
        with self.assertRaisesRegex(RuntimeError, "invalid IQ byte count: 15"):
            sdk.transfer(object(), data, transmit=True)

        sdk = self._sdk_with_lib(FakeLib(actual=lambda n: n + 4))
        with self.assertRaisesRegex(RuntimeError, "invalid IQ byte count: 20"):
            sdk.transfer(object(), data)

        sdk = self._sdk_with_lib(FakeLib(actual=lambda n: n))
        self.assertEqual(sdk.transfer(object(), data, transmit=True), 4)
        self.assertEqual(sdk.transfer(object(), data), 4)

    def test_transfer_partial_read_count(self):
        data = np.zeros((4, 2), dtype="<i2")
        sdk = self._sdk_with_lib(FakeLib(actual=8))
        self.assertEqual(sdk.transfer(object(), data), 2)

    def test_transfer_error_raises_sdk_error(self):
        data = np.zeros((4, 2), dtype="<i2")
        sdk = self._sdk_with_lib(FakeLib(actual=16, status=-5))
        with self.assertRaises(_sdk.SDKError) as caught:
            sdk.transfer(object(), data)
        self.assertEqual(caught.exception.code, -5)
        self.assertIn("fake error", str(caught.exception))

    def test_command_passes_opcode_and_payload(self):
        sdk = self._sdk_with_lib(FakeLib())
        payload = struct.pack("<Q", 0x0123456789ABCDEF)
        sdk.command(object(), 2, payload)
        sdk.command(object(), 6)
        self.assertEqual(
            sdk.lib.calls,
            [
                ("command", 2, payload, len(payload)),
                ("command", 6, b"", 0),
            ],
        )

    def test_close_calls_stop_then_close_once(self):
        sdk = self._sdk_with_lib(FakeLib(stop_status=-5))
        with self.assertRaises(_sdk.SDKError) as caught:
            sdk.close(object())
        self.assertEqual(caught.exception.code, -5)
        self.assertEqual(sdk.lib.calls, [("stop",), ("close",)])

        sdk = self._sdk_with_lib(FakeLib())
        sdk.close(object())
        self.assertEqual(sdk.lib.calls, [("stop",), ("close",)])


@unittest.skipUnless(
    EMULATOR_LIBRARY,
    "KTOS_SDK_LIBRARY is not set; set it to the in-tree fake FT601 build",
)
class TestEmulatorIntegration(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        library = os.path.abspath(EMULATOR_LIBRARY)
        if not os.path.isfile(library):
            raise unittest.SkipTest(
                "KTOS_SDK_LIBRARY does not exist: %s" % library
            )
        parent = os.path.dirname(library)
        fake_transport = os.path.join(parent, "libusb-1.0.so.0")
        if not os.path.isfile(fake_transport):
            fake_transport = os.path.join(parent, "FTD3XX.dll")
        if not os.path.isfile(fake_transport):
            raise unittest.SkipTest(
                "KTOS_SDK_LIBRARY is not beside an in-tree fake FT601 "
                "transport; refusing to touch hardware (%s)" % parent
            )
        cls.library = library

    def setUp(self):
        CountingSDK.instances.clear()

    def _assert_rx_samples(self, data, expected_count):
        data = np.asarray(data, dtype=np.complex64)
        self.assertEqual(len(data), expected_count)
        self.assertGreater(len(data) * 4, 1 << 20)  # raw int16 I/Q > 1 MiB
        # Recover the int16 I/Q values from the exact float32 output.  The
        # emulator writes one little-endian uint32 per complex sample.
        parts = np.empty((len(data), 2), dtype=np.int16)
        parts[:, 0] = np.rint(data.real * np.float32(32768.0))
        parts[:, 1] = np.rint(data.imag * np.float32(32768.0))
        i = parts[:, 0].astype(np.uint16).astype(np.uint32)
        q = parts[:, 1].astype(np.uint16).astype(np.uint32)
        words = i | (q << np.uint32(16))
        # The emulator writes one monotonic uint32 per complex sample; the
        # block must preserve the little-endian I/Q halves.
        np.testing.assert_array_equal(
            words[1:], words[:-1] + np.uint32(1)
        )
        expected_i = parts[:, 0].astype(np.float32) / np.float32(32768.0)
        expected_q = parts[:, 1].astype(np.float32) / np.float32(32768.0)
        np.testing.assert_array_equal(data.real, expected_i)
        np.testing.assert_array_equal(data.imag, expected_q)

    def test_library_is_loaded_from_environment(self):
        sdk = _sdk.SDK()
        self.assertEqual(
            os.path.realpath(sdk.lib._name),
            os.path.realpath(self.library),
        )
        self.assertIsNone(getattr(sdk, "_handle", None))

    def _source_flowgraph(self, mode):
        count = 300_000
        with emulator_env(mode=mode):
            with mock.patch.object(kc908_blocks, "SDK", CountingSDK):
                tb = gr.top_block()
                src = kc908.source(buffer_samples=16384)
                head = blocks.head(gr.sizeof_gr_complex, count)
                vsink = blocks.vector_sink_c()
                tb.connect(src, head, vsink)
                wrapper = None
                try:
                    _run_flowgraph(tb)
                    data = np.asarray(vsink.data(), dtype=np.complex64)
                    self._assert_rx_samples(data, count)
                    wrapper = CountingSDK.instances[-1]
                    self.assertEqual(wrapper.opens, [0])
                    self.assertEqual(len(wrapper.closes), 1)
                    self.assertIsNone(src._handle)
                finally:
                    src.stop()
                if wrapper is not None:
                    self.assertEqual(len(wrapper.closes), 1)
                    src.stop()
                    self.assertEqual(len(wrapper.closes), 1)

    def test_source_flowgraph_original_firmware(self):
        self._source_flowgraph(0)

    def test_source_flowgraph_ktos_iq(self):
        self._source_flowgraph(1)

    def test_sink_flowgraph_exact_tx_bytes(self):
        samples = np.array(
            [
                complex(-1.0, 1.0),
                complex(np.nan, -np.inf),
                complex(np.inf, 0.5),
                complex(-0.5, -0.50001),
                complex(0.49999, -0.49999),
                complex(0.0, -0.0),
                complex(2.0, -2.0),
                complex(1.0, -1.0),
            ],
            dtype=np.complex64,
        )
        expected = np.array(
            [
                [-32768, 32767],
                [0, -32768],
                [32767, 16384],
                [-16384, -16384],
                [16384, -16384],
                [0, 0],
                [32767, -32768],
                [32767, -32768],
            ],
            dtype="<i2",
        )
        with emulator_env(mode=1):
            with mock.patch.object(kc908_blocks, "SDK", CountingSDK):
                tb = gr.top_block()
                vsrc = blocks.vector_source_c(samples.tolist(), False)
                snk = kc908.sink(buffer_samples=32)
                tb.connect(vsrc, snk)
                wrapper = None
                try:
                    _run_flowgraph(tb)
                    wrapper = CountingSDK.instances[-1]
                    chunks = [
                        chunk
                        for transmit, chunk in wrapper.transfers
                        if transmit
                    ]
                    captured = np.concatenate(chunks, axis=0)
                    self.assertEqual(captured.dtype, np.dtype("<i2"))
                    self.assertEqual(
                        captured.shape, (len(samples), 2)
                    )
                    np.testing.assert_array_equal(captured, expected)
                    self.assertEqual(
                        captured[:3].tobytes(),
                        b"\x00\x80\xff\x7f"
                        b"\x00\x00\x00\x80"
                        b"\xff\x7f\x00\x40",
                    )
                    self.assertEqual(len(wrapper.opens), 1)
                    self.assertEqual(len(wrapper.closes), 1)
                    self.assertIsNone(snk._handle)
                finally:
                    snk.stop()
                if wrapper is not None:
                    self.assertEqual(len(wrapper.closes), 1)
                    snk.stop()
                    self.assertEqual(len(wrapper.closes), 1)

    def test_restart_and_stop_are_bounded(self):
        with emulator_env(mode=1):
            with mock.patch.object(kc908_blocks, "SDK", CountingSDK):
                src = kc908.source(buffer_samples=1024)
                try:
                    for _ in range(3):
                        self.assertTrue(src.start())
                        out = np.empty(1024, dtype=np.complex64)
                        self.assertEqual(src.work([], [out]), 1024)
                        self.assertTrue(np.isfinite(out).all())
                        self.assertTrue(src.stop())
                        self.assertIsNone(src._handle)
                    self.assertEqual(len(CountingSDK.instances), 3)
                    for wrapper in CountingSDK.instances:
                        self.assertEqual(len(wrapper.opens), 1)
                        self.assertEqual(len(wrapper.closes), 1)
                finally:
                    src.stop()
                self.assertEqual(
                    sum(len(w.closes) for w in CountingSDK.instances), 3
                )
                src.stop()
                self.assertEqual(
                    sum(len(w.closes) for w in CountingSDK.instances), 3
                )

    def test_mcp_mode_rejected_and_released(self):
        with emulator_env(mode=2):
            with mock.patch.object(kc908_blocks, "SDK", CountingSDK):
                src = kc908.source()
                try:
                    with self.assertRaises(RuntimeError) as caught:
                        src.start()
                    self.assertIn("Select Remote -> IQ mode", str(caught.exception))
                    self.assertIsNone(src._handle)
                    wrapper = CountingSDK.instances[-1]
                    self.assertEqual(wrapper.opens, [0])
                    self.assertEqual(wrapper.closes, [])
                finally:
                    src.stop()
        # A subsequent IQ-mode open proves the failed MCP open released the
        # fake FT601 handle and both interfaces (the emulator exits if not).
        with emulator_env(mode=1):
            with mock.patch.object(kc908_blocks, "SDK", CountingSDK):
                src = kc908.source(buffer_samples=16)
                try:
                    self.assertTrue(src.start())
                    self.assertEqual(
                        src.work([], [np.empty(16, dtype=np.complex64)]), 16
                    )
                finally:
                    src.stop()
                self.assertEqual(len(CountingSDK.instances[-1].closes), 1)

    def test_open_failures_surface(self):
        for failure, expected_text in ((1, "enumerated"), (2, "permission denied")):
            with self.subTest(failure=failure):
                with emulator_env(mode=1, failure=failure):
                    with mock.patch.object(kc908_blocks, "SDK", CountingSDK):
                        src = kc908.source()
                        try:
                            with self.assertRaises(_sdk.SDKError) as caught:
                                src.start()
                            self.assertLess(caught.exception.code, 0)
                            self.assertIn(expected_text, str(caught.exception))
                            self.assertIsNone(src._handle)
                            wrapper = CountingSDK.instances[-1]
                            self.assertEqual(wrapper.opens, [0])
                            self.assertEqual(wrapper.closes, [])
                        finally:
                            src.stop()
        with emulator_env(mode=1):
            with mock.patch.object(kc908_blocks, "SDK", CountingSDK):
                src = kc908.source(buffer_samples=16)
                try:
                    self.assertTrue(src.start())
                    self.assertEqual(
                        src.work([], [np.empty(16, dtype=np.complex64)]), 16
                    )
                finally:
                    src.stop()

    def test_empty_iq_transfer_surfaces_protocol_error(self):
        with emulator_env(mode=1, failure=3):
            with mock.patch.object(kc908_blocks, "SDK", CountingSDK):
                src = kc908.source(buffer_samples=256)
                wrapper = None
                try:
                    self.assertTrue(src.start())
                    with self.assertRaises(_sdk.SDKError) as caught:
                        src.work([], [np.empty(256, dtype=np.complex64)])
                    self.assertEqual(caught.exception.code, -errno.EPROTO)
                    self.assertIn("length 0", str(caught.exception))
                    self.assertIsNone(src._handle)
                    wrapper = CountingSDK.instances[-1]
                    self.assertEqual(len(wrapper.closes), 1)
                finally:
                    src.stop()
                if wrapper is not None:
                    self.assertEqual(len(wrapper.closes), 1)
        # Recovery after the empty-IQ failure, with the failure env cleared.
        with emulator_env(mode=1):
            with mock.patch.object(kc908_blocks, "SDK", CountingSDK):
                src = kc908.source(buffer_samples=16)
                try:
                    self.assertTrue(src.start())
                    self.assertEqual(
                        src.work([], [np.empty(16, dtype=np.complex64)]), 16
                    )
                finally:
                    src.stop()
                self.assertEqual(len(CountingSDK.instances[-1].closes), 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
