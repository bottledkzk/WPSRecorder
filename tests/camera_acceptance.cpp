// A native local-only QA window. Run outside the Low-labeled build workspace.
#include "recordingengine.h"
#include "cameracapture.h"
#include "displaygeometry.h"
#include "controlserver.h"
#include <windows.h>
#include <QApplication>
#include <QCloseEvent>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QProcess>
#include <QTimer>
#include <QThread>
#include <QVBoxLayout>

class CameraAcceptance : public QWidget
{
public:
	CameraAcceptance()
	{
		setWindowTitle("WPS Recorder · Camera acceptance"); resize(760, 520);
		auto layout = new QVBoxLayout(this);
		layout->addWidget(new QLabel("摄像头 / 画中画 / 独立轨 本机验收", this));
		layout->addWidget(new QLabel("约 1 分钟；录制媒体仅保存到以下本机目录，不上传。", this));
		m_path = new QLineEdit(QCoreApplication::applicationDirPath() + "/camera-qa-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss"), this); layout->addWidget(m_path);
		m_run = new QPushButton("开始本机验收", this); layout->addWidget(m_run);
		m_log = new QPlainTextEdit(this); m_log->setReadOnly(true); layout->addWidget(m_log);
		connect(m_run, &QPushButton::clicked, this, [this] { begin(); });
		connect(&m_engine, &KRRRecordingEngine::completed, this, [this](const QString&, const QString& error) { complete(error); });
		HideConsoleProcess(m_cli);
		connect(&m_cli, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this](int code, QProcess::ExitStatus exit)
		{
			m_cliResult = QJsonDocument::fromJson(m_cli.readAllStandardOutput()).object();
			QString error = code == 0 && exit == QProcess::NormalExit && m_cliResult["ok"].toBool() ? QString() : QString::fromUtf8(m_cli.readAllStandardError()) + QString(" CLI exit %1").arg(code);
			complete(error);
		});
		connect(&m_tick, &QTimer::timeout, this, [this] { tick(); }); m_tick.start(100);
	}
	void autoRun(const QString &path){m_auto=true;m_path->setText(path);QTimer::singleShot(200,this,[this]{begin();});}
protected:
	void closeEvent(QCloseEvent* event) override { if (m_running) { m_engine.requestStop(); event->ignore(); } else event->accept(); }
