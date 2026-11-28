#include "recordingengine.h"
#include "recordingrecovery.h"
#include "recordingmarkers.h"
#include "recordingtiming.h"
#include "recordingpixels.h"
#include "recordingframestream.h"
#include "audiocapture.h"
#include "framecapture.h"
#include "cameracapture.h"
#include "compositor.h"

#include <windows.h>
#include <psapi.h>
#include <QDir>
#include <QBuffer>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QProcess>
#include <QPainter>
#include <QJsonDocument>
#include <QSaveFile>
#include <QUuid>
#include <memory>
#include <algorithm>
#include <cmath>

namespace
{
class FramePacer
{
public:
	FramePacer() : m_timer(CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
		TIMER_MODIFY_STATE | SYNCHRONIZE)) {}
	~FramePacer() { if (m_timer) CloseHandle(m_timer); }

	void wait(double seconds) const
	{
		LARGE_INTEGER due{};
		due.QuadPart = -qMax<qint64>(1, qRound64(qMin(seconds, .02) * 10000000.));
		if (!m_timer || !SetWaitableTimer(m_timer, &due, 0, nullptr, nullptr, FALSE))
			Sleep(1);
		else
			WaitForSingleObject(m_timer, INFINITE);
	}
private:
	HANDLE m_timer;
};

QStringList HdrEncoderOptions(const QString& encoder, int bitrate)
{
	QStringList args{"-c:v", encoder, "-pix_fmt", encoder == "hevc_nvenc" ? "p010le" : "yuv420p10le",
		"-b:v", QString::number(bitrate) + "k", "-profile:v", "main10"};
	if (encoder == "hevc_nvenc") args << "-preset" << "p4" << "-tune" << "ll" << "-bf" << "0" << "-zerolatency" << "1";
	else args << "-preset" << "ultrafast" << "-tune" << "zerolatency" << "-x265-params" << "pools=4:log-level=error";
	return args;
}

QString SelectHdrEncoder(const QString& requested, QString& error)
{
	QString details;
	for (const auto& candidate : (requested == "libx264" ? QStringList{"libx265"} : QStringList{"hevc_nvenc", "libx265"}))
	{
		QStringList probe{"-hide_banner", "-loglevel", "error", "-nostdin", "-f", "lavfi", "-i",
			"color=black:s=320x240:r=30", "-frames:v", "1"};
		probe << HdrEncoderOptions(candidate, 1000) << "-f" << "null" << "-";
		if (RunMediaProcess(FindFFmpeg(), probe, details, 4500)) return candidate;
	}
	error = "Could not initialize a 10-bit HEVC encoder: " + details;
	return {};
}

void MarkHdrCursor(QImage& frame, const QRect& area, const KRRRecordingConfig& config)
{
	CURSORINFO cursor{};
	cursor.cbSize = sizeof(cursor);
	if (area.isEmpty() || !GetCursorInfo(&cursor) || !(cursor.flags & CURSOR_SHOWING)) return;
	const QPoint cursorPoint(cursor.ptScreenPos.x, cursor.ptScreenPos.y);
	if (!area.contains(cursorPoint)) return;
	const auto scale = frame.width() / double(area.width());
	const QPointF at((cursorPoint.x() - area.x()) * scale, (cursorPoint.y() - area.y()) * frame.height() / double(area.height()));
	QPainter painter(&frame);
	painter.setRenderHint(QPainter::Antialiasing);
	const auto white = QColor::fromRgbF(.58, .58, .58);
	if (config.clicks && (GetAsyncKeyState(VK_LBUTTON) & 0x8000))
	{
		painter.setPen(QPen(QColor::fromRgbF(.56, .44, .18), 2));
		painter.drawEllipse(at, config.clickRadius * scale, config.clickRadius * scale);
	}
	if (config.cursor)
	{
		const auto size = qMax(.8, scale);
		painter.setPen(QPen(QColor::fromRgbF(.18, .18, .18), 1.2));
		painter.setBrush(white);
		painter.drawPolygon(QPolygonF({at, at + QPointF(0, 16) * size, at + QPointF(5, 12) * size,
			at + QPointF(10, 13) * size}));
	}
}

bool StartRecordingEncoder(QProcess& process, const KRRRecordingConfig& config, const QString& output,
	int width, int height, int bitrate, bool nv12, QString& error, bool hdrPixels = false)
{
	QStringList options{"-hide_banner", "-loglevel", "error", "-nostdin", "-y"};
	if (nv12) options << "-filter_threads" << "1" << "-threads" << "1";
	options << "-f" << "matroska" << "-probesize" << "32" << "-analyzeduration" << "1" << "-i" << "pipe:0"
		<< "-an" << "-fps_mode" << "passthrough" << "-enc_time_base" << QString("1:%1").arg(config.fps);
	if (nv12) options << "-threads" << "1" << "-chroma_sample_location" << "center";
	if (hdrPixels)
	{
		options << "-vf" << "scale=in_range=full:out_range=tv:out_color_matrix=bt2020,setparams=range=limited:color_primaries=bt2020:color_trc=smpte2084:colorspace=bt2020nc"
			<< "-colorspace" << "bt2020nc" << "-color_primaries" << "bt2020" << "-color_trc" << "smpte2084"
			<< "-color_range" << "tv" << HdrEncoderOptions(config.encoder, bitrate);
	}
	else
	{
		const QString tags = "setparams=range=limited:color_primaries=bt709:color_trc=iec61966-2-1:colorspace=bt709";
		const QString filter = nv12 ? tags : "scale=in_range=full:out_range=tv:out_color_matrix=bt709," + tags;
		options << "-vf" << filter << "-colorspace" << "bt709" << "-color_primaries" << "bt709"
			<< "-color_trc" << "iec61966-2-1" << "-color_range" << "tv"
			<< VideoEncoderArguments(config.encoder, bitrate, true, nv12 ? "nv12" : "yuv420p");
	}
	options << output;
	process.start(FindFFmpeg(), options);
	if (!process.waitForStarted(10000))
	{
		error = process.errorString();
		return false;
	}
	const auto header = RecordingVideoHeader(width, height, config.fps, nv12, hdrPixels);
	if (process.write(header) == header.size()) return true;
	error = "Could not send the recording header to the encoder";
	return false;
}

