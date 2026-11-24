#include "webstudio.h"

#include "annotationoverlay.h"
#include "audiocapture.h"
#include "cameracapture.h"
#include "controlserver.h"
#include "mainwindow.h"
#include "recordingmarkers.h"
#include "recordingwidgets.h"
#include "speechservices.h"
#include "uilanguage.h"
#include "uitheme.h"

#include <QBuffer>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSlider>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStorageInfo>
#include <QTextBrowser>
#include <QThreadPool>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>
#include <QWindow>
#include <windows.h>
#include <limits>
#include <utility>

namespace
{
constexpr auto VideoFiles = "视频 (*.mp4 *.mkv *.mov *.webm *.avi *.flv *.m4v)";
const QStringList Formats{"MP4", "MKV", "MOV", "WebM", "AVI", "FLV"};
const QStringList Colors{"SDR", "HDR", "SDR 转 HDR", "HDR 转 SDR"};
const QStringList Encoders{"自动", "CPU · x264", "NVIDIA NVENC H.264", "AMD AMF H.264", "Intel QSV H.264"};

void Choose(QComboBox* combo, const QVariant& value)
{
	const int index = combo->findData(value);
	if (index >= 0) combo->setCurrentIndex(index);
}

QString SourceKey(const QString& type)
{
	if (type == "camera") return "camera";
	if (type == "window") return "window";
	return "monitor";
}

QVariant SourceValue(const QJsonValue& value)
{
	return value.isString() ? value.toString() : value.toVariant();
}

QJsonArray TtsChoices()
{
	QJsonArray choices;
	for (const auto& id : TtsServiceIds())
	{
		const auto service = SpeechService(id);
		choices.append(QJsonObject{{"id", id}, {"label", service.label.isEmpty() ? service.model : service.label}});
	}
	return choices;
}

QJsonObject ModelSettings()
{
	QJsonArray profiles;
	const auto voices = TtsServiceIds();
	for (const auto& id : QStringList{"assistant", "translate", "video"} + voices)
	{
		const auto service = SpeechService(id);
		profiles.append(QJsonObject{{"id", id}, {"endpoint", service.endpoint}, {"model", service.model},
			{"protocol", service.protocol}, {"voice", service.voice}, {"label", service.label},
			{"requestTemplate", service.requestTemplate}, {"responsePath", service.responsePath},
			{"audioEncoding", service.audioEncoding}, {"credentialHeader", service.credentialHeader},
			{"credentialPrefix", service.credentialPrefix}, {"hasKey", !service.key.isEmpty()}});
	}
	return {{"profiles", profiles}, {"tts", TtsChoices()}};
}

KRRSpeechService ModelForm(const QJsonObject& data)
{
	KRRSpeechService service;
	service.endpoint = data["endpoint"].toString().trimmed();
	service.model = data["model"].toString().trimmed();
	service.protocol = data["protocol"].toString();
	service.voice = data["voice"].toString().trimmed();
	service.label = data["label"].toString().trimmed();
	service.requestTemplate = data["requestTemplate"].toString().trimmed();
	service.responsePath = data["responsePath"].toString().trimmed();
	service.audioEncoding = data["audioEncoding"].toString("binary");
	service.credentialHeader = data["credentialHeader"].toString().trimmed();
	service.credentialPrefix = data["credentialPrefix"].toString();
	if (!service.credentialHeader.isEmpty()) service.key = data["key"].toString().trimmed().toUtf8();
	return service;
}
}

KRRWebStudio::KRRWebStudio(KRRMainWindow& window)
	: QObject(&window), m_window(window), m_surface(new KRRWebStudioSurface(&window)),
	  m_cache(QDir::tempPath() + "/wps-studio-XXXXXX")
{
	window.m_webStudio = this;
	auto native = window.takeCentralWidget();
	native->setParent(&window);
	native->hide();
	window.setCentralWidget(m_surface);
	window.statusBar()->hide();
	connect(m_surface, &KRRWebStudioSurface::request, this, &KRRWebStudio::dispatch);
	connect(m_surface, &KRRWebStudioSurface::failed, this, [this, native](const QString& error)
	{
		if (m_window.m_webStudio != this) return;
		m_frameTimer.stop();
		m_statusTimer.stop();
		m_mixerTimer.stop();
		m_handshake = false;
		m_window.m_webStudio = nullptr;
		m_window.takeCentralWidget();
		m_surface->hide();
		m_surface->deleteLater();
		m_surface = nullptr;
		m_window.setCentralWidget(native);
		native->show();
		m_window.statusBar()->show();
		m_window.statusBar()->showMessage(error + " " + Ui("已切换至 Qt 工作台。"), 9000);
		if (!m_startupFile.isEmpty()) m_window.editFile(m_startupFile);
	});
	connect(&m_frameTimer, &QTimer::timeout, this, &KRRWebStudio::updateFrame);
	connect(&m_statusTimer, &QTimer::timeout, this, &KRRWebStudio::updateStatus);
	connect(&m_mixerTimer, &QTimer::timeout, this, &KRRWebStudio::updateMixerLevels);
	connect(&window, &KRRMainWindow::workbenchRestored, this, [this] { event("navigate", "record"); });
	connect(window.m_overlay, &KRRAnnotationOverlay::activeChanged, this, [this](bool active) { event("annotation", active); });
	connect(window.m_floatBar, &KRRRecordingToolbar::hideRequested, this, [this] { event("floating", false); });
	connect(qApp, &QCoreApplication::aboutToQuit, this, [this] { if (m_handshake && !m_skipExitSave) saveDraft(); });
}

