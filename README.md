# CD.404

CD.404 是一款面向 Windows 10/11 的轻量原生 CD 播放器，目标是提供可靠的元数据获取、ListenBrainz 播放记录上报，以及可验证的采样级无缝播放。

项目目前处于底层技术验证阶段，已经能够从实体音频 CD 读取原始 CDDA，
并通过 Windows 默认音频设备完成最小播放。

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
- 连续 CDDA 流、跨轨样本连续性测试和原始扇区读取探针。
- CDDA 到 44.1 kHz、16 位、双声道 PCM 的无损格式适配。
- 事件驱动的 WASAPI 共享模式输出和实体光盘播放探针。
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

读取当前光盘的原始 CDDA 扇区并计算诊断哈希：

```powershell
.\out\build\ninja-msvc-x64\apps\cdda_probe\cd404_cdda_probe.exe
```

播放当前音频 CD 的首个音轨，默认播放 15 秒：

```powershell
.\out\build\ninja-msvc-x64\apps\play_probe\cd404_play_probe.exe
```

也可以指定音轨和最长播放秒数：

```powershell
.\out\build\ninja-msvc-x64\apps\play_probe\cd404_play_probe.exe --track 2 --seconds 30
```

当前播放探针会先把指定片段完整读入内存，再交给 WASAPI，目的是先验证端到端
播放链路。后续播放器引擎将改为光驱预读线程与有界环形缓冲，以支持整轨/整盘播放、
取消操作和稳定的采样级无缝衔接。

构建产物位于 `out/`，不会加入 Git。
