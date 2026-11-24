// framecapture.h | 3226535826@qq.com | 2026-10-31 | Native GDI frame acquisition.
#ifndef WPSRECORDER_FRAMECAPTURE_H
#define WPSRECORDER_FRAMECAPTURE_H
#include "recordingconfig.h"
#include <QImage>
#include <QJsonArray>
#include <windows.h>
#include "desktopcapture.h"
#include "windowcapture.h"

class KRRFrameCapture
{
public:
	KRRFrameCapture() = default;
	~KRRFrameCapture();
	KRRFrameCapture(const KRRFrameCapture&) = delete;
	KRRFrameCapture& operator=(const KRRFrameCapture&) = delete;
	QImage capture(const KRRRecordingConfig& config, QString& error, KRRRecordingTimings* timings = nullptr);
	QJsonObject diagnostics() const { return m_info; }
	static QRect VirtualDesktop();
	static QJsonArray Monitors();
	static QString MonitorDevice(int index);
	static QRect CaptureArea(const KRRRecordingConfig& config);
private:
	HDC m_screen = nullptr;
	HDC m_memory = nullptr;
	HBITMAP m_bitmap = nullptr;
	HGDIOBJ m_previous = nullptr;
	void* m_pixels = nullptr;
	QSize m_size;
	QString m_imagePath;
	QImage m_stillFrame;
	KRRDesktopCapture m_desktop;
	KRRWindowCapture m_window;
	QRect m_desktopArea;
	QSize m_desktopOutput;
	bool m_checkDesktop = true, m_preferGdi = false;
	QJsonObject m_info;
};
#endif