void KRRWebStudio::start()
{
	m_surface->start();
	if (m_window.m_webStudio != this) return;
	m_frameTimer.start(33);
	m_statusTimer.start(230);
	m_mixerTimer.start(65);
}

bool KRRWebStudio::requestFrontendClose()
{
	if (!m_handshake) return false;
	event("request-close", true);
	return true;
}

bool KRRWebStudio::requestFrontendMinimal()
{
	if (!m_handshake) return false;
	event("request-minimal", true);
	return true;
}

bool KRRWebStudio::workInProgress() const
{
	return m_jobs.isBusy() || !m_assistantJobs.isEmpty();
}

bool KRRWebStudio::saveExitDraft()
{
	return saveDraft() && !m_exitSyncFailed;
}

void KRRWebStudio::cancelExitWork()
{
	m_skipExitSave = true;
	m_jobs.cancel();
	if (m_modelCheck) m_modelCheck->store(true);
	for (const auto& job : std::as_const(m_assistantJobs)) job->store(true);
}

void KRRWebStudio::reply(const QString& request, const QJsonValue& result, const QString& error)
{
	if (m_surface && !request.isEmpty()) m_surface->send({{"id", request}, {"result", result}, {"error", error}});
}

void KRRWebStudio::event(const QString& type, const QJsonValue& value)
{
	if (m_surface) m_surface->send({{"event", type}, {"value", value}});
}

void KRRWebStudio::updateFrame()
{
	auto& w = m_window;
	if (!m_handshake || !w.isVisible() || w.isMinimized() ||
		w.m_workspace->currentIndex() != KRRMainWindow::Record) return;
	const QImage frame = w.m_liveFrame->frame();
	if (frame.isNull())
	{
		const auto notice = w.m_liveFrame->text().isEmpty() ? Ui("正在连接画面源…") : w.m_liveFrame->text();
		if (m_lastFrame || notice != m_previewNotice)
		{
			++m_previewGeneration;
			m_lastFrame = 0;
			if (m_surface->send({{"event", "frame"}, {"value", QJsonObject{{"notice", notice}}}}))
				m_previewNotice = notice;
		}
		return;
	}
	if (m_encoding || frame.cacheKey() == m_lastFrame) return;
	const auto generation = m_previewGeneration;
	m_encoding = true;
	const QPointer<KRRWebStudio> self(this);
	QThreadPool::globalInstance()->start(QRunnable::create([self, frame, generation]
	{
		QByteArray jpeg;
		QBuffer buffer(&jpeg);
		buffer.open(QIODevice::WriteOnly);
		frame.save(&buffer, "JPEG", 88);
		if (!self) return;
		const auto data = QString::fromLatin1(jpeg.toBase64());
		QMetaObject::invokeMethod(self.data(), [self, data, size = frame.size(), key = frame.cacheKey(), generation]
		{
			if (!self) return;
			self->m_encoding = false;
			if (generation != self->m_previewGeneration || data.isEmpty()) return;
			const QJsonObject payload{{"src", "data:image/jpeg;base64," + data}, {"width", size.width()}, {"height", size.height()}};
			if (self->m_surface->send({{"event", "frame"}, {"value", payload}}))
			{
				self->m_lastFrame = key;
				self->m_previewNotice.clear();
			}
		}, Qt::QueuedConnection);
	}));
}

