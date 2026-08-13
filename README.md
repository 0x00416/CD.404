# CD.404

CD.404 是一款面向 Windows 10/11 的轻量原生 CD 播放器，目标是提供可靠的元数据获取、ListenBrainz 播放记录上报，以及可验证的采样级无缝播放。

当前版本为 `0.2.0-public-beta.1` 公开测试候选开发分支：核心功能、离线/故障自动测试、
诊断、隐私和每用户分发方案均已落地；发布前仍需完成检查表中的多硬件矩阵与签名步骤。

## 文档

- [产品与工程规划](docs/PRODUCT_PLAN.md)
- [公开测试版执行计划](docs/PUBLIC_BETA_PLAN.md)
- [实施状态](docs/IMPLEMENTATION_STATUS.md)
- [真实硬件验证手册](docs/HARDWARE_VALIDATION.md)
- [隐私说明](docs/PRIVACY.md)
- [第三方说明](THIRD_PARTY_NOTICES.md)
- [公开测试发布检查表](docs/RELEASE_CHECKLIST.md)

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
- 光驱生产线程、约 6 秒有界 SPSC 环形缓冲和 3 秒首播水位。
- CDDA 有限重试、连续块重叠校验及可观测的读取/欠载统计。
- 支持显式启动、排空和跨线程取消的事件驱动 WASAPI 共享模式输出。
- 可复用的阻塞式 CDDA 播放引擎、可查询提交/实际渲染采样帧进度及经过测试的正式状态机。
- 可跨相邻音轨保持同一数据流与音频会话的实体光盘播放后台组件。
- 图形界面在工作线程中直接调用进程内播放引擎，停止时使用协作取消，不再创建或强制终止播放子进程。
- Windows 系统媒体控制（SMTC）、媒体键、系统播放状态及时间轴同步。
- 同一 WASAPI 会话内的真实暂停/恢复；播放中选择、上一首或下一首会继续播放目标曲目。
- 曲内已播放/总时长显示，以及直接作用于 PCM 消费端的实时音量控制；首次启动默认为 100%，单位增益保持样本不变。
- 按光盘 TOC 记忆当前曲目和精确采样帧位置，并持久化音量。
- ListenBrainz `playing_now` 和符合“半曲或 4 分钟取较短者”规则的 `single` 上报。
- ListenBrainz Token 在线验证、账户状态、SQLite 离线队列及速率限制感知重试。
- CD-TEXT、MusicBrainz、CDDB/freedb 与 iTunes 多源元数据补全，并显示本次实际命中的来源。
- CDDB/freedb 默认使用 `gnudb.gnudb.org`，可自定义服务器或关闭；支持本地元数据编辑、服务器测试校验和用户确认后的正式上传。
- 根据当前曲名、艺术家、专辑和时长自动匹配 LRCLIB、网易云音乐、QQ 音乐与酷狗歌词；支持增强 LRC/YRC/QRC/KRC，并优先采用可信的原生逐字时间。
- 播放页在封面与专辑信息之间按可用高度显示连续歌词上下文；当前句固定在区域中央并逐字高亮，前后句向两侧扩展并弱化，暂停和定位后仍直接跟随实际渲染采样帧，普通行歌词不伪造逐字时间。
- 切句使用 280 ms 帧率无关的纵向缓出与淡入动画；播放画面按 DWM 当前合成刷新率调度，在 120/144/165 Hz 等高刷新率显示器上不受传统 `WM_TIMER` 100 FPS 上限约束，窗口不可见或无活动画面时停止高频刷新。
- 当前曲目歌词就绪后只前瞻获取紧邻的下一条音频轨；无缝跨轨时直接切换内存时间轴，不在后台扫描整张光盘。
- 双语歌词优先使用来源自带译文；缺失时批量生成简体中文机器译文。每句原文与译文保持为同一显示单元，按实际换行高度完整排版。
- 一句的词级时间结束后仍连同译文保留到下一句起始点，原文和译文在同一个时间点整组切换，不暴露句间空档。
- 基础自动测试。

## 构建

