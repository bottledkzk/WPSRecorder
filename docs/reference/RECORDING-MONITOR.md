# WPS Recorder 后台性能监测

`scripts/monitor_recording.py` 以约 1 秒一次的频率读取 2.2.6 的录制状态。运行监测时，在已打开的录屏软件与目标程序中正常操作。先用 60 秒试测确认路径和权限。正式对比可按同一场景各测无录屏、正常录屏、高负载录屏三组，每组 3 轮、每轮 10 分钟。每轮使用独立的报告目录。

`--bin` 指向测试版本里含 `wpsrecorder_cli.exe`、`ffmpeg.exe` 和 `ffprobe.exe` 的目录。下面的命令从 `D:\Study\Work\WPSwork` 运行，先在任务管理器“详细信息”中核对游戏或演示程序的 PID。

```powershell
$project = 'D:\Study\Work\WPSwork\gongweichao\ClassExamProject\wpsrecorder'
$bin = Read-Host '测试版本的程序目录'
$presentMon = 'D:\Study\Work\WPSwork\tmp\wpsrecorder-followup-20261114\tools\PresentMon-2.6.0-x64.exe'
$evidence = 'D:\Study\Work\WPSwork\tmp\recorder-high-load-run-01'
$targetPid = [int](Read-Host '目标应用的 PID')
python "$project\scripts\monitor_recording.py" sample --bin $bin --out $evidence --seconds 60 --label smoke --presentmon $presentMon --target-pid $targetPid
```

Windows 的 ETW 实时帧耗采集要求管理员权限或 `Performance Log Users` 组权限。PowerShell 权限要足以访问正在使用的本机录制服务。暂时只测录屏时可省略 `--presentmon` 和 `--target-pid`。PresentMon 启动失败时，记下 `presentmon.log` 中的原因，仍会保存录屏数据。对仅使用软件 GDI 绘制的目标，可能没有可用的 PresentMon 帧事件。

停录后用下面的命令离线核对写入的视频帧与画面变化。离线解码耗时不进入录制负载。

```powershell
$recorded = Read-Host '已完成录像的完整路径'
python "$project\scripts\monitor_recording.py" analyze --bin $bin --media $recorded --out "$evidence\media.json"
```

| 文件 | 用法 |
| --- | --- |
| `summary.json` | 录屏状态转换、采样峰值、调度补帧比例、非补帧速率、前台进程的帧间隔中位数与 P95/P99。录制期间切换素材或状态时，按同一进程与输出文件分段求增量。 |
| `status.jsonl` | 各时刻的状态、活动时长、录制进程及采集和编码摘要。连接失败时写入 `connected: false`，在汇总的状态转换中显示为 `unreachable`。 |
| `presentmon.csv` | 带时间的原始前台帧事件。汇总默认用样本最多的 swap chain。检查目标拥有多个渲染窗口时的选中链。 |
| `media.json` | 成片完整解码帧数、低分辨率近似画面变化帧数和速率。静止页面和微小变化会影响后两项，用场景稳定的动态素材对比。 |

`frames_written - duplicate_frames` 反映录屏管线交付的非调度补帧。DXGI 复用旧画面或桌面内容本身静止时，画面变化帧率会更低。前台游戏帧耗用 PresentMon 的呈现间隔和显示间隔衡量。对比时固定原应用窗口、操作或回放、GPU 与编码设置、分辨率和刷新率，并用相同采集组合对照。将统计窗口切到较稳定的场景，切换桌面、最小化和暂停单独注明。只记录本机既有状态和指定进程事件。

## 录制管线分段计时

从独立命令行实例启动录制前，将环境变量 `WPSRECORDER_STAGE_TIMINGS` 设为 `1`。例如先设置 `$env:WPSRECORDER_INSTANCE = 'capture-profile'`，再运行 `--start`。停录、封装或失败处理完成时，输出路径旁保存 `.mp4.timing.json`（若使用其他容器则沿用对应扩展名）。日常录制可省略此变量。计时样本在内存累计，成片仍按原有流程保存。

- `dxgi_acquire` 记录获取桌面帧的 API 调用时长。HDR 转换的 CPU 发令时间记录为 `gpu_color_submit`，GPU 执行完成与读回的同步等待计入 `dxgi_readback`。
- `crop_resize`、`capture_copies` 和 `encoder_pixel_conversion` 分别记录读回后的裁剪/缩放、为光标等叠加进行的帧拷贝，以及 NVENC 输入的 NV12 打包。合计采集与合成用时见 `capture_compose`。
- `pipe_buffer` 记录把帧交给 QProcess，`pipe_wait` 和 `repeat_pipe_wait` 分别记录普通帧、调度补帧写入的等待。超时时记录错误详情及剩余待写字节。
- 每个阶段提供样本数、均值、P50/P95/P99 和最大值。分位数取各阶段的前 120,000 个样本，计数与最大值覆盖整段。

本机短测用 7 秒静态图片经 x264 录入。监测识别录制和封装后空闲，成片完整解码 105 帧，近似画面变化计数为 1。另用街霸 6 实战 CSV 核对 PresentMon 2.6.0 的实际列名和 P95/P99，采集器经 Windows 提权启动。 [PresentMon 官方 CLI 与列定义](https://github.com/GameTechDev/PresentMon/blob/main/README-ConsoleApplication.md)。
