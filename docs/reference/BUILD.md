# 构建与分发

## 构建环境

- Windows 10/11 x64。
- Visual Studio 2022，安装桌面 C++ 工作负载及 Windows SDK。
- CMake 3.16 或更高版本。
- Qt 5.15，包含 Core、Gui、Widgets、Network、Multimedia、MultimediaWidgets，以及简体中文 Qt 翻译（vcpkg：qt5-translations）。如果 CMake 找不到翻译，可设置 RECORDER_QT_ZH_QM 指向 qtbase_zh_CN.qm。
- Microsoft.Web.WebView2 SDK 1.0.2903.40。解压 [官方 NuGet 包](https://www.nuget.org/packages/Microsoft.Web.WebView2/1.0.2903.40)，将 CMake 的 `RECORDER_WEBVIEW2_SDK` 指向解压目录。课程工作区默认读取 `thirdparty_install/webview2-1.0.2903.40`。Microsoft Edge WebView2 Runtime 用于 Studio 界面。系统无法加载 WebView2 时，程序打开 Qt 工作台。
- FFmpeg 与 ffprobe 的 Windows 版本，需包含 libx264、AAC、rawvideo、f32le、amix、concat、subtitles 等常用能力。当前体验包使用 FFmpeg 7.1.1 essentials，与本机 NVENC 驱动兼容。

## 独立构建

在 wpsrecorder 工程目录中执行，Qt 前缀换成实际安装目录：

```bat
cmake -S docs -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="D:/path/to/vcpkg/installed/x64-windows" -DRECORDER_WEBVIEW2_SDK="D:/path/to/webview2-sdk"
cmake --build build --config Debug --parallel 4
cmake --build build --config Release --parallel 4
```

课程兼容入口为 `docs/CMakeLists.txt`，也可从项目根目录配置。两个可执行文件分别是 `build/bin/Release/wpsrecorder.exe` 和 `build/bin/Release/wpsrecorder_cli.exe`。Debug 的可执行文件在相邻的 `build/bin/Debug/` 中。用匹配配置的 Qt 运行库做本机启动检查，然后从 Release 发布。

构建会同步 `studio/` 到可执行文件旁。网页界面、应用设置、视频和导出均在本机运行。网页预览通过本机回环读取当前应用注册的素材，模型请求由原生服务层处理。可以设置绝对路径环境变量 `WPSRECORDER_DATA_DIR` 以隔离本机设置和草稿。运行独立验收时请同时设置唯一的 `WPSRECORDER_INSTANCE`。

本机课程依赖位于 `WPSwork/thirdparty_install/vcpkg/installed/x64-windows`。源文件和命令行参数使用 UTF-8，编译启用 `/W4`、`/permissive-` 和 `/EHsc`。

## 运行库部署

在 PowerShell 中调用：

```powershell
.\scripts\deploy.ps1 `
  -OutputDirectory .\build\bin\Release `
  -QtPrefix D:\path\to\installed\x64-windows `
  -FFmpegDirectory D:\path\to\ffmpeg\bin
```

脚本先用 windeployqt 部署 Qt 插件，再用 dumpbin 解析导入依赖，递归补齐 vcpkg DLL 和 VC 运行库。脚本默认 VS2022 Community 路径，其他版本使用 -VisualStudio 指定。复制 FFmpeg/ffprobe 到程序同目录，运行时不依赖开发机 PATH。

vcpkg 的 Qt 构建没有 ANGLE DLL，部署参数禁用 ANGLE/software OpenGL 自动复制，并保留本机 Windows D3D 能力。临时目录放在输出目录下，避免 Qt 工具在受限 TEMP 目录失败。打包脚本不包含部署临时目录和 QA 程序。

## 自动验收

构建独立验证窗口：

```bat
cmake -S . -B build -DBUILD_QA_TARGETS=ON
cmake --build build --config Release --parallel 4
python tests/e2e.py --bin build/bin/Release --output D:/temp/recorder-qa --evidence D:/temp/recorder-qa-evidence --long-seconds 600
```

Python 验收环境需要 NumPy 测量解码音频和画面变化。本地 Audio8 的 Python 依赖随完整发行包提供。测试写入独立的新目录，使用自有动画窗口和低音量测试音测量真实采集、编码与解码。长测会使用实际桌面，运行前准备可录制的测试画面。

## 打包

先完成部署和验证，再运行：

```powershell
.\scripts\package.ps1 `
  -RuntimeDirectory D:\path\to\build\bin\Release `
  -Sample D:\path\to\sample.mp4 `
  -Destination D:\path\to\delivery\release-1 `
  -FFmpegRoot D:\path\to\ffmpeg `
  -QtPrefix D:\path\to\installed\x64-windows
```

Destination 使用仓库外的新目录。课程提交文件为 `gongweichao.zip`，根目录为 `exe_cli/` 和 `sample/`。前者包含两个可执行文件和运行依赖，后者包含三分钟以内的真实录制样片。校验清单留在 ZIP 同级，源码、构建说明和用户指南随 Git 仓库提供。

将整个 ZIP 解压到 Windows 下载目录，双击 `exe_cli/wpsrecorder.exe`。自动评测可调用 `exe_cli/wpsrecorder_cli.exe`。ZIP 是完整运行目录分发，Qt/VC、插件、Studio、FFmpeg 和语音资源跟随程序一起解压。课程打包指南要求在另一台同学的机器上验收。可用同版便携试用包进行，该包把程序放在同名文件夹中并附「开始使用.txt」。

设置保存在当前用户 `LocalAppData/WPSCourse/wpsrecorder/ui-settings.ini`，与解压位置独立。提交 ZIP 使用公开运行依赖和样片。

## 2.0 可选工具与验收

摄像头使用 Windows SDK Media Foundation，无额外摄像头 SDK。NVENC/QSV/AMF 必须由本机驱动实际支持；FFmpeg 含编码器不等于可用。该机器上的 FFmpeg 9.0.2 与现有 NVENC API 不匹配，验证选择独立 FFmpeg 7.1.1 构建，未修改驱动或系统安全设置。

离线字幕的依赖准备脚本会校验固定 SHA256 并在指定目录编译，不需要将源码、模型或二进制加入 Git：

```powershell
.\scripts\setup-optional-tools.ps1 -RuntimeDirectory D:\path\to\build\bin\Release -ToolsDirectory D:\path\to\local-tools
```

可选 `-CompatibleFFmpeg` 将经过本机兼容测试的 7.1.1 复制到 runtime。字幕工具默认采用 AVX2 CPU 构建，模型为多语言 base。分发包中含 whisper 子目录时，打包脚本会带上该目录；其中应包含 whisper-cli、相关 DLL、ggml-base.bin、LICENSE 与来源说明。

```bat
python tests/upgrade.py --bin build/bin/Release --output D:/temp/new-v2-check --evidence D:/temp/new-v2-evidence --speech D:/tools/whisper.cpp/samples/jfk.wav
python tests/stability.py --bin build/bin/Release --output D:/temp/nvenc-600.mp4 --evidence D:/temp/new-nvenc-evidence --seconds 600
```

upgrade.py 默认要求真实摄像头通过；设备访问受阻时，可显式加 --skip-camera，结果中会保留 camera_acceptance_pending，不能表述为全部设备通过。多个验证进程可用不同 WPSRECORDER_INSTANCE 隔离服务，正常运行时应清除此变量。

## 摄像头专项验收

开发目录的 Low 文件标签会限制摄像头。应将包含 `recorder_camera_qa.exe` 的 QA 运行目录压缩，再通过正常资源管理器解压到工作区外的普通用户目录，运行原生验收窗口。若 Windows 提示未知发布者，由用户核对本地编译来源并决定是否运行；不要修改系统防护规则。

点击“开始本机验收”会在该运行目录的新 `camera-qa-时间` 子目录录制 10 个短片：仅摄像头、矩形画中画与双音轨和暂停、镜像圆形、带边框的圆角与位置、运行中切源、三次重启，以及真实后台 CLI 的仅摄像头和画中画分轨。摄像头媒体留在本机，不放入源码仓库或交付样片。`camera-acceptance.json` 记录源分辨率、启动完整性、配置、帧数及阶段；出现超时或错误不能记为通过。

```bat
python tests/verify_camera.py --bin D:/path/to/runtime --recordings D:/path/to/camera-qa-time --evidence D:/temp/camera-verified.json
```

独立分析会完整解码计数，检查主画面/摄像头视频时长与帧数、像素变化、镜像方向、画中画位置及裁剪、形状边角、边框、暂停、双音轨和运行中切源。只把指标 JSON 放入仓库；分析不会输出摄像头截图。

UI 使用 Qt Fusion 和本地矢量图标。2.0.2 在 Qt 构造前声明 Per-Monitor V2 DPI 感知；Qt 控件几何保留原生像素，界面设计尺寸按系统 DPI 和可用工作区计算，采集/选区/光标采用同一物理坐标。不要移除显示坐标初始化而只修改 Qt 字体缩放。未加入远程素材或网页依赖。打包排除 `recorder_*qa*.exe`，只分发用户程序。

`recorder_camera_qa` 同时检查实际主画面和摄像头预览 JPEG 可解码、尺寸上限及序号连续增长；暂停显示最后录制画面。`verify_camera.py` 在新报告含 preview_valid 时要求它通过。

## 多屏与讲解交互回归

```bat
build/bin/Release/recorder_ui_qa.exe D:/temp/region-native.json
```

程序短暂打开并自动关闭选区/讲解窗口，检查当前真实显示器的原生边界、每块屏幕底部鼠标命中、反向框选、框内命中、拖边角、整体移动、确认/恢复/取消、跨屏物理坐标、绘图状态下工具栏的真实输入孔和原生命中、撤销、收起和两次 Esc 退出。它通过 Qt 事件驱动自有控件，不操作其他应用。测试依赖交互式桌面；全部通过返回 0。未连接的分辨率/缩放组合仍需其他设备验证。

## 2.1 可视化剪辑验收

`BUILD_QA_TARGETS=ON` 同时构建 `recorder_editor_qa.exe`。先按前述步骤部署完整运行库，再将 QA 程序放入同一测试运行目录：

```bat
python tests/editor.py --bin D:/runtime --output D:/temp/editor-new --evidence D:/temp/editor-new.json
D:/runtime/recorder_editor_qa.exe "D:/temp/editor-new/source 双音轨.mp4" D:/temp/ui-new.json D:/temp/ui-new.mp4
```

核心检查实际解码画面、烧录与可选字幕、音频 RMS、选轨与混音、非法计划、输出保护和原文件哈希。原生界面检查定位、拖框、区间操作、字幕排序、播放切点、声音监听和导出关闭保护。额外 `--asr` 模式需要第二音轨包含公开英文语音，验证选轨识别集成。结果边界和失败修复记录见 [2.1.0 验收](../validation/VALIDATION-V210.md)。

发布后运行 `python tests/verify_package.py --archive D:/delivery/gongweichao.zip --installed D:/installed/runtime --output D:/temp/package.json`，检查 ZIP CRC、全部 manifest 文件哈希、安装运行库一致性、系统 PATH 下 CLI 版本，并拒绝包内个人设置、QA 程序及样片之外的媒体。

## 2.2 进阶与离线语音

先部署 Qt、FFmpeg 和 whisper-cli，再复制 Audio8 0.1B 便携运行包并准备 Whisper small Q5 模型：

```powershell
.\scripts\setup-speech-tools.ps1 -RuntimeDirectory D:\runtime -ToolsDirectory D:\tools\speech -Audio8RuntimeDirectory D:\audio8
```

脚本校验 Whisper small Q5 文件并部署所选的 Audio8 运行包。程序构建会部署普通话女声音色配置。完整体验包包含两种本地识别模型和 Audio8 便携运行环境，各资源保留许可和来源说明。

BUILD_QA_TARGETS还生成recorder_workbench_qa、recorder_narration_qa；前者用真实主窗口录制，后者验证取消/配音/剪辑计划合成。所有QA在正式打包时排除。具体参数及测试边界见[2.2.0 验收](../validation/VALIDATION-V220.md)。测试输出使用独立目录；历史记录见[验收索引](../validation/README.md)。
