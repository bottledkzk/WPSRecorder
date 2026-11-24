#include "uilanguage.h"
#include "recordingconfig.h"
#include "compositor.h"
#include "framecapture.h"

#include <windows.h>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QJsonArray>
#include <QMutex>
#include <QMutexLocker>
#include <QElapsedTimer>
#include <algorithm>
#include <cmath>
#include <iterator>

int RecordingFrameRatePreset(int savedRate)
{
	const auto next = std::upper_bound(RecordingFrameRates.begin(), RecordingFrameRates.end(), savedRate);
	return next == RecordingFrameRates.begin() ? RecordingFrameRates.front() : *std::prev(next);
}

int RecommendedRecordingBitrate(int width, int height, int fps)
{
	// Scale the 1080p60 reference of 6000 kbps by the number of pixels encoded each second.
	if (width <= 0 || height <= 0) { width = 1920; height = 1080; }
	const double units = 12.0 * width / 1920 * height / 1080 * fps / 60;
	return qBound(100, qRound(qBound(0.0, units, 200.0)) * 500, 100000);
}

QJsonObject KRRRecordingConfig::toJson() const
{
	return {{"output", output}, {"width", width}, {"height", height}, {"fps", fps},
		{"screenColor",screenColor},{"outputColor",outputColor},{"screenWhiteNits",screenWhiteNits},{"screenExposureEV",screenExposureEV},{"bitrate", bitrate}, {"audio", audio}, {"source", source},
		{"micGain", micGain}, {"systemGain", systemGain}, {"audioSources", audioSources}, {"x", region.x()}, {"y", region.y()}, {"rw", region.width()}, {"rh", region.height()},
		{"window", QString::number(window)}, {"cursor", cursor}, {"clicks", clicks},
		{"clickColor", clickColor}, {"clickRadius", clickRadius}, {"monitor", monitor}, {"monitorDevice", monitorDevice}, {"encoder", encoder},
		{"separateAudio", separateAudio}, {"cameraTrack", cameraTrack}, {"camera", camera}, {"cameraEnabled", cameraEnabled},
		{"cameraShape", cameraShape}, {"cameraMirror", cameraMirror}, {"cameraBorder", cameraBorder},
		{"cameraRect", QJsonArray{cameraRect.x(), cameraRect.y(), cameraRect.width(), cameraRect.height()}},
		{"image", image}, {"denoise", denoise}, {"maxDuration", maxDuration}, {"effects", effects},
		{"publishPreview", publishPreview}};
}

KRRRecordingConfig KRRRecordingConfig::FromJson(const QJsonObject& object)
{
	KRRRecordingConfig config;
	config.output = object["output"].toString();
	config.width = object["width"].toInt(1920);
	config.height = object["height"].toInt(1080);
	config.fps = object["fps"].toInt(30);
	config.bitrate = object["bitrate"].toInt(6000);
	config.audio = object["audio"].toString("none");
	config.micGain = object["micGain"].toDouble(1.);
	config.systemGain = object["systemGain"].toDouble(1.);
	config.audioSources = object["audioSources"].toArray();
	config.source = object["source"].toString("screen");
	config.region = QRect(object["x"].toInt(), object["y"].toInt(), object["rw"].toInt(), object["rh"].toInt());
	config.window = static_cast<quintptr>(object["window"].toString().toULongLong());
	config.cursor = object["cursor"].toBool(true);
	config.clicks = object["clicks"].toBool(false);
	config.clickColor = object["clickColor"].toString("#ffcc33");
	config.clickRadius = object["clickRadius"].toInt(26);
	config.monitor = object["monitor"].toInt(0);
	config.monitorDevice = object["monitorDevice"].toString();
	config.encoder = object["encoder"].toString("auto");
	config.screenColor=object["screenColor"].toString("auto");config.screenWhiteNits=object["screenWhiteNits"].toDouble();config.screenExposureEV=object["screenExposureEV"].toDouble();
	config.separateAudio = object["separateAudio"].toBool();
	config.cameraTrack = object["cameraTrack"].toBool();
	config.camera = object["camera"].toInt();
	config.cameraEnabled = object["cameraEnabled"].toBool();
	config.cameraShape = object["cameraShape"].toString("rounded");
	config.cameraMirror = object["cameraMirror"].toBool(true);
	config.cameraBorder = object["cameraBorder"].toString("#ffffff");
	auto rectangle = object["cameraRect"].toArray();
	if (rectangle.size() == 4) config.cameraRect = QRectF(rectangle[0].toDouble(), rectangle[1].toDouble(), rectangle[2].toDouble(), rectangle[3].toDouble());
	config.image = object["image"].toString();
	config.outputColor = object["outputColor"].toString("sdr");
	config.denoise = object["denoise"].toBool();
	config.maxDuration = object["maxDuration"].toDouble();
	config.publishPreview = object["publishPreview"].toBool(true);
	config.effects = object["effects"].toObject();
	return config;
}