bool WriteRecordingFrame(QProcess& process, qint64 slot, int fps, const char* pixels, qint64 size)
{
	const auto header = RecordingFrameHeader(slot, fps, size);
	return process.write(header) == header.size() && process.write(pixels, size) == size;
}
}

KRRRecordingEngine::KRRRecordingEngine(QObject* parent) : QObject(parent) {}
KRRRecordingEngine::~KRRRecordingEngine()
{
	requestStop();
	if (m_thread.joinable()) m_thread.join();
}
bool KRRRecordingEngine::isActive() const { return m_active.load(); }
void KRRRecordingEngine::requestStop()
{
	double unset = -1;
	m_stopTime.compare_exchange_strong(unset, m_clock.elapsed());
	m_stop.store(true);
}
void KRRRecordingEngine::setPaused(bool paused)
{
	if (!m_active.load()) return;
	m_clock.setPaused(paused);
	m_paused.store(paused);
}
bool KRRRecordingEngine::start(const KRRRecordingConfig& requested, QString& error)
{
	if (m_active.load()) { error = "A recording is already active"; return false; }
	auto config = requested;
	if (config.source == "screen" && config.monitorDevice.isEmpty())
	{
		config.monitorDevice = KRRFrameCapture::MonitorDevice(config.monitor);
		if (config.monitorDevice.isEmpty()) { error = "Selected monitor is unavailable"; return false; }
	}
	if (!config.validate(error)) return false;
	if (m_thread.joinable()) m_thread.join();
	m_stop.store(false);
	m_paused.store(false);
	setPreviewPublication(config.publishPreview);
	m_duplicates.store(0);
	m_missedSlots.store(0);
	m_stopTime.store(-1);
	m_duration.store(0);
	m_micLevel.store(0);
	m_systemLevel.store(0);
	for (auto& level : m_audioLevels) level.store(0);
	m_clock.reset();
	m_previewPublisher.reset();
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_frames.store(0);
		m_previousFpsSample = 0;
		m_previousFrameCount = 0;
		m_effectiveFps = 0;
		m_active.store(true);
		m_phase = "starting";m_captureInfo={};
		m_output = config.output;
		m_error.clear();
		m_live = config;
		m_captureControlError.clear();
		m_encoder.clear();
		m_failureStage.clear();
		m_recoveryWorkspace.clear();
		m_markerWorkspace.clear();
		m_markers = {};
		m_outputPublished = false;
	}
	m_thread = std::thread([this, config] { record(config); });
	return true;
}
bool KRRRecordingEngine::updateScene(const QJsonObject& scene, QString& error)
{
	if (!m_active.load()) { error = "No active recording"; return false; }
	std::lock_guard<std::mutex> lock(m_mutex);
	auto object = m_live.toJson();
	const QStringList allowed{"screenColor", "screenWhiteNits", "screenExposureEV", "source", "monitor", "monitorDevice", "x", "y", "rw", "rh", "window", "image", "cameraEnabled", "cameraShape", "cameraMirror", "cameraRect", "cameraBorder", "effects", "cursor", "clicks", "micGain", "systemGain", "audioSources"};
	for (auto it = scene.begin(); it != scene.end(); ++it)
	{
		if (it.key() == "micEnabled" || it.key() == "systemEnabled")
		{
			if (!it.value().isBool()) { error = "Audio toggle must be boolean"; return false; }
			const auto audio = object["audio"].toString();
			const bool mic = it.key() == "micEnabled" ? it.value().toBool() : audio == "mic" || audio == "mix";
			const bool system = it.key() == "systemEnabled" ? it.value().toBool() : audio == "system" || audio == "mix";
			object["audio"] = mic ? (system ? "mix" : "mic") : (system ? "system" : "none");
			continue;
		}
		if (!allowed.contains(it.key())) { error = "This field cannot change during recording: " + it.key(); return false; }
		object[it.key()] = it.value();
	}
	auto candidate = KRRRecordingConfig::FromJson(object);
	if (candidate.source == "screen")
	{
		if (candidate.monitorDevice.isEmpty() || (!scene.contains("monitorDevice") &&
			(m_live.source != "screen" || candidate.monitor != m_live.monitor)))
			candidate.monitorDevice = KRRFrameCapture::MonitorDevice(candidate.monitor);
	}
	else candidate.monitorDevice.clear();
	if (!candidate.validate(error)) return false;
	// Device assignment is fixed at the start. Apply mute and gain to the matching endpoints.
	auto sources = m_live.audioSources;
	for (int i = 0; i < sources.size(); ++i)
	{
		auto row = sources[i].toObject();
		for (const auto& value : candidate.audioSources)
		{
			const auto update = value.toObject();
			if (update["id"] != row["id"]) continue;
			row["gain"] = update["gain"];
			row["muted"] = update["muted"];
			break;
		}
		sources.replace(i, row);
	}
	candidate.audioSources = sources;
	if (candidate.cameraEnabled != m_live.cameraEnabled || candidate.audio != m_live.audio)
		m_captureControlError.clear();
	m_live = candidate;
	return true;
}

