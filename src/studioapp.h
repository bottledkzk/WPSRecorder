#ifndef WPSRECORDER_STUDIOAPP_H
#define WPSRECORDER_STUDIOAPP_H
class QApplication;
inline constexpr int SwitchToMinimalRecorder = 78;
inline constexpr int SwitchToRecordingStudio = 79;
int RunRecordingStudio(QApplication &app);
#endif