void KRRWebStudio::updateStatus()
{
	if (!m_handshake) return;
	auto& w = m_window;
	const auto& status = w.m_lastRecordingStatus;
	const auto phase = w.m_countdownRemaining ? "countdown" : w.m_startPending ? "starting" : !w.m_active ? "idle" :
		status["phase"] == "finalizing" || w.m_stopProcess.state() != QProcess::NotRunning ? "saving" :
		status["phase"] == "starting" ? "starting" :
		w.m_paused ? "paused" : "recording";
	const QStorageInfo volume(w.m_directory->text());
	const bool active = w.m_active;
	const auto frameSize = active ? QSize(status["width"].toInt(), status["height"].toInt()) : w.m_liveFrame->frameSize();
	const bool cameraReady = active ? status["pip_enabled"].toBool() && w.m_liveCameraReady : w.m_idleCameraReady;
	const auto recordingError = status["capture_control_error"].toString();
	const auto cameraError = active ? (recordingError.startsWith("Camera ") ? recordingError : QString{}) : w.m_idleCameraError;
	static const auto integrity = ProcessIntegrityRid();
	const QJsonObject result{{"phase", phase}, {"seconds", active ? status["duration_sec"].toDouble() : 0},
		{"countdown", w.m_countdownRemaining}, {"frames", active ? status["frames_written"].toDouble() : 0},
		{"duplicates", active ? status["duplicate_frames"].toDouble() : 0},
		{"fps", active ? status["avg_fps"].toDouble() : w.m_idleFps},
		{"cpu", w.m_guiCpu + (active ? status["cpu_pct"].toDouble() : 0)},
		{"disk", volume.isValid() ? volume.bytesAvailable() / 1e9 : -1.},
		{"encoder", active ? status["encoder"].toString() : QString{}},
		{"maximized", w.isMaximized()}, {"floating", w.m_floatButton->isChecked()},
		{"size", frameSize.isValid() ? QString("%1×%2").arg(frameSize.width()).arg(frameSize.height()) : QString{}},
		{"cameraReady", cameraReady}, {"cameraError", cameraError.isEmpty() ? QString{} : UiDiagnostic(cameraError)},
		{"cameraPermissionRestricted", integrity >= 0 && integrity < SECURITY_MANDATORY_MEDIUM_RID &&
			cameraError.contains("0x80070005")},
		{"message", w.m_message->text()}};
	const auto serialized = QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact));
	if (m_lastStatus != serialized)
	{
		m_lastStatus = serialized;
		event("status", result);
	}
}

void KRRWebStudio::updateMixerLevels()
{
	const auto& w = m_window;
	if (!m_handshake || w.m_workspace->currentIndex() != KRRMainWindow::Record || !w.isVisible() || w.isMinimized()) return;
	QJsonObject levels{{"mic", w.m_micMeter->level()}, {"system", w.m_systemMeter->level()}};
	for (auto meter = w.m_extraMeters.cbegin(); meter != w.m_extraMeters.cend(); ++meter)
		levels[meter.key()] = meter.value()->level();
	if (levels != m_mixerLevels)
	{
		m_mixerLevels = levels;
		event("levels", levels);
	}
}

QJsonObject KRRWebStudio::captureTargets() const
{
	const auto& w = m_window;
	QJsonObject targets;
	for (const auto& pair : {std::make_pair(QString("screen"), w.m_monitor), std::make_pair(QString("window"), w.m_window),
		std::make_pair(QString("camera"), w.m_camera)})
	{
		QJsonArray options;
		for (int i = 0; i < pair.second->count(); ++i)
		{
			const auto data = pair.second->itemData(i);
			if (pair.first == "camera" && data.toInt() < 0) continue;
			const QJsonValue id = pair.first == "window" ? QJsonValue(data.toString()) : QJsonValue(data.toInt());
			QJsonArray option{pair.second->itemText(i), id};
			if (pair.first == "screen") option.append(pair.second->itemData(i, Qt::UserRole + 1).toString());
			options.append(option);
		}
		targets[pair.first] = options;
	}
	return targets;
}

QJsonObject KRRWebStudio::snapshot()
{
	auto& w = m_window;
	const auto config = w.configuration();
	const auto resolution = w.m_resolution->currentText();
	static const QStringList presets{"1280x720", "1920x1080", "2560x1440", "3840x2160"};
	const int height = resolution == w.m_resolution->itemText(0) ? 0 : presets.contains(resolution) ? config.height : -1;
	QJsonObject settings{{"resolution", height}, {"width", config.width}, {"height", config.height}, {"fps", config.fps},
		{"bitrate", config.bitrate}, {"customBitrate", !w.m_recommendedBitrate->isChecked()},
		{"encoder", Encoders.value(w.m_encoder->currentIndex())}, {"color", Colors.value(w.m_outputColor->currentIndex())},
		{"format", w.m_format->currentText().toUpper()}, {"outputPath", w.m_directory->text()},
		{"countdown", w.m_countdown->currentData().toInt()}, {"audioMode", config.separateAudio ? "双独立音轨" :
			config.audio == "none" ? "无音频" : "混合音轨"}, {"denoise", config.denoise}, {"language", ReadUiLanguage()}};
	QJsonArray audio;
	for (const auto& val : w.m_additionalAudio) audio.append(val);
	QString error;
	const auto startup = m_startupFile.isEmpty() ? QJsonObject{} : inspectMedia(m_startupFile, error);
	return {{"targets", captureTargets()}, {"settings", settings}, {"configuration", config.toJson()}, {"scenes", w.m_scenePresets},
		{"scene", w.m_sceneIndex}, {"audio", audio}, {"assets", library()}, {"startupAsset", startup},
		{"audio8", Audio8Available()}, {"ttsServices", TtsChoices()}, {"messages", UiMessages()}, {"version", QCoreApplication::applicationVersion()},
		{"hotkeys", QJsonArray::fromStringList(w.m_hotkeys)}, {"hotkeyLabels", QJsonArray::fromStringList(w.hotkeyLabels())}};
}