static bool ValidAudioSources(const QJsonArray& sources)
{
	if (sources.size() > MaxAdditionalAudioSources) return false;
	QStringList seen;
	for (const auto& value : sources)
	{
		const auto row = value.toObject();
		const auto id = row["id"].toString();
		const auto gain = row["gain"].toDouble(1.);
		if (id.isEmpty() || id.size() > 1024 || id.contains(QChar(0)) || seen.contains(id) ||
			row["name"].toString().size() > 200 || !row["loopback"].isBool() ||
			!std::isfinite(gain) || gain < 0 || gain > 2) return false;
		seen << id;
	}
	return true;
}

bool KRRRecordingConfig::validate(QString& error) const
{
	const QString suffix = QFileInfo(output).suffix().toLower();
	if (output.isEmpty() || !QStringList{"mp4", "mov", "mkv", "avi", "flv"}.contains(suffix))
		error = "Output must use .mp4, .mov, .mkv, .avi or .flv";
	else if (QFileInfo::exists(output))
		error = "Output already exists; choose a new file";
	else if (cameraTrack && QFileInfo::exists(output + ".camera.mkv"))
		error = "Camera track already exists; choose a new output file";
	else if (width < 160 || height < 120 || width > 7680 || height > 4320 || width % 2 || height % 2)
		error = "Resolution must be even and within 160x120 to 7680x4320";
	else if (fps < 1 || fps > RecordingFrameRates.back() || bitrate < 100 || bitrate > 100000)
		error = "FPS must be 1..144; bitrate must be 100..100000 kbps";
	else if (!QStringList{"none", "mic", "system", "mix"}.contains(audio))
		error = "Unknown audio source";
	else if (!std::isfinite(micGain) || micGain < 0 || micGain > 2 || !std::isfinite(systemGain) || systemGain < 0 || systemGain > 2)
		error = "Invalid audio gain";
	else if (!ValidAudioSources(audioSources)) error = "Invalid additional audio sources";
	else if (!QStringList{"screen", "region", "window", "camera", "image"}.contains(source))
		error = "Unknown capture source";
	else if (source == "region" && (region.width() < 2 || region.height() < 2))
		error = "Select a non-empty screen region";
	else if (source == "window" && (!IsWindow(reinterpret_cast<HWND>(window)) || IsIconic(reinterpret_cast<HWND>(window))))
		error = "Selected window is unavailable or minimized";
	else if (source == "image" && !QFileInfo::exists(image)) error = "Select an existing image";
	else if (!QStringList{"auto", "libx264", "h264_nvenc", "h264_qsv", "h264_amf"}.contains(encoder)) error = "Unknown video encoder";
	else if (separateAudio && !QStringList{"mp4", "mov", "mkv"}.contains(suffix)) error = "Separate audio tracks require MP4/MOV/MKV";
	else if (source == "screen" && monitor < 0) error = "Selected monitor is unavailable";
	else if (camera < 0 || monitor < 0 || !std::isfinite(maxDuration) || maxDuration < 0 || maxDuration > 604800) error = "Invalid camera, monitor or duration";
	else if (cameraRect.width() < .05 || cameraRect.height() < .05 || cameraRect.x() < 0 || cameraRect.y() < 0 || cameraRect.right() > 1 || cameraRect.bottom() > 1) error = "Camera rectangle must fit normalized 0..1 bounds";
	else if (!QStringList{"rectangle", "rounded", "circle"}.contains(cameraShape)) error = "Unknown camera shape";
	else if (!QStringList{"auto","highlights"}.contains(screenColor)||!std::isfinite(screenWhiteNits)||screenWhiteNits<0||screenWhiteNits>1000||!std::isfinite(screenExposureEV)||std::abs(screenExposureEV)>2) error="Invalid screen color settings";
	else if (!QStringList{"sdr", "hdr-to-sdr", "hdr", "sdr-to-hdr"}.contains(outputColor)) error = "Unknown output color";
	else if ((outputColor == "hdr" || outputColor == "sdr-to-hdr") && !QStringList{"mp4", "mov", "mkv"}.contains(suffix))
		error = "HDR output requires MP4/MOV/MKV";
	else if (!ValidateEffects(effects, error)) return false;
	else if (source == "screen" && KRRFrameCapture::CaptureArea(*this).isEmpty()) error = "Selected monitor is unavailable";
	else if (FindFFmpeg().isEmpty())
		error = "ffmpeg.exe was not found beside the application or in PATH";
	else
		return true;
	return false;
}

