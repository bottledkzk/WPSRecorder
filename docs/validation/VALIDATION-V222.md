# 2.2.2 系统颜色自动适配与预览交互验收

2026-11-03，Windows 11 x64、Qt5、FFmpeg7.1.1。当前双屏为B160QAN02.7（2560×1600、150%、SDR）与G72 Max（2560×1440、125%、HDR）。所有生产代码按系统设备信息处理，不包含上述型号的判断分支。

## 原录制的原因

用户指定中间目录中的video.mkv为49.666秒、1490帧、2560×1440 H.264。解码无报错；50张按秒采样图均有明显紫绿错色，问题已写入码流。原文件未改动，大小与SHA256见evidence/2.2.2/original-diagnosis.json；本地分析帧不进入Git或正式包。

原采集代码把任何D3D纹理都包装为QImage::Format_RGB32。只读探针实际读到：主屏纹理格式87（BGRA8），G72 Max纹理格式10（RGBA16_FLOAT），输出颜色空间12（HDR PQ/Rec.2020）。显示器输出位深与抓帧纹理格式是不同信息。64位浮点像素被按32位BGRA读取，导致通道与像素错读；这不是视频解码器故障。旧码流已经丢失原始像素信息，不能保证完整恢复原颜色。

正常形状却偏亮的另一原因是HDR亮度未归一化。Windows的scRGB单位白为80nit，但本机G72 Max系统SDR内容白电平实际为320nit。直接截断或只转8位会压掉高光、使中间灰偏亮。默认改为读系统白电平进行映射；不是针对G72 Max写死320。计算依据见[微软SDR白电平定义](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-displayconfig_sdr_white_level)，HDR捕获的格式与映射要求见[微软屏幕捕获文档](https://learn.microsoft.com/en-us/windows/apps/develop/media-authoring-processing/screen-capture)。

## 自动与兼容设计

| 项目 | 行为 |
|---|---|
| 屏幕信息 | Windows读回友好名、GDI设备名、位置、分辨率、逐屏DPI，以及输出HDR/位深/白电平；列表标注HDR。读不到友好名时回退系统设备描述。 |
| 采集格式 | DuplicateOutput1协商并兼容回退DuplicateOutput，始终检查实际纹理格式、尺寸和RowPitch；按格式处理，不按屏幕名称判断。 |
| GPU转换 | scRGB FP16归一化并转sRGB；10位PQ线性化并转换至Rec.709；RGBA/BGRA按正确通道读取。输出BGRA8再读回CPU，避免CPU逐像素pow。 |
| 动态变化 | 实际格式变化重建纹理；系统白电平/颜色空间按格式变化或周期刷新；采集丢失限时重连，短时保留最近画面，超时允许兼容回退。没有改写系统HDR或亮度。 |
| 可选调整 | 默认自动。高光压缩、−2～+2 EV、手动参考白均为微调；参考白0继续跟随系统。支持运行中更新、用户配置及旧场景预设兼容。 |
| 输出信号 | RGB→YUV明确采用BT.709矩阵、有限范围与sRGB传递标记，不让播放器猜颜色范围。 |

## 验收结果

- **GPU数值与真屏**：color-native.json的FP16/白电平、曝光、高光和RGBA8/BGRA8/10位PQ数值测试通过；两块实际屏幕色卡通过。主屏直接8位路径，HDR屏自动320nit映射。10位PQ为人工纹理数值测试，本机HDR桌面实际返回的是FP16。
- **实际HDR成片**：7秒、210帧全解码通过；自动、−1 EV、高光压缩按顺序在录制中生效。color-decoded.json独立解码13个时刻的8块色卡：最大通道误差约3.45/255。自动白色约253、灰色约126；−1 EV白色约186、灰色约90；高光压缩白色约238、灰色约126。调整期间发生9个补帧，不作为纯稳定帧率基准。
- **预览操作**：player-native.json八项通过：普通点击、拖动、工作区内控件、放大后点击、完全全屏原生边界/置顶状态、右上×保留位置、Esc，以及第二屏工作区。默认按当前屏幕availableGeometry铺满工作区，控件不进入任务栏区域。完全全屏为可选，不更改任务栏设置。
- **拖动提交**：45ms合并移动请求，松手提交最终时间；异步解码完成后核对真实播放位置和暂停状态。
- **既有回归**：editor-core.json七组、editor-ui.json十三项、逐帧独立解码通过；regression/acceptance.json十五项通过。稳定性本轮30.7667秒、923帧、补帧0；采样峰值约318MB、CPU约4.15%。旧stability-10min文件名不代表本轮重跑十分钟。
- **配置与构建**：五项非法颜色参数拒绝通过；Release/Debug GUI与CLI通过，哈希见build.json。包/安装副本与仅系统PATH运行见package-verification.json及installed-smoke.json。

## 范围与限制

这是基于探测结果的兼容实现，不能等同所有设备均验收。真屏仅当前一块SDR和一块HDR；未主动切换Windows HDR、拔插屏幕、测试克隆显示拓扑、其他驱动/GPU或小时级录制。恢复和动态刷新路径已实现，但这些系统状态变化未做本轮物理操作验收。

HDR映射目前针对能取得DXGI表面的单屏或单屏内选区。跨屏选区/GDI回退、固定窗口PrintWindow不保证相同HDR映射；系统白电平API不可用时使用80nit并以white_detected=false报告，可手动校正。未知格式不会再强制按BGRA解释，而会走兼容回退。成片为SDR H.264，不提供HDR10成片或完整专业色彩管理。摄像头/标注仍使用既有CPU合成，未声称整个录制管线GPU零拷贝。

语音、配音与摄像头样式沿用既有版本，本轮没有重跑其全部语料/设备验收。原用户中间目录持续保留；只在交付推送后清理本轮指定测试目录与验收副本，见cleanup.json。

## 复现

```bat
D:/runtime/recorder_color_qa.exe D:/temp/new-color D:/evidence/color-native.json
python tests/verify_color.py --bin D:/runtime --native D:/evidence/color-native.json --evidence D:/evidence/color-decoded.json
D:/runtime/recorder_player_qa.exe D:/sample/screen-recording.mp4 D:/evidence/player-native.json
```

颜色真屏测试需要至少一块已开启HDR的屏幕，短时显示自有色卡，不自动改变系统设置。QA与用户视频不进入正式包。
