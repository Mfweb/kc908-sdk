# Copyright (c) 2026 Mfweb
# SPDX-License-Identifier: MIT
"""GNU Radio 3.10 complex64 source and sink for KC908 USB3 IQ mode."""

import struct
import threading

import numpy as np
from gnuradio import gr

from ._sdk import SDK


def _integer(name, value, low, high):
    try:
        number = int(value)
    except (TypeError, ValueError, OverflowError) as error:
        raise ValueError(f"{name} must be an integer in [{low}, {high}]") from error
    if number != value or not low <= number <= high:
        raise ValueError(f"{name} must be an integer in [{low}, {high}]")
    return number


class _DeviceBlock(gr.sync_block):
    def __init__(
        self,
        transmit,
        device_index,
        sample_rate,
        center_freq,
        port,
        attenuation,
        if_gain,
        amp,
        library_path,
        buffer_samples,
    ):
        gr.sync_block.__init__(
            self,
            name="KC908 IQ Sink" if transmit else "KC908 IQ Source",
            in_sig=[np.complex64] if transmit else None,
            out_sig=None if transmit else [np.complex64],
        )
        self._lock = threading.RLock()
        self._transmit = transmit
        self._index = _integer("device_index", device_index, 0, 0xFFFFFFFF)
        self._library_path = library_path
        self._sdk = None
        self._handle = None
        self._settings = {}
        size = _integer("buffer_samples", buffer_samples, 1, 262144)
        self._raw = np.empty((size, 2), dtype="<i2")
        self._scaled = np.empty((size, 2), dtype=np.float32) if transmit else None
        self.set_sample_rate(sample_rate)
        self.set_center_freq(center_freq)
        self.set_port(port)
        self.set_attenuation(attenuation)
        self.set_if_gain(if_gain)
        if not transmit:
            self.set_amp(amp)

    def _set(self, name, value, low, high, opcode, fmt):
        value = _integer(name, value, low, high)
        with self._lock:
            if self._handle is not None:
                try:
                    self._sdk.command(self._handle, opcode, struct.pack(fmt, value))
                except BaseException:
                    self._close_after_error()
                    raise
            self._settings[opcode] = struct.pack(fmt, value)

    def set_sample_rate(self, sample_rate):
        """Set the device IQ clock in Hz (2 MS/s through 40 MS/s)."""
        self._set("sample_rate", sample_rate, 2000000, 40000000, self._op(5), "<I")

    def set_center_freq(self, center_freq):
        """Tune in Hz; the device enforces its model/port frequency limits."""
        self._set("center_freq", center_freq, 0, 0x7FFFFFFFFFFFFFFF, self._op(2), "<Q")

    def set_port(self, port):
        """Select physical PORT1 or PORT2 (wire values 0 and 1)."""
        port = _integer("port", port, 1, 2)
        self._set("port", port - 1, 0, 1, self._op(1), "B")

    def set_attenuation(self, attenuation):
        """Set RF attenuation in dB; device limits depend on the model/port."""
        self._set("attenuation", attenuation, 0, 31, self._op(3), "B")

    def set_if_gain(self, if_gain):
        """Set the protocol IF gain byte; valid range depends on the device."""
        self._set("if_gain", if_gain, 0, 255, self._op(4), "B")

    def set_amp(self, amp):
        """Set RX amplifier mode: -1 / 0 / 1 for 0 / 20 / 40 dB."""
        if self._transmit:
            raise ValueError("RX amplifier mode is unavailable on the sink")
        self._set("amp", amp, -1, 1, 8, "b")

    def _op(self, opcode):
        return opcode + (0x80 if self._transmit else 0)

    def start(self):
        with self._lock:
            if self._handle is not None:
                return True
            self._sdk = SDK(self._library_path)
            self._handle = self._sdk.open(self._index)
            try:
                for opcode in sorted(self._settings):
                    self._sdk.command(self._handle, opcode, self._settings[opcode])
                self._sdk.command(self._handle, self._op(6))
            except BaseException:
                self._close_after_error()
                raise
        return True

    def _close(self):
        handle, self._handle = self._handle, None
        if handle is not None:
            self._sdk.close(handle)

    def _close_after_error(self):
        try:
            self._close()
        except Exception:
            pass  # Preserve the operation error; SDK close still releases the handle.

    def stop(self):
        # Serialize close with ctypes I/O and live setters: C handles cannot be
        # freed while an SDK request is running. I/O uses the SDK's bounded timeout.
        with self._lock:
            self._close()
        return True


class source(_DeviceBlock):
    """Receive signed little-endian I/Q as complex64, scaled by 1/32768.

    Construction does not open hardware. GNU Radio start/stop owns the exclusive
    SDK connection. Each block requires a separate device; a source and sink
    cannot share one KC908.
    """

    def __init__(
        self,
        device_index=0,
        sample_rate=2000000,
        center_freq=100000000,
        port=2,
        attenuation=0,
        if_gain=0,
        amp=0,
        library_path="",
        buffer_samples=16384,
    ):
        super().__init__(
            False,
            device_index,
            sample_rate,
            center_freq,
            port,
            attenuation,
            if_gain,
            amp,
            library_path,
            buffer_samples,
        )

    def work(self, input_items, output_items):
        with self._lock:
            if self._handle is None:
                return -1
            raw = self._raw[: min(len(output_items[0]), len(self._raw))]
            if not len(raw):
                return 0
            try:
                count = self._sdk.transfer(self._handle, raw)
                output = output_items[0][:count]
                np.multiply(raw[:count, 0], 1.0 / 32768, out=output.real)
                np.multiply(raw[:count, 1], 1.0 / 32768, out=output.imag)
                return count
            except BaseException:
                self._close_after_error()
                raise


class sink(_DeviceBlock):
    """Transmit complex64 as interleaved int16 I/Q, saturating at full scale.

    Real and imaginary components are multiplied by 32768, rounded and clipped
    to [-32768, 32767]. NaN becomes zero; infinities saturate. RF output begins
    only when the flowgraph starts, and stops when it stops or I/O fails.
    """

    def __init__(
        self,
        device_index=0,
        sample_rate=2000000,
        center_freq=100000000,
        port=1,
        attenuation=31,
        if_gain=0,
        library_path="",
        buffer_samples=16384,
    ):
        super().__init__(
            True,
            device_index,
            sample_rate,
            center_freq,
            port,
            attenuation,
            if_gain,
            0,
            library_path,
            buffer_samples,
        )

    def work(self, input_items, output_items):
        with self._lock:
            if self._handle is None:
                return -1
            count = min(len(input_items[0]), len(self._raw))
            if not count:
                return 0
            raw, scaled = self._raw[:count], self._scaled[:count]
            scaled[:, 0] = input_items[0][:count].real
            scaled[:, 1] = input_items[0][:count].imag
            np.nan_to_num(scaled, copy=False, nan=0.0, posinf=1.0, neginf=-1.0)
            np.clip(scaled, -1.0, 1.0, out=scaled)
            np.multiply(scaled, 32768.0, out=scaled)
            np.rint(scaled, out=scaled)
            np.clip(scaled, -32768, 32767, out=scaled)
            raw[:] = scaled
            try:
                return self._sdk.transfer(self._handle, raw, transmit=True)
            except BaseException:
                self._close_after_error()
                raise
