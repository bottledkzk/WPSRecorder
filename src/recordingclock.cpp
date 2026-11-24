#include "recordingclock.h"
#include <windows.h>

double KRRRecordingClock::Now()
{
	LARGE_INTEGER counter{}, frequency{};
	QueryPerformanceCounter(&counter);
	QueryPerformanceFrequency(&frequency);
	return static_cast<double>(counter.QuadPart) / static_cast<double>(frequency.QuadPart);
}
void KRRRecordingClock::reset()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_started = false;
	m_pauses.clear();
}
void KRRRecordingClock::begin()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_origin = Now();
	m_pauses.clear();
	m_started = true;
}
void KRRRecordingClock::setPaused(bool paused)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	bool current = !m_pauses.empty() && m_pauses.back().second == 0;
	if (paused && !current)
		m_pauses.emplace_back(Now(), 0);
	else if (!paused && current)
		m_pauses.back().second = Now();
}
double KRRRecordingClock::activeTimeAt(double timestamp) const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (!m_started || timestamp < m_origin)
		return -1;
	double removed = 0;
	for (const auto& pause : m_pauses)
	{
		if (timestamp < pause.first)
			break;
		if (pause.second == 0 || timestamp < pause.second)
			return -1;
		removed += pause.second - pause.first;
	}
	return timestamp - m_origin - removed;
}
double KRRRecordingClock::elapsed() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (!m_started)
		return 0;
	double timestamp = Now(), removed = 0;
	for (const auto& pause : m_pauses)
		removed += (pause.second == 0 ? timestamp : pause.second) - pause.first;
	return timestamp - m_origin - removed;
}