private:
	void write()
	{
		QJsonObject report{{"version", 1}, {"integrity_rid", ProcessIntegrityRid()}, {"app_container", IsContainerProcess()}, {"native_camera_width", m_nativeSize.width()}, {"native_camera_height", m_nativeSize.height()}, {"camera_track_collision_rejected", m_collisionRejected}, {"camera_devices", KRRCameraCapture::Devices()}, {"active_status", m_engine.status()}, {"complete", !m_running}, {"cases", m_results}};
		QFile file(m_root + "/camera-acceptance.json"); if (file.open(QIODevice::WriteOnly)) file.write(QJsonDocument(report).toJson(QJsonDocument::Indented));
	}
	void begin()
	{
		m_root = m_path->text(); if (QDir(m_root).exists()) { m_log->appendPlainText("Output exists. Choose a fresh directory."); return; }
		if (!QDir().mkpath(m_root)) return;
		KRRCameraCapture probe; QString error;
		if (!probe.start(0, error)) { m_log->appendPlainText(error); write(); if(m_auto)QCoreApplication::exit(1);return; }
		m_nativeSize = probe.frame(error).size(); probe.stop();
		QImage fixture(640, 360, QImage::Format_RGB32); fixture.fill(QColor("#25374f"));
		{ QPainter p(&fixture); p.fillRect(QRect(24, 24, 120, 44), QColor("#45b5ac")); p.fillRect(QRect(24, 84, 180, 8), QColor("#69839f")); }
		fixture.save(m_root + "/fixture.png");
		KRRRecordingConfig collision; collision.output = m_root + "/collision.mp4"; collision.cameraTrack = true;
		QFile sidecar(collision.output + ".camera.mkv"); sidecar.open(QIODevice::WriteOnly); sidecar.write("existing"); sidecar.close();
		m_collisionRejected = !collision.validate(error) && error.contains("Camera track already exists"); sidecar.remove();
		m_running = true; m_run->setEnabled(false); m_path->setEnabled(false); m_index = -1; next();
	}
	void next()
	{
		++m_index; if (m_index >= 10) { QString ignored; RunMediaProcess(QCoreApplication::applicationDirPath() + "/wpsrecorder_cli.exe", {"--service-exit"}, ignored, 10000); m_running = false; write(); m_log->appendPlainText("COMPLETE: camera-acceptance.json written. Independent media analysis required."); if(m_auto)QCoreApplication::exit(0);return; }
		m_inCli = m_index >= 8; m_timeout = false; m_lastProgress = -1;
		m_pauseIssued = m_sceneOne = m_sceneTwo = false; m_pauseDelta = -1;
		m_previewUpdates=0;m_previewLast=-1;m_previewValid=true;
		m_config = {}; m_config.width = 640; m_config.height = 360; m_config.fps = 30; m_config.bitrate = 3500; m_config.encoder = "h264_nvenc"; m_config.cursor = false;
		m_config.cameraTrack = true; m_config.cameraMirror = false; m_config.source = "image"; m_config.image = m_root + "/fixture.png"; m_config.cameraEnabled = true; m_config.cameraShape = "rectangle"; m_config.cameraRect = QRectF(.55, .30, .40, .60);
		m_config.maxDuration = 3;
		if (m_index == 0) { m_name = "camera_only"; m_config.source = "camera"; m_config.cameraMirror = true; m_config.maxDuration = 5; }
		else if (m_index == 1) { m_name = "pip_rectangle_audio_pause"; m_config.maxDuration = 6; m_config.audio = "mix"; m_config.separateAudio = true; }
		else if (m_index == 2) { m_name = "pip_circle_mirrored"; m_config.cameraShape = "circle"; m_config.cameraMirror = true; }
		else if (m_index == 3) { m_name = "pip_rounded_position"; m_config.cameraShape = "rounded"; m_config.cameraRect = QRectF(.10, .20, .35, .65); m_config.cameraBorder = "#ed4e62"; }
		else if (m_index == 4) { m_name = "live_source_switch"; m_config.cameraEnabled = false; m_config.maxDuration = 5; }
		else if (m_index < 8) { m_name = "camera_restart_" + QString::number(m_index - 4); m_config.source = "camera"; m_config.maxDuration = 1; }
		else if (m_index == 8) { m_name = "cli_camera_only"; m_config.source = "camera"; m_config.cameraMirror = true; m_config.maxDuration = 2; }
		else { m_name = "cli_pip_tracks"; m_config.audio = "mix"; m_config.separateAudio = true; m_config.maxDuration = 2; }
		m_config.output = m_root + "/" + m_name + ".mp4";
		QString error; m_log->appendPlainText("START " + m_name); m_wall.start();
		if (m_inCli)
		{
			qputenv("WPSRECORDER_INSTANCE", ("camera-qa-cli-" + QByteArray::number(QCoreApplication::applicationPid())));
			auto scene = m_config.toJson(); scene["maxDuration"] = 0;
			QFile preset(m_root + "/cli-scene.json"); preset.open(QIODevice::WriteOnly); preset.write(QJsonDocument(QJsonObject{{"version", 1}, {"scene", scene}}).toJson()); preset.close();
			m_cliResult = {}; m_cli.start(QCoreApplication::applicationDirPath() + "/wpsrecorder_cli.exe", {"--record", "--duration", "2", "--preset", preset.fileName(), "--output", m_config.output}); write(); return;
		}
		if (!m_engine.start(m_config, error)) { complete(error); return; }
		write();
	}
	void tick()
	{
		if (!m_running) return;
		QJsonObject preview;
		if(m_inCli){QString ignored;SendControl({{"command","preview"}},preview,ignored,200);}else preview=m_engine.preview();
		qint64 sequence=static_cast<qint64>(preview["sequence"].toDouble());
		if(preview["active"].toBool() && sequence>0 && sequence!=m_previewLast)
		{
			m_previewLast=sequence;++m_previewUpdates;
			QImage main=QImage::fromData(QByteArray::fromBase64(preview["frame"].toString().toLatin1()),"JPEG");
			QImage camera=QImage::fromData(QByteArray::fromBase64(preview["camera"].toString().toLatin1()),"JPEG");
			m_previewValid=m_previewValid&&!main.isNull()&&!camera.isNull()&&main.width()<=1920&&main.height()<=1200&&camera.width()<=960&&camera.height()<=720;
		}
		if (m_inCli) { if (m_wall.elapsed() > 45000 && !m_timeout) { m_timeout = true; m_cli.kill(); } return; }
		if (!m_engine.isActive()) return;
		auto status = m_engine.status(); double time = status["duration_sec"].toDouble();
		if (m_wall.elapsed() / 5000 != m_lastProgress) { m_lastProgress = m_wall.elapsed() / 5000; write(); m_log->appendPlainText(QString::fromUtf8(QJsonDocument(status).toJson(QJsonDocument::Compact))); }
		if (m_wall.elapsed() > 45000 && !m_timeout) { m_timeout = true; m_engine.requestStop(); m_log->appendPlainText("TIMEOUT"); }
		if (m_index == 1 && time > 1.3 && !m_pauseIssued)
		{
			m_pauseIssued = true; m_engine.setPaused(true); qint64 before = m_engine.status()["frames_written"].toVariant().toLongLong();
			QTimer::singleShot(1000, this, [this, before] { m_pauseDelta = m_engine.status()["frames_written"].toVariant().toLongLong() - before; m_engine.setPaused(false); });
		}
		if (m_index == 4)
		{
			QString error;
			if (time > 1.2 && !m_sceneOne) { m_sceneOne = true; if (!m_engine.updateScene({{"source", "camera"}, {"cameraMirror", false}}, error)) m_log->appendPlainText(error); }
			if (time > 3.1 && !m_sceneTwo) { m_sceneTwo = true; if (!m_engine.updateScene({{"source", "image"}, {"cameraEnabled", true}, {"cameraShape", "circle"}, {"cameraMirror", true}}, error)) m_log->appendPlainText(error); }
		}
	}
	void complete(const QString& error)
	{
		auto result = m_inCli ? m_cliResult : m_engine.status(); result["case"] = m_name; result["config"] = m_config.toJson(); result["wall_ms"] = m_wall.elapsed(); result["pause_delta_frames"] = static_cast<double>(m_pauseDelta); result["error"] = error;
		result["preview_updates"]=m_previewUpdates;result["preview_valid"]=m_previewValid&&m_previewUpdates>=2;
		m_results.append(result); m_log->appendPlainText((error.isEmpty() ? "RECORDED " : "FAILED ") + m_name + " " + error); write();
		QTimer::singleShot(250, this, [this] { next(); });
	}
	KRRRecordingEngine m_engine;
	QProcess m_cli;
	QJsonObject m_cliResult;
	bool m_inCli = false;
	QTimer m_tick;
	QElapsedTimer m_wall;
	QLineEdit* m_path = nullptr;
	QPushButton* m_run = nullptr;
	QPlainTextEdit* m_log = nullptr;
	QString m_root, m_name;
	QSize m_nativeSize;
	bool m_collisionRejected = false;
	KRRRecordingConfig m_config;
	QJsonArray m_results;
	int m_index = -1;
	qint64 m_pauseDelta = -1;
	qint64 m_lastProgress = -1;
	bool m_timeout = false;
	int m_previewUpdates=0;
	qint64 m_previewLast=-1;
	bool m_previewValid=true;
	bool m_running = false, m_pauseIssued = false, m_sceneOne = false, m_sceneTwo = false;
	bool m_auto=false;
};
static int ProbeDeviceAccess(const QString& folder, const QString& output)
{
	QString writeError, cameraError;
	const bool canWrite = ProbeRecordingFolder(folder, writeError, false);
	KRRCameraCapture probe;
	const bool started = probe.start(0, cameraError);
	QSize size;
	quint64 frames = 0;
	if (started)
	{
		QElapsedTimer clock;
		clock.start();
		while (clock.elapsed() < 3500 && (frames < 12 || size.isEmpty()))
		{
			const auto frame = probe.frame(cameraError);
			if (!cameraError.isEmpty()) break;
			if (!frame.isNull()) size = frame.size();
			frames = probe.frameSequence();
			QThread::msleep(20);
		}
		probe.stop();
	}
	const bool passed = canWrite && frames >= 12 && !size.isEmpty() && cameraError.isEmpty();
	QFile report(output);
	if (!report.open(QIODevice::WriteOnly)) return 2;
	report.write(QJsonDocument(QJsonObject{{"passed", passed}, {"integrity_rid", ProcessIntegrityRid()},
		{"app_container", IsContainerProcess()}, {"directory_writable", canWrite}, {"directory_error", writeError},
		{"camera_error", cameraError}, {"camera_frames", qint64(frames)}, {"width", size.width()}, {"height", size.height()}}).toJson());
	return passed ? 0 : 1;
}

int main(int argc, char** argv)
{
	InitializeDisplayCoordinates(); QApplication::setAttribute(Qt::AA_DisableHighDpiScaling); QApplication app(argc, argv);
	const auto args = app.arguments();
	if (args.size() == 4 && args[1] == "--device-access") return ProbeDeviceAccess(args[2], args[3]);
	CameraAcceptance window; window.show();if(app.arguments().size()==3&&app.arguments()[1]=="--auto")window.autoRun(app.arguments()[2]);return app.exec();
}
