# 摄像头访问拒绝原因定位（2026-11-02）

## 结论

开发工作区的 EXE 继承了 NTFS 的 Low 完整性文件标签。由这些 EXE 启动的普通桌面进程也运行在 Low，摄像头访问被系统拒绝。把同一诊断二进制通过资源管理器从 ZIP 正常解压到工作区外的用户目录后，进程为 Medium，原项目采用的 Media Foundation RGB32 采集链路成功取得帧。

本次对照支持把原因定位到构建产物标签及启动环境，而不是缺少摄像头、STA/MTA 选择或尚未调用设备激活 API。未调整 Windows 摄像头开关、防护软件规则或现有工作区权限。

## 对照结果

| 检查 | 工作区 EXE | 普通桌面解压的相同 EXE |
|---|---|---|
| 二进制 SHA256 | 6fa74ad79e7d3786dea58d879b84f22e32287584753b9198c9e8ec3f044d2be8 | 相同 |
| 文件完整性标签 | 继承 Low Mandatory Level | 无 Low 标签，正常用户目录默认级别 |
| 进程完整性 RID | 4096 / Low | 8192 / Medium |
| AppContainer | 0 | 0 |
| TokenIsRestricted | 0 | 0 |
| MF 设备枚举 | 1 个摄像头 | 1 个摄像头 |
| MF MTA / STA 激活 | 0x80070005 | 0x0 |
| Source Reader / RGB32 | 激活已被拒绝，未进入转换 | 均为 0x0 |
| RGB32 帧 | 无 | MTA、STA 各取得 3,686,400 字节帧 |
| WinRT 设备访问状态 | 3 / DeniedBySystem | 1 / Allowed |
| WinRT InitializeAsync | 0x80070005 | 成功 |

Windows 自带相机也实际预览成功。独立 WinRT 诊断的初始化之后，FrameSources 访问因诊断程序回调跨线程产生 0x8001010e；此项不视为 WinRT 帧采集通过，也不影响原项目使用的 MF RGB32 采集结果。

## 使用方式

正式体验包应由正常桌面会话通过资源管理器解压到工作区外的新用户目录，再运行其中的 wpsrecorder.exe。在带 Low 文件标签的开发目录直接双击 EXE，仍会保持 Low。无需通过修改系统隐私设置来解决这个文件标签导致的问题。

CLI 的 devices 诊断补充 process_integrity_rid，摄像头拒绝访问消息补充低完整性环境提示。旧 2.0 ZIP 不含该补充；2.0.1 构建已包含，并在 Low 环境约 0.94 秒内返回明确提示和失败退出码。

## 主应用预览复核

现有 2.0 体验包通过资源管理器解压到 `C:\Users\bottl\AppData\Local\Programs\WPSRecorder2-CameraCheck` 后，直接运行 `exe_cli\wpsrecorder.exe`。EXE 无 Low 文件标签，SHA256 为 `0a4f111f2d7b3f004e04b4def24a97f2524fa6224ebcfaaf1376d61172c9b2b7`，与体验包中的二进制一致。

旧 2.0 的“测试摄像头”成功显示 USB2.0 HD UVC WebCam 画面。复查代码确认旧窗口只取一帧，因此该结果只证明单帧采集；此前将其称为实时预览不准确。该轮诊断未保存摄像头照片或视频，也未把预览截图写入仓库。

对照日志副本与元数据保存在 `../validation/evidence/2.0/camera-low-result.txt`、`camera-medium-result.txt` 和 `camera-environment.json`。这些文本不包含摄像头画面。

## 2.0.1 实录验收

2.0.1 已将测试窗口改为连续刷新，实测接收计数从 9 增长到 614，关闭后释放设备。采集采用异步 Source Reader；所有设备清理在 MTA 采集线程完成，最后保留的样本在 MFShutdown/CoUninitialize 前释放，CLI 主线程建立 COM 生命周期。

本轮先复现首段录制超时，再复现 GUI 后台服务停止时在 OLEAUT32.dll 中出现 0xc0000005；两项历史失败均有元数据保留。修复后 10 项实录及独立解码检查通过，主画面/摄像头轨各 872 帧；另在 GUI 录制 1920×1080、28.066 秒、842 帧，独立视频同为 842 帧，停止后自动载入播放器并开始播放。详情见 [2.0.1 验收](../validation/VALIDATION-V201.md) 与 ../validation/evidence/2.0.1。

实录媒体只保存在本机测试目录和用户视频目录；仓库只保存指标、配置与哈希，不包含摄像头图像、音频或视频。

依据：[Windows 强制完整性控制](https://learn.microsoft.com/en-us/windows/win32/secauthz/mandatory-integrity-control)、[DeviceAccessStatus 定义](https://learn.microsoft.com/en-us/uwp/api/windows.devices.enumeration.deviceaccessstatus)、[异步 Source Reader](https://learn.microsoft.com/en-us/windows/win32/medfound/using-the-source-reader-in-asynchronous-mode)、[取消待处理读帧请求](https://learn.microsoft.com/en-us/windows/win32/api/mfreadwrite/nf-mfreadwrite-imfsourcereader-flush)。
