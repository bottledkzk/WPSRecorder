// desktopcapture.h | 3226535826@qq.com | 2026-10-31 | DXGI desktop duplication with a GDI fallback.
#ifndef WPSRECORDER_DESKTOPCAPTURE_H
#define WPSRECORDER_DESKTOPCAPTURE_H
#include <QImage>
#include <QRect>
#include <QString>
#include <memory>
#include <QJsonObject>
struct KRRRecordingConfig;
class KRRRecordingTimings;

class KRRDesktopCapture
{
public:
	KRRDesktopCapture();
	~KRRDesktopCapture();
	QImage capture(const QRect& area, const QSize& output, const KRRRecordingConfig& config,
		KRRRecordingTimings* timings = nullptr);
	QJsonObject diagnostics() const;
private:
	struct State;
	std::unique_ptr<State> m_state;
	bool initialize(const QRect& area);
};
#endif
