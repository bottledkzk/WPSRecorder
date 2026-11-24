#include "minimalrecorder.h"
#include "annotationoverlay.h"
#include "controlserver.h"
#include "framecapture.h"
#include "recordingassistanttools.h"
#include "recordingcheck.h"
#include "recordinghotkeys.h"
#include "recordingwidgets.h"
#include "studioapp.h"
#include "uilanguage.h"
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QJsonDocument>
#include <QMenu>
#include <QMessageBox>
#include <QSettings>
#include <QStandardPaths>
#include <QSystemTrayIcon>
#include <QThreadPool>
#include <functional>

namespace
{
QString Stamp()
{
	return QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss-zzz");
}

class MinimalRecorder final : public QObject
{
public:
	explicit MinimalRecorder(QApplication &app);
	~MinimalRecorder() override
	{
		if (m_hotkeyWindow)
		{
			UnregisterHotKey(m_hotkeyWindow, RecordingHotkeys::FirstId + RecordingHotkeys::Minimal);
			DestroyWindow(m_hotkeyWindow);
		}
		m_worker.waitForDone();
	}
	bool show();

private:
	struct Reply { QJsonObject value; QString error; };
	using Work = std::function<Reply()>;
	using Completion = std::function<void(const Reply&)>;
	void run(Work work, Completion complete);
	void request(QJsonObject request, int timeout, Completion complete);
	void start();
	void stop(bool exitAfter = false);
	void poll();
	void queueEffects(int delay = 33);
	void flushEffects();
	void sync(const QJsonObject &status);
	void captureStill();
	void refresh();
	void notify(const QString &text);
	void openWorkbench();
	void registerModeHotkey();
	static LRESULT CALLBACK hotkeyWindowProc(HWND window, UINT message, WPARAM key, LPARAM data);
	KRRRecordingConfig captureConfig() const;