QJsonObject KRRWebStudio::libraryEntry(const QString& file)
{
	const QFileInfo info(file);
	const auto url = m_surface->localResource(file);
	if (url.isEmpty()) return {};
	const auto id = url.section('/', -1);
	m_media[id] = file;
	QJsonArray marks;
	for (const auto& val : ReadRecordingMarkers(file))
	{
		const auto row = val.toObject();
		marks.append(QJsonObject{{"at", row["at_ms"].toDouble() / 1000.}, {"name", row["label"]}});
	}
	QJsonObject entry{{"id", id}, {"name", info.fileName()}, {"title", info.completeBaseName()}, {"path", file},
		{"date", info.lastModified().toString("MM-dd HH:mm")}, {"bytes", qint64(info.size())},
		{"kind", "normal"}, {"src", url}, {"marks", marks}, {"frameRate", 30}, {"duration", 0}};
	if (m_inspected.contains(file))
	{
		const auto metadata = m_inspected.value(file);
		for (auto it = metadata.begin(); it != metadata.end(); ++it) entry[it.key()] = it.value();
	}
	return entry;
}

QJsonArray KRRWebStudio::library()
{
	m_window.updateReviewLibrary();
	QJsonArray result;
	for (int i = 0; i < qMin(120, m_window.m_recent->count()); ++i)
	{
		const auto file = m_window.m_recent->item(i)->data(Qt::UserRole).toString();
		const auto entry = libraryEntry(file);
		if (!entry.isEmpty()) result.append(entry);
	}
	return result;
}

QString KRRWebStudio::openFolder(const QString& title, const QString& initial)
{
	return QFileDialog::getExistingDirectory(&m_window, title, initial.isEmpty() ? m_window.m_directory->text() : initial);
}

QString KRRWebStudio::openMediaDialog(const QString& purpose)
{
	const auto filter = purpose == "image" ? "图片 (*.png *.jpg *.jpeg *.bmp *.webp)" :
		purpose == "audio" ? "音频 (*.wav *.mp3 *.m4a *.aac *.ogg *.flac *.opus)" :
		purpose == "srt" ? "字幕 (*.srt *.vtt)" :
		purpose == "project" ? "剪辑工程 (*.krrproj *.json)" : VideoFiles;
	return QFileDialog::getOpenFileName(&m_window, Ui("打开"), m_window.m_directory->text(), Ui(filter));
}

void KRRWebStudio::applyPreferences(const QJsonObject& data)
{
	auto& w = m_window;
	const auto change = [&data](const char* key, auto setter)
	{
		if (data.contains(key)) setter(data.value(key));
	};
	change("resolution", [&w](const auto& v)
	{
		const int h = v.toInt();
		const QString size = h == 720 ? "1280x720" : h == 1080 ? "1920x1080" : h == 1440 ? "2560x1440" : h == 2160 ? "3840x2160" : "";
		if (h == 0) w.m_resolution->setCurrentIndex(0);
		else if (!size.isEmpty()) w.m_resolution->setCurrentText(size);
	});
	const auto dimensions = data["customSize"].toArray();
	if (dimensions.size() == 2)
	{
		const auto even = [](int value, int minimum, int maximum) { return qBound(minimum, value / 2 * 2, maximum); };
		const auto width = even(dimensions.at(0).toInt(), 160, 7680);
		const auto height = even(dimensions.at(1).toInt(), 120, 4320);
		w.m_resolution->setCurrentText(QString("%1x%2").arg(width).arg(height));
	}
	change("fps", [&w](const auto& v) { Choose(w.m_fps, v.toInt()); });
	change("customBitrate", [&w](const auto& v) { w.m_recommendedBitrate->setChecked(!v.toBool()); });
	change("bitrate", [&w](const auto& v)
	{
		if (w.m_recommendedBitrate->isChecked()) w.updateBitrateRecommendation();
		else w.m_bitrate->setValue(v.toInt());
	});
	change("encoder", [&w](const auto& v) { const int i = Encoders.indexOf(v.toString()); if (i >= 0) w.m_encoder->setCurrentIndex(i); });
	change("color", [&w](const auto& v) { const int i = Colors.indexOf(v.toString()); if (i >= 0) w.m_outputColor->setCurrentIndex(i); });
	change("format", [&w](const auto& v) { w.m_format->setCurrentIndex(qMax(0, w.m_format->findText(v.toString(), Qt::MatchFixedString))); });
	change("countdown", [&w](const auto& v) { Choose(w.m_countdown, v.toInt()); });
	change("audioMode", [&w](const auto& v)
	{
		const auto mode = v.toString();
		w.m_separateAudio->setChecked(mode == "双独立音轨");
		if (mode == "无音频") Choose(w.m_audio, "none");
		else if (w.m_audio->currentData() == "none") Choose(w.m_audio, "mix");
	});
	if (w.m_recommendedBitrate->isChecked()) w.updateBitrateRecommendation();
	w.saveSettings();
	w.m_idleConfigClock.invalidate();
}

