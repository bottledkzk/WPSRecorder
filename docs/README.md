# WPS Recorder 文档索引

当前版本为 2.3.11，包含录制工作台、回看与管理、剪辑工坊和智能 AI 助手。运行环境、工程结构和本机实测见 [项目总览](../ReadMe.md)。

实时预览、打点和悬浮条见 [使用说明](guide/USER-GUIDE.md)。多素材顺排和混剪见 [剪辑指南](guide/EDITING.md#多素材顺排)，服务配置及能力检验见 [字幕与配音](guide/SPEECH.md#模型服务与能力检验)。

[大作业设计说明](DESIGN.md)介绍模块划分、录制链路的设计取舍，以及写入等待、预览卡顿和多素材音轨的解决过程。

| 分类 | 内容 |
|---|---|
| `guide/` 使用指南 | [使用说明](guide/USER-GUIDE.md)、[功能介绍](guide/FEATURE-INTRODUCTION.md)、[可视化剪辑](guide/EDITING.md)、[字幕与配音](guide/SPEECH.md) |
| `reference/` 技术参考 | [构建与部署](reference/BUILD.md)、[CLI](reference/CLI.md)、[架构设计](reference/DESIGN.md)、[性能与多进程](reference/PERFORMANCE.md)、[后台性能监测](reference/RECORDING-MONITOR.md)、[摄像头诊断](reference/CAMERA-DIAGNOSIS.md) |
| `course/` 课程对照 | [2.3.9 提交与打包核对](course/COURSE-CHECKLIST.md)、[历史功能验收](course/FEATURE-STATUS.md)、[2.0 可行性评估](course/UPGRADE.md) |
| `validation/` 版本验收 | [2.3.11 验收](validation/VALIDATION-V2311.md)、[2.3.9 实测](validation/VALIDATION-V239.md) 和 [历次验收索引](validation/README.md) |

`CMakeLists.txt` 是课程构建的兼容入口，具体用法见[构建与部署](reference/BUILD.md)。