	QApplication &m_app;
	QThreadPool m_worker;
	KRRRecordingToolbar m_bar{nullptr};
	QSystemTrayIcon m_tray;
	QMenu m_menu;
	QTimer m_poll, m_effectFlush;
	std::unique_ptr<KRRAnnotationOverlay> m_overlay;
	KRRRecordingConfig m_preferences;
	QString m_directory, m_format, m_output;
	QString m_modeHotkey;
	QString m_clock = "00:00:00";
	QJsonObject m_effects, m_sentEffects;
	QAction *m_workbench = nullptr;
	bool m_auto = true, m_recommended = true;
	bool m_camera = false, m_mic = false, m_system = false;
	bool m_pending = false, m_control = false, m_paused = false, m_stopping = false;
	HWND m_hotkeyWindow = nullptr;
	int m_failedPolls = 0;
	QString m_recordingMonitor;
	DWORD m_servicePid = 0;
};

MinimalRecorder::MinimalRecorder(QApplication &app) : m_app(app), m_tray(app.windowIcon())
{
	m_worker.setMaxThreadCount(1);
	QSettings settings(RecorderUiSettingsPath(), QSettings::IniFormat);
	settings.setIniCodec("UTF-8");
	m_modeHotkey = RecordingHotkeys::FromSettings(settings.value("hotkeys").toStringList()).value(RecordingHotkeys::Minimal);
	const auto saved = QJsonDocument::fromJson(settings.value("minimalRecordingProfile").toByteArray()).object();
	if (!saved.isEmpty()) m_preferences = KRRRecordingConfig::FromJson(saved);
	else
	{
		m_preferences.audio = QStringList{"none", "mic", "system", "mix"}.value(settings.value("audio").toInt(), "none");
		m_preferences.fps = RecordingFrameRatePreset(settings.value("fps", 30).toInt());
		m_preferences.bitrate = settings.value("bitrate", 6000).toInt();
		m_preferences.encoder = settings.value("encoder", "auto").toString();
		m_preferences.outputColor = settings.value("outputColor", "sdr").toString();
		m_preferences.cursor = settings.value("cursor", true).toBool();
		m_preferences.denoise = settings.value("denoise", false).toBool();
	}
	const auto dimensions = settings.value("resolution", "auto").toString().split('x');
	const auto w = dimensions.value(0).toInt(), h = dimensions.value(1).toInt();
	m_auto = w < 160 || h < 120 || w > 7680 || h > 4320 || w % 2 || h % 2;
	if (!m_auto) { m_preferences.width = w; m_preferences.height = h; }
	m_recommended = settings.value("recommendedBitrate", !settings.contains("bitrate")).toBool();
	m_mic = m_preferences.audio == "mic" || m_preferences.audio == "mix";
	m_system = m_preferences.audio == "system" || m_preferences.audio == "mix";
	m_format = settings.value("format", "mp4").toString().toLower();
	const auto movies = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
	m_directory = settings.value("directory", QDir(movies.isEmpty() ? DataDirectory() : movies).filePath("WPSRecorder")).toString();
	if (m_directory.isEmpty()) m_directory = QDir(DataDirectory()).filePath("Recordings");

	auto raiseBar = [this] { QString error; if (!m_bar.present(error)) notify(error); };
	m_menu.addAction(Ui("显示悬浮条"), this, raiseBar);
	m_workbench = m_menu.addAction(Ui("录制工作台"), this, &MinimalRecorder::openWorkbench);
	m_menu.addSeparator();
	m_menu.addAction(Ui("退出"), this, [this]
	{
		if (!m_control) stop(true);
	});
	m_tray.setContextMenu(&m_menu);
	m_tray.setToolTip(Ui("WPS Recorder · 极简录制"));
	connect(&m_tray, &QSystemTrayIcon::activated, this, [raiseBar](auto reason)
	{
		if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) raiseBar();
	});
	connect(&m_bar, &KRRRecordingToolbar::startRequested, this, &MinimalRecorder::start);
	connect(&m_bar, &KRRRecordingToolbar::stopRequested, this, [this] { stop(); });
	connect(&m_bar, &KRRRecordingToolbar::returnRequested, this, &MinimalRecorder::openWorkbench);
	connect(&m_bar, &KRRRecordingToolbar::hideRequested, this, [this]
	{
		if (m_tray.isVisible()) m_bar.hide();
		else if (!m_control && m_output.isEmpty()) m_app.quit();
	});
	connect(&m_bar, &KRRRecordingToolbar::pauseRequested, this, [this]
	{
		if (m_control || m_output.isEmpty()) return;
		const auto paused = !m_paused;
		request({{"command", paused ? "pause" : "resume"}}, 1200, [this, paused](const auto &reply)
		{
			if (reply.error.isEmpty()) m_paused = paused;
			else notify(reply.error);
		});
	});
	connect(&m_bar, &KRRRecordingToolbar::markerRequested, this, [this]
	{
		if (m_control || m_output.isEmpty() || m_paused) return;
		request({{"command", "mark"}}, 1200, [this](const auto &reply) { m_bar.markerFeedback(reply.error.isEmpty()); });
	});
	connect(&m_bar, &KRRRecordingToolbar::cameraRequested, this, [this](bool value)
	{
		m_camera = value;
		if (!m_output.isEmpty()) request({{"command", "scene"}, {"scene", QJsonObject{{"cameraEnabled", value}}}}, 1200,
			[this, value](const auto &reply) { if (!reply.error.isEmpty()) { m_camera = !value; notify(reply.error); } });
	});
	connect(&m_bar, &KRRRecordingToolbar::micRequested, this, [this](bool value)
	{
		m_mic = value;
		if (!m_output.isEmpty()) request({{"command", "scene"}, {"scene", QJsonObject{{"micEnabled", value}}}}, 1200,
			[this, value](const auto &reply) { if (!reply.error.isEmpty()) { m_mic = !value; notify(reply.error); } });
	});
	connect(&m_bar, &KRRRecordingToolbar::screenshotRequested, this, &MinimalRecorder::captureStill);
	connect(&m_bar, &KRRRecordingToolbar::annotationRequested, this, [this]
	{
		if (!m_overlay)
		{
			m_overlay = std::make_unique<KRRAnnotationOverlay>();
			connect(m_overlay.get(), &KRRAnnotationOverlay::activeChanged, &m_bar, &KRRRecordingToolbar::setAnnotationActive);
			connect(m_overlay.get(), &KRRAnnotationOverlay::effectsChanged, this, [this](const auto &effects)
			{
				m_effects = effects;
				queueEffects();
			});
		}
		if (m_overlay->isVisible()) { m_overlay->dismiss(); return; }
		QString error;
		if (!m_overlay->present(KRRFrameCapture::CaptureArea(captureConfig()), m_effects, error)) notify(error);
	});
	m_poll.setInterval(400);
	connect(&m_poll, &QTimer::timeout, this, &MinimalRecorder::poll);
	m_effectFlush.setSingleShot(true);
	connect(&m_effectFlush, &QTimer::timeout, this, &MinimalRecorder::flushEffects);
}