在普通 PowerShell 中可用 Makefile 一键配置、构建完整 Debug 程序并运行测试：

```powershell
make
```

构建并测试 Release 版本：

```powershell
make release
```

其他常用入口包括 `make run`、`make run-release`、`make clean` 和 `make help`。
Makefile 会通过 Visual Studio Installer 自带的 `vswhere` 自动定位 MSVC；本机路径不会写入仓库。
`make`、`cmake` 和 `ctest` 需位于 `PATH`，也可以通过同名 Make 变量显式指定其路径。
`make release` 会先执行 `tools/release-check.ps1`，检查版本同步、发布文件、工作树空白错误、
本机路径以及误提交的数据库/日志/二进制产物。

## 每用户安装包

Release 使用静态 MSVC 运行库，安装包无需先以管理员权限安装 VC Redistributable。
项目自带原生 Win32 每用户安装器，不依赖 Inno Setup、WiX 等第三方打包程序。执行：

```powershell
make package
```

脚本会先重新运行完整 `make release`，再生成自包含 x64 安装包和 SHA-256 文件。安装向导
默认使用 `%LOCALAPPDATA%\Programs\CD.404`，也允许用户输入或选择其他安装目录，并会创建当前
用户开始菜单快捷方式、登记 Windows 卸载入口，并在卸载时保留 `%LOCALAPPDATA%\CD.404`
中的设置、缓存和播放记录。用户也可以在卸载向导中选择彻底删除上述数据及 Windows
凭据。安装器还会把 CD.404 添加到 Windows“音频 CD”自动播放候选列表，但不会更改
用户现有的默认操作；从该候选项启动时，程序只读取并自动播放 Windows 传入的光驱。
安装后双击资源管理器中的音频 CD 驱动器也会调用 CD.404；安装器会保存此前的双击 Verb，
卸载时在用户未另行修改的前提下恢复。卸载还会删除自身的候选项、ProgID 以及仍指向它的
失效默认引用，不留下不可用条目。
设置页会检测 Windows 策略是否阻止音频 CD 自动播放；用户点击“修复”后只清除当前账户
中禁用 CD-ROM 或现有光驱盘符的位，不改写自动播放默认应用。Windows 资源管理器重启后
读取新策略。
安装与卸载向导会跟随 Windows 应用浅色/深色主题。签名构建需在受控构建机
设置 `CD404_SIGNING_THUMBPRINT` 并显式加 `-Sign`；证书、私钥和 Token 均不得进入仓库。
没有签名证书仍可生成安装包，但不得声称 Authenticode 已通过。

推荐在 **Developer PowerShell for VS 2026** 中使用 Ninja：

```powershell
cmake --preset ninja-msvc-x64
cmake --build --preset ninja-msvc-debug
ctest --preset ninja-msvc-debug
```

启动图形播放器：

```powershell
.\out\build\ninja-msvc-x64\apps\cd404\CD.404.exe
```

当前界面会自动读取可用音频 CD 的 TOC，支持曲目选择、滚动、上一首、下一首、播放/暂停、进度定位、音量、刷新和弹出。暂停会停止而不重置当前 WASAPI 客户端，恢复后继续消费同一端点缓冲；播放中切换曲目会自动继续播放目标曲目。图形界面在工作线程中直接运行 `CddaPlaybackEngine`，并根据已经离开 WASAPI 端点缓冲的采样帧更新曲目、曲内时长和进度；`apps/playback` 仅保留为命令行诊断入口。Windows 音量浮层、媒体键和支持 SMTC 的外设可以控制播放、暂停、停止、切轨和定位，应用会向系统同步当前曲目及时间轴。
界面跟随 Windows 应用浅色/深色主题和系统高对比度颜色，并在主题变化时即时重建资源。
窗口通过 MSAA 暴露随所选曲目更新的可访问名称；所有主要播放和设置操作都有键盘入口，
按 `F1` 可打开由屏幕阅读器直接读取的原生快捷键帮助。
元数据优先使用光盘内嵌 CD-TEXT；后台先按标准 MusicBrainz Disc ID 精确查询，未关联时才使用 TOC 模糊后备，同时按设置查询 CDDB/freedb，再以可信的专辑/艺术家结果查询 iTunes，并通过音轨数、编号和 TOC 时长差严格校验后补全空字段。多个 MusicBrainz 发行版可按 `M` 切换并按光盘记忆。CDDB/freedb 默认以 HTTPS 连接 `gnudb.gnudb.org`，设置页可更换兼容服务器，也可完全关闭；出于旧服务兼容性允许 `http://`，但会明文传输 CDDB 邮箱和元数据。专辑下方每个来源胶囊只显示一个本次实际命中的服务。用户修订不会被刷新覆盖，文本元数据使用当前用户目录下的版本化持久缓存支持离线启动；正面封面仅使用 Cover Art Archive 的 1200px 缩略图，不会写入仓库，也不会下载或复用 iTunes 宣传图。

