#ifndef WPSRECORDER_WINDOWIDENTITY_H
#define WPSRECORDER_WINDOWIDENTITY_H

#include <windows.h>
#include <QString>

inline constexpr wchar_t RecorderAppId[] = L"WPSCourse.WPSRecorder";

class KRRWindowIdentity
{
public:
	explicit KRRWindowIdentity(HWND window);
	~KRRWindowIdentity();
	KRRWindowIdentity(const KRRWindowIdentity&) = delete;
	KRRWindowIdentity& operator=(const KRRWindowIdentity&) = delete;

private:
	HWND m_window;
};

bool CreateRecorderShortcut(const QString& output, QString& error);

#endif