void KRRWebStudio::applyScenes(const QJsonObject& data)
{
	auto& w = m_window;
	const auto scenes = data["scenes"].toArray();
	if (scenes.isEmpty() || scenes.size() > 100) return;
	const int current = qBound(0, data["scene"].toInt(), scenes.size() - 1);
	const bool automatic = w.m_resolution->currentText() == w.m_resolution->itemText(0);
	const auto base = w.configuration();
	QJsonArray presets;
	KRRRecordingConfig selected;
	for (int i = 0; i < scenes.size(); ++i)
	{
		const auto scene = scenes[i].toObject();
		const auto name = scene["name"].toString().trimmed().left(120);
		auto config = base;
		config.cameraEnabled = false;
		config.cameraTrack = false;
		QJsonArray sources;
		int selectedIndex = 0;
		bool foundPrimary = false;
		QJsonObject camera;
		for (const auto& value : scene["sources"].toArray())
		{
			const auto s = value.toObject();
			const auto type = s["type"].toString();
			if (!QStringList{"screen", "window", "region", "camera", "image"}.contains(type)) continue;
			QJsonObject source{{"source", type}, {"cursor", s["showCursor"].toBool(true)}, {"visible", s["visible"].toBool(true)},
				{"label", s["label"].toString()}, {"id", s["id"].toString()}};
			if (type == "screen" || type == "window" || type == "region")
				for (const auto& key : QStringList{"screenColor", "screenWhiteNits", "screenExposureEV", "clicks"})
					if (s.contains(key)) source[key] = s[key];
			const auto target = s["device"];
			if (type == "region")
			{
				const auto rect = s["region"].toArray();
				source["x"] = rect.at(0); source["y"] = rect.at(1);
				source["rw"] = rect.at(2); source["rh"] = rect.at(3);
			}
			else if (type == "image") source["image"] = trackFile(s["mediaId"].toString()).isEmpty() ? s["file"].toString() : trackFile(s["mediaId"].toString());
			else source[SourceKey(type)] = target;
			if (type == "screen")
			{
				const auto device = s["monitorDevice"].toString();
				source["monitorDevice"] = device.isEmpty() ?
					w.m_monitor->itemData(target.toInt(), Qt::UserRole + 1).toString() : device;
			}
			if (type == "camera")
				for (const auto& key : QStringList{"cameraTrack", "pipMirror", "pipShape", "pipSize", "pipX", "pipY", "pipBorder"})
					if (s.contains(key)) source[key] = s[key];
			if (!s["id"].toString().isEmpty() && s["id"] == scene["selected"]) selectedIndex = sources.size();
			sources.append(source);
			if (!source["visible"].toBool()) continue;
			if (type == "camera")
			{
				camera = s;
				continue;
			}
			config.source = type;
			config.cursor = source["cursor"].toBool();
			config.clicks = source["clicks"].toBool();
			config.screenColor = source["screenColor"].toString("auto") == "highlights" ? "highlights" : "auto";
			config.screenWhiteNits = qBound(0., source["screenWhiteNits"].toDouble(), 1000.);
			config.screenExposureEV = qBound(-2., source["screenExposureEV"].toDouble(), 2.);
			if (type == "window") config.window = SourceValue(target).toULongLong();
			if (type == "screen")
			{
				config.monitor = target.toInt();
				config.monitorDevice = source["monitorDevice"].toString();
			}
			if (type == "region") config.region = QRect(source["x"].toInt(), source["y"].toInt(), source["rw"].toInt(), source["rh"].toInt());
			if (type == "image") config.image = source["image"].toString();
			foundPrimary = true;
		}
		if (!camera.isEmpty())
		{
			if (!foundPrimary) config.source = "camera";
			config.camera = camera["device"].toInt(-1);
			config.cameraEnabled = foundPrimary;
			config.cameraTrack = camera["cameraTrack"].toBool();
			config.cameraMirror = camera["pipMirror"].toBool(true);
			const auto shape = camera["pipShape"].toString();
			config.cameraShape = shape == "圆形" ? "circle" : shape == "矩形" || shape == "方形" ? "rectangle" : "rounded";
			config.cameraBorder = camera["pipBorder"].toBool(true) ? "#ffffff" : "#00000000";
			const double size = qBound(10., camera["pipSize"].toDouble(23), 45.) / 100.;
			config.cameraRect = {qBound(0., camera["pipX"].toDouble(.74), 1. - size),
				qBound(0., camera["pipY"].toDouble(.66), 1. - size), size, size};
		}
		else if (!foundPrimary) config.source = "image", config.image.clear();
		if (i == current) selected = config;
		auto json = config.toJson();
		json.remove("output");
		presets.append(QJsonObject{{"name", name.isEmpty() ? Ui("场景 %1").arg(i + 1) : name}, {"config", json}, {"sources", sources},
			{"sourceIndex", selectedIndex}, {"layeredSources", true},
			{"automaticResolution", automatic}, {"format", w.m_format->currentText()}});
	}
	w.m_sceneIndex = -1;
	w.m_updatingSources = true;
	w.applySceneConfiguration(selected);
	if (automatic) w.m_resolution->setCurrentIndex(0);
	w.m_updatingSources = false;
	w.m_sceneIndex = current;
	w.m_visualSources = presets[current].toObject()["sources"].toArray();
	w.m_sourceIndex = presets[current].toObject()["sourceIndex"].toInt();
	w.m_scenePresets = presets;
	w.refreshSceneList();
	w.refreshSources();
	w.saveSceneList();
	w.saveSettings();
	w.m_idleConfigClock.invalidate();
}

