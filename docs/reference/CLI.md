# CLI 使用与自动评测接口

## 必需命令

```bat
wpsrecorder_cli.exe --version
wpsrecorder_cli.exe --record --duration 30 --output D:\Videos\demo.mp4 --res 1920x1080 --fps 30
wpsrecorder_cli.exe --start --output D:\Videos\background.mp4 --audio mix
wpsrecorder_cli.exe --status
wpsrecorder_cli.exe --stop
```

--record 在前台等待指定活动时长并保存，Ctrl+C 请求停止并封装；--start 启动后台任务后立即返回，后续进程用 --stop 收尾。--stop 在文件完成封装后才返回，超时或失败返回非 0。重复开始会失败，既有输出不会覆盖。空闲状态下 --status 仍返回合法 JSON。

所有成功命令退出码为 0，错误为非 0 并写入 stderr。状态命令只向 stdout 输出一行 JSON。--version 输出当前程序版本，例如 `wpsrecorder 2.2.6`。

## 录制参数

| 参数 | 默认值 | 含义 |
|---|---|---|
| --output 路径 | 必填 | MP4/MOV/MKV/AVI/FLV 输出文件 |
| --res 宽x高 | 1920x1080 | 偶数尺寸，160x120 至 7680x4320 |
| --fps 数值 | 30 | 1 至 144；实际独立采集帧率另受资源约束 |
| --bitrate 数值 | 6000 | 100 至 100000 kbps |
| --audio 来源 | none | none、mic、system、mix |
| --source 来源 | screen | screen 显示器、region 区域、window 窗口、camera 摄像头、image 图片 |
| --region x,y,w,h | 无 | 区域物理像素矩形，配合 source=region |
| --window HWND | 无 | 十进制原生窗口句柄，配合 source=window |
| --no-cursor | 关闭 | 指定时隐藏鼠标 |
| --clicks | 关闭 | 鼠标按下时高亮 |
| --monitor 索引 | 0 | `--devices` 枚举的显示器下标 |
| --encoder 名称 | auto | auto、libx264、h264_nvenc、h264_qsv、h264_amf；显式选择失败会报错，auto 会回退 CPU |
| --separate-audio | 关闭 | 系统声与麦克风独立音轨，限 MP4/MOV/MKV |
| --camera 索引 | 0 | `--devices` 枚举的摄像头下标 |
| --pip | 关闭 | 摄像头叠加 |
| --camera-track | 关闭 | 独立保存 output.camera.mkv，640×480 与主视频同帧率 |
| --image 路径 | 无 | image 模式输入图片 |
| --preset JSON | 无 | version:1、scene 对象；示例见 presets 目录 |
| --denoise | 关闭 | 录制收尾或导出时执行音频滤镜 |
| --max-duration 秒 | 0 | 自动停止有效时长，0 表示不限制 |

