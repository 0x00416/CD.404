# CD.404 隐私说明

版本：0.2.0-public-beta.1（2026-08-09）

CD.404 不包含遥测、广告、用户画像或崩溃自动上传。播放音频在本机从光驱读取并送往
Windows 音频设备，不会上传音频样本。

## 本机保存的数据

- `%LOCALAPPDATA%\CD.404\settings.json`：音量、输出模式/端点选择、ListenBrainz 开关和
  以不可读 TOC 哈希为键的播放位置。
- `%LOCALAPPDATA%\CD.404\metadata\`：按 TOC 键保存的专辑/曲目元数据、字段来源、
  用户修订和发行版选择。封面缓存来自 Cover Art Archive。
- `%LOCALAPPDATA%\CD.404\listenbrainz.db`：待同步正式播放记录、重试状态和不可逆账户
  指纹；数据库不保存 Token。
- Windows 凭据管理器的 `CD.404/ListenBrainz`：仅在用户主动配置时保存 User Token。

卸载程序只删除应用文件，不自动删除上述用户数据，避免意外丢失。用户可先在设置页
清理当前账户待同步队列、清除凭据，再删除 `%LOCALAPPDATA%\CD.404` 完成本机数据清理。

## 网络请求

启用相应功能时，应用会直接连接 MusicBrainz/Cover Art Archive、GnuDB、Apple iTunes
Search API 和 ListenBrainz。元数据请求发送由光盘 TOC/Disc ID 推导的标识、轨数/时长
及已有专辑/艺术家查询词；封面请求发送 MusicBrainz 发行版 ID。ListenBrainz 请求只在
用户配置 Token 且开启上报后发送曲名、艺术家、专辑、时长、起播时间和可用 MBID。
各服务会按其隐私政策处理 IP 地址和请求内容。关闭 ListenBrainz 上报不会影响播放。

## 诊断导出

诊断只能由用户在设置页主动选择目标文件后导出。事件环容量有限，只记录组件、时间、
状态码、计数和输出模式，不记录媒体标题或账户名。Authorization/Token、绝对本机路径和
稳定音频端点 ID 在写入及导出时各脱敏一次。导出文件不会自动上传。

## 公开测试注意事项

本版本尚未接入自动更新。删除应用或诊断文件前应自行备份；向问题跟踪系统上传诊断前，
仍建议人工检查内容。隐私问题应通过项目发布页公布的维护渠道反馈。
