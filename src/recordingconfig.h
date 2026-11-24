// recordingconfig.h | 3226535826@qq.com | 2026-10-31 | Validated recording parameters.
#ifndef WPSRECORDER_RECORDINGCONFIG_H
#define WPSRECORDER_RECORDINGCONFIG_H

#include <QJsonObject>
#include <QJsonArray>
#include <QRect>
#include <QStringList>
#include <array>
#include <atomic>

inline constexpr int MaxAdditionalAudioSources = 6;
inline constexpr std::array<int, 4> RecordingFrameRates{30, 60, 90, 144};
int RecordingFrameRatePreset(int savedRate);
int RecommendedRecordingBitrate(int width, int height, int fps);

struct KRRRecordingConfig
{
	QString output;
	int width = 1920;
	int height = 1080;
	int fps = 30;
	int bitrate = 6000;
	QString audio = "none";
	double micGain = 1., systemGain = 1.;
	QJsonArray audioSources;
	QString source = "screen";
	QRect region;
	quintptr window = 0;
	bool cursor = true;
	bool clicks = false;
	QString clickColor = "#ffcc33";
	int clickRadius = 26;
	int monitor = 0;
	QString monitorDevice;
	QString encoder = "auto";
	QString screenColor = "auto";
	QString outputColor = "sdr";
	double screenWhiteNits = 0, screenExposureEV = 0;
	bool separateAudio = false;
	bool cameraTrack = false;
	int camera = 0;
	bool cameraEnabled = false;
	QString cameraShape = "rounded";
	bool cameraMirror = true;
	QRectF cameraRect{0.75, 0.65, 0.22, 0.30};
	QString cameraBorder = "#ffffff";
	QString image;
	bool denoise = false;
	double maxDuration = 0;
	bool publishPreview = true;
	QJsonObject effects;
	QJsonObject toJson() const;
	static KRRRecordingConfig FromJson(const QJsonObject& object);
	bool validate(QString& error) const;
};

QString FindFFmpeg();
QString FindFFprobe();
QString ControlServerName();
QString DataDirectory();
bool ProbeRecordingFolder(const QString& folder, QString& error, bool create = true);
void HideConsoleProcess(class QProcess& process, bool background = false);
bool RunMediaProcess(const QString& executable, const QStringList& arguments, QString& error, int timeout,
					 const std::atomic<bool>* cancel = nullptr, bool background = false);
QString SelectVideoEncoder(const QString& requested, QString& error);
QStringList VideoEncoderArguments(const QString& encoder, int bitrate, bool recording = true,
	const QString& pixelFormat = "yuv420p");
bool IsContainerProcess();
int ProcessIntegrityRid();
#endif
