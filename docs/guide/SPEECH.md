# 录后字幕、翻译与配音

## 字幕与配音工作台

停录后从「更多 → 字幕与配音」进入，也可以在剪辑器「字幕」页或工程菜单打开。「字幕配音」页可离线识别，或使用导入的 SRT。「自填旁白」页可直接填稿。两页共用右侧的配音模型、音色、语速、读音词表和试听。

- 新配音默认读原文。需要译文时展开「翻译（可选）」，可以填写术语、翻译全部或重译单句。译文可单独编辑，也可以在配音设置里选择朗读译文。字幕导出按保留区间重排。仅有原文时导出一份 SRT，有译文时同时导出原文、译文和双语 SRT。
- 在「起止时间」和「配音」两列可核对完整时间码、实际配音时长或待重做状态。选中一句后在右侧听原片和生成的配音。可填写单独的配音稿来缩短句子或调整读音。试听保留完整音频，超时的句子会提示重做。点击「生成配音轨」，再应用到时间轴，就能剪切该轨或调整音量。保存剪辑工程时会保留音轨与逐句试听的 WAV 引用。
- 「模型服务」按录屏助手、字幕翻译、视频理解和语音合成分类。API 密钥用 Windows 凭据管理器保存，按服务配置和接口地址读取。本机可以使用 HTTP，远端使用 HTTPS。
- 本地配音使用 Audio8 0.1B，提供默认音色和普通话女声。缺少运行包时，选中 Audio8 后点「本地模型目录」。在线配音可选「模型服务 → 语音合成」里保存的配置。

### 校对译文、配音稿与术语

1. 在字幕工作台填入翻译术语，例如要求保留「Qt Creator」，然后打开「AI 校对」。它使用「服务设置 → 录屏助手」里的 DeepSeek 或用户配置的兼容接口。选择当前句或指定起止句，每次最多 96 句，输入「统一产品名称，检查数字和单位」等要求。点击「开始审核」后，服务收到选段的原文、译文、配音稿和术语。
2. 查看原片时间、修改前后的文字和理由，勾选要采纳的行。建议先进入工作稿，确认后点「应用到时间轴」。想保留修改可保存剪辑工程，应用后可用时间轴的撤销恢复。
3. 「读音词表」按行填写 `GPU => G P U` 等规则，最多 40 条。规则作用于读原文、读译文和独立配音稿，其效果可用「重做本句」试听。工程保存翻译术语和读音词表。
4. 如果旧试听对应的朗读文字有变化，相关行显示「待重做」。其他已配句仍可试听。旧混合配音轨会标为待更新。重做受影响的句子后点「更新配音轨」，即可用可用的逐句 WAV 重新对齐。

### 模型服务与能力检验

新设备首次使用时，协议、接口地址和模型由用户填写。应用会读取本机已保存的个人配置。语音合成可点「新增配置」，也可从「配音配置」选择已有项，修改名称、接口、模型或默认音色后保存。「删除配置」会移除所选配置及其保存在本机的密钥。

