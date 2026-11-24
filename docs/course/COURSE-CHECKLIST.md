# WPS Recorder 2.3.9 课程提交与打包核对

现行包见 [2.3.11 验收](../validation/VALIDATION-V2311.md)。下面保留 2.3.9 的课程要求逐项核对及同学反馈。

2026-11-23 对照《大项目：wpsrecorder 屏幕录制工具开发》第 1–5、7–8 页和《VS Qt 打包发布方法》第 1–4、12 页核对。版本实测按 [2.3.9 验收记录](../validation/VALIDATION-V239.md) 追溯，历史项目按 [验收索引](../validation/README.md) 追溯。

## 提交物

| 要求 | 2.3.9 交付 |
| --- | --- |
| 课程 Git 的 `ClassExamProject/wpsrecorder` 包含源码、文档和项目说明 | [源码与项目入口](../../ReadMe.md)、[设计说明](../DESIGN.md)、[课程 CMake 入口](../CMakeLists.txt)、[构建分发](../reference/BUILD.md)、[CLI](../reference/CLI.md)。C++17、Qt、FFmpeg，Studio 页面与主题位于 `studio/`。 |
| 绿色运行包采用姓名拼音命名 | `gongweichao.zip` 的根目录是 `exe_cli/` 和 `sample/`。`exe_cli/` 包含 `wpsrecorder.exe` 与 `wpsrecorder_cli.exe`、Qt / VC 运行库、插件及本地语音工具。 |
| 至少一段 3 分钟以内的真实样片 | `sample/screen-recording.mp4`，126.767 秒，2560×1440、30 fps。由 WPS Recorder 实录和剪辑导出，内嵌中英字幕。 |
| 源码提交与运行物提交 | EXE、DLL、模型、提交 ZIP 和录制样片留在交付目录。运行物通过课程收集表提交。 |
| Debug 自检、Release 发布 | 从 `docs/CMakeLists.txt` 构建 Debug 两个目标并启动核对。Release 提交包的 3594 个文件逐项核对 SHA-256。 |
| 无开发环境也可运行 | 按任务书的绿色目录方式交付，完整解压即可运行。可在本机只使用系统 PATH 启动试用包中的同版 Release GUI 和 CLI，已实录并解码。Studio 使用系统 WebView2 Runtime，加载失败时可进入 Qt 工作台。同学异机试用通过。 |

## 录制、剪辑和命令行

| 任务书条目 | 实现与证据 |
| --- | --- |
| B1 全屏、连续录制 | 完成实体副屏 1920×1080、60 fps、6000 kbps 连续录制 602.2 秒。可手动启动、查状态和停止，文件完整解码 36,130 帧。 |
| B2 参数与封装 | 支持可配置分辨率、帧率、码率以及 MP4、MOV、AVI、FLV、MKV。WAV 可独立导出音频。按把 WAV 也列为录制格式的字面要求，此项有差异。 |
| B3 音频 | WASAPI 麦克风、系统声、混录或双独立音轨。历史全链路包含真实音源、不同录制方式和解码检查。2.3.9 剪辑回归复核了双轨独立剪辑、混音和静音。此次 10 分钟录制关闭音频。 |
| B4 控制界面 | 工作台或悬浮条控制开始、暂停、继续、停止，查看计时和状态，停止后可在回看中定位作品。 |
| B5 播放和剪辑 | 回看支持播放、暂停、拖动进度、音量、倍速、全屏和分节。剪辑支持分割后移除首尾或按段导出，视频多段顺排合并。2.3.9 验证了裁剪、分割与重排导出。 |
| B6 来源和其他控制 | 支持窗口、区域、暂停继续、3 / 5 秒倒计时、光标与点击高亮、截图和设置持久化。Studio 的源设置提供点击高亮开关，颜色和半径目前只在 Qt 工作台可调。2.3.9 实录复验窗口、摄像头和画中画。原生选区、暂停、截图及配置等验证见 [此前验收](../validation/VALIDATION-V226.md)。 |
| CLI 自动评测 | 提供 `--version`、`--record --duration N`、`--start`、`--stop`、`--status`。录制中与空闲的状态都以单行 JSON 输出，包含 `state`、`duration_sec`、`frames_written`、`avg_fps`、`mem_mb`、`cpu_pct`。成功退出 0，失败非 0。缺省尺寸、帧率、音源为 1920×1080、30 fps、none。当前 CLI 省略 `--bitrate` 时使用固定 6000 kbps，任务书要求按画质自动。 |

选做功能包含常驻悬浮条、摄像头画中画、讲解标注、本地语音识别、字幕编辑与烧录、AI 配音混音。2.3.9 具备本地 Audio8 0.1B 普通话女声和可配置的在线配音。进入相应工作区的操作及设置见 [完整功能介绍](../guide/FEATURE-INTRODUCTION.md)。

## 同学异机验收

2026-11-23 收到反馈，同学在另一台电脑上试用 2.3.9 Windows x64 便携包，未发现问题。试用包的程序和运行库与课程包一致。设备型号和逐项测试明细未提供。
