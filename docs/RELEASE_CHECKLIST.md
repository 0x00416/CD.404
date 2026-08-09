# CD.404 公开测试发布检查表

目标版本：0.2.0-public-beta.1。每个候选必须保存勾选结果、操作者、UTC 时间和产物哈希。

## 自动门禁

- [ ] 工作树干净；当前提交来自 `codex/public-beta-hardening-release`，未包含 Token、
  数据库、日志、绝对本机路径或构建产物。
- [ ] `powershell -File tools/release-check.ps1` 通过。
- [ ] `make release` 全量配置、静态 MSVC Release 构建和 CTest 全绿。
- [ ] EXE 的 `ProductVersion` 为 `0.2.0-public-beta.1`，manifest 为 `asInvoker`、x64、
  PerMonitorV2；普通用户启动不出现 UAC。
- [ ] 若已安装 Inno Setup，运行 `powershell -File tools/build-installer.ps1`；在干净普通
  用户账户完成安装、覆盖安装、启动和卸载，确认安装目录为 LocalAppData。

## 功能与真实硬件

- [ ] 完成 `HARDWARE_VALIDATION.md` 的 Windows 10/11 光驱、共享/独占、多端点、拔换盘、
  休眠/唤醒和 50 次资源释放矩阵；跳过项必须列为风险，不能勾选。
- [ ] 验证默认共享、显式独占、解释性失败和显式共享回退；主界面不显示 ListenBrainz
  同步进度。
- [ ] 断网/重启/账户切换/401/429 恢复符合自动测试，并以本地模拟为主；正式账户只做
  经授权的最小冒烟验证。
- [ ] Narrator 和 NVDA 分别读取窗口动态名称、F1 帮助和原生编辑框；浅色、深色、系统
  高对比度、100%/150%/200% DPI 下文字可读且键盘路径完整。

## 隐私、许可与供应链

- [ ] 人工检查 `PRIVACY.md`、`THIRD_PARTY_NOTICES.md` 和诊断导出样本；确认无媒体标题、
  账户名、Token、绝对路径或稳定端点 ID。
- [ ] 确认 MusicBrainz/Cover Art Archive、GnuDB、Apple 与 ListenBrainz 的当前服务条款
  仍允许本次公开测试用途；新增依赖/资源的许可文本已补齐。
- [ ] 在隔离构建机记录 MSVC、Windows SDK、CMake、Ninja 和 Inno Setup 版本；仅从可信
  来源安装工具，不把编译器许可证或签名私钥提交到仓库。

## 签名、哈希与发布

- [ ] 使用组织控制的代码签名证书运行 `tools/build-installer.ps1 -Sign`，验证 Authenticode
  链和时间戳。没有证书时不得伪称已签名，发布页必须醒目标注“未签名公开测试版”。
- [ ] 核对脚本生成的 `.sha256`，从下载位置重新获取安装包并再次计算 SHA-256。
- [ ] 发布说明列出提交范围、测试结果、真实硬件覆盖、未覆盖风险、隐私说明和已知问题；
  不包含本机路径或真实账户数据。

## 回滚

- [ ] 保留上一候选安装包、SHA-256 和提交标签；确认旧版可在不删除用户数据的情况下
  覆盖安装。
- [ ] 若出现数据损坏、无法停止播放、隐私泄漏或设备资源无法释放，立即撤下新包，发布
  已知问题公告并回滚到上一哈希；数据库迁移仅向前时先复制用户数据库再降级。
- [ ] 卸载不会自动删除 `%LOCALAPPDATA%\CD.404`；需要彻底清理时按隐私说明手动执行，
  避免回滚过程误删元数据修订或待同步记录。
