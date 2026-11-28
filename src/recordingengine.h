// recordingengine.h | 3226535826@qq.com | 2026-10-31 | Capture, encode and transactional output.
#ifndef WPSRECORDER_RECORDINGENGINE_H
#define WPSRECORDER_RECORDINGENGINE_H
#include "recordingconfig.h"
#include "recordingclock.h"
#include "previewchannel.h"
#include <QObject>
#include <QImage>
#include <QJsonArray>
#include <atomic>
#include <mutex>
#include <thread>

class KRRRecordingEngine : public QObject
{
	Q_OBJECT
public:
	explicit KRRRecordingEngine(QObject* parent = nullptr);
	~KRRRecordingEngine() override;
	bool start(const KRRRecordingConfig& config, QString& error);
	void requestStop();
	void setPaused(bool paused);
	void setPreviewPublication(bool enabled) { m_publishPreview.store(enabled, std::memory_order_relaxed); }
	QJsonObject status();
	QJsonObject audioLevels();
	bool isActive() const;
	bool updateScene(const QJsonObject& scene, QString& error);
	bool addMarker(const QString& label, QJsonObject& marker, QString& error);
	QJsonObject preview();
signals:
	void completed(const QString& output, const QString& error);
private:
	void record(KRRRecordingConfig config);
	std::thread m_thread;
	std::atomic<bool> m_active{false};
	std::atomic<bool> m_stop{false};
	std::atomic<bool> m_paused{false};
	std::atomic<bool> m_publishPreview{true};
	std::atomic<qint64> m_frames{0};
	std::atomic<qint64> m_duplicates{0};
	std::atomic<qint64> m_missedSlots{0};
	std::atomic<double> m_stopTime{-1};
	std::atomic<qint64> m_encoderPid{0};
	std::atomic<qint64> m_cameraEncoderPid{0};
	std::atomic<double> m_duration{0};
	std::atomic<float> m_micLevel{0}, m_systemLevel{0};
	std::array<std::atomic<float>, MaxAdditionalAudioSources> m_audioLevels{};
	QString m_captureControlError;
	QString m_recoveryWorkspace;
	QString m_failureStage;
	QString m_markerWorkspace;
	QJsonArray m_markers;
	std::mutex m_markerWriteMutex;
	bool m_outputPublished = false;
	std::atomic<const char*> m_pipelineStage{"idle"};
	KRRRecordingClock m_clock;
	std::mutex m_mutex;
	KRRPreviewPublisher m_previewPublisher;
	QJsonObject m_captureInfo;
	QString m_phase = "idle";
	QString m_output;
	QString m_error;
	QString m_encoder;
	KRRRecordingConfig m_live;
	std::array<double, 3> m_previousCpu{};
	std::array<qint64, 2> m_sampledEncoders{};
	double m_previousCpuSample = 0;
	double m_previousFpsSample = 0;
	qint64 m_previousFrameCount = 0;
	double m_cpuPercent = 0, m_memoryMB = 0, m_effectiveFps = 0;
};
#endif
