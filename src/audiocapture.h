// audiocapture.h | 3226535826@qq.com | 2026-10-31 | WASAPI PCM capture with QPC timestamps.
#ifndef WPSRECORDER_AUDIOCAPTURE_H
#define WPSRECORDER_AUDIOCAPTURE_H
#include "recordingclock.h"
#include <QString>
#include <QVector>
#include <atomic>
#include <future>
#include <mutex>
#include <thread>

struct KRRAudioEndpoint
{
	QString id, name;
	bool loopback;
};
QVector<KRRAudioEndpoint> RecordingAudioEndpoints();

class KRRAudioCapture
{
public:
	KRRAudioCapture(bool loopback, KRRRecordingClock& clock, const QString& endpoint = {});
	explicit KRRAudioCapture(bool loopback, const QString& endpoint = {});
	~KRRAudioCapture();
	bool start(const QString& path, QString& error);
	void startMeter();
	void stop();
	void setMuted(bool muted) { m_muted.store(muted); }
	void setGain(float gain) { m_gain.store(gain); }
	QString error() const;
	QString deviceName() const;
	bool meterReady() const { return m_meterReady.load(); }
	float takePeak() { return static_cast<float>(m_meterPeak.exchange(0)) / 10000.0f; }
private:
	std::future<QString> launch(const QString& path);
	void captureLoop(const QString& path, std::promise<QString>& initialized);
	void measure(const float* pcm, qint64 count, float gain);
	bool m_loopback = false;
	KRRRecordingClock* m_clock = nullptr;
	std::atomic<bool> m_stop{false}, m_muted{false};
	std::atomic<bool> m_meterReady{false};
	std::atomic<int> m_meterPeak{0};
	std::atomic<float> m_gain{1.f};
	std::thread m_thread;
	mutable std::mutex m_mutex;
	QString m_error, m_endpoint;
	QString m_deviceName;
};
#endif