区域录制的 `--region` 直接接收物理像素坐标；GUI 中的拖框和确认操作见[录制选区指南](../guide/USER-GUIDE.md#调整录制选区)。

附加控制：`--pause`、`--resume`、`--service-exit`（只结束空闲服务）。

`--mark` 可在录制和暂停时记下成片时间，`--label` 可附名称。`--markers --input <video>` 可读取保存的标记。`--recover --input <workspace> --output <new-video>` 从录制临时目录恢复 MP4 或 MKV，并保留原始文件。

录制工作台为助手提供了版本化的本地 [任务约定和技能](../../skills/recorder-workbench/SKILL.md)。`--task` 从标准输入读取一个 UTF-8 JSON 对象并将结果写为一行 JSON。模型只需给出任务和结构化参数，素材或输出路径由本地调用方提供。

### 多素材混剪任务

把多素材工程路径传给 `--task --input`。提交 `sequence.smart-cut` 时使用 `args.goal` 传入原始剪辑要求，最多 1500 字符，可指定要保留的操作、画面或菜单。可选的 `target_ms` 指定默认勾选的成片目标时长，0 表示不限。`candidates_per_source` 指定每段素材最多提供的候选数，范围为 1 至 24。省略这两项时从请求中读取，未指定时分别取 45 秒和每段 5 个。`sample_fps` 省略或为 0 时使用 4 FPS 初筛、8 FPS 复核及整片编排。指定 1 至 10 时使用固定采样。结果包含实际采样设置、候选片段、证据毫秒、局部复核及编排状态和各阶段耗时。

```json
{"version":1,"task":"sequence.smart-cut","args":{"goal":"找出每局的反击和终结，剪成约 35 秒，每段素材最多 6 个候选","sample_fps":0}}
```

宿主审阅并调整 `draft.parts` 的顺序、勾选状态与切点后，用 `sequence.apply-cut` 的 `args.draft` 和新的 `--output` 工程路径应用。用 `--edit-project` 导出已应用的多素材工程。过期草稿或必保区间缺失会返回错误。

生成的 `selected_duration_ms` 为实际勾选片段的总时长。应用调整后的草稿时，以 `sequence.apply-cut` 返回的 `duration_ms` 为准。

智能剪辑的 `edit.subtitle-search`、`edit.smart-cut` 和 `edit.apply-cut` 也走上述任务协议。用 `--input` 选本地视频或工程。工程能提供已有字幕、必保区间和剪切状态。`edit.smart-cut` 可用 `--output` 另存草稿；`edit.apply-cut` 接收该草稿对象并将已确认的成片计划另存为工程 JSON。使用 `--edit-project` 可渲染计划，或在桌面剪辑窗口打开继续编辑。视频理解的临时密钥可以从 `WPSRECORDER_VIDEO_KEY` 环境变量读取。

字幕审核同样使用 `--task --input 工程.json`。例如向标准输入传入以下 JSON。

```json
{"version":1,"task":"edit.subtitle-review","args":{"first":3,"last":8,"goal":"统一术语，检查数字和单位"}}
```

`first`、`last` 是从 1 开始的闭区间，默认审核第一句，每次最多 96 句。使用工程当前译文语言，初次审核可传 `target` 为 `en` 或 `zh`。`review.suggestions` 返回原文、源时间、具体理由、建议字段及其旧值。`revision` 对应审核时的文本和术语。应用修改请在字幕工作台逐条核对并采纳。

### 录屏助手

下列请求分别以 UTF-8 写入 `wpsrecorder_cli.exe --task` 的标准输入。有需要时用 `--input` 指定当前本地录像目录。

```json
{"version":1,"task":"recording.search","args":{"query":"运行成功","from":"2026-11-14","until":"2026-11-14","limit":5}}
{"version":1,"task":"recording.diagnose","args":{"topic":"encoder"}}
{"version":1,"task":"recording.settings-preview","args":{"changes":{"resolution":"1280x720","fps":60,"audio":"system"}}}
{"version":1,"task":"assistant.ask","args":{"prompt":"下次录成 720p、60 帧，只录系统声音。"}}
```

| 任务 | 结果和条件 |
|---|---|
| `recording.search` | 本地结果含文件名、路径、录制日期、命中来源和证据位置 `at_ms`。`-1` 表示按名称或时间命中。可省略查询词按时间查，`from` 和 `until` 为包含首尾的本地日期。结果数、查询时间和扫描量有上限。`truncated` 提示可缩小范围。 |
| `recording.diagnose` | `topic` 可取 `latest`、`devices` 或 `encoder`。返回当前设置、最近成功配置、最近记录的故障阶段与类型。`devices` 会在空闲时查询麦克风和系统声端点是否可枚举。 |
| `recording.settings-preview` | `changes` 支持 `resolution`（`auto` 或有效偶数尺寸）、`fps`、`bitrate_kbps`、`audio`、`encoder`、`format`、`separate_audio`、`denoise` 和 `cursor`。结果含已校验的改动和 `diff`，`applied` 为 `false`。 |
| `assistant.ask` | 把自然语言交给用户设置的模型。工具会返回真实匹配、诊断或 `settings_preview`，并报告已成功调用的工具。录制期间会推迟新的助手请求。 |

DeepSeek 默认使用 `deepseek-flash`。终端可通过进程环境变量 `WPSRECORDER_ASSISTANT_KEY` 为 `api.deepseek.com` 提供临时密钥。也可在「更多 → 录屏助手 → 模型服务设置」填写自定义模型和支持 function tools 的 OpenAI Chat Completions 地址。

找录像并剪辑时，`assistant.ask` 返回 `recordings` 和 `smart_cut`。桌面沿用已选择的录像并开始生成草稿。CLI 宿主用选定的录像或剪辑工程作为 `--input`，把 `smart_cut.options` 与可选的 `chapters` 传给 `edit.smart-cut` 的 `args`。`chapters:true` 会在应用草稿时按保留片段标题创建章节。检查草稿后才调用 `edit.apply-cut`，用新的 JSON 路径保存成片计划。`edit.chapters` 接受空参数，输出工程的源时间和成片章节时间。

### 录后语音流程

```bat
wpsrecorder_cli.exe --transcribe --input lesson.mp4 --output lesson.srt --language zh
wpsrecorder_cli.exe --translate-subtitles --input lesson.mp4 --subtitles lesson.srt --language en --output bilingual.json
wpsrecorder_cli.exe --export-bilingual --input bilingual.json --output subtitle-delivery
wpsrecorder_cli.exe --synthesize --tts-engine audio8 --language zh --text-file sentence.txt --output sentence.wav
wpsrecorder_cli.exe --dub-track --input takes.json --output timeline.wav
```

翻译支持 `--line`（从 1 起算）和 `--glossary`。译文写入工程，支持从 JSON 再次打开。本地配音用 `audio8`，通过 `--voice mandarin-female` 选择普通话女声。配置在线配音服务后还可用 `qwen`、`minimax` 或 `custom`。`takes.json` 包含 `duration_ms` 和 `lines`，每条包含 `begin_ms`、`end_ms` 及单句 WAV 路径 `audio`。单句 WAV 为 48 kHz、16 bit PCM、单声道。时间从源视频起点算起。超出时段或互相重叠的音频会报错。 [模型服务与能力检验](../guide/SPEECH.md#模型服务与能力检验)

```bat
wpsrecorder_cli.exe --record --duration 15 --output region.mp4 --source region --region 100,100,1280,720 --res 1280x720 --fps 30 --audio system
```

## 状态结构

```json
{"state":"recording","phase":"recording","paused":false,"duration_sec":12.0,"frames_written":360,"avg_fps":30.0,"duplicate_frames":0,"missed_frame_slots":0,"mem_mb":85.0,"cpu_pct":3.0,"output":"D:/Videos/out.mp4","error":"","pid":1234,"ok":true}
```

state 为 idle 或 recording，phase 进一步区分 starting、recording、finalizing 和 idle。duration_sec 在录制中表示活动时长，保存后为视频时间轴时长。frames_written 为交给编码器的完整帧数，avg_fps 为这些帧除以对应时长。missed_frame_slots 为未写入新画面的帧位数量，播放器会把前一帧保留到下一帧的时间戳。duplicate_frames 统计为达到停止时刻而写入的末尾保留帧。

`mem_mb` 为服务进程和当前 FFmpeg 视频编码进程的工作集之和，单位 MiB。`cpu_pct` 按同一窗口内各进程的 CPU 时间差计算，除以采样间隔和逻辑 CPU 数。首次采样为 0，后续每 250 毫秒最多更新一次。服务处于空闲期时会保留上一任务的结果和错误。

## 剪辑命令

```bat
wpsrecorder_cli.exe --trim --input input.mp4 --output clip.mp4 --begin 2.5 --end 8
wpsrecorder_cli.exe --split --input input.mp4 --output D:\Videos\parts --intervals "0-3;3-6;10-15"
wpsrecorder_cli.exe --merge --input first.mp4 --input second.mp4 --output joined.mp4
wpsrecorder_cli.exe --merge --input first.mp4 --input second.mp4 --output joined-1080p.mp4 --res 1920x1080 --fps 60 --bitrate 12000
wpsrecorder_cli.exe --merge --input first.mp4 --input second.mp4 --output joined-copy.mp4 --copy-streams
```

区间单位为秒。拆分文件为 `part-001.mp4`、`part-002.mp4` 等，已有文件拒绝覆盖。

`--merge` 按 `--input` 的顺序拼接为 MP4。普通合并默认继承首段的显示分辨率和帧率，奇数宽高补齐为偶数，画面等比适配。可用 `--res 宽x高`、`--fps` 和 `--bitrate` 覆盖输出尺寸、帧率和视频目标码率。音轨按输入顺序分别合并，缺少音轨的片段补等长静音，全程静音素材输出无音轨视频。`--subtitles` 可添加 SRT 字幕轨，`--denoise` 可整理已有音轨。

`--merge --copy-streams` 复制首路视频和已有音轨的编码数据。各段需为兼容的本地视频，流顺序、编码参数、时间基和分辨率须一致。可传入与原始编码尺寸或显示方向一致的 `--res`。HDR 素材用此模式保留 HDR 信号。普通合并支持 SDR 素材。

## 2.1 可视化工程导出

先在 GUI 剪辑窗口的“工程 → 保存剪辑工程”生成 version:1 JSON，再用 CLI 重复导出同一计划：

```bat
wpsrecorder.exe --edit "D:\Videos\lesson.mp4"
wpsrecorder_cli.exe --edit-project --input "D:\Videos\lesson.edit.json" --output "D:\Videos\lesson-edited.mp4" --encoder auto
wpsrecorder_cli.exe --edit-project --input "D:\Videos\lesson.edit.json" --output "D:\Videos\lesson-mixed.mkv" --mix-tracks --soft-captions --encoder libx264
```

输入是工程 JSON，原视频路径记录在工程中。验证原视频尺寸、时长和文件大小后，按保留区间、画面裁剪、逐轨声音及字幕计划生成 MP4/MOV/MKV。默认保留独立声音轨、字幕写入画面；`--mix-tracks` 改为混音，`--soft-captions` 改为可选字幕轨。既有输出拒绝覆盖。完整交互和源时间语义见 [剪辑说明](../guide/EDITING.md)；此命令与旧版 `--trim/--merge/--export` 各自独立。

多素材工程默认导出 MP4 和可选字幕轨。使用 `--burn-captions` 将字幕写入画面，`--bitrate` 可为单素材和多素材导出指定视频码率（100 至 100000 kbps）。

## 2.2 配音与旁白

新增 `--voices`、`--synthesize --text-file UTF8.txt --output 新.wav`、`--narrate --input 视频 --speech 旁白.wav --output 新.mp4`。本地 Audio8 0.1B 可选默认音色或 `mandarin-female`。音色、语速、音量和插入时间见[SPEECH](../guide/SPEECH.md)。原有命令保持兼容；中文明确选择zh时输出简体字幕。

## 2.0 扩展

```bat
wpsrecorder_cli.exe --devices
wpsrecorder_cli.exe --record --duration 10 --output second.mp4 --monitor 1 --fps 60 --encoder h264_nvenc
wpsrecorder_cli.exe --record --duration 10 --output tracks.mkv --audio mix --separate-audio
wpsrecorder_cli.exe --schedule --start-at "2026-12-01T10:00:00-08:00" --max-duration 600 --output lesson.mp4
wpsrecorder_cli.exe --cancel-schedule
wpsrecorder_cli.exe --apply-scene --preset scene.json
wpsrecorder_cli.exe --export --input lesson.mp4 --output short.gif --begin 1 --end 5
wpsrecorder_cli.exe --export --input lesson.mp4 --output audio.mp3 --denoise
wpsrecorder_cli.exe --export --input lesson.mp4 --output full.mov --copy-streams
wpsrecorder_cli.exe --transcribe --input lesson.mp4 --output lesson.srt --language auto
wpsrecorder_cli.exe --export --input lesson.mp4 --output subtitled.mp4 --subtitles lesson.srt
```

预约时间必须在未来，带时区的 ISO8601 值可避免歧义；必须有正 max-duration。已有预约时拒绝重叠开始。预约不唤醒休眠、服务重启后不恢复；状态含 scheduled_start、scheduled_output、schedule_error。

`--export` 的 `--end` 默认 `-1`，表示视频结尾。MP4/MOV/MKV 可硬件编码，WebM 为 VP9/Opus，GIF 为 12 fps、960 px 宽的调色板输出。M4A/MP3/WAV 提取第一个音轨，WAV 使用 PCM 16 位。`--bitrate` 设置重新编码的视频的目标码率。`--export --copy-streams` 将完整视频封装为与源编码兼容的 MP4/MOV/MKV，用于原样复制编码数据。

字幕工具默认查找应用目录下 whisper/whisper-cli.exe 和 whisper/ggml-base.bin，也可通过 --whisper、--model 指定。识别完全在本地，先转 16kHz 单声道 WAV，再调用 whisper.cpp 输出 SRT。--subtitles 以可选字幕轨嵌入，不烧录到画面；播放器需开启字幕显示。

场景 JSON 不执行代码，主要字段包括 source、monitor、window、image、cameraEnabled、cameraRect（x,y,w,h 的 0..1 相对值）、cameraShape、cameraMirror、effects。effects 中 shapes 提供 pen/arrow/rectangle/highlight/text，masks 提供 pixel/solid 和相对 rect；whiteboard、spotlight、magnifier 为布尔开关。运行中 --apply-scene 只允许修改画面相关字段。

测试隔离可设置 WPSRECORDER_INSTANCE，为 CLI 服务和锁文件增加独立命名空间；日常使用应保持该变量未设置，GUI/CLI 共用同一任务。devices 的 process_app_container 为诊断字段，不代表摄像头一定可用。

2.0.1 的 `--devices` 还提供 `process_integrity_rid`；4096 为 Low、8192 为 Medium。摄像头访问拒绝时，Low 环境会提示正常解压到工作区外再启动。`pipeline_stage` 是录制管道的诊断阶段，供启动/保存异常定位；界面不显示这些实现字段。

```bat
wpsrecorder_cli.exe --record --duration 5 --source camera --camera-track --res 640x360 --output camera.mp4
wpsrecorder_cli.exe --record --duration 5 --source screen --pip --camera-track --audio mix --separate-audio --output pip.mp4
```

`--camera-track` 在主视频旁保存 `主文件名.camera.mkv`（640×480、与主视频相同帧率），是独立视频文件。主容器中的系统声与麦克风为两条音轨，MP4/MOV 通过 handler_name、MKV 通过 title 标注 mic/system。仅摄像头模式和画中画均遵循 cameraMirror；独立摄像头视频保留未镜像、未加标注/遮挡的内容。

2.0.2 的 `--devices` 为每块屏幕补充 dpi_x、dpi_y 和 scale_percent，坐标/尺寸均为物理像素。GUI 的“自动（跟随来源）”在启动前转换为具体宽高；CLI 的 `--res` 保持显式宽高语义。

GUI 监看使用当前用户本地 IPC 请求 `{"command":"preview"}`，响应含 active、paused、sequence、frame、camera。图像是 Base64 JPEG：合成画面不超过 960×540，摄像头不超过 480×360；仅在内存中保留一份最新缩略图，不落盘、不上传。空闲返回 active:false、sequence:0 和空图像。预览异步读取、最多 2MB、超时 1200ms，独立于状态/停止请求。

## 2.2.2 屏幕颜色

`--screen-color auto|highlights` 默认auto；`--screen-white 0..1000` 指定HDR的SDR参考白（nit），0默认读取系统；`--screen-exposure -2..2` 指定屏幕曝光补偿（EV），默认0。JSON场景对应screenColor、screenWhiteNits、screenExposureEV，可运行中更新；旧预设缺省兼容自动模式。`--devices`包含屏幕HDR状态/位深/白电平；录制`--status`中的capture对象记录实际采集API、源格式和处理方式。