QJsonObject KRRRecordingEngine::audioLevels()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	const bool active = m_active.load();
	const bool audible = active && !m_paused.load() && !m_stop.load();
	QJsonObject extra;
	for (int i = 0; i < m_live.audioSources.size(); ++i)
		extra[m_live.audioSources[i].toObject()["id"].toString()] = audible ? m_audioLevels[i].load() : 0.f;
	return {{"ok", true}, {"state", active ? "recording" : "idle"}, {"audio_levels", extra},
		{"mic_level", audible ? m_micLevel.load() : 0.f}, {"system_level", audible ? m_systemLevel.load() : 0.f}};
}

bool KRRRecordingEngine::addMarker(const QString& label, QJsonObject& marker, QString& error)
{
	const auto ms = qRound64(m_clock.elapsed() * 1000);
	std::lock_guard<std::mutex> markerWrite(m_markerWriteMutex);
	QJsonArray pending;
	QString path;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (!m_active.load() || m_phase != "recording" || m_stop.load() || m_markerWorkspace.isEmpty())
		{
			error = "Markers need an active recording";
			return false;
		}
		if (m_markers.size() >= 500 || label.size() > 120)
		{
			error = "Use up to 500 recording markers, with no more than 120 characters each";
			return false;
		}
		pending = m_markers;
		path = RecordingMarkersPath(QDir(m_markerWorkspace).filePath("video.mkv"));
	}
	marker = {{"at_ms", ms}, {"label", label.simplified()}};
	pending.append(marker);
	if (!WriteRecordingMarkers(path, pending, error)) return false;
	std::lock_guard<std::mutex> lock(m_mutex);
	m_markers = pending;
	return true;
}