bool MinimalRecorder::show()
{
	QString error;
	if (!m_bar.present(error))
	{
		QMessageBox::warning(nullptr, Ui("极简录制"), error);
		return false;
	}
	if (QSystemTrayIcon::isSystemTrayAvailable()) m_tray.show();
	if (!m_modeHotkey.isEmpty()) registerModeHotkey();
	refresh();
	return true;
}

void MinimalRecorder::registerModeHotkey()
{
	constexpr wchar_t className[] = L"WPSRecorderMinimalModeHotkey";
	WNDCLASSW windowClass{};
	windowClass.lpfnWndProc = &MinimalRecorder::hotkeyWindowProc;
	windowClass.hInstance = GetModuleHandleW(nullptr);
	windowClass.lpszClassName = className;
	if (RegisterClassW(&windowClass) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS)
		m_hotkeyWindow = CreateWindowExW(0, className, nullptr, 0, 0, 0, 0, 0,
			HWND_MESSAGE, nullptr, windowClass.hInstance, this);
	if (!m_hotkeyWindow || !RecordingHotkeys::Register(m_hotkeyWindow, RecordingHotkeys::Minimal, m_modeHotkey))
		qWarning("WPS Recorder could not register the Minimal mode shortcut.");
}

LRESULT CALLBACK MinimalRecorder::hotkeyWindowProc(HWND window, UINT message, WPARAM key, LPARAM data)
{
	if (message == WM_NCCREATE)
	{
		const auto create = reinterpret_cast<const CREATESTRUCTW*>(data);
		SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
	}
	else if (message == WM_HOTKEY && key == RecordingHotkeys::FirstId + RecordingHotkeys::Minimal)
	{
		auto* recorder = reinterpret_cast<MinimalRecorder*>(GetWindowLongPtrW(window, GWLP_USERDATA));
		if (recorder) QTimer::singleShot(0, recorder, &MinimalRecorder::openWorkbench);
		return 0;
	}
	return DefWindowProcW(window, message, key, data);
}

void MinimalRecorder::openWorkbench()
{
	if (m_control) return;
	if (m_output.isEmpty())
	{
		m_app.exit(SwitchToRecordingStudio);
		return;
	}
	request({{"command", "publish-preview"}, {"enabled", true}}, 1200, [this](const auto &reply)
	{
		if (reply.error.isEmpty()) m_app.exit(SwitchToRecordingStudio);
		else notify(reply.error);
	});
}

void MinimalRecorder::run(Work work, Completion complete)
{
	m_pending = true;
	m_worker.start(QRunnable::create([this, work = std::move(work), complete = std::move(complete)]
	{
		auto reply = work();
		QMetaObject::invokeMethod(this, [this, reply = std::move(reply), complete = std::move(complete)]
		{
			m_pending = false;
			complete(reply);
			refresh();
			queueEffects();
		}, Qt::QueuedConnection);
	}));
}