播放页的“编辑元数据”会打开独立编辑页，可修改专辑名、专辑艺术家、CDDB 分类、年份及每轨曲名/艺术家。“保存到本机”以 TOC 指纹记住结果，换盘后再次插入仍优先使用；不会自动上传。“测试上传”仅请服务器检查 UTF-8 xmcd 内容，“正式上传”会再次弹出确认后才写入配置的公共数据库。上传必须在设置页填写有效邮箱，并且会拒绝未编辑、缺失曲名或仍包含 `Track 01` / `音轨 01` 占位名的提交。

首次启动的音量为 100%。应用会在 `%LOCALAPPDATA%\CD.404\settings.json` 保存音量、ListenBrainz 上报选项、CDDB 服务器/开关/提交邮箱、用户编辑的元数据，以及按 TOC 区分的光盘播放位置；进度使用 44.1 kHz 采样帧而非整秒记录。CDDB 提交邮箱只在用户启用该数据源并发起请求时发送到所配置的服务器。播放位置记忆和 Windows 媒体控制始终启用，不在设置页显示。该文件是当前用户的本机配置，不属于仓库内容，也不包含 ListenBrainz Token。

## ListenBrainz 配置

点击播放器右上角的设置按钮会进入独立 ListenBrainz 设置页，可输入、替换或清除 User Token，并开关播放记录上报；输入框会隐藏内容，保存后写入 Windows 凭据管理器并立即生效。播放器优先读取通用凭据 `CD.404/ListenBrainz`，也可读取当前进程的 `CD404_LISTENBRAINZ_TOKEN` 环境变量。Token 不会写入仓库、配置文件或日志。

设置页可按 `G` 或点击“导出脱敏诊断”保存有界运行事件。日志只记录组件、状态码、
计数和模式，不记录曲名、专辑或账户名；Token、本机绝对路径和稳定音频端点 ID 在写入
及导出时各脱敏一次。导出位置由用户通过 Windows 保存对话框明确选择。

推荐打开“控制面板 → 凭据管理器 → Windows 凭据 → 添加通用凭据”，将网络地址填写为 `CD.404/ListenBrainz`，用户名可填写 `ListenBrainz`，密码填写个人 User Token。重新启动播放器后生效。

用于临时测试时，也可在启动播放器的同一 PowerShell 会话中设置：

```powershell
$env:CD404_LISTENBRAINZ_TOKEN = '<个人 User Token>'
make run-release
```

用户单击曲目并创建播放会话时，只要曲名和艺术家可用就立即提交一次 `playing_now`，无需等待光驱预缓冲完成；若 MusicBrainz 身份随后到达，只补发一次带身份的修正。实际渲染达到曲目时长的一半或 4 分钟（取较短者）后，才将带起播时间的 `single` 写入本地 SQLite 队列并由后台线程上报。设置页会显示已验证账户、待同步、重试和凭据状态，并可只清理当前账户的待同步队列；播放主界面不显示上报进度。网络失败按指数退避重试，HTTP 429 优先遵循 ListenBrainz 的 `X-RateLimit-Reset-In`，401 暂停发送且不删除队列，应用退出后未完成的正式记录仍会保留。