void KRRRecordingEngine::record(KRRRecordingConfig config)
{
	QString failure;
	QString sourceLoss;
	const auto profile = qEnvironmentVariableIntValue("WPSRECORDER_STAGE_TIMINGS") == 1 ?
		std::make_unique<KRRRecordingTimings>() : nullptr;
	QJsonObject pipeError;
	QString directory = QFileInfo(config.output).absolutePath();
	QString workspace = directory + "/.wpsrecorder-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
	QString temporaryVideo = workspace + "/video.mkv";
	QString partial = workspace + "/result." + QFileInfo(config.output).suffix();
	QLockFile outputLock(config.output + ".lock");
	QLockFile workspaceLock(workspace + "/recording.lock");
	outputLock.setStaleLockTime(0);
	workspaceLock.setStaleLockTime(0);
	bool published = false;
	std::vector<std::unique_ptr<KRRAudioCapture>> audio;
	QStringList audioFiles, audioNames;
	QVector<bool> removeNoise;
	std::array<KRRAudioCapture*, MaxAdditionalAudioSources> extraAudio{};
	for (auto& level : m_audioLevels) level.store(0);
	const auto addAudio = [&](std::unique_ptr<KRRAudioCapture> source, const QString& path, const QString& name, bool denoise)
	{
		auto pointer = source.get();
		audioFiles.append(path);
		audioNames.append(name);
		removeNoise.append(denoise);
		audio.push_back(std::move(source));
		return pointer;
	};
	KRRAudioCapture *mic = nullptr, *systemAudio = nullptr;
	m_micLevel.store(0);
	m_systemLevel.store(0);
	QProcess encoder;
	QProcess cameraEncoder;
	HideConsoleProcess(cameraEncoder);
	KRRCameraCapture camera;
	bool cameraStarted = false;
	QString cameraVideo = workspace + "/camera.mkv";
	qint64 cameraFrames = 0;
	HideConsoleProcess(encoder);
	encoder.setStandardErrorFile(workspace + "/encoder.log");
	auto run = [&]() -> bool
	{
		m_pipelineStage.store("prepare");
		if (!outputLock.tryLock(0) || !QDir().mkpath(workspace) || !workspaceLock.tryLock(0))
		{
			failure = "Could not create output workspace or output is locked";
			return false;
		}
		WriteRecordingRecovery(workspace, config.output, "recording");
		{ std::lock_guard<std::mutex> lock(m_mutex); m_markerWorkspace = workspace; }
		m_pipelineStage.store("select-encoder");
		const bool hdrPixels = config.outputColor == "hdr" || config.outputColor == "sdr-to-hdr";
		const auto requestedEncoder = config.encoder;
		config.encoder = hdrPixels ? SelectHdrEncoder(requestedEncoder, failure) : SelectVideoEncoder(requestedEncoder, failure);
		if (config.encoder.isEmpty()) return false;
		if (m_stop.load()) return true;
		{ std::lock_guard<std::mutex> lock(m_mutex); m_encoder = config.encoder; }
		if (config.source == "camera" || config.cameraTrack)
		{
			m_pipelineStage.store("open-camera");
			if (!camera.start(config.camera, failure)) return false;
			cameraStarted = true;
		}
		if (config.cameraTrack)
		{
			QString cameraError;
			camera.frame(cameraError);
			cameraEncoder.setStandardErrorFile(workspace + "/camera-encoder.log");
			auto cameraConfig = config;
			if (hdrPixels) cameraConfig.encoder = config.encoder == "hevc_nvenc" ? "h264_nvenc" : "libx264";
			if (!StartRecordingEncoder(cameraEncoder, cameraConfig, cameraVideo, 640, 480, 2500, false, failure)) return false;
			m_cameraEncoderPid.store(cameraEncoder.processId());
		}
		// Startup audio packets are discarded until the first video frame reaches the encoder.
		m_pipelineStage.store("open-audio");
		for (const QString& source : QStringList{"mic", "system"})
		{
			if (config.audio != source && config.audio != "mix") continue;
			QString path = workspace + "/" + source + ".f32";
			auto device = std::make_unique<KRRAudioCapture>(source == "system", m_clock);
			device->setGain(float(source == "system" ? config.systemGain : config.micGain));
			if (!device->start(path, failure)) return false;
			if (source == "mic") mic = device.get();
			else systemAudio = device.get();
			addAudio(std::move(device), path, source, source == "mic" && config.denoise);
		}
		for (int i = 0; i < config.audioSources.size(); ++i)
		{
			const auto row = config.audioSources[i].toObject();
			const auto path = workspace + QString("/extra-%1.f32").arg(i);
			const bool loopback = row["loopback"].toBool();
			auto device = std::make_unique<KRRAudioCapture>(loopback, m_clock, row["id"].toString());
			device->setGain(float(row["gain"].toDouble(1.)));
			device->setMuted(row["muted"].toBool());
			QString audioError;
			if (device->start(path, audioError))
			{
				extraAudio[i] = addAudio(std::move(device), path, row["name"].toString(), !loopback && row["denoise"].toBool());
			}
			else
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				m_captureControlError = audioError;
			}
		}
		const bool nv12 = config.encoder == "h264_nvenc";
		const qint64 rawBytesPerFrame = static_cast<qint64>(config.width) * config.height * (hdrPixels ? 8 : 4);
		m_pipelineStage.store("start-encoder");
		if (!StartRecordingEncoder(encoder, config, temporaryVideo, config.width, config.height, config.bitrate, nv12, failure, hdrPixels))
			return false;
		m_encoderPid.store(encoder.processId());
		KRRFrameCapture capture;
		FramePacer pacer;
		QImage previousFrame;
		QImage previousCamera;
		QImage currentCamera;
		QByteArray videoPixels;
		qint64 pixelKey = 0;
		qint64 lastSlot = -1;
		qint64 lastCameraSlot = -1, lastCameraKey = 0;
		QImage cameraPixels;
		QElapsedTimer stopBudget;
		auto drainInput = [&](QProcess& process, qint64 queuedLimit)
		{
			QElapsedTimer progress;
			progress.start();
			while (process.bytesToWrite() > queuedLimit)
			{
				if (m_stop.load() && !stopBudget.isValid()) stopBudget.start();
				if (process.waitForBytesWritten(100)) progress.restart();
				if (process.bytesToWrite() <= queuedLimit) break;
				if (process.state() == QProcess::NotRunning || progress.hasExpired(10000) ||
					(stopBudget.isValid() && stopBudget.hasExpired(1500))) return false;
			}
			return process.state() != QProcess::NotRunning;
		};
		auto writeCameraFrame = [&](const QImage& source, qint64 slot, bool hold) -> bool
		{
			if (!config.cameraTrack || source.isNull() || slot <= lastCameraSlot) return true;
			const bool changed = lastCameraKey != source.cacheKey();
			if (!changed && !hold) return true;
			if (changed)
			{
				cameraPixels = source.convertToFormat(QImage::Format_RGB32).scaled(640, 480, Qt::IgnoreAspectRatio, Qt::FastTransformation);
				lastCameraKey = source.cacheKey();
			}
			m_pipelineStage.store("write-camera-track");
			const qint64 bytes = cameraPixels.sizeInBytes();
			if (!WriteRecordingFrame(cameraEncoder, slot, config.fps, reinterpret_cast<const char*>(cameraPixels.constBits()), bytes))
			{
				failure = "Camera track pipe closed";
				return false;
			}
			if (!drainInput(cameraEncoder, cameraFrames > 0 ? bytes : 0))
			{
				failure = "Camera track encoder stalled";
				return false;
			}
			lastCameraSlot = slot;
			++cameraFrames;
			return true;
		};
		auto writeFrame = [&](const QImage& frame, const QImage& cameraFrame, qint64 slot, bool repeat = false) -> bool
		{
			const char* pixels = reinterpret_cast<const char*>(frame.constBits());
			qint64 bytes = rawBytesPerFrame;
			if (nv12)
			{
				if (videoPixels.isEmpty() || frame.cacheKey() != pixelKey)
				{
					KRRStageTimer convert(profile.get(), KRRRecordingTimings::ConvertVideo);
					PackRecordingNv12(frame.format() == QImage::Format_RGB32 ? frame :
						frame.convertToFormat(QImage::Format_RGB32), videoPixels);
					pixelKey = frame.cacheKey();
				}
				pixels = videoPixels.constData();
				bytes = videoPixels.size();
			}
			m_pipelineStage.store("write-video");
			KRRStageTimer buffer(profile.get(), KRRRecordingTimings::PipeBuffer);
			const bool accepted = WriteRecordingFrame(encoder, slot, config.fps, pixels, bytes);
			buffer.finish();
			if (!accepted)
			{
				failure = "Encoder input pipe closed";
				return false;
			}
			{
				KRRStageTimer wait(profile.get(), repeat ? KRRRecordingTimings::RepeatWait : KRRRecordingTimings::PipeWait);
				// Overlap the next capture with one queued frame. Start the audio clock after the first frame is consumed.
				if (!drainInput(encoder, lastSlot >= 0 ? bytes : 0))
				{
					if (profile) pipeError = {{"error", static_cast<int>(encoder.error())},
						{"detail", encoder.errorString()}, {"state", static_cast<int>(encoder.state())},
						{"pending_bytes", encoder.bytesToWrite()}, {"repeat", repeat}};
					failure = "Encoder input pipe stalled";
					return false;
				}
			}
			if (!writeCameraFrame(cameraFrame, slot, repeat)) return false;
			m_missedSlots.fetch_add(slot - lastSlot - 1);
			if (repeat) m_duplicates.fetch_add(1);
			lastSlot = slot;
			m_frames.fetch_add(1);
			m_duration.store(static_cast<double>(slot + 1) / config.fps);
			return true;
		};
		while (!m_stop.load())
		{
			if (m_paused.load()) { Sleep(5); continue; }
			double elapsed = m_clock.elapsed();
			if (config.maxDuration > 0 && elapsed >= config.maxDuration)
			{
				double unset = -1;
				m_stopTime.compare_exchange_strong(unset, config.maxDuration);
				break;
			}
			double nextFrame = static_cast<double>(lastSlot + 1) / config.fps;
			if (elapsed < nextFrame)
			{
				KRRStageTimer pacing(profile.get(), KRRRecordingTimings::Pacing);
				pacer.wait(nextFrame - elapsed);
				continue;
			}
			KRRRecordingConfig scene;
			{ std::lock_guard<std::mutex> lock(m_mutex); scene = m_live; }
			const bool micEnabled = scene.audio == "mic" || scene.audio == "mix";
			if (micEnabled && !mic)
			{
				const auto path = workspace + "/mic.f32";
				auto device = std::make_unique<KRRAudioCapture>(false, m_clock);
				device->setGain(float(scene.micGain));
				QString audioError;
				if (device->start(path, audioError))
				{
					mic = addAudio(std::move(device), path, "mic", config.denoise);
				}
				else
				{
					std::lock_guard<std::mutex> lock(m_mutex);
					m_live.audio = m_live.audio == "mix" ? "system" : "none";
					m_captureControlError = audioError;
				}
			}
			if (mic)
			{
				mic->setMuted(!micEnabled);
				mic->setGain(float(scene.micGain));
				m_micLevel.store(micEnabled ? qMax(mic->takePeak(), m_micLevel.load() * .95f) : 0.f);
			}
			const bool systemEnabled = scene.audio == "system" || scene.audio == "mix";
			if (systemEnabled && !systemAudio)
			{
				const auto path = workspace + "/system.f32";
				auto device = std::make_unique<KRRAudioCapture>(true, m_clock);
				device->setGain(float(scene.systemGain));
				QString audioError;
				if (device->start(path, audioError))
					systemAudio = addAudio(std::move(device), path, "system", false);
				else
				{
					std::lock_guard<std::mutex> lock(m_mutex);
					m_live.audio = m_live.audio == "mix" ? "mic" : "none";
					m_captureControlError = audioError;
				}
			}
			if (systemAudio)
			{
				systemAudio->setMuted(!systemEnabled);
				systemAudio->setGain(float(scene.systemGain));
				m_systemLevel.store(systemEnabled ? qMax(systemAudio->takePeak(), m_systemLevel.load() * .95f) : 0.f);
			}
			for (int i = 0; i < scene.audioSources.size(); ++i)
				if (auto device = extraAudio[i])
				{
					const auto row = scene.audioSources[i].toObject();
					const bool muted = row["muted"].toBool();
					device->setMuted(muted);
					device->setGain(float(row["gain"].toDouble(1.)));
					m_audioLevels[i].store(muted ? 0.f : qMax(device->takePeak(), m_audioLevels[i].load() * .95f));
				}
			const bool needsCamera = scene.source == "camera" || scene.cameraEnabled || config.cameraTrack;
			if (needsCamera && !cameraStarted && !camera.isRunning())
			{
				camera.startAsync(scene.camera);
				cameraStarted = true;
			}
			else if (!needsCamera && cameraStarted)
			{
				camera.stopAsync();
				cameraStarted = false;
				currentCamera = {};
			}
			m_pipelineStage.store("read-camera");
			if (cameraStarted)
			{
				QString cameraError;
				currentCamera = camera.frame(cameraError);
				if (!cameraError.isEmpty())
				{
					if (scene.source == "camera" || config.cameraTrack) { failure = cameraError; break; }
					camera.stopAsync();
					cameraStarted = false;
					std::lock_guard<std::mutex> lock(m_mutex);
					m_live.cameraEnabled = scene.cameraEnabled = false;
					m_captureControlError = cameraError;
				}
			}
			m_pipelineStage.store("compose");
			KRRStageTimer compose(profile.get(), KRRRecordingTimings::Compose);
			QImage frame;
			if (scene.source == "camera")
			{
				if (currentCamera.isNull())
				{
					pacer.wait(1.0 / config.fps);
					continue;
				}
				frame = currentCamera.convertToFormat(QImage::Format_RGB32);
				if (scene.cameraMirror) frame = frame.mirrored(true, false);
				frame = frame.scaled(config.width, config.height);
			}
			else
			{
				auto captureScene = scene;
				if (scene.outputColor == "hdr-to-sdr") captureScene.screenColor = "highlights";
				// Effects are composited in SDR, so privacy masks and overlays stay effective on HDR exports.
				if (scene.cameraEnabled || !scene.effects.isEmpty()) captureScene.outputColor = "sdr";
				frame = capture.capture(captureScene, failure, profile.get());
				if (captureScene.outputColor != "hdr" && frame.format() == QImage::Format_RGBA64)
					frame = ToneMapRecordingPreview(frame);
				std::lock_guard<std::mutex> lock(m_mutex);
				m_captureInfo = capture.diagnostics();
				const auto status = m_captureInfo["source_status"].toString();
				if (frame.isNull() && (status == "display_disconnected" || status == "window_closed" || status == "window_minimized"))
				{
					sourceLoss = failure;
					failure.clear();
				}
			}
			if (frame.isNull()) break;
			const bool nativeHdr = frame.format() == QImage::Format_RGBA64;
			if (nativeHdr && (scene.cursor || scene.clicks))
				MarkHdrCursor(frame, KRRFrameCapture::CaptureArea(scene), scene);
			if (scene.cameraEnabled && scene.source != "camera") DrawCamera(frame, currentCamera, scene);
			if (!scene.effects.isEmpty())
			{
				POINT cursor{};
				GetCursorPos(&cursor);
				const auto area = KRRFrameCapture::CaptureArea(scene);
				QPointF pointer(area.width() > 0 ? (cursor.x - area.x()) / static_cast<double>(area.width()) : .5,
					area.height() > 0 ? (cursor.y - area.y()) / static_cast<double>(area.height()) : .5);
				ApplyEffects(frame, scene.effects, pointer);
			}
			const auto preview = frame;
			if (hdrPixels && !nativeHdr) frame = ExpandSdrRecordingFrame(frame);
			compose.finish();
			// Submit implicitly shared images only. Scaling and IPC run off the recording thread.
			if (m_publishPreview.load(std::memory_order_relaxed))
				m_previewPublisher.submit(preview, (scene.source == "camera" || scene.cameraEnabled || scene.cameraTrack) ? currentCamera : QImage{}, scene.cameraMirror, m_frames.load() + 1);
			const bool firstFrame = lastSlot < 0;
			// A missed slot is a gap on the recording clock. Rewriting its pixels would increase backpressure.
			const auto slot = qMax(lastSlot + 1, static_cast<qint64>(std::floor(elapsed * config.fps)));
			if (m_stop.load() || !failure.isEmpty() || !writeFrame(frame, currentCamera, slot)) break;
			previousFrame = frame;
			previousCamera = currentCamera;
			if (firstFrame)
			{
				m_clock.begin();
				std::lock_guard<std::mutex> lock(m_mutex);
				m_previousFrameCount = m_frames.load();
				m_phase = "recording";
			}
			for (const auto& device : audio)
			{
				if (!device->error().isEmpty()) { m_pipelineStage.store("capture-audio"); failure = device->error(); break; }
			}
			if (!failure.isEmpty()) break;
		}
		// Hold the last image through Stop with a single timed frame, even after a sustained gap.
		const auto finalSlot = static_cast<qint64>(std::ceil(m_stopTime.load() * config.fps)) - 1;
		if (failure.isEmpty() && sourceLoss.isEmpty() && !previousFrame.isNull() && finalSlot > lastSlot)
		{
			if (!stopBudget.isValid()) stopBudget.start();
			if (!writeFrame(previousFrame, previousCamera, finalSlot, true)) return false;
		}
		if (failure.isEmpty() && !writeCameraFrame(previousCamera, lastSlot, true)) return false;
		return true;
	};
	run();
	if (!sourceLoss.isEmpty() && m_frames.load() == 0) failure = sourceLoss;
	const bool stoppedBeforeFirstFrame = m_stop.load() && m_frames.load() == 0 && failure.isEmpty();
	QString failureStage;
	if (!failure.isEmpty())
		failureStage = sourceLoss.isEmpty() ? QString::fromLatin1(m_pipelineStage.load()) : QStringLiteral("capture-source");
	m_pipelineStage.store("stop-camera");
	camera.stop();
	for (auto& device : audio) device->stop();
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_phase = "finalizing";
	}
	// A marker accepted before finalizing must reach the workspace before it can be moved.
	{ std::lock_guard<std::mutex> pendingMarker(m_markerWriteMutex); }
	const auto reportEncoderFailure = [&](const QString& detail)
	{
		if (stoppedBeforeFirstFrame) return;
		if (!failure.isEmpty()) failure += '\n';
		failure += detail;
		if (failureStage.isEmpty()) failureStage = QString::fromLatin1(m_pipelineStage.load());
	};
	if (encoder.state() != QProcess::NotRunning)
	{
		m_pipelineStage.store("finalize-video");
		encoder.closeWriteChannel();
		const int timeout = stoppedBeforeFirstFrame || (m_stop.load() && encoder.bytesToWrite() > 0) ? 3000 : 30000;
		if (!encoder.waitForFinished(timeout)) { encoder.kill(); encoder.waitForFinished(3000); reportEncoderFailure("Encoder finalization timed out."); }
		else if (encoder.exitCode() != 0) reportEncoderFailure("Encoder failed; see " + workspace + "/encoder.log");
	}
	m_encoderPid.store(0);
	if (cameraEncoder.state() != QProcess::NotRunning)
	{
		m_pipelineStage.store("finalize-camera-track");
		cameraEncoder.closeWriteChannel();
		const int timeout = stoppedBeforeFirstFrame || (m_stop.load() && cameraEncoder.bytesToWrite() > 0) ? 3000 : 30000;
		if (!cameraEncoder.waitForFinished(timeout)) { cameraEncoder.kill(); cameraEncoder.waitForFinished(3000); reportEncoderFailure("Camera encoder timed out."); }
		else if (cameraEncoder.exitCode() != 0) reportEncoderFailure("Camera encoder failed.");
	}
	m_cameraEncoderPid.store(0);
	if (m_frames.load() > 0 && QFileInfo::exists(temporaryVideo))
	{
		m_pipelineStage.store("mux");
		QStringList mux{"-hide_banner", "-loglevel", "error", "-nostdin", "-y", "-i", temporaryVideo};
		for (const QString& path : audioFiles)
			mux << "-f" << "f32le" << "-ar" << "48000" << "-ac" << "2" << "-i" << path;
		mux << "-map" << "0:v:0" << "-c:v" << "copy";
		const auto cleanup = [&removeNoise](int i) { return removeNoise.value(i) ? QString("highpass=f=80,lowpass=f=12000,afftdn=nf=-25,") : QString{}; };
		if (config.separateAudio)
		{
			for (int i = 0; i < audioFiles.size(); ++i)
			{
				const auto name = audioNames[i];
				mux << "-map" << QString("%1:a:0").arg(i + 1) << QString("-filter:a:%1").arg(i) << cleanup(i) + "apad"
					<< QString("-metadata:s:a:%1").arg(i) << ("title=" + name)
					<< QString("-metadata:s:a:%1").arg(i) << ("handler_name=" + name);
			}
		}
		else if (audioFiles.size() > 1)
		{
			QString filter, inputs;
			for (int i = 0; i < audioFiles.size(); ++i)
			{
				const auto stream = QString("[%1:a]").arg(i + 1);
				if (removeNoise.value(i))
				{
					const auto name = QString("[clean%1]").arg(i);
					filter += stream + cleanup(i).chopped(1) + name + ";";
					inputs += name;
				}
				else inputs += stream;
			}
			mux << "-filter_complex" << filter + inputs + QString("amix=inputs=%1:duration=longest:normalize=1,apad[a]").arg(audioFiles.size())
				<< "-map" << "[a]";
		}
		else if (audioFiles.size() == 1)
			mux << "-map" << "1:a:0" << "-af" << cleanup(0) + "apad";
		if (!audioFiles.empty()) mux << "-c:a" << "aac" << "-b:a" << "192k";
		mux << "-t" << QString::number(m_duration.load(), 'f', 6);
		QString extension = QFileInfo(config.output).suffix().toLower();
		if (extension == "avi") mux << "-bsf:v" << "h264_mp4toannexb";
		if (extension == "mp4" || extension == "mov")
		{
			// Recover the exact frame ticks after the intermediate Matroska rounded them to milliseconds.
			mux << "-video_track_timescale" << QString::number(config.fps) << "-movflags" << "+faststart";
		}
		mux << partial;
		QString muxError;
		const auto muxBudget = static_cast<int>(qBound(120000.,
			60000. + m_duration.load() * 150. + QFileInfo(temporaryVideo).size() / 1048576. * 100., 1800000.));
		if (!RunMediaProcess(FindFFmpeg(), mux, muxError, muxBudget)) failure += " Mux failed: " + muxError;
		else if (!QFile::rename(partial, config.output)) failure += " Could not publish the completed output";
		else
		{
			published = true;
			if (QFile::exists(RecordingMarkersPath(temporaryVideo)))
			{
				const auto staged = RecordingMarkersPath(temporaryVideo);
				const auto destination = RecordingMarkersPath(config.output);
				if (!QFile::rename(staged, destination)) failure += " Could not save recording markers";
			}
			if (config.cameraTrack && cameraFrames > 0)
			{
				QString trackOutput = config.output + ".camera.mkv";
				if (!QFile::rename(cameraVideo, trackOutput)) failure += " Could not publish camera track; retained at " + cameraVideo;
			}
			// Workspace is a freshly generated child directory owned by this recording.
			if (failure.isEmpty())
			{
				QFile complete(workspace + "/complete");
				if (complete.open(QIODevice::WriteOnly)) complete.write("ok", 2);
				complete.close();
				workspaceLock.unlock();
				QDir(workspace).removeRecursively();
			}
		}
		if (failureStage.isEmpty() && !failure.isEmpty()) failureStage = "mux";
	}
	else if (stoppedBeforeFirstFrame && workspaceLock.isLocked())
	{
		workspaceLock.unlock();
		QDir(workspace).removeRecursively();
	}
	else if (failure.isEmpty())
	{
		failure = "Recording stopped before a video frame was written";
		failureStage = "finalize-video";
	}
	QString recovered;
	if (workspaceLock.isLocked())
	{
		if (QFileInfo(temporaryVideo).size() > 0)
		{
			WriteRecordingRecovery(workspace, config.output, failureStage, failure.trimmed());
			recovered = workspace;
		}
		workspaceLock.unlock();
	}
	if (failure.isEmpty() && !sourceLoss.isEmpty())
	{
		failure = sourceLoss;
		failureStage = "capture-source";
	}
	if (profile)
	{
		const QJsonObject timing{{"version", 1}, {"encoder", config.encoder}, {"fps", config.fps},
			{"frames_written", m_frames.load()}, {"duplicate_frames", m_duplicates.load()},
			{"missed_frame_slots", m_missedSlots.load()}, {"pipe_error", pipeError},
			{"failure", failure.trimmed()}, {"failure_stage", failureStage}, {"stages", profile->summary()}};
		QSaveFile file(config.output + ".timing.json");
		const auto json = QJsonDocument(timing).toJson();
		if (file.open(QIODevice::WriteOnly) && file.write(json) == json.size()) file.commit();
	}
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_phase = "idle";
		m_error = failure;
		m_failureStage = failureStage;
		m_recoveryWorkspace = recovered;
		m_outputPublished = published;
	}
	m_active.store(false);
	m_previewPublisher.clear();
	m_pipelineStage.store("idle");
	emit completed(config.output, failure);
}

