# CD.404 第三方说明

版本：0.2.0-public-beta.1

CD.404 不嵌入浏览器运行时，也不随安装包分发第三方动态库。程序链接 Windows SDK/
系统组件（Win32、WASAPI、Direct2D、DirectWrite、WIC、WinHTTP、Windows Runtime 和
Windows 自带 SQLite 接口）；Release 使用静态 MSVC 运行库。相应组件仍受 Microsoft
Windows 与 Visual Studio Build Tools 的许可条款约束。

在线元数据/服务说明：

- MusicBrainz 核心数据库采用 CC0；其补充数据可能采用 CC BY-NC-SA 3.0。CD.404 显示
  查询得到的音乐元数据并保留 MusicBrainz 来源标记。详情：
  <https://musicbrainz.org/doc/About/Data_License>
- Cover Art Archive 提供封面文件；每个图像的著作权/使用权取决于原始权利人，缓存与
  显示不改变其权利状态。详情：<https://coverartarchive.org/>
- ListenBrainz 由 MetaBrainz Foundation 运营；应用按用户选择提交播放记录。详情：
  <https://listenbrainz.org/>
- GnuDB 查询结果和服务使用受其条款约束。详情：<https://gnudb.org/>
- Apple iTunes Search API 返回的元数据仅用于应用内查询和呈现；不得将宣传图作为应用
  资源重新分发。详情：<https://developer.apple.com/library/archive/documentation/AudioVideo/Conceptual/iTuneSearchAPI/>
- LRCLIB、网易云音乐、QQ 音乐和酷狗音乐仅用于按曲目元数据查询歌词。歌词文本及翻译的
  著作权归原始权利人，服务可用性与使用条件由各服务提供方决定。LRCLIB API 说明：
  <https://lrclib.net/docs>
- 当歌词来源未提供简体中文译文时，CD.404 使用 Microsoft Edge 翻译服务补全缺失行；
  机器翻译可能不准确，结果仅作为同步字幕显示并写入本地歌词缓存。

安装包由项目内的原生 Win32 安装器生成，不依赖或分发 Inno Setup、WiX 等第三方打包程序。

若正式发行加入新的代码库、字体、图标、音频或其他资源，发布者必须在签名之前更新
本文件并附上其完整许可文本（如许可要求）。
