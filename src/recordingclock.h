// recordingclock.h | 3226535826@qq.com | 2026-10-31 | Shared video/audio active-time clock.
#ifndef WPSRECORDER_RECORDINGCLOCK_H
#define WPSRECORDER_RECORDINGCLOCK_H
#include <mutex>
#include <utility>
#include <vector>

class KRRRecordingClock
{
public:
	void reset();
	void begin();
	void setPaused(bool paused);
	double elapsed() const;
	double activeTimeAt(double timestamp) const;
	static double Now();
private:
	mutable std::mutex m_mutex;
	double m_origin = 0;
	bool m_started = false;
	std::vector<std::pair<double, double>> m_pauses;
};
#endif
