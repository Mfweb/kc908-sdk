# KTOS 主机 SDK（`kcsdr.dll`）

用于通过 USB 3.0（FTDI FT601 / D3XX）与 KC908 通信的 Windows C 动态库。支持原版 USB3 IQ 协议以及 KTOS 自制固件协议（带序号/CRC 的 IQ 流、UTF-8 JSON 控制 RPC、二进制 SD 图片/应用传输），同时保留与原版 Windows `kcsdr.dll` 1.3.4 函数表兼容的 ABI。

[English](README.md)

## 主要特性

- **自动协议协商**：`ktos_open()` 时自动识别原版固件与 KTOS 固件（IQ 模式或 MCP 模式），不向原版后端发送扩展指令。
- **预提交异步控制通道**：发送控制命令前先提交异步 IN 接收，避免主机调度延迟超过 FPGA 发送窗口。
- **250 ms 保活心跳**：连接 KTOS 后端时自动维护保活（支持能力位时使用 `0xD1` 单向保活），主机异常断开后设备自动停发。
- **高速重叠 IQ 流**：使用四个 1 MiB 重叠接收槽持续读取 FT601 数据，调用方可按任意 4 字节对齐块消费。
- **控制 RPC 与二进制传输**：提供带 CRC-32 校验的分片 JSON 控制接口（`ktos_rpc`）、调制分析（VSA）与软件 GPS L1 封装接口（`ktos_vsa_*`、`ktos_gps_*`），以及带整文件 SHA-256 校验的图片上传/下载接口（`ktos_image_upload`、`ktos_image_download`、`ktos_image_request`）。
- **原有上位机兼容**：导出 `kcsdr_init()` 26 项 Windows 1.3.4 函数表，供原有 32/64 位桌面软件直接替换使用（不含网络连接及私有工厂校准接口）。

## 目录结构

```text
.
├── CMakeLists.txt           # 独立 CMake 构建与 CTest 测试配置
├── LICENSE                  # MIT 许可证
├── include/
│   ├── ktos_sdk.h           # 公开 C ABI 头文件
│   └── image_protocol.h     # 二进制 SD 图片/应用传输协议定义
├── resources/
│   └── version.rc.in        # Windows 文件版本资源模板
├── src/
│   ├── internal.h           # 内部设备状态与 D3XX 函数表定义
│   ├── sdk.c                # 设备生命周期、协议协商、保活与分片 RPC
│   ├── vsa_gps.c            # 调制分析（VSA）与软件 GPS L1 C 封装接口
│   ├── transport_windows.c  # Windows FTDI D3XX 传输与重叠 IQ 队列
│   ├── image.c              # 带 SHA-256 校验的高层图片上传/下载接口
│   ├── legacy.c             # Windows 原版 1.3.4 26 项函数表适配层
│   ├── ktos_protocol.{h,c}  # 可移植 KTOS 协议编解码与 CRC-32
│   └── sha256.{h,c}         # 独立 SHA-256 实现
└── tests/
    ├── fake_d3xx.c          # 离线测试用 FT601 模拟器（严禁随发布包分发）
    ├── test_sdk.c           # 协议协商、IQ 流、RPC、图片与旧 ABI 单测
    └── test_dynamic.c       # 动态加载/卸载与线程隔离错误测试
```

## 运行依赖

- Windows（x86 或 x64）
- 已安装 FTDI D3XX 驱动
- 与目标进程位数一致的官方 **`FTD3XX.dll` 1.3.0.4 或更新版本**，放置在 `kcsdr.dll` 同目录（本仓库不包含该厂商二进制）

> **注意**：测试构建产生的 `tests/FTD3XX.dll` 仅为离线模拟器，切勿复制到实际部署目录。

## 编译与测试

需要 CMake（>= 3.16）以及支持 `-m32`/`-m64` 和 `windres` 的 MinGW-w64 GCC 工具链。

```powershell
# 配置并编译 64 位 kcsdr.dll 及单元测试
cmake -S . -B build-x64 -G "MinGW Makefiles" `
    -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_C_FLAGS="-m64" `
    -DBUILD_TESTING=ON
cmake --build build-x64 -j 4

# 运行离线单元测试
ctest --test-dir build-x64 --output-on-failure
```

如需编译 32 位版本供旧版 x86 上位机使用，将 `-m64` 替换为 `-m32`（并使用独立构建目录 `build-x86`）即可。

## 错误码说明

- 返回 `0` 表示成功，负数表示失败。
- FTDI D3XX 错误编码为 `-1000 - FT_STATUS`。
- 设备与协议错误沿用 Linux 负 `errno` 编码。
- `ktos_open()` 失败后可调用 `ktos_error(NULL, buf, cap)` 读取当前线程最近的错误说明。

## 许可证

本项目基于 [MIT 许可证](LICENSE) 开源。
