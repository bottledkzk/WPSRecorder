# 2.2.0 进阶功能与交付验收

日期：2026-11-03。当前 Windows 11 x64、Ryzen 9 7940HX、RTX 4060 Laptop、约48GB内存、Qt5、FFmpeg7.1.1。证据保存在 evidence/2.2.0；测试媒体、模型及程序放在 Git 外。

## 进阶验收

| 项目 | 证据与结果 |
|---|---|
| 悬浮条/画中画/标注控件 | ui-native-release.json：混合DPI底部选区、工具栏输入孔、绘图/撤销、粗细设为10的值、整笔橡皮及撤销/重做、折叠/退出、预览留白下的画中画拖动与缩放、悬浮条信号/状态和捕获排除。 |
| 真实录制控制 | workbench-minimized.json：真实主窗口启动摄像头画中画/双音轨，拖动改变参数，最小化主窗口后浮条仍可暂停、截图、继续、停止，停止自动加载视频。 |
| 画中画成片位置 | workbench-minimized-pixels.json：分别解码拖动前后主画面及对应原始摄像头帧，重建取景后比较，验证位置确实改变，而非只移动监看窗。 |
| 摄像头专项 | camera-native.json、camera-verified.json：10项实录完整解码，主画面/独立摄像头各874帧。仅摄像头、矩形/圆形/圆角、镜像、位置/边框、暂停、运行中切源、三次重启及后台CLI。双轨读回mic/system，主画面/原始轨对应。 |
| 中英配音 | speech-final.json：Kokoro中文女/男声及英文、语速、音量；Windows中文Huihui/英文Zira；WAV可解码并有有效信号。 |
| 旁白合成 | speech-final.json：在2秒处插入，前段静音与后段信号检查，原声混音/替换；成片20秒、单混音轨，视频包哈希保持不变。拒绝超出片尾、覆盖和非法速度，源文件哈希不变。 |
| 公开多语言字幕 | speech-final.json：SenseVoiceSmall官方仓库中英日三段短语音，经转视频后用small Q5识别；SRT格式与预期关键词检查通过。是通路检查，不是CER/WER准确率评测。 |
| 配音窗口与剪辑集成 | narration-ui-final.json：取消语音任务后重新启动、修改文字时禁用过期试听/保存、原生按钮生成试听、可保存/播放状态、当前裁剪与17秒保留计划及字幕共同导出，单条混音轨，忙碌关闭保护。 |
| 可视化剪辑窗口 | editor-ui.json：继承定位/裁剪/保留删除/撤销/分轨/字幕重排/播放跳切/监听/导出，并增加字幕删除实际操作检查。 |

暂停边界可能有一个已经在途的视频帧；本轮摄像头专项观测到1帧，录制期间保持暂停后不持续增长，主视频与原始摄像头轨同步。未把此边界写成完全零帧瞬时响应。

## 基础回归与构建

- editor-core.json：7项通过，包括成片90帧、画面裁剪映射、中文硬/软字幕、独立声音静音与混音、选轨和输入保护。
- regression-final/acceptance.json：15项通过，持续录制本轮为30.733秒/922帧；沿用stability-10min文件名，不代表重跑十分钟。
- asr-selected-track.json：新版small模型在第二条音轨识别公开英文语音并导出字幕，通过。
- Release与Debug GUI/CLI构建通过；最终GUI仅补按钮中文与过期试听保护，其对应原生回归单独重跑。
- gui-visual.json：实际观察剪辑/字幕区、识别设置弹窗和配音弹窗，控件可见、样式一致。

## 性能与实现取舍

- 录制沿用DXGI缓存、GPU探测回退、有界编码管道和增量声音落盘。旧600秒NVENC证据见VALIDATION-V2；本轮没有重跑小时级测试。
- 录制监看复用设备、缩略图约6fps；画中画坐标按实际预览图像矩形换算。
- 剪辑采用960×540/30fps预览与最多四帧队列，保留实际显示画面和播放头同步；成片仍按支持的源平均帧率导出。
- 自动字幕与配音/导出在工作线程运行，日志限长，取消时结束子进程并删除临时目录。配音的基础剪辑按需重编码，随后添加声音复制视频流；不能据此承诺任意素材无需重编码。
- Kokoro本机约6.81秒中文生成全流程17.42秒；Windows相应文字约0.89秒。两者音色和时长不同，仅报告所测流程，不提供固定倍数或音质评分。详情见SPEECH。

## 发布、原始记录与范围

包内只含用户程序、Qt/VC/FFmpeg、离线语音工具/模型、许可、文档和3分钟以内的自有窗口真实录制样片；摄像头媒体、测试程序、个人配置和其余测试媒体不进入包或Git。package-verification.json记录最终ZIP CRC、manifest哈希、安装副本一致性和仅系统PATH运行；cleanup.json记录交付同步后才执行的定向清理。

临时清理保留旧正式包、源码、构建依赖和用户原有录制。用户原有 `recording-20261102-222843-623.mp4` 不属于测试产物，不删除。

本轮仅一款UVC摄像头、当前主副屏与GPU；未覆盖其他设备、拔插、磁盘写满、所有快捷键冲突、小时级编辑或另一台洁净电脑。法语等语言有选择入口和底层模型支持，但未做该语言语料验收；日语样例仍有识别错误。Kokoro未做专业主观音质测试。其他早期扩展不在本轮范围。

## 复现入口

```bat
python tests/speech.py --bin D:/runtime --public-speech D:/public-speech --output D:/temp/new-speech --evidence D:/evidence/speech.json
D:/runtime/recorder_workbench_qa.exe D:/temp/new-workbench D:/evidence/workbench.json
python tests/verify_workbench.py --bin D:/runtime --report D:/evidence/workbench.json --camera-report D:/evidence/camera-native.json --evidence D:/evidence/pixels.json
D:/runtime/recorder_camera_qa.exe --auto D:/temp/new-camera
python tests/verify_camera.py --bin D:/runtime --recordings D:/temp/new-camera --evidence D:/evidence/camera.json
D:/runtime/recorder_narration_qa.exe D:/source-20s-640x360.mp4 D:/evidence/narration.json D:/temp/narrated.mp4
```

测试目录必须为新路径。摄像头/麦克风实录需要用户授权和普通用户进程；QA不改变隐私、安全或防护设置。公开语音来源、模型版本及SHA256见SPEECH和setup-speech-tools.ps1。QA可执行文件不随正式包分发。
