# KC908 GNU Radio support

Python source/sink blocks and GNU Radio Companion (GRC) definitions for **GNU Radio
3.10**, using this SDK's public C ABI through `ctypes`. Windows uses the SDK's D3XX
transport; Linux uses libusb. No C++ bindings or proprietary Linux driver are needed.

## Build and install

Run from `host/sdk`. Install GNU Radio in the Python environment that will run the
flowgraph. On Ubuntu 24.04, the additional runtime package is `gnuradio`.

```sh
cmake -S . -B ../../agentsCache/gnuradio/build \
    -DBUILD_TESTING=ON \
    -DPython3_EXECUTABLE=/usr/bin/python3
cmake --build ../../agentsCache/gnuradio/build -j4
ctest --test-dir ../../agentsCache/gnuradio/build --output-on-failure
sudo cmake --install ../../agentsCache/gnuradio/build
```

GNU Radio support is **on by default** and requires GNU Radio and NumPy in the
selected Python environment. Pass `-DKTOS_ENABLE_GNURADIO=OFF` for a C SDK-only
build without these dependencies. Installation puts the `kc908` package and the
real SDK library together under the CMake installation prefix, and adds the GRC
blocks to the installation prefix's `share/gnuradio/grc/blocks` directory. Test
transport libraries are never installed.

For a custom installation, set `CMAKE_INSTALL_PREFIX` during **configuration**.
`KTOS_GNURADIO_PYTHON_DIR` and `KTOS_GNURADIO_GRC_DIR` override the destinations.
Use the Python interpreter from your GNU Radio installation, including on Windows.
On Windows, set `KTOS_GNURADIO_PYTHON_DIR` to that interpreter's
`sysconfig.get_path('purelib')` and `KTOS_GNURADIO_GRC_DIR` to GNU Radio's
`share/gnuradio/grc/blocks` directory, or add the chosen destinations to the paths below.
Restart GRC after installation; the blocks appear under **KC908**. For nonstandard
destinations, add the Python directory to `PYTHONPATH` and the GRC directory to
`GRC_BLOCKS_PATH`.

For use without installation:

```sh
export PYTHONPATH="$PWD/gnuradio/python${PYTHONPATH:+:$PYTHONPATH}"
export KTOS_SDK_LIBRARY="$PWD/../../agentsCache/gnuradio/build/libkcsdr.so"
python3 gnuradio/examples/rx_to_file.py --help
```

Use the real library in the build root for hardware, **not** the copy in `tests/`,
which is next to the FT601 emulator. On Windows, point `KTOS_SDK_LIBRARY` to
`kcsdr.dll` and place the matching official `FTD3XX.dll` beside it. Linux users
still need the SDK's [udev rule](../resources/99-kc908-ft601.rules).

## Flowgraphs and parameters

```python
from gnuradio import blocks, gr
import kc908

tb = gr.top_block()
rx = kc908.source(sample_rate=2_000_000, center_freq=100_000_000, port=2)
head = blocks.head(gr.sizeof_gr_complex, 100_000)
samples = blocks.vector_sink_c()
tb.connect(rx, head, samples)
tb.run()
```

Select **Remote -> IQ** on the device and enable USB3 before running. Original
firmware and KTOS IQ mode are supported. MCP mode produces an explicit error.
Each source or sink opens one exclusive device in `start()` and stops/closes it
in `stop()`. A source and sink cannot share the same KC908. Constructing or
editing blocks in GRC does not open the device or start RF output.

| Parameter | Meaning |
| --- | --- |
| `device_index` | SDK enumeration index; default 0 |
| `sample_rate` | Integer IQ clock in Hz, 2,000,000–40,000,000; default 2,000,000 |
| `center_freq` | Integer Hz; default 100,000,000; model/port limits are enforced by firmware |
| `port` | Physical connector **1 or 2**, converted to protocol values 0 or 1; RX default 2, TX default 1 |
| `attenuation` | RF attenuation in dB, 0–31; RX default 0, TX default 31 |
| `if_gain` | Raw protocol IF gain byte, 0–255; default 0; device-specific limits apply |
| `amp` | RX only: **-1 / 0 / 1** selects **0 / 20 / 40 dB**; default 0 |
| `library_path` | Explicit SDK library filename; otherwise `KTOS_SDK_LIBRARY`, bundled library, then system lookup |
| `buffer_samples` | Maximum samples processed per work call, 1–262144; default 16384 |