void MinimalRecorder::request(QJsonObject command, int timeout, Completion complete)
{
	if (m_control) return;
	m_control = true;
	refresh();
	run([command = std::move(command), timeout]
	{
		Reply result;
		SendControl(command, result.value, result.error, timeout);
		return result;
	}, [this, complete = std::move(complete)](const auto &result)
	{
		m_control = false;
		complete(result);
	});
}

KRRRecordingConfig MinimalRecorder::captureConfig() const
{
	auto config = m_preferences;
	config.source = "screen";
	config.window = 0;
	config.image.clear();
	config.region = {};
	config.effects = m_effects;
	config.publishPreview = false;
	const auto anchor = m_bar.captureAnchor();
	const auto screens = KRRFrameCapture::Monitors();
	config.monitor = 0;
	config.monitorDevice = m_recordingMonitor;
	for (int i = 0; i < screens.size(); ++i)
	{
		const auto screen = screens[i].toObject();
		const auto device = screen["name"].toString();
		const QRect area(screen["x"].toInt(), screen["y"].toInt(), screen["width"].toInt(), screen["height"].toInt());
		if (m_recordingMonitor.isEmpty() ? area.contains(anchor) : device == m_recordingMonitor)
		{
			config.monitor = i;
			config.monitorDevice = device;
			break;
		}
	}
	if (config.monitorDevice.isEmpty() && !screens.isEmpty())
		config.monitorDevice = screens.first().toObject()["name"].toString();
	if (m_auto)
	{
		const auto size = KRRFrameCapture::CaptureArea(config).size();
		config.width = qBound(160, size.width() / 2 * 2, 7680);
		config.height = qBound(120, size.height() / 2 * 2, 4320);
	}
	if (m_recommended) config.bitrate = RecommendedRecordingBitrate(config.width, config.height, config.fps);
	config.camera = qMax(0, config.camera);
	config.cameraEnabled = m_camera;
	config.cameraTrack = false;
	config.audio = m_mic ? (m_system ? "mix" : "mic") : (m_system ? "system" : "none");
	return config;
}

void MinimalRecorder::start()
{
	if (m_control || !m_output.isEmpty()) return;
	auto config = captureConfig();
	config.output = QDir(m_directory).absoluteFilePath("recording-" + Stamp() + "." + m_format);
	m_control = true;
	refresh();
	run([config]
	{
		Reply result;
		if (config.validate(result.error) && EnsureControlService(result.error))
			SendControl({{"command", "start"}, {"config", config.toJson()}}, result.value, result.error, 2400);
		return result;
	}, [this, config](const auto &reply)
	{
		m_control = false;
		if (!reply.value["ok"].toBool())
		{
			notify(reply.error.isEmpty() ? Ui("无法开始录制。") : UiDiagnostic(reply.error));
			return;
		}
		m_output = config.output;
		m_recordingMonitor = config.monitorDevice;
		m_servicePid = static_cast<DWORD>(reply.value["pid"].toDouble());
		m_sentEffects = config.effects;
		m_failedPolls = 0;
		m_poll.start();
		queueEffects();
	});
}

void MinimalRecorder::stop(bool exitAfter)
{
	if (m_control) return;
	if (m_output.isEmpty())
	{
		if (exitAfter) m_app.quit();
		return;
	}
	m_stopping = true;
	request({{"command", "stop"}}, 30000, [this, exitAfter](const auto &reply)
	{
		m_stopping = false;
		if (!reply.value.isEmpty() && reply.value["output"].toString() == m_output) sync(reply.value);
		else if (!reply.error.isEmpty()) notify(reply.error);
		if (exitAfter && m_output.isEmpty()) m_app.quit();
	});
}

void MinimalRecorder::poll()
{
	if (m_pending || m_control || m_output.isEmpty()) return;
	run([]
	{
		Reply result;
		SendControl({{"command", "status"}}, result.value, result.error, 900);
		return result;
	}, [this](const auto &reply)
	{
		if (m_output.isEmpty()) return;
		if (!reply.error.isEmpty())
		{
			if (++m_failedPolls >= 3 && m_servicePid)
			{
				const auto process = OpenProcess(SYNCHRONIZE, FALSE, m_servicePid);
				const bool exited = process ? WaitForSingleObject(process, 0) == WAIT_OBJECT_0 :
					GetLastError() == ERROR_INVALID_PARAMETER;
				if (process) CloseHandle(process);
				if (exited) sync({{"output", m_output}, {"state", "idle"}, {"error", reply.error}});
			}
			return;
		}
		m_failedPolls = 0;
		sync(reply.value);
	});
}

