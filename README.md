# CD.404

CD.404 是一款面向 Windows 10/11 的轻量原生 CD 播放器，目标是提供可靠的元数据获取、ListenBrainz 播放记录上报，以及可验证的采样级无缝播放。

项目目前处于规划和底层技术验证阶段。

## 文档

- [产品与工程规划](docs/PRODUCT_PLAN.md)
- [实施状态](docs/IMPLEMENTATION_STATUS.md)

## 预定技术栈

- C++20
- CMake
- Win32、Direct2D、DirectWrite
- Windows Core Audio（WASAPI）
- Windows CD-ROM Device I/O
- WinHTTP、SQLite、Windows Imaging Component

## 当前阶段

首个里程碑是验证光驱 TOC/CD-TEXT 读取、CDDA 连续扇区读取、WASAPI 输出和跨曲目边界的采样连续性。

目前已经完成：

- CMake/CMake Presets 项目骨架。
- CD MSF、LBA、扇区和采样帧转换。
- 平台无关的 TOC 验证与曲目时间轴模型。
- Windows 光驱枚举和 TOC 读取探针。
- 基础自动测试。

## 构建

推荐在 **Developer PowerShell for VS 2026** 中使用 Ninja：

```powershell
cmake --preset ninja-msvc-x64
cmake --build --preset ninja-msvc-debug
ctest --preset ninja-msvc-debug
```

也可以直接生成 Visual Studio 工程：

```powershell
cmake --preset vs2026-x64
cmake --build --preset vs2026-debug
ctest --preset vs2026-debug
```

枚举光驱并读取当前光盘 TOC：

```powershell
.\out\build\ninja-msvc-x64\apps\drive_probe\cd404_drive_probe.exe
```

构建产物位于 `out/`，不会加入 Git。