void KRRWebStudio::applyMixer(const QJsonObject& data)
{
	auto& w = m_window;
	const auto mic = data["mic"].toObject(), system = data["system"].toObject();
	const bool m = data.contains("mic") && !mic["muted"].toBool();
	const bool s = data.contains("system") && !system["muted"].toBool();
	Choose(w.m_audio, m && s ? "mix" : m ? "mic" : s ? "system" : "none");
	w.m_micGain->setValue(mic["volume"].toInt(100));
	w.m_systemGain->setValue(system["volume"].toInt(100));
	w.m_denoise->setChecked(mic["denoise"].toBool());
	auto current = w.m_additionalAudio;
	for (int i = 0; i < current.size(); ++i)
	{
		auto source = current[i].toObject();
		const auto entry = data[source["id"].toString()].toObject();
		if (entry.isEmpty()) { if (!w.m_active) current.removeAt(i--); continue; }
		source["muted"] = entry["muted"].toBool();
		source["gain"] = entry["volume"].toDouble(100) / 100;
		source["denoise"] = !source["loopback"].toBool() && entry["denoise"].toBool();
		current.replace(i, source);
	}
	const bool countChanged = current.size() != w.m_additionalAudio.size();
	w.m_additionalAudio = current;
	if (countChanged) w.rebuildAudioSources();
	w.sendScene();
	w.saveSettings();
}

