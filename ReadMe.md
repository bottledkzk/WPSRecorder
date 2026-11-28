# WPS Recorder 2.3.13 录制与剪辑工作台

本项目是 Windows 10/11 x64 上的桌面应用。Studio 窗口在 Microsoft Edge WebView2 中加载随程序附带的 HTML、CSS 和 JavaScript。C++17 和 Qt 负责 Windows 设备、录制控制和本地文件操作，FFmpeg 负责编码与剪辑导出。

## 使用

从 [GitHub Releases](https://github.com/bottledkzk/WPSRecorder/releases/tag/v2.3.13) 下载 `WPSRecorder-2.3.13-Windows-x64.zip`，将 ZIP 完整解压到 Windows 下载目录，双击包内的 `wpsrecorder.exe`。课程包的启动入口为 `exe_cli/wpsrecorder.exe`。自动评测可调用同目录的 `wpsrecorder_cli.exe`。系统有 Microsoft Edge WebView2 Runtime 时使用 Studio 界面，加载失败时切换到 Qt 工作台。录制文件默认保存在用户视频目录下的 `WPSRecorder` 文件夹，路径可在录制工作台中更改。

- **录制工作台**　打开即显示当前来源的实时画面。预览下方可切换目标、打开当前源属性、缩放或适配画面。底部的场景、源、混音器、录制设置和录制控制可拖动调整宽度及顺序。源列表保存当前场景中的可切换主画面源，摄像头画中画可叠加。混音器支持默认麦克风和系统声，也可添加系统枚举的音频设备。
- **录制与预约**　可选 30、60、90、144 fps。码率的「默认」随分辨率和帧率计算，手动调整后保留用户输入。「预约录制」支持多任务、冲突和过期状态，时、分、秒留空或填 0 时由用户手动停录。悬浮条可拖动、收起、开始或停止录制，并打开工作台或讲解工具。
- **极简录制**　从系统托盘或「视图 → 极简录制」进入，也可运行 `wpsrecorder.exe --minimal`。点击悬浮条的 ▶ 开始录制它所在的屏幕，点停止自动保存。可在条上切换摄像头和麦克风、截图或打点。
- **回看与管理**　素材库按视频名称搜索。点击进度条或右侧分节的打点跳转；可直接新增并编辑打点名称。无边框全屏预览可按 Esc 退出，播放位置保留。
- **剪辑工坊**　从「文件 → 打开剪辑工程」打开工程或已保存的草稿。在播放头处分割后处理片段；可拖动重排、删除补齐或锁定单轨。磁吸对齐打点、播放头或片段边缘。方向键可逐帧定位。可倍速预览、裁剪取景，处理多条音轨和字幕轨并导出独立音轨、混音、烧录字幕或可选字幕。程序关闭时自动保存未完成的草稿。
- **智能 AI 助手**　录制和回看共用录屏助手侧栏，剪辑侧栏独立，选工程后才开始对话。独立工作区可按项目管理对话、文件、素材、视频预览和 Recorder CLI。长对话由已配置的模型压缩较早上下文，历史保留原消息；也可执行 `/compact`。模型及已归档对话在设置中管理。
- **字幕与配音**　支持本地识别、SRT、翻译审校、单句试听与重配、独立旁白轨和章节长稿。本地配音使用 Audio8 0.1B，包含默认音色及普通话女声。

支持 MP4、MOV、MKV、AVI、FLV 录制。HDR 和 SDR 转 HDR 输出为 10 位 HEVC，使用 MP4、MOV 或 MKV。HDR 显示器的原始色彩保留依赖 DXGI HDR 采集。使用画中画、标注或兼容采集时会先完成 SDR 合成，再转换为 HDR。HDR 转 SDR 使用高光压缩。帧率和硬件编码能力随设备及来源变化。

「帮助」中可查询快捷键和模型服务。「设置」可切换简体中文、English 和 13 套主题。使用方法与本地、在线能力见 [使用说明](docs/guide/USER-GUIDE.md) 和 [完整功能介绍](docs/guide/FEATURE-INTRODUCTION.md)。

## 性能简介

GUI 与 CLI 通过当前用户的本机套接字控制录制服务，采集、合成和编码分工运行。屏幕优先走 DXGI，窗口优先走 Windows Graphics Capture。声音使用 WASAPI，摄像头使用 Media Foundation。录制自动探测 NVENC、QSV 或 AMF，并提供 x264 软件路径。实时监看消费最新帧，声音按设备包写入磁盘，封装完成后发布成片。

| 本机实录 | 成片时长 | 实际解码帧数 |
| --- | ---: | ---: |
| 实体副屏长录 1920×1080、60 fps | 602.20 秒 | 36,130 |
| 窗口 1920×1080、60 fps | 12.00 秒 | 720 |
| 独立摄像头 1280×720、30 fps | 6.00 秒 | 180 |
| 副屏画中画 1920×1080、30 fps | 9.00 秒 | 270 |
| 实体副屏 2560×1440、30 fps | 16.00 秒 | 480 |

上述 2.3.9 实录使用 NVIDIA H.264 硬件编码。长录的录制服务 CPU 采样中位数 2.03%，内存采样峰值 309.46 MB。场景条件、帧位和测量范围见 [2.3.9 验收](docs/validation/VALIDATION-V239.md) 与 [性能细节](docs/reference/PERFORMANCE.md)。CLI 合并与 WorkView 复测见 [2.3.13 验收](docs/validation/VALIDATION-V2313.md)。

## 命令行

CLI 与 GUI 共用当前用户的录制服务，支持跨进程启动、停止和读取状态。标准输出便于脚本捕获，失败时标准错误有说明且退出码非 0。

```bat
bin\wpsrecorder_cli.exe --version
bin\wpsrecorder_cli.exe --record --duration 30 --output out.mp4 --res 1920x1080 --fps 30 --bitrate 6000 --audio none
bin\wpsrecorder_cli.exe --start --output out.mp4 --audio mix
bin\wpsrecorder_cli.exe --status
bin\wpsrecorder_cli.exe --pause
bin\wpsrecorder_cli.exe --resume
bin\wpsrecorder_cli.exe --stop
bin\wpsrecorder_cli.exe --merge --input first.mp4 --input second.mp4 --output joined.mp4 --res 1920x1080
```

开发构建的程序在 `build/bin/Release/`；交付包在 `exe_cli/`，将示例中的 bin 替换为对应位置即可。完整参数和剪辑命令见 [CLI 文档](docs/reference/CLI.md)。

## 源码与构建

```text
wpsrecorder/
  src/          采集、录制核心、IPC、GUI、播放器、剪辑、CLI
  studio/       WebView2 工作台页面、主题、前端交互和自带静止画面
  i18n/         界面和诊断翻译
  docs/         文档总索引、使用指南、技术参考、课程对照、版本验收
  tests/        真实采集、解码、剪辑及运行包验证
  scripts/      Qt 与本地语音运行库部署及打包
  skills/       录屏助手工具和工作流程说明
  installer/    当前用户安装和卸载脚本
  docs/CMakeLists.txt  课程指定构建兼容入口
  CMakeLists.txt       C++17 项目构建入口
```

从 [文档总索引](docs/README.md) 可进入使用指南、技术参考与版本验收。模块划分、设计取舍和处理链路见 [设计说明](docs/DESIGN.md)，交付目录和验收状态见 [课程清单](docs/course/COURSE-CHECKLIST.md)。

可执行程序、DLL、模型、样片及验证视频独立打包，不放入 Git。