待同步记录位于 `%LOCALAPPDATA%\CD.404\listenbrainz.db`。数据库不保存 User Token，并以不可逆的本地凭据指纹隔离不同账户的队列；Token 始终只保存在 Windows 凭据管理器中。

也可以直接生成 Visual Studio 工程：

```powershell
cmake --preset vs2026-x64
cmake --build --preset vs2026-debug
ctest --preset vs2026-debug
```

枚举光驱并读取当前光盘 TOC：

```powershell
.\out\build\ninja-msvc-x64\tools\probes\drive\cd404_drive_probe.exe
```

读取当前光盘的原始 CDDA 扇区并计算诊断哈希：

```powershell
.\out\build\ninja-msvc-x64\tools\probes\cdda\cd404_cdda_probe.exe
```

显示当前光盘命中的在线元数据来源及合并结果：

```powershell
.\out\build\ninja-msvc-x64\tools\probes\metadata\cd404_metadata_probe.exe
```

枚举音频端点，以及显式执行共享回环或独占标记音验证：

```powershell
.\out\build\ninja-msvc-x64\tools\probes\wasapi\cd404_wasapi_probe.exe --list
.\out\build\ninja-msvc-x64\tools\probes\wasapi\cd404_wasapi_probe.exe --loopback-shared
.\out\build\ninja-msvc-x64\tools\probes\wasapi\cd404_wasapi_probe.exe --render-exclusive
```

后两条命令会输出一秒低幅 997 Hz 标记音；无活动端点时工具打印 `SKIP` 并返回 2，
不会把缺少硬件误报为成功。端点选择、外部采集和硬件矩阵见验证手册。

播放当前音频 CD 的首个音轨，默认播放 15 秒：

```powershell
.\out\build\ninja-msvc-x64\apps\playback\CD.404.Playback.exe
```

也可以指定音轨和最长播放秒数：

```powershell
.\out\build\ninja-msvc-x64\apps\playback\CD.404.Playback.exe --track 2 --seconds 30
```

连续播放当前音频轨区间，或从所选音轨内部偏移位置开始诊断：

```powershell
.\out\build\ninja-msvc-x64\apps\playback\CD.404.Playback.exe --track 1 --all
.\out\build\ninja-msvc-x64\apps\playback\CD.404.Playback.exe --track 1 --offset-seconds 170 --seconds 20
```

播放过程中可按 `Ctrl+C` 停止；该路径会同时中断待处理的光驱重叠 I/O 和 WASAPI
事件等待。播放后台组件会在独立线程持续读取光盘，以固定容量环形缓冲向 WASAPI 供给
PCM；相邻音轨之间不会重建光盘数据源、连续流或音频会话。
混合模式光盘遇到数据轨时会结束当前连续音频区间，避免把数据扇区当作声音播放。
图形界面在同一连续音频区间内定位或任意切轨时，会清空旧端点/PCM 缓冲并在同一个
WASAPI 客户端上继续；跨数据轨时显式走安全重建路径。

播放期间会观察 WASAPI 端点的剩余帧数；若光驱供给中断并耗尽端点缓冲，探针会明确
报告欠载并以失败结束，不会把插入的静音误报为无缝播放成功。

当前默认使用兼容性更好的 WASAPI 共享模式，Windows 可能将音频 CD 的原生格式转换到
设备混音格式。设置页通过下拉菜单选择稳定的输出端点并显式启用独占模式；音频引擎
也保留为独立下拉项，当前只提供 WASAPI，为后续可选 ASIO 后端预留明确边界。独占模式只接受
44.1 kHz、16 位、双声道 PCM。格式不支持、设备占用或初始化失败会显示明确原因，且仅在
用户同时启用“失败后回退共享”时回退，不会静默掩盖独占失败。端到端位精确性仍需按
发布检查表使用真实端点和外部采集设备验证。

所有正式构建产物统一位于 `out/build/<preset>/`，不会加入 Git。根目录不保留编译中间文件；
`make clean` 会删除整个 `out/`、旧版 `build/` 目录及 SDK 探测残留，随后可直接用 `make` 重新生成。