void KRRWebStudio::dispatch(const QJsonObject& message)
{
	auto& w = m_window;
	const auto name = message["command"].toString();
	const auto request = message["id"].toString();
	const auto data = message["data"].toObject();
	const auto done = [this, &request](const QJsonValue& result = {}, const QString& error = {}) { reply(request, result, error); };
	if (name == "init")
	{
		m_handshake = true;
		m_lastFrame = 0;
		m_previewNotice.clear();
		++m_previewGeneration;
		done(snapshot());
		updateStatus();
	}
	else if (name == "preview-refresh")
	{
		m_lastFrame = 0;
		m_previewNotice.clear();
		++m_previewGeneration;
		done();
	}
	else if (name == "navigate")
	{
		const auto page = QStringList{"record", "review", "editor", "ai"}.indexOf(data["page"].toString());
		if (page >= 0) w.selectWorkspace(static_cast<KRRMainWindow::Workspace>(page));
		done();
	}
	else if (name == "window")
	{
		const auto action = data["action"].toString();
		if (action == "move") w.windowHandle()->startSystemMove();
		if (action == "minimize") w.showMinimized();
		if (action == "maximize") { if (w.isMaximized()) w.showNormal(); else w.showMaximized(); }
		if (action == "close")
		{
			m_exitSyncFailed = data["draftSyncFailed"].toBool();
			const bool closed = w.requestUserClose();
			m_exitSyncFailed = false;
			done(closed);
			return;
		}
		if (action == "fullscreen" && !m_previewWindowState)
		{
			m_previewWindowState = w.windowState();
			if (!w.isFullScreen()) w.showFullScreen();
		}
		if (action == "restore" && m_previewWindowState)
		{
			const auto previous = *m_previewWindowState;
			m_previewWindowState.reset();
			if (w.windowState() != previous) w.setWindowState(previous);
		}
		done();
	}
	else if (name == "toolbar-theme")
	{
		RecorderUi::ToolbarTheme theme;
		if (!RecorderUi::WriteToolbarTheme(data, theme))
		{
			done({}, Ui("无法保存工具栏主题。"));
			return;
		}
		w.m_floatBar->setTheme(theme);
		w.m_overlay->setTheme(theme);
		done();
	}
	else if (name == "minimal-recorder") { emit w.minimalRecorderRequested(); done(); }
	else if (name == "preferences") { applyPreferences(data); done(w.m_bitrate->value()); }
	else if (name == "recommended") { w.m_recommendedBitrate->setChecked(true); w.updateBitrateRecommendation(); w.saveSettings(); done(w.m_bitrate->value()); }
	else if (name == "scene") { applyScenes(data); done(); }
	else if (name == "targets")
	{
		w.refreshWindows();
		w.refreshMonitors();
		const int selectedCamera = w.m_camera->currentIndex() < 0 ? w.m_unavailableCameraIndex : w.m_camera->currentData().toInt();
		const QSignalBlocker blockCamera(w.m_camera);
		w.m_camera->clear();
		for (const auto& row : KRRCameraCapture::Devices())
		{
			const auto device = row.toObject();
			w.m_camera->addItem(device["name"].toString(), device["index"].toInt());
		}
		w.m_unavailableCameraIndex = selectedCamera;
		w.m_camera->setCurrentIndex(w.m_camera->findData(selectedCamera));
		done(captureTargets());
	}
	else if (name == "choose-region")
	{
		w.selectRecordingRegion();
		const auto r = w.m_region;
		done(QJsonArray{r.x(), r.y(), r.width(), r.height()});
	}
	else if (name == "choose-image" || name == "choose-audio")
	{
		const auto file = openMediaDialog(name == "choose-image" ? "image" : "audio");
		const auto url = m_surface->localResource(file);
		const auto id = url.section('/', -1);
		if (!id.isEmpty()) m_tracks[id] = file;
		done(QJsonObject{{"id", id}, {"url", url}, {"file", file}, {"name", QFileInfo(file).fileName()}});
	}
	else if (name == "choose-directory")
	{
		const auto path = openFolder(Ui("选择保存路径"));
		if (!path.isEmpty() && data["save"].toBool()) { w.m_directory->setText(path); w.m_defaultDirectory = false; w.saveSettings(); }
		done(QDir::toNativeSeparators(path));
	}
	else if (name == "open-directory") { QDesktopServices::openUrl(QUrl::fromLocalFile(w.m_directory->text())); done(); }
	else if (name == "mixer") { applyMixer(data); done(); }
	else if (name == "audio-devices")
	{
		QJsonArray items;
		for (const auto& entry : RecordingAudioEndpoints())
			items.append(QJsonObject{{"id", entry.id}, {"name", entry.name}, {"loopback", entry.loopback}});
		done(items);
	}
	else if (name == "add-audio")
	{
		if (w.m_active || w.m_additionalAudio.size() >= MaxAdditionalAudioSources) { done({}, Ui("请停止录制后添加音源。")); return; }
		const auto id = data["id"].toString();
		for (const auto& original : w.m_additionalAudio)
			if (original.toObject()["id"] == id) { done({}, Ui("该设备已添加。")); return; }
		for (const auto& entry : RecordingAudioEndpoints())
			if (entry.id == id)
			{
				const QJsonObject item{{"id", id}, {"name", entry.name}, {"loopback", entry.loopback}, {"gain", 1.}, {"muted", false}};
				w.m_additionalAudio.append(item);
				w.rebuildAudioSources();
				w.saveSettings();
				done(item);
				return;
			}
		done({}, Ui("音频设备不可用。"));
	}
	else if (name == "record")
	{
		if (w.m_countdownRemaining)
		{
			w.m_countdownRemaining = 0;
			w.m_countdownTimer.stop();
			w.updateStatus();
		}
		else if (w.m_active || w.m_startPending) w.stopRecording();
		else w.beginCountdown();
		done();
		updateStatus();
	}
	else if (name == "pause") { if (w.m_active) w.m_pause->click(); done(); }
	else if (name == "record-mark") { w.addRecordingMarker(); done(); }
	else if (name == "screenshot") { w.takeScreenshot(); done(); }
	else if (name == "floating") { w.m_floatButton->setChecked(data["active"].toBool()); done(); }
	else if (name == "annotate") { w.showAnnotation(); done(); }
	else if (name == "hotkeys-edit") { w.setHotkeyEditing(data["active"].toBool()); done(); }
	else if (name == "hotkeys-save")
	{
		QStringList keys;
		for (const auto& key : data["keys"].toArray())
		{
			if (!key.isString()) { done({}, Ui("快捷键格式无效。")); return; }
			keys << key.toString();
		}
		QString error;
		if (!w.setHotkeys(keys, error)) { done({}, error); return; }
		done(QJsonArray::fromStringList(w.m_hotkeys));
	}
	else if (name == "models-list") done(ModelSettings());
	else if (name == "models-save" || name == "models-remove")
	{
		const auto id = data["id"].toString();
		QString error;
		const bool saved = name == "models-save" ?
			SaveSpeechService(id, ModelForm(data["service"].toObject()), data["clearKey"].toBool(), error) :
			RemoveTtsService(id, error);
		if (!saved) { done({}, UiDiagnostic(error)); return; }
		done(ModelSettings());
	}
	else if (name == "models-cancel")
	{
		if (m_modelCheck) m_modelCheck->store(true);
		done();
	}
	else if (name == "models-check")
	{
		if (m_modelCheck && !m_modelCheck->load()) { done({}, Ui("正在发送测试数据并检验服务能力…")); return; }
		const auto cancel = std::make_shared<std::atomic<bool>>(false);
		m_modelCheck = cancel;
		const auto id = data["id"].toString();
		const auto service = ModelForm(data["service"].toObject());
		const QPointer<KRRWebStudio> self(this);
		QThreadPool::globalInstance()->start(QRunnable::create([self, request, cancel, id, service]
		{
			const auto error = CheckSpeechService(id, service, cancel.get());
			if (!self) return;
			QMetaObject::invokeMethod(self.data(), [self, request, cancel, error]
			{
				if (!self) return;
				if (self->m_modelCheck == cancel) self->m_modelCheck.reset();
				self->reply(request, error.isEmpty(), UiDiagnostic(error));
			}, Qt::QueuedConnection);
		}));
	}
	else if (name == "language") { done(); emit w.languageChangeRequested(data["language"].toString()); }
	else if (name == "recover") { w.showRecoveries(); done(library()); }
	else if (name == "library") done(library());
	else if (name == "open-guide")
	{
		QFile guide(":/ui/user-guide.md");
		if (!guide.open(QIODevice::ReadOnly)) { done({}, Ui("无法读取使用指南。")); return; }
		auto *dialog = new QDialog(&w);
		dialog->setAttribute(Qt::WA_DeleteOnClose);
		dialog->setWindowTitle(Ui("使用指南"));
		dialog->resize(RecorderUi::Px(720), RecorderUi::Px(560));
		auto *layout = new QVBoxLayout(dialog);
		auto *body = new QTextBrowser(dialog);
		body->setOpenLinks(false);
		body->setMarkdown(QString::fromUtf8(guide.readAll()));
		layout->addWidget(body);
		dialog->open();
		done();
	}
	else if (name == "schedule-list" || name == "schedule-add" || name == "schedule-remove")
	{
		const auto backend = name == "schedule-add" ? "schedule" : name == "schedule-remove" ? "cancel-schedule" : "schedules";
		QJsonObject command{{"command", backend}};
		if (name == "schedule-remove") command["id"] = data["id"];
		if (name == "schedule-add")
		{
			auto cfg = w.configuration();
			const int index = data["scene"].toInt(-1);
			if (index >= 0 && index < w.m_scenePresets.size())
			{
				const auto preset = KRRRecordingConfig::FromJson(w.m_scenePresets[index].toObject()["config"].toObject());
				cfg = preset;
			}
			cfg.maxDuration = data["duration"].toDouble();
			const auto target = w.configuration().output;
			cfg.output = QFileInfo(target).path() + "/WPS_Scheduled_" +
				QDateTime::fromString(data["at"].toString(), Qt::ISODateWithMs).toLocalTime().toString("yyyyMMdd_HHmmss") +
				"-" + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8) + "." + QFileInfo(target).suffix();
			command["name"] = data["name"];
			command["startAt"] = data["at"];
			command["config"] = cfg.toJson();
		}
		const QPointer<KRRWebStudio> self(this);
		QThreadPool::globalInstance()->start(QRunnable::create([self, request, command, adding = name == "schedule-add", refresh = name != "schedule-list"]
		{
			const QJsonObject listRequest{{"command", "schedules"}};
			QJsonObject response;
			QString error;
			const bool queueAvailable = !adding && SendControl(listRequest, response, error, 500);
			if (refresh || !queueAvailable)
			{
				const bool ready = queueAvailable || EnsureControlService(error, true);
				if (ready && SendControl(command, response, error, 1800) && refresh)
					SendControl(listRequest, response, error, 1800);
			}
			if (!self) return;
			QMetaObject::invokeMethod(self.data(), [self, request, schedules = response["schedules"].toArray(), error]
			{
				if (self) self->reply(request, schedules, error.isEmpty() ? error : UiDiagnostic(error));
			}, Qt::QueuedConnection);
		}));
	}
	else if (!editCommand(name, request, data) && !assistantCommand(name, request, data))
		done({}, Ui("此操作不可用。"));
}
