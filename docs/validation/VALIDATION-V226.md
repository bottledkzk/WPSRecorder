# 2.2.6 区域录制选区验收

先拖出选区，松手后仍可拖动边角缩放、从框内移动，或在框外重画。悬浮面板显示当前像素尺寸，确认按钮或 Enter 应用，Esc 取消。再次进入时恢复上次确认的选区。拖动只刷新旧框和新框覆盖的区域。

本机两块不同 DPI 的显示器均通过反向框选、屏幕底部命中、框内原生命中、两个角与单边缩放、整体移动、确认、恢复和取消。实际跨屏区域按物理像素核对。讲解、悬浮条、画中画拖动、中英文切换和热键留空回归通过。原始证据见源码仓库的 `evidence/2.2.6/region-native.json`、`language-hotkeys.json` 和 `package.json`。

透明区域仍保留少量透明度以接收鼠标事件；Windows 完全透明的分层窗口像素会让输入穿透，见 [Win32 分层窗口说明](https://learn.microsoft.com/en-us/windows/win32/winmsg/window-features#layered-windows)。

性能和多进程设计见 [架构与性能优化](../reference/PERFORMANCE.md)。本版未重新进行长录制性能对比。体验包继续包含原先 60.5 秒游戏展示成片，视频不进 Git。