static QString FindTool(const QString& name)
{
	QString local = QCoreApplication::applicationDirPath() + "/" + name + ".exe";
	if (QFileInfo::exists(local))
		return local;
	return QStandardPaths::findExecutable(name);
}
QString FindFFmpeg() { return FindTool("ffmpeg"); }
QString FindFFprobe() { return FindTool("ffprobe"); }
bool IsContainerProcess()
{
	HANDLE token = nullptr; DWORD value = 0, bytes = 0;
	if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) { GetTokenInformation(token, TokenIsAppContainer, &value, sizeof(value), &bytes); CloseHandle(token); }
	return value != 0;
}
int ProcessIntegrityRid()
{
	HANDLE token = nullptr;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return -1;
	DWORD bytes = 0; GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &bytes);
	QByteArray storage(static_cast<int>(bytes), 0); int rid = -1;
	if (bytes && GetTokenInformation(token, TokenIntegrityLevel, storage.data(), bytes, &bytes))
	{
		auto sid = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(storage.data())->Label.Sid;
		if (IsValidSid(sid)) rid = static_cast<int>(*GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1));
	}
	CloseHandle(token); return rid;
}
QString DataDirectory()
{
	const auto configured = qEnvironmentVariable("WPSRECORDER_DATA_DIR");
	const auto directory = QDir::isAbsolutePath(configured) ? QDir::cleanPath(configured) :
		QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
	QDir().mkpath(directory);
	return directory;
}
bool ProbeRecordingFolder(const QString& folder, QString& error, bool create)
{
	error.clear();
	if (folder.trimmed().isEmpty() || !QDir::isAbsolutePath(folder))
	{
		error = Ui("请选择绝对路径。");
		return false;
	}
	const QFileInfo target(folder);
	if ((target.exists() && !target.isDir()) || (!target.exists() && (!create || !QDir().mkpath(folder))))
	{
		error = Ui("无法创建保存目录，请检查路径和文件夹权限。");
		return false;
	}
	QTemporaryFile probe(QDir(folder).filePath(".wpsrecorder-write-XXXXXX.tmp"));
	if (probe.open() && probe.write("1", 1) == 1 && probe.flush()) return true;
	error = probe.errorString();
	return false;
}
QString ControlServerName()
{
	return "wpsrecorder-v2-" + QString::number(qHash(qEnvironmentVariable("USERNAME") + qEnvironmentVariable("WPSRECORDER_INSTANCE")));
}
void HideConsoleProcess(QProcess& process, bool background)
{
	process.setCreateProcessArgumentsModifier([background](QProcess::CreateProcessArguments* arguments)
	{
		arguments->flags |= CREATE_NO_WINDOW;
		if (background) arguments->flags |= BELOW_NORMAL_PRIORITY_CLASS;
	});
}
bool RunMediaProcess(const QString& executable, const QStringList& arguments, QString& error, int timeout,
					 const std::atomic<bool>* cancel, bool background)
{
	QProcess process;
	HideConsoleProcess(process, background);
	process.start(executable, arguments);
	if (!process.waitForStarted(10000))
	{
		error = process.errorString();
		return false;
	}
	QElapsedTimer timer;timer.start();QByteArray errors;
	do{
		process.waitForFinished(100);errors=(errors+process.readAllStandardError()).right(12000);process.readAllStandardOutput();
		if((cancel&&cancel->load())||timer.elapsed()>timeout){process.kill();process.waitForFinished(3000);error=(cancel&&cancel->load())?Ui("任务已取消。"):"Media process timed out: "+QString::fromUtf8(errors).right(4000);return false;}
	}while(process.state()!=QProcess::NotRunning);
	if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
	{
		error = QString::fromUtf8(errors).right(6000);if(error.isEmpty())error="Media process failed";
		return false;
	}
	return true;
}

QString SelectVideoEncoder(const QString& requested, QString& error)
{
	if (requested == "libx264") return requested;
	static QMutex mutex;
	static QMap<QString, QString> failures;
	QMutexLocker lock(&mutex);
	QStringList candidates = requested == "auto" ? QStringList{"h264_nvenc", "h264_qsv", "h264_amf"} : QStringList{requested};
	for (const QString& candidate : candidates)
	{
		if (!failures.contains(candidate))
		{
			QString failure;
			RunMediaProcess(FindFFmpeg(), {"-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i", "color=s=320x240:r=30", "-frames:v", "1", "-c:v", candidate, "-f", "null", "-"}, failure, 12000);
			failures[candidate] = failure;
		}
		if (failures[candidate].isEmpty()) return candidate;
		if (requested != "auto") error = failures[candidate];
	}
	return requested == "auto" ? QString("libx264") : QString();
}
QStringList VideoEncoderArguments(const QString& encoder, int bitrate, bool recording, const QString& pixelFormat)
{
	QStringList arguments{"-c:v", encoder, "-pix_fmt", pixelFormat, "-b:v", QString::number(bitrate) + "k"};
	if (encoder == "libx264") arguments << "-preset" << (recording ? "ultrafast" : "veryfast") << "-threads" << "4";
	else if (encoder == "h264_nvenc")
	{
		arguments << "-preset" << "p4" << "-rc" << "vbr";
		if (recording) arguments << "-tune" << "ll" << "-bf" << "0" << "-zerolatency" << "1";
	}
	else if (encoder == "h264_amf") arguments << "-quality" << "speed";
	if (recording && encoder == "libx264") arguments << "-tune" << "zerolatency";
	return arguments;
}
