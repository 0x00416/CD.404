# 实施状态

> 更新时间：2026-08-06

## 当前里程碑

P0：底层技术验证。

## 已完成

- 初始化 C++20/CMake 工程。
- 添加 Visual Studio 2026 x64 和 Ninja/MSVC 构建预设。
- 添加空的 vcpkg manifest，为后续按项目锁定第三方依赖预留入口。
- 实现 CDDA 常量及 MSF、绝对扇区、LBA、采样帧转换。
- 实现转换范围和乘法溢出检查。
- 实现平台无关 TOC 模型、轨号连续性、轨道位置和 lead-out 验证。
- 从 TOC 自动计算轨道结束 LBA、整盘起始采样帧和轨道采样帧数。
- 实现 Windows 光驱枚举。
- 使用 `IOCTL_CDROM_READ_TOC_EX` 读取原生 TOC 并转换为平台无关模型。
- 实现命令行光驱探针。
- 添加基础单元测试并接入 CTest。

## 本次验证结果

- CMake 使用 Ninja/MSVC 成功配置。
- MSVC Debug 构建成功。
- CTest：1/1 测试程序通过，覆盖时间转换、边界、溢出和 TOC 验证场景。
- 已验证系统光驱枚举能力。
- 无介质时 TOC 读取返回“设备未就绪”；该错误路径工作正常。

受限自动化环境可能阻止 MSBuild FileTracker 访问部分系统资源；遇到这种情况可使用 Ninja/MSVC 预设。该限制不影响普通开发终端使用 Visual Studio 生成器预设。

## 尚未完成

- 插入真实音频 CD 后验证 TOC 内容。
- 读取和解析 CD-TEXT。
- 使用 `IOCTL_CDROM_RAW_READ` 读取 CDDA 扇区。
- 合成连续 PCM/CDDA 测试源。
- 跨曲目边界的样本连续性自动验证。
- WASAPI 共享模式和独占模式输出。
- 预读环形缓冲与读取重试。

## 下一实施批次

1. 定义连续扇区源接口。
2. 实现合成 CDDA 源和跨轨哨兵样本测试。
3. 实现 Windows 原始 CDDA 扇区读取。
4. 实现最小 WASAPI 事件驱动输出探针。
5. 插入音频 CD 后验证真实 TOC、扇区读取和跨轨行为。
