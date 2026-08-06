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
- 光驱生产线程、约 6 秒有界 SPSC 环形缓冲和 1 秒首播水位。
- 支持显式启动、排空和跨线程取消的事件驱动 WASAPI 共享模式输出。
- 可跨相邻音轨保持同一数据流与音频会话的实体光盘播放探针。
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

连续播放当前音频轨区间，或从所选音轨内部偏移位置开始诊断：

```powershell
.\out\build\ninja-msvc-x64\apps\play_probe\cd404_play_probe.exe --track 1 --all
.\out\build\ninja-msvc-x64\apps\play_probe\cd404_play_probe.exe --track 1 --offset-seconds 170 --seconds 20
```

播放过程中可按 `Ctrl+C` 停止；该路径会同时中断待处理的光驱重叠 I/O 和 WASAPI
事件等待。播放探针会在独立线程持续读取光盘，以固定容量环形缓冲向 WASAPI 供给
PCM；相邻音轨之间不会重建光盘数据源、连续流或音频会话。
混合模式光盘遇到数据轨时会结束当前连续音频区间，避免把数据扇区当作声音播放。

播放期间会观察 WASAPI 端点的剩余帧数；若光驱供给中断并耗尽端点缓冲，探针会明确
报告欠载并以失败结束，不会把插入的静音误报为无缝播放成功。

当前默认使用兼容性更好的 WASAPI 共享模式，Windows 可能将音频 CD 的原生格式转换到
设备混音格式。独占模式和端到端位精确输出仍属于后续工作。

构建产物位于 `out/`，不会加入 Git。