QJsonObject KRRRecordingEngine::preview()
{
	if(!isActive())return {{"ok",true},{"active",false},{"paused",false},{"sequence",0},{"frame",""},{"camera",""}};
	auto latest = m_previewPublisher.snapshot();
	const auto& frame = latest.frame; const auto& camera = latest.camera; qint64 sequence = latest.sequence;
	auto encode = [](const QImage& image) { QByteArray bytes; if (!image.isNull()) { QBuffer buffer(&bytes); buffer.open(QIODevice::WriteOnly); image.save(&buffer, "JPEG", 92); } return QString::fromLatin1(bytes.toBase64()); };
	return {{"ok", true}, {"active", isActive()}, {"paused", m_paused.load()}, {"sequence", sequence}, {"frame", encode(frame)}, {"camera", encode(camera)}};
}

static double ProcessCpu(HANDLE process)
{
	FILETIME created{}, ended{}, kernel{}, user{};
	if (!GetProcessTimes(process, &created, &ended, &kernel, &user)) return 0;
	ULARGE_INTEGER kernelTime{}, userTime{};
	kernelTime.LowPart = kernel.dwLowDateTime; kernelTime.HighPart = kernel.dwHighDateTime;
	userTime.LowPart = user.dwLowDateTime; userTime.HighPart = user.dwHighDateTime;
	return static_cast<double>(kernelTime.QuadPart + userTime.QuadPart) / 10000000.0;
}
QJsonObject KRRRecordingEngine::status()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	const double now = KRRRecordingClock::Now();
	const auto samplingTime = now - m_previousCpuSample;
	const bool active = m_active.load();
	const auto phase = active && m_stop.load() ? QStringLiteral("finalizing") : m_phase;
	const bool recording = active && phase == "recording";
	const bool paused = m_paused.load();
	const double duration = recording ? m_clock.elapsed() : m_duration.load();
	const auto frameCount = m_frames.load();
	if (recording && !paused && duration - m_previousFpsSample >= .25)
	{
		m_effectiveFps = std::max<qint64>(0, frameCount - m_previousFrameCount) / (duration - m_previousFpsSample);
		m_previousFrameCount = frameCount;
		m_previousFpsSample = duration;
	}
	if (m_previousCpuSample == 0 || samplingTime >= .25)
	{
		PROCESS_MEMORY_COUNTERS memory{};
		GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory));
		m_memoryMB = static_cast<double>(memory.WorkingSetSize) / 1048576.0;
		std::array<double, 3> cpuTime{ProcessCpu(GetCurrentProcess())};
		const std::array<qint64, 2> encoders{m_encoderPid.load(), m_cameraEncoderPid.load()};
		double cpuDelta = std::max(0.0, cpuTime[0] - m_previousCpu[0]);
		for (size_t i = 0; i < encoders.size(); ++i)
		{
			if (!encoders[i]) continue;
			const auto encoder = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, static_cast<DWORD>(encoders[i]));
			if (!encoder) continue;
			if (GetProcessMemoryInfo(encoder, &memory, sizeof(memory))) m_memoryMB += static_cast<double>(memory.WorkingSetSize) / 1048576.0;
			cpuTime[i + 1] = ProcessCpu(encoder);
			if (encoders[i] == m_sampledEncoders[i] && m_previousCpu[i + 1] > 0)
				cpuDelta += std::max(0.0, cpuTime[i + 1] - m_previousCpu[i + 1]);
			CloseHandle(encoder);
		}
		if (m_previousCpuSample > 0)
		{
			const auto processors = std::max<DWORD>(1, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
			m_cpuPercent = cpuDelta / samplingTime * 100.0 / processors;
		}
		m_previousCpuSample = now;
		m_previousCpu = cpuTime;
		m_sampledEncoders = encoders;
	}
	const double effectiveRate = recording && !paused ? m_effectiveFps : 0.;
	QJsonObject extraLevels;
	for (int i = 0; i < m_live.audioSources.size(); ++i)
		extraLevels[m_live.audioSources[i].toObject()["id"].toString()] = m_audioLevels[i].load();
	return {{"audio_levels", extraLevels}, {"system_enabled", m_live.audio == "system" || m_live.audio == "mix"}, {"capture",m_captureInfo},{"state", active ? "recording" : "idle"}, {"phase", phase},
		{"failure_stage", m_failureStage}, {"recovery_workspace", m_recoveryWorkspace},
		{"output_published", m_outputPublished},
		{"marker_count", m_markers.size()}, {"target_fps", m_live.fps}, {"width", m_live.width},
		{"height", m_live.height}, {"bitrate", m_live.bitrate}, {"effective_fps", effectiveRate},
		{"mic_level", m_micLevel.load()}, {"system_level", m_systemLevel.load()},
		{"capture_control_error", m_captureControlError}, {"pip_enabled", m_live.cameraEnabled},
		{"mic_enabled", m_live.audio == "mic" || m_live.audio == "mix"}, {"paused", paused}, {"duration_sec", duration}, {"frames_written", frameCount},
		{"duplicate_frames", m_duplicates.load()}, {"missed_frame_slots", m_missedSlots.load()},
		{"pipeline_stage", QString::fromLatin1(m_pipelineStage.load())},
		{"avg_fps", duration > 0 ? frameCount / duration : 0}, {"mem_mb", m_memoryMB}, {"cpu_pct", m_cpuPercent},
		{"output", m_output}, {"error", m_error}, {"encoder", m_encoder}, {"source", m_live.source},
		{"output_color", m_live.outputColor}, {"audio_mode", m_live.audio}, {"pid", static_cast<qint64>(GetCurrentProcessId())}};
}
