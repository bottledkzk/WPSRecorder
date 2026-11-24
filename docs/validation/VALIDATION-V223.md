# 2.2.3 剪辑与录制控制验收

Windows 11 x64、Qt 5.15、FFmpeg 7.1.1。本版扩展录制控制和非破坏性剪辑，保留 2.2.2 的屏幕自动探测、HDR 映射及预览导航。

- **录制控制**：相机与麦克风图标在主工具栏、对应设置页及悬浮条同步。真实摄像头和麦克风在 6.467 秒测试录制中从关闭切换到开启，再切回关闭；成片有独立、等时长的麦克风轨。关闭麦克风后的样本静音，已打开的设备留到录制结束以便重新打开；关闭画中画则释放摄像头，纯摄像头源或独立摄像头轨仍需保持设备。录制预览的删除只针对最近生成的文件，确认后移到回收站。
- **剪辑**：原生 UI 14 项检查通过，包括数字输入 480×270 即时联动及边角缩到 384×216，仍为 16:9。原时间轴核心 7 组通过。新增 225 帧、7.5 秒的独立解码检查验证默认静止画面、本地图片、字幕烧录/软字幕和外部轨剪切，字幕颜色像素及音频静音窗口均有断言，输入素材哈希未变化。
- **配音**：20 个预制 Kokoro 试听逐个在原生配音窗口载入；语言切换后 Windows SAPI 音色与本机安装语言匹配。本机只有中文 Huihui 和英文 Zira，均为女声。原生配音、取消后恢复和导出通过；不承诺其他机器具备相同 Windows 音色。
- **本地演示**：指定游戏窗口与摄像头实录 122.667 秒、1840 帧，18 补帧，NVENC。由实录选段、2.8 秒默认静止画面、7 段中文 Kokoro 和字幕导出 60.50 秒/907 帧、1216×684、H.264/AAC。完整解码及 6 个时点的非黑小窗检查通过。带摄像头的原片、成片和工程仅留本地，不随仓库或课程体验包分发。
- **边界**：时间轴以源视频时间定位，插入静止画面时原轨静音。外部音频解码与导出由 FFmpeg 完成，预听由本机媒体组件支持。ASS 只导入第一个有效 V4+ 样式的字体、字号、主色和透明度；软字幕不强制播放器显示样式。

测试入口：`tests/timeline_assets.py`、`tests/editor.py`、`recorder_editor_qa.exe` 和 `recorder_narration_qa.exe`。发行包使用无个人画面的既有真实录屏样片。

默认静止画面 `src/assets/intermission.png` 由内置 imagegen 生成。提示词：

> Create a widescreen 16:9 production intermission still image for WPS Recorder, a clean Windows screen recording and editing app. Dark graphite-to-slate background, restrained cyan-blue accents, a centered sculptural camera lens in a rounded square with a tiny red recording indicator, subtle concentric recording ripples, soft shadows, and quiet negative space at the bottom for an optional subtitle. Modern desktop app aesthetic, crisp balanced layout, no text, no letters, no logos, no watermark. Edge-to-edge composition.