Runtime setters are `set_sample_rate`, `set_center_freq`, `set_port`,
`set_attenuation`, `set_if_gain`, and RX-only `set_amp`. Changes are serialized
with I/O; downstream rate-dependent blocks must be updated by the flowgraph.
Changing the clock does not resample or tag the stream. Firmware determines the
actual clock precision; this adapter has no clock readback or RF calibration.

RX outputs `numpy.complex64`: little-endian signed int16 **I then Q**, each divided
by 32768. TX consumes the same complex type, multiplies each component by 32768,
rounds to nearest even, and saturates to [-32768, 32767]. NaN becomes zero and
infinities saturate. TX attenuation defaults to 31 dB; starting a sink starts RF
output. Use a suitable frequency, load and attenuation for your setup.

Hardware provides the flowgraph clock; a Throttle block is unnecessary. SDK
failures raise `SDKError` with the original status and diagnostic. Transfer,
configuration and startup failures close the connection. Close is serialized
with in-flight SDK calls, so stopping may wait for the SDK's bounded I/O timeout.
The adapter does not add a background queue or claim lossless capture at 40 MS/s.

The receive-only `examples/rx_to_file.py` records a finite number of complex64
samples, suitable for GNU Radio's complex File Source. Use `--help` for options.

## Validation

With `BUILD_TESTING=ON`, CTest runs the native SDK checks and Python GNU Radio QA
using the test-only FT601 emulator. Tests exercise original/KTOS IQ negotiation,
stream conversion, finite flowgraphs, restart, rejection, cleanup, and GRC code
generation with variable callbacks. No physical
device is opened by these tests. Emulator results do not establish physical USB
throughput, RF performance, or full-duplex operation.

The block model follows GNU Radio's [Python OOT workflow](https://wiki.gnuradio.org/index.php/Creating_Python_OOT_with_gr-modtool).

## 中文使用说明

这是基于 `host/sdk` C ABI 的 GNU Radio **3.10** Python 收发模块，可在 Python
中 `import kc908`，也可在 GRC 的 **KC908** 分类中添加 IQ Source / IQ Sink。
默认构建已启用 GNU Radio 支持；选择与 GNU Radio 相同的 Python 解释器，再编译、
测试和安装。只构建 C SDK 时使用 `-DKTOS_ENABLE_GNURADIO=OFF`，无需这些 Python 依赖。

设备应进入“远程 → IQ”并开启 USB3；支持原版固件和 KTOS IQ 模式，MCP 模式会报错。
RX 输出复数 float32，I、Q 按 int16 满量程 32768 归一化。TX 做对应的缩放、舍入和
饱和转换。端口参数填物理编号 **1 / 2**；RX 放大参数填 **-1 / 0 / 1**，分别表示
**0 / 20 / 40 dB**。采样率为整数 Hz，范围 **2–40 MS/s**，实际频率、增益能力由
设备型号和端口决定。改变采样率时，需同时更新下游模块，不会自动重采样。

模块在流图启动时独占打开设备，在停止或出错时关闭；一个 KC908 不能同时分配给
两个收发块。TX 块启动后会开启输出，默认衰减 31 dB。示例只做有限长度接收。
Windows 需要匹配位数的官方 `FTD3XX.dll`；Linux 使用 libusb 和 SDK 的 udev 规则。

离线测试使用 FT601 模拟器，不接触实物。模拟器、测试目录中的库不能部署到实际
运行环境；实物吞吐、射频性能及全双工能力需另行验证。
