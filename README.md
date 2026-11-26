# KTOS Host SDK (`kcsdr.dll`)

Windows C shared library for communicating with the KC908 over USB 3.0 (FTDI FT601 / D3XX). It supports both the original KC908 USB3 IQ protocol and the KTOS custom firmware protocols (sequenced/CRC IQ streaming, JSON control RPC, and binary SD image/app transfers), while preserving binary compatibility with the Windows vendor `kcsdr.dll` 1.3.4 function table.

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
│   └── version.rc.in        # Windows VERSIONINFO template
├── src/
│   ├── internal.h           # Internal device state and D3XX bindings
│   ├── sdk.c                # Lifecycle, negotiation, heartbeat, RPC
│   ├── vsa_gps.c            # Typed C wrappers for VSA and software GPS L1
│   ├── drm.c                # Typed C wrappers for DRM30 / DRM+ digital broadcast
│   ├── transport_windows.c  # Windows FTDI D3XX transport & overlapped IQ
│   ├── image.c              # High-level SHA-256 image upload/download
│   ├── legacy.c             # Windows vendor 1.3.4 compatibility table
│   ├── ktos_protocol.{h,c}  # Portable KTOS packet framing and CRC-32
│   └── sha256.{h,c}         # Self-contained SHA-256 implementation
└── tests/
    ├── fake_d3xx.c          # Offline FT601 emulator (test-only)
    ├── test_sdk.c           # Protocol, IQ, RPC, image, and ABI tests
    └── test_dynamic.c       # Dynamic load/unload and FLS error tests
```

## Runtime Requirements

- Windows (x86 or x64)
- FTDI D3XX driver installed
- Official **`FTD3XX.dll` 1.3.0.4 or later** matching the host process architecture, placed next to `kcsdr.dll` (not bundled in this repository)

> **Note:** `tests/FTD3XX.dll` built during unit testing is an offline emulator and must never be deployed with `kcsdr.dll`.

## Building and Testing

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