void MinimalRecorder::queueEffects(int delay)
{
	if (!m_pending && !m_control && !m_output.isEmpty() && m_effects != m_sentEffects && !m_effectFlush.isActive())
		m_effectFlush.start(delay);
}

void MinimalRecorder::flushEffects()
{
	if (m_pending || m_control || m_output.isEmpty() || m_effects == m_sentEffects) return;
	const auto effects = m_effects;
	run([effects]
	{
		Reply reply;
		SendControl({{"command", "scene"}, {"scene", QJsonObject{{"effects", effects}}}}, reply.value, reply.error, 1000);
		return reply;
	}, [this, effects](const auto &reply)
	{
		if (reply.error.isEmpty()) m_sentEffects = effects;
		queueEffects(reply.error.isEmpty() ? 33 : 400);
	});
}

void MinimalRecorder::sync(const QJsonObject &status)
{
	if (status["output"].toString() != m_output || status["state"] == "idle")
	{
		m_output.clear();
		m_recordingMonitor.clear();
		m_servicePid = 0;
		m_poll.stop();
		m_effectFlush.stop();
		m_paused = false;
		m_clock = "00:00:00";
		const auto error = status["error"].toString();
		if (!error.isEmpty())
		{
			QString notice = UiDiagnostic(error);
			if (status["output_published"].toBool() && status["failure_stage"] == "capture-source")
				notice += " " + Ui("已保存：") + status["output"].toString();
			notify(notice);
		}
		return;
	}
	m_servicePid = static_cast<DWORD>(status["pid"].toDouble(m_servicePid));
	m_paused = status["paused"].toBool();
	const int seconds = qMax(0, static_cast<int>(status["duration_sec"].toDouble()));
	m_clock = QString("%1:%2:%3").arg(seconds / 3600, 2, 10, QLatin1Char('0'))
		.arg(seconds / 60 % 60, 2, 10, QLatin1Char('0')).arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

void MinimalRecorder::captureStill()
{
	if (m_pending || m_control) return;
	const auto config = captureConfig();
	const auto directory = m_directory;
	run([config, directory]
	{
		Reply result;
		KRRFrameCapture capture;
		auto image = capture.capture(config, result.error);
		ApplyRecordingPreviewEffects(image, config);
		if (!image.isNull() && ProbeRecordingFolder(directory, result.error) &&
			!image.save(QDir(directory).filePath("screenshot-" + Stamp() + ".png")))
			result.error = Ui("无法保存截图。");
		return result;
	}, [this](const auto &reply) { if (!reply.error.isEmpty()) notify(reply.error); });
}

void MinimalRecorder::refresh()
{
	const bool active = !m_output.isEmpty();
	m_bar.setStatus(active, m_paused, m_control || m_stopping, m_clock);
	m_bar.setCaptureButtons(m_camera, m_mic, true, !m_control);
	m_bar.setMarkerEnabled(active && !m_paused && !m_control);
	m_workbench->setEnabled(!m_control);
}

void MinimalRecorder::notify(const QString &text)
{
	if (m_tray.isVisible()) m_tray.showMessage(Ui("WPS Recorder · 极简录制"), text, QSystemTrayIcon::Warning, 3500);
	else QMessageBox::warning(&m_bar, Ui("极简录制"), text);
}
}

int RunMinimalRecorder(QApplication &app)
{
	app.setQuitOnLastWindowClosed(false);
	MinimalRecorder recorder(app);
	const int result = recorder.show() ? app.exec() : SwitchToRecordingStudio;
	app.setQuitOnLastWindowClosed(true);
	return result;
}
