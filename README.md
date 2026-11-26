# KTOS Host SDK (`kcsdr.dll` / `libkcsdr.so`)

Cross-platform C shared library (Windows and Linux) for communicating with the KC908 over USB 3.0 (FTDI FT601). It supports both the original KC908 USB3 IQ protocol and the KTOS custom firmware protocols (sequenced/CRC IQ streaming, JSON control RPC, and binary SD image/app transfers), while preserving binary compatibility with the Windows vendor `kcsdr.dll` 1.3.4 function table.

[简体中文](README.zh-CN.md)

## Features

- **Automatic protocol negotiation**: Detects original KC908 firmware versus KTOS firmware (IQ mode or MCP mode) on `ktos_open()`.
- **Pre-armed asynchronous control I/O**: Arms overlapped IN transfers before control OUT writes to meet the FPGA's short TX-ready window.
- **Keepalive heartbeat**: Sends 250 ms keepalives (one-way `0xD1` when supported) so the device safely stops remote output if the host disconnects.
- **High-throughput IQ streaming**: Uses four 1 MiB overlapped RX slots on FT601 while letting callers consume arbitrary 4-byte-aligned blocks.
- **Control RPC & binary transfer**: Fragmented, CRC-32-checked UTF-8 JSON RPC (`ktos_rpc`), typed VSA, software GPS L1, and DRM30/DRM+ helpers (`ktos_vsa_*`, `ktos_gps_*`, `ktos_drm_*`), and SHA-256-verified SD image upload/download (`ktos_image_upload`, `ktos_image_download`, `ktos_image_request`).
- **Legacy ABI compatibility**: Exports `kcsdr_init()` with the exact 26-slot Windows 1.3.4 function table for existing desktop SDR software.

## Repository Layout

```text
.
├── CMakeLists.txt           # Standalone CMake build & CTest definitions
├── LICENSE                  # MIT License
├── include/
│   ├── ktos_sdk.h           # Public C ABI header
│   └── image_protocol.h     # Binary SD image / KCApp packet definitions
├── resources/
│   ├── version.rc.in          # Windows VERSIONINFO template
│   └── 99-kc908-ft601.rules   # Linux udev permissions rule for FT601 (0403:601f)
├── src/
│   ├── internal.h             # Internal device state and transport declarations
│   ├── sdk.c                  # Lifecycle, negotiation, heartbeat, RPC
│   ├── vsa_gps.c              # Typed C wrappers for VSA and software GPS L1
│   ├── drm.c                  # Typed C wrappers for DRM30 / DRM+ digital broadcast
│   ├── transport_windows.c    # Windows FTDI D3XX transport & overlapped IQ
│   ├── transport_linux.c      # Linux libusb-1.0 FT601 transport & async IQ
│   ├── image.c                # High-level SHA-256 image upload/download
│   ├── legacy.c               # Windows vendor 1.3.4 compatibility table
│   ├── ktos_protocol.{h,c}    # Portable KTOS packet framing and CRC-32
│   └── sha256.{h,c}           # Self-contained SHA-256 implementation
└── tests/
    ├── fake_d3xx.c            # Windows offline FT601 emulator (test-only)
    ├── fake_libusb.c          # Linux offline FT601 libusb emulator (test-only)
    ├── test_sdk.c             # Protocol, IQ, RPC, image, and ABI tests
    └── test_dynamic.c         # Dynamic load/unload and per-thread error tests
```

## Runtime Requirements

### Windows
- Windows (x86 or x64)
- FTDI D3XX driver installed
- Official **`FTD3XX.dll` 1.3.0.4 or later** matching the host process architecture, placed next to `kcsdr.dll` (not bundled in this repository)

### Linux
- Linux (`x86_64`, `aarch64`, etc.) with **`libusb-1.0`** (`libusb-1.0-0`) installed — **no proprietary FTDI `libftd3xx` driver is required**
- Install `resources/99-kc908-ft601.rules` into `/etc/udev/rules.d/` and reload udev so non-root users can open the FT601 USB3 device (`0403:601f`):
  ```sh
  sudo cp resources/99-kc908-ft601.rules /etc/udev/rules.d/
  sudo udevadm control --reload-rules
  sudo udevadm trigger
  ```

> **Note:** `tests/FTD3XX.dll` (Windows) and `tests/libusb-1.0.so.0` (Linux) built during unit testing are offline emulators and must never be deployed with `kcsdr.dll` / `libkcsdr.so`.

## Building and Testing

### Windows

Requires CMake (>= 3.16) and a MinGW-w64 GCC toolchain (supporting `-m64` / `-m32` and `windres`).

```powershell
# Configure and build 64-bit kcsdr.dll with tests
cmake -S . -B build-x64 -G "MinGW Makefiles" `
    -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_C_FLAGS="-m64" `
    -DBUILD_TESTING=ON
cmake --build build-x64 -j 4

# Run offline unit tests
ctest --test-dir build-x64 --output-on-failure
```

Replace `-m64` with `-m32` (and `build-x86`) to build the 32-bit DLL for legacy x86 applications.

### Linux

Requires CMake (>= 3.16), GCC or Clang, `pkg-config`, and `libusb-1.0` headers (e.g. `sudo apt install libusb-1.0-0-dev`).

```sh
# Configure and build libkcsdr.so with offline unit tests
cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build-linux -j "$(nproc)"

# Run offline unit tests (against tests/libusb-1.0.so.0 emulator)
ctest --test-dir build-linux --output-on-failure
```

## Quick Start (C)

```c
#include "ktos_sdk.h"
#include <stdio.h>

int main(void) {
    ktos_device *dev = NULL;
    char buf[4096];
    uint32_t needed = 0;

    if (ktos_open(0, &dev) != 0) {
        ktos_error(NULL, buf, sizeof(buf));
        fprintf(stderr, "open failed: %s\n", buf);
        return 1;
    }

    if (ktos_identity(dev, buf, sizeof(buf)) == 0) {
        printf("device: %s\n", buf);
    }

    /* Available when the device is in KTOS Remote -> MCP mode. */
    if (ktos_rpc(dev, "capabilities", buf, sizeof(buf), &needed) == 0) {
        printf("capabilities: %s\n", buf);
    }

    ktos_close(dev);
    return 0;
}
```

## License

This project is licensed under the [MIT License](LICENSE).