录屏助手、字幕翻译和视频理解可选 Anthropic Messages。填写接口地址、模型 ID 和 API 密钥，点「检验能力」后保存。选用该协议进行视频理解时，程序发送带时间点的抽帧图片。参见 [Messages API](https://platform.claude.com/docs/en/api/messages) 和 [图片输入](https://platform.claude.com/docs/en/build-with-claude/vision)。

| 服务示例 | 模型示例 | 接口 |
|---|---|---|
| 千问翻译 | `qwen3.8-flash` | Token Plan 的 OpenAI 兼容 `/compatible-mode/v1/chat/completions` |
| 千问配音 | `qwen-audio-3.0-tts-plus` | Token Plan 的原生 SpeechSynthesizer，支持覆盖完整地址与模型 |
| MiniMax 配音 | `speech-2.8-turbo` | `https://api.minimax.cn/v1/t2a_v2`，音色 ID 可修改 |
| 自定义配音 | 由用户填写 | OpenAI 兼容 `/v1/audio/speech`，返回音频数据，推荐 WAV |

可以改用其他模型或新增语音合成配置。录屏助手和字幕翻译支持 [Ollama 原生对话](https://docs.ollama.com/api/chat)；助手需要所选模型具备 [工具调用能力](https://docs.ollama.com/capabilities/tool-calling)。语音合成可选 DashScope、MiniMax 原生协议，也可使用 OpenAI 兼容接口。各功能还支持自定义 JSON HTTP POST：在请求中用独立 JSON 值指定变量（如 `"speaker":"$voice"`），响应可按 `result.0.text` 这样的字段路径读取。配音支持直接返回音频，或从 JSON 读取 Base64 / 十六进制音频。工具模型须返回能识别的 `content` 和 `tool_calls` 消息字段，视频接口须接受当前分析使用的视频片段和图片。

点「检验能力」可用尚未保存的配置测试。助手检查工具调用和工具结果回传。翻译检查中英双向，视频理解检查合成画面和结果中的时间点，语音合成实际解码固定测试短句。检验通过后点「保存当前配置」。API 密钥和套餐必须与所选服务匹配。在「认证选项」可改用其他请求头与前缀。CLI 的环境密钥按服务商和已配置地址匹配。Audio8 目录可用 `WPSRECORDER_AUDIO8_DIR` 覆盖。

参考 [千问 Token Plan 语音合成](https://help.aliyun.com/zh/model-studio/token-plan-speech-synthesis)、[千问 OpenAI 兼容接入](https://help.aliyun.com/zh/model-studio/use-chat-client-or-development-tool/)、[MiniMax 语音协议](https://platform.minimax.io/docs/api-reference/speech-t2a-http)、[Audio8 INT8 模型](https://huggingface.co/Edge0/audio8-TTS-0.1B-ONNX-INT8)。

## 章节长稿与原声压低

1. 在剪辑器的「章节」页添加本次讲解的章节，然后打开「章节旁白」。选「导入长稿」可使用模板、粘贴文字或打开 UTF-8 TXT／Markdown。`## 章节标题` 或 `## 2. 章节标题` 对应已有的章节。空章节跳过，换行和句末标点可分句。
2. 先核对分句，再选旁白语言、配音引擎和音色。可逐句生成、试听和改稿，也可生成全部待配句。预估位置随导入显示。发音规则沿用工程的读音词表。
3. 选「对齐并生成旁白轨」，程序从章节起点按每句生成的实际 WAV 时长在保留片段中排布，优先留出 150 毫秒句间停顿。空间紧时可将句子紧邻排放。时长仍超出章节可用范围时，会提示相关章节。可缩短稿件、调快语速或调整剪切后再生成。已确认的逐句音频仍可复用。
4. 「旁白期间原声」默认 35%，100% 等于保留原音量。应用到时间轴后，剪辑预听与成片导出在实际旁白句段内使用同一原声比例。开始前 80 毫秒渐降，结束后 120 毫秒恢复。关闭章节旁白轨时使用完整原音量。可保存剪辑工程，原视频和生成的 WAV 随工程保留。

章节旁白独立保存，仍可沿用现有双语字幕和逐句配音。它按源片时间对齐，导出时跟随保留区间。保存工程后移动素材时请一并保留工程引用的 WAV。

## 自动字幕

停止录制后点击「剪辑 → 字幕 → 自动识别」，或从主窗口「更多 → 字幕与配音」进入。识别音轨、语言和模型放在弹窗中。完整体验包包含 whisper.cpp、base 与 small Q5 识别模型。

- 声音轨：分轨素材应选包含讲话的一轨，系统声和麦克风可以分别识别。
- 语言：自动检测、中文、英语、日语、法语、德语、西班牙语、韩语、意大利语、葡萄牙语和俄语。
- 模型：small Q5 用于较细致识别；base 更快。缺少模型时可选择本地 GGML 文件。模型文件名中的 `.en` 表示仅英语，不适用于中文。
- 中文明确选择 `zh` 时，结果转换为简体；其他语言保持模型输出，不强制改写日文汉字。
- 识别在后台运行，可取消。结果加入字幕轨，可修改文字/时间、删除、导入或导出 UTF-8 SRT；剪切画面后字幕按保留区间重新计时。成片可以烧录字幕或保留可选字幕轨。

本机使用 SenseVoiceSmall 官方模型仓库的三段公开中英日语音作通路检查，输出带时间戳的字幕并检查关键词。它们不是完整语料库：没有报告 CER/WER、中文课程、多说话人、噪声或法语准确率。日语样例仍有同音词/用词错误，需人工校对。base 与 small 对比只作短样例观察，不保证 small 在每种声音中都更准确。

## 自填旁白

1. 在「字幕与配音」切到「自填旁白」，输入最多 4000 字，选择朗读语言、配音模型、音色和语速。外部语音服务支持输入自定义音色 ID，生成时会发送朗读文字给所选服务。
2. 起点默认取剪辑器播放头，按源片时间填写。点击「生成旁白」后，可以在右侧试听音色和实际旁白。若旁白超出源片结尾，可以提前起点、缩短稿件或加快语速。
3. 点击「加入时间轴」，剪辑器会选中新增的独立声音轨。可对该轨做选区剪切、静音或调音量。保存工程后，配音 WAV 仍可重开，迁移工程时需一并保留引用的 WAV。

## 本地配音部署与 CLI

按[构建与部署](../reference/BUILD.md)准备程序和 Whisper。部署时可通过语音工具脚本加入现有 Audio8 0.1B 便携运行包。普通话女声随程序部署，使用同一套 Audio8 模型。

```powershell
.\scripts\setup-speech-tools.ps1 -RuntimeDirectory D:\runtime -ToolsDirectory D:\tools\speech -Audio8RuntimeDirectory D:\audio8
```

```bat
wpsrecorder_cli.exe --voices
wpsrecorder_cli.exe --synthesize --text-file narration.txt --output narration.wav --tts-engine audio8 --voice mandarin-female --speed 1.2 --voice-volume 90
wpsrecorder_cli.exe --narrate --input lesson.mp4 --speech narration.wav --output narrated.mp4 --begin 2 --original-volume 30
wpsrecorder_cli.exe --transcribe --input lesson.mp4 --output lesson.srt --language zh --model whisper/ggml-small-q5_1.bin
```

配音文本使用 UTF-8，默认使用 Audio8 的 `default` 音色。语速范围 0.5–2，配音音量范围 0–150%，原声音量范围 0–100%。长文本按句生成，可取消任务。音色预览和正文使用同一引擎生成。

[whisper.cpp](https://github.com/ggml-org/whisper.cpp) 提供本地识别。[公开测试语音](https://huggingface.co/FunAudioLLM/SenseVoiceSmall/tree/main/example)用于验收。Audio8 运行包随附模型许可，额外女声的来源见包内 `tts/voices/NOTICE.txt`。
