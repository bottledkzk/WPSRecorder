#ifndef WPSRECORDER_WINDOWCAPTURE_H
#define WPSRECORDER_WINDOWCAPTURE_H

#include <QImage>
#include <QJsonObject>
#include <memory>
#include <windows.h>

class KRRRecordingTimings;

class KRRWindowCapture
{
public:
	KRRWindowCapture();
	~KRRWindowCapture();
	static QRect Bounds(HWND window);
	QImage capture(HWND window, QRect& bounds, bool cursor, QString& error, KRRRecordingTimings* timings);
	const QJsonObject& diagnostics() const { return m_info; }
	void reset();

private:
	struct State;
	std::unique_ptr<State> m_state;
	QJsonObject m_info;
};

#endif
