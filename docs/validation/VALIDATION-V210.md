# 2.1.0 可视化剪辑验收

日期：2026-11-03。Windows 11 x64、Qt 5、FFmpeg 7.1.1、本地 whisper.cpp 多语言 base。此版本集中完善剪辑；录制采集链路保持既有实现。原始记录位于 `evidence/2.1.0/`，媒体测试产出不进入 Git。

## 已完成检查

| 检查 | 证据和结果 |
|---|---|
| 剪辑导出核心 | `editor-final.json`：7 项通过。6 秒 640×360/30fps 双音轨素材，保留两段并裁到 480×240，得到 3 秒、90 帧。源文件 SHA256 不变。 |
| 实际画面映射 | 解码成片 0.2/2.2 秒，与源视频 1.2/4.7 秒的对应裁剪画面比较，像素 MAE 小于 10。 |
| 字幕 | 中文烧录画面有实际像素差；可选字幕轨读回，跨删除部分的字幕被切开并重新计时。 |
| 独立音轨 | 只剪麦克风时，被剪区间 RMS=0，系统声仍有信号；混音后仍保留系统声。分别验证只开一轨和全部关闭。 |
| 输入保护 | 5 类非法计划被拒绝；已有输出拒绝覆盖，原素材不变。 |
| 原生界面交互 | `ui-release-final.json`：缩略图/波形、两次实际解码定位、画面拖框、保留/删除/撤销、声音分轨与人工字幕、字幕排序后连续修改、播放跳过删除部分、试听静音和导出。 |
| 预览稳定性 | 更换受控解码后 `ui-decode-1/2/3.json` 连续三轮通过；`ui-final-pass.json` 测得四帧缓存上限并验证播放画面继续变化。最终发布检查另见 `ui-release-final.json`。 |
| 自动字幕选轨 | `asr-final.json`：两条声音轨，第一轨为测试音，第二轨为公开英文语音。编辑器选择第二轨识别到预期词，再导出带字幕的 12 秒视频。 |
| 录制与旧剪辑回归 | `regression-final/acceptance.json`：15 项通过，覆盖 CLI、真实窗口/区域采集、系统声信号、暂停继续、三次启停、容器和旧剪辑。持续录制参数为 30 秒，实际成片 30.8 秒；脚本沿用 stability-10min 文件名，不代表本轮做了十分钟长测。 |
| 构建 | Visual Studio 2022 x64，最终 Release GUI/CLI/剪辑 QA 与 Debug GUI/CLI 编译通过。 |

## 本轮修复

1. 多段 concat 后显式设置源平均帧率，修复滤镜时间基导致编码质量下降；通过实际解码画面对比确认。
2. 原 Qt 系统视频后端曾出现播放头增长而画面不刷新。没有将偶发成功当作通过：改用 FFmpeg 有界队列解码，按展示帧推进播放头，缓冲时暂停声音，源视频跳切后重新同步。删除了该工作台中旧的视频 surface 路径。
3. 暂停拖动采用单帧解码并对齐源帧网格；补齐片尾重播和切点后的定位处理。
4. 时间轴高度按声音轨数适配，双轨均能看到；视频删除区间同步压暗字幕和声音轨。
5. 字幕改时间造成排序变化后重新定位选中项，防止第二次编辑改错字幕。
6. 异步分析用来源代数过滤过期回调；导出使用快照，关闭忙碌任务提示等待，退出程序可保存未保存工程。

历史失败记录保留用于说明定位过程：`ui-final.json`、`ui-repeat-1.json` 对应已移除的视频后端；不是本版本验收通过证据。首次基础回归目录因缺少专用测试窗口未能开始，最终回归使用补齐 QA 依赖的新目录。

## 复现

```bat
cmake -S . -B build -DBUILD_QA_TARGETS=ON
cmake --build build --config Release --parallel 4
python tests/editor.py --bin D:/runtime --output D:/temp/editor-new --evidence D:/temp/editor-new.json
D:/runtime/recorder_editor_qa.exe "D:/temp/editor-new/source 双音轨.mp4" D:/temp/ui-new.json D:/temp/ui-new.mp4
python tests/e2e.py --bin D:/runtime --output D:/temp/regression-new --evidence D:/temp/regression-new-evidence --long-seconds 30
```

`editor.py` 的实际源文件名为 `source 双音轨.mp4`，命令行含空格时需加引号。QA 程序只用于本机验收，不进入体验包。Python 验收依赖 NumPy；软件本身不依赖 Python。

自动字幕验收模式为 `recorder_editor_qa.exe 输入视频 证据.json 新成片.mp4 --asr`，测试素材第二条声音轨需含公开英文样例语音，第一轨为非语音对照。

## 边界

本轮是实际 Windows 原生窗口和 FFmpeg 解码检查，没有把它表述为所有机器、所有比例、小时级编辑、复杂 VFR 或任意媒体兼容性验收。预览上限 960×540/30fps，导出按支持的源平均帧率；声音试听采用位置校准，未声称样本级无延迟监听。字幕内容仍须人工校对。完整剪辑范围和操作见 [EDITING](../guide/EDITING.md)。

既有摄像头、逐屏 DPI、讲解与存储验收见 [2.0.2](VALIDATION-V202.md)，并非本轮再次覆盖所有摄像头组合。用户已确认现有录制功能足够，早期尚未实现的扩展已退出范围。
