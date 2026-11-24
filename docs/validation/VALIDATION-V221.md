# 2.2.1 播放、控制与预览修复验收

2026-11-03，Windows 11 x64，Ryzen 9 7940HX、RTX 4060 Laptop、Qt5、FFmpeg7.1.1。源码与指标进入Git；测试媒体和摄像头画面仅留本地临时目录，交付同步后定向删除。原有用户录制不属于测试产物。

## 六项反馈

| 反馈 | 修复与验收 |
|---|---|
| 下一帧在n.m99停住 | 原实现把毫秒向下取帧号、再向下取毫秒，333ms可能再次变成299ms。改为先确定整数帧号，再换算时间；按钮和方向键共用计算。editor-ui.json验证24/25/29.97/30/59.94/60/120fps共1680次连续按钮操作，以及片尾边界。decoded-frame-step.json独立解码比较第9/10帧，像素MAE均为0，画面哈希不同。 |
| 第二条滑杆无反应 | 它用于放大后的时间轴横向平移，不缩放视频画面。增加“时间轴缩放”“位置”标签及提示；1×禁用位置条，4×可平移至后段。editor-ui.json验证实际时间轴坐标随平移变化。 |
| 全屏缺少控件 | 独立全屏窗口加入进度、播放/暂停、已播/总时长和退出；空格、左右键、Esc可操作。复用原播放器及同一视频控件，不重新绑定解码输出。验收发现的返回播放位置丢失已修复；两个真实GUI实录场景均验证拖动、播放、暂停及返回后位置保持。 |
| 悬浮条入口和状态 | 主工具栏直接显示“悬浮条：开/关”，开启浅红高亮，可录制前设置、录制中显示/隐藏、收起后唤回，并保存偏好。讲解开关与退出事件同步；聚光、放大、白板的高亮状态在选择画笔后仍保持正确。ui-native.json、workbench.json通过。 |
| 显示器名称笼统 | QueryDisplayConfig与DisplayConfigGetDeviceInfo读取系统目标友好名，失败回退EnumDisplayDevices/GDI设备名。当前读回B160QAN02.7与G72 Max，与Windows WmiMonitorID一致；保留主显示器标记、分辨率、DPI与负坐标。名称查询不进入逐帧几何枚举。 |
| 预览卡顿 | 原设计约150ms采样、160ms请求，设计上限约6fps，还需JPEG/Base64编解码。新previewchannel独立缩放线程、单帧待处理槽、Windows共享内存BGRA、GUI约33ms读取最新帧；忙时跳过，不累积队列。摄像头与最终合成画面复用采集结果；旧JSON preview接口兼容。 |

## 实测和回归

| 场景 | 结果 |
|---|---|
| 640×360/30fps画中画+双音轨+原始摄像头轨 | 4250ms采样中GUI收到101个新序号，23.76fps；间隔P95为70ms。183帧主视频与183帧摄像头轨全部解码通过；位置拖动前后像素比较MAE约2.57/2.60。 |
| 1920×1080/60fps画中画+双音轨+原始摄像头轨 | 4251ms采样中GUI收到95个新序号，22.35fps；间隔P95为70ms。主/摄像头各363帧完整解码。主视频容器平均帧率约60fps，时长约6.05秒。 |
| 基础录制 | regression/acceptance.json共15项通过：自有窗口真采集、系统音与麦克风、混音、暂停/继续、格式、区域、旧剪辑、三次重启及稳定性。稳定性本轮30.7秒/921帧，重复帧0；采样峰值约316MB，CPU约4.69%。保留旧stability-10min文件名，不代表本轮重跑十分钟。 |
| 同屏剪辑 | editor-core.json七组独立解码回归；editor-ui.json新增逐帧/平移/实帧检查后共13项，覆盖原有裁剪、声音轴、字幕、撤销及成片导出。 |
| 原生控件 | 混合150%/125%DPI屏幕、底部框选、标注工具命中、宽度/橡皮/撤销、效果开关、画中画拖动及浮条控制通过。主界面与编辑窗口实际巡检见gui-visual.json；全屏交互由应用原生验收驱动验证。 |
| 构建与交付 | Release/Debug GUI及CLI通过，二进制哈希见build.json。包与安装目录逐文件校验、仅系统PATH启动结果见package-verification.json。 |

上述预览帧率是短时、本机、GUI接收的新画面数量，不是显示器呈现率或跨设备保证；旧6fps是设计上限，不是同条件跑出的对照成绩。预览上限约30fps、960×540，独立于成片帧率和尺寸。摄像头源、全分辨率合成与编码仍可能成为慢设备或4K场景的瓶颈。

## 实现范围

当前录制链路是Windows采集→CPU QImage合成摄像头/标注→编码器，NVENC加速编码。预览取最终合成结果，独立线程和共享内存移除了常规预览的JPEG往返。OBS采用Direct3D11/OpenGL图形体系及显示绘制回调，见[官方渲染文档](https://docs.obsproject.com/graphics)；本轮没有移植libobs、全GPU合成或零拷贝编码。

变量帧率视频仍按探测的平均帧率网格逐帧；未对任意原始PTS序列、更多显示器、其他GPU、小时级录制或另一台洁净电脑作本轮验收。2.2.0语音和配音能力沿用已有证据，本轮不重复声称重跑全部语料。

## 复现

```bat
python tests/editor.py --bin D:/runtime --output D:/temp/new-editor --evidence D:/evidence/editor-core.json
D:/runtime/recorder_editor_qa.exe D:/temp/new-editor/source.mp4 D:/evidence/editor-ui.json D:/temp/editor-ui.mp4
python tests/verify_frame_step.py --bin D:/runtime --source D:/temp/new-editor/source.mp4 --editor-output D:/temp/editor-ui.mp4 --evidence D:/evidence/frames.json
D:/runtime/recorder_workbench_qa.exe D:/temp/new-workbench D:/evidence/workbench.json
D:/runtime/recorder_workbench_qa.exe D:/temp/new-hd D:/evidence/workbench-hd.json --hd
```

editor.py实际生成的文件名含“source 双音轨”，请按生成路径传参。工作台验收需要用户允许短时摄像头/麦克风实录。QA程序不进入正式包。
