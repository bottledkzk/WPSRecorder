#include "recordingcheckdialog.h"
#include "audiocapture.h"
#include "cameracapture.h"
#include "compositor.h"
#include "recordingcheck.h"
#include "uilanguage.h"
#include "uitheme.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QPixmap>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>
#include <QUrl>
#include <cmath>
#include <windows.h>

KRRRecordingCheckDialog::KRRRecordingCheckDialog(const KRRRecordingConfig& config, const QString& source,
												 const QJsonObject& lastStatus, QWidget* parent)
	: QDialog(parent), m_config(config), m_lastStatus(lastStatus)
{
	setObjectName("recordingCheckDialog");
	setWindowTitle(Ui("录前检查"));
	setWindowFlag(Qt::WindowContextHelpButtonHint, false);
	resize(RecorderUi::Px(680), RecorderUi::Px(640));
	auto layout = new QVBoxLayout(this);
	auto target = new QLabel(source, this);
	target->setTextFormat(Qt::PlainText);
	target->setWordWrap(true);
	layout->addWidget(target);
	m_preview = new QLabel(this);
	m_preview->setObjectName("recordingCheckFrame");
	m_preview->setAlignment(Qt::AlignCenter);
	m_preview->setTextFormat(Qt::PlainText);
	m_preview->setWordWrap(true);
	m_preview->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
	m_preview->setMinimumHeight(RecorderUi::Px(200));
	m_preview->setStyleSheet("background:#152133;color:#fff;border-radius:8px;");
	auto pictures = new QHBoxLayout;
	pictures->addWidget(m_preview, 1);
	layout->addLayout(pictures, 1);
	if (config.source == "camera" || config.cameraEnabled || config.cameraTrack)
	{
		if (config.cameraTrack && config.source != "camera" && !config.cameraEnabled)
		{
			m_cameraPreview = new QLabel(this);
			m_cameraPreview->setObjectName("recordingCheckCameraTrack");
			m_cameraPreview->setAlignment(Qt::AlignCenter);
			m_cameraPreview->setFixedSize(RecorderUi::Px(180), RecorderUi::Px(120));
			m_cameraPreview->setStyleSheet("background:#152133;border-radius:8px;");
			pictures->addWidget(m_cameraPreview, 0, Qt::AlignTop);
		}
		m_cameraStatus = new QLabel(this);
		m_cameraStatus->setObjectName("preflightCameraStatus");
		m_cameraStatus->setTextFormat(Qt::PlainText);
		m_cameraStatus->setWordWrap(true);
		layout->addWidget(m_cameraStatus);
		connect(&m_cameraTimer, &QTimer::timeout, this, &KRRRecordingCheckDialog::updateCamera);
	}
	auto levels = new QFormLayout;
	for (size_t i = 0; i < m_levels.size(); ++i)
	{
		auto row = new QVBoxLayout;
		m_levels[i] = new QProgressBar(this);
		m_levels[i]->setObjectName(i == 0 ? "preflightMicLevel" : "preflightSystemLevel");
		m_levels[i]->setRange(0, 60);
		m_levels[i]->setValue(0);
		m_levels[i]->setTextVisible(false);
		m_info[i] = new QLabel(this);
		m_info[i]->setObjectName(i == 0 ? "preflightMicDevice" : "preflightSystemDevice");
		m_info[i]->setTextFormat(Qt::PlainText);
		m_info[i]->setWordWrap(true);
		row->addWidget(m_levels[i]);
		row->addWidget(m_info[i]);
		levels->addRow(i == 0 ? Ui("麦克风") : Ui("系统声音"), row);
	}
	layout->addLayout(levels);
	m_report = new QLabel(this);
	m_report->setObjectName("recordingDiagnostics");
	m_report->setTextFormat(Qt::PlainText);
	m_report->setWordWrap(true);
	m_report->setTextInteractionFlags(Qt::TextSelectableByMouse);
	layout->addWidget(m_report);
	auto help = new QHBoxLayout;
	for (const auto& entry : {qMakePair(Ui("Windows 声音设置"), QString("ms-settings:sound")),
							 qMakePair(Ui("麦克风权限"), QString("ms-settings:privacy-microphone"))})
	{
		auto button = new QPushButton(entry.first, this);
		connect(button, &QPushButton::clicked, this, [url = entry.second] { QDesktopServices::openUrl(QUrl(url)); });
		help->addWidget(button);
	}
	help->addStretch();
	layout->addLayout(help);
	auto buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
	m_refresh = buttons->addButton(Ui("重新检查"), QDialogButtonBox::ActionRole);
	m_trial = buttons->addButton(Ui("试录 6 秒"), QDialogButtonBox::ActionRole);
	m_trial->setObjectName("preflightTrial");
	m_trial->setProperty("role", "primary");
	connect(m_refresh, &QPushButton::clicked, this, [this] { startMeters(); requestPreview(); });
	connect(m_trial, &QPushButton::clicked, this, [this]
	{
		accept();
		emit trialRequested();
	});
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	layout->addWidget(buttons);
	RecorderUi::PolishButtons(this);
	connect(&m_timer, &QTimer::timeout, this, &KRRRecordingCheckDialog::updateMeters);
	m_frameTimer.setTimerType(Qt::PreciseTimer);
	connect(&m_frameTimer, &QTimer::timeout, this, &KRRRecordingCheckDialog::updateLivePreview);
	m_probeTimeout.setSingleShot(true);
	connect(&m_probeTimeout, &QTimer::timeout, this, [this] { if (m_probe) m_probe->kill(); });
	SetWindowDisplayAffinity(reinterpret_cast<HWND>(winId()), 0x11);
	startMeters();
	requestPreview();
}

KRRRecordingCheckDialog::~KRRRecordingCheckDialog() = default;

void KRRRecordingCheckDialog::done(int result)
{
	m_timer.stop();
	m_frameTimer.stop();
	m_livePreview.reset();
	m_cameraTimer.stop();
	m_camera.reset();
	m_probeTimeout.stop();
	if (m_probe && m_probe->state() != QProcess::NotRunning)
	{
		m_probe->blockSignals(true);
		m_probe->kill();
		m_probe->waitForFinished(500);
	}
	for (auto& meter : m_audio) meter.reset();
	QDialog::done(result);
}

void KRRRecordingCheckDialog::resizeEvent(QResizeEvent* event)
{
	QDialog::resizeEvent(event);
	displayPreview();
}

void KRRRecordingCheckDialog::startMeters()
{
	for (size_t i = 0; i < m_audio.size(); ++i)
	{
		m_audio[i].reset();
		m_audio[i] = std::make_unique<KRRAudioCapture>(i == 1);
		m_audio[i]->startMeter();
		m_smoothed[i] = 0;
	}
	updateMeters();
	m_timer.start(100);
}

void KRRRecordingCheckDialog::updateMeters()
{
	for (size_t i = 0; i < m_audio.size(); ++i)
	{
		const auto& meter = m_audio[i];
		if (!meter) continue;
		const bool included = m_config.audio == "mix" || m_config.audio == (i == 0 ? "mic" : "system");
		const auto error = meter->error();
		m_smoothed[i] = qMax(meter->takePeak(), m_smoothed[i] * .76f);
		int db = qBound(-60, qRound(20 * std::log10(qMax(.001f, m_smoothed[i]))), 0);
		m_levels[i]->setValue(error.isEmpty() ? db + 60 : 0);
		QString status = included ? Ui("将录入") : Ui("未选入录制");
		if (!error.isEmpty()) status += " · " + UiDiagnostic(error);
		else if (meter->meterReady())
		{
			const auto name = meter->deviceName();
			if (!name.isEmpty()) status += " · " + name;
			status += db == -60 ? " · " + Ui("暂无声音") : QString(" · %1 dBFS").arg(db);
		}
		else status += " · " + Ui("正在打开设备…");
		if (m_info[i]->text() != status) m_info[i]->setText(status);
	}
}

void KRRRecordingCheckDialog::requestPreview()
{
	if (!m_probe)
	{
		m_probe = new QProcess(this);
		HideConsoleProcess(*m_probe);
		connect(m_probe, &QProcess::started, this, [this]
		{
			auto config = m_config.toJson();
			config["liveCamera"] = m_cameraStatus != nullptr;
			config["livePreview"] = m_livePreview != nullptr;
			m_probe->write(QJsonDocument(config).toJson(QJsonDocument::Compact));
			m_probe->closeWriteChannel();
		});
		connect(m_probe, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
			[this](int code, QProcess::ExitStatus status)
		{
			m_probeTimeout.stop();
			m_checks = QJsonDocument::fromJson(m_probe->readAllStandardOutput()).object();
			if (m_livePreview)
				m_checks["capture"] = m_livePreview->latest().capture;
			else
				m_sourceError = UiDiagnostic(m_checks["capture_error"].toString());
			if (m_sourceError.isEmpty() && (code || status != QProcess::NormalExit))
				m_sourceError = Ui("无法预览当前来源，请检查窗口或画面设备。");
			displayPreview();
			updateReport();
			m_refresh->setEnabled(true);
			updateTrial();
		});
		connect(m_probe, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error)
		{
			if (error != QProcess::FailedToStart) return;
			m_probeTimeout.stop();
			m_sourceError = Ui("无法启动录前检查，请确认 CLI 与主程序在同一个目录。");
			displayPreview();
			m_refresh->setEnabled(true);
		});
	}
	if (m_probe->state() != QProcess::NotRunning) return;
	if (m_cameraStatus && (!m_camera || m_cameraFailed)) startCamera();
	m_checks = {};
	m_image = {};
	if (m_config.source != "camera")
	{
		m_livePreview.reset();
		m_livePreview = std::make_unique<KRRLiveCapturePreview>(m_config, 33);
		m_frameSequence = 0;
		m_frameTimer.start(33);
	}
	m_sourceError = Ui("正在获取录制画面…");
	displayPreview();
	updateReport();
	m_refresh->setEnabled(false);
	m_trial->setEnabled(false);
	m_probe->start(QCoreApplication::applicationDirPath() + "/wpsrecorder_cli.exe", {"--capture-check"});
	m_probeTimeout.start(42000);
}

void KRRRecordingCheckDialog::updateLivePreview()
{
	const auto frame = m_livePreview->latest();
	if (frame.sequence == m_frameSequence) return;
	const bool readyChanged = m_image.isNull() != frame.image.isNull();
	m_frameSequence = frame.sequence;
	m_image = frame.image;
	m_sourceError = UiDiagnostic(frame.error);
	if (m_image.isNull() && m_sourceError.isEmpty())
		m_sourceError = Ui("无法预览当前来源，请检查窗口或画面设备。");
	displayPreview();
	if (m_checks["capture"].toObject() != frame.capture)
	{
		m_checks["capture"] = frame.capture;
		updateReport();
	}
	if (readyChanged) updateTrial();
}

void KRRRecordingCheckDialog::displayPreview()
{
	if (!m_preview) return;
	QImage image = m_image;
	if (m_config.source == "camera" && !m_cameraImage.isNull())
	{
		const QSize target = QSize(m_config.width, m_config.height).scaled(
			m_preview->size().boundedTo(QSize(2560, 1440)), Qt::KeepAspectRatio);
		image = m_cameraImage.scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
		if (m_config.cameraMirror) image = image.mirrored(true, false);
		ApplyRecordingPreviewEffects(image, m_config);
	}
	else if (m_config.cameraEnabled && !image.isNull())
	{
		image = image.convertToFormat(QImage::Format_RGB32);
		DrawCamera(image, m_cameraImage, m_config);
	}
	if (m_config.source != "camera") ApplyRecordingPreviewEffects(image, m_config);
	m_preview->setProperty("sourceFrameSize", image.size());
	if (image.isNull())
		m_preview->setText(m_sourceError.isEmpty() && m_cameraStatus ? m_cameraStatus->text() : m_sourceError);
	else
	{
		m_preview->setPixmap(QPixmap::fromImage(image.scaled(m_preview->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation)));
		m_preview->setProperty("cameraFrameSequence", QVariant::fromValue(m_cameraSequence));
		m_preview->setProperty("sourceFrameSequence", m_frameSequence);
	}
	if (m_cameraPreview)
	{
		if (m_cameraImage.isNull()) m_cameraPreview->clear();
		else m_cameraPreview->setPixmap(QPixmap::fromImage(
			m_cameraImage.scaled(m_cameraPreview->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation)));
	}
}

void KRRRecordingCheckDialog::startCamera()
{
	m_camera = std::make_unique<KRRCameraCapture>();
	m_cameraImage = {};
	m_cameraSequence = 0;
	m_cameraFailed = false;
	m_cameraStatus->setText(Ui("正在打开摄像头…"));
	m_cameraActivity.start();
	m_camera->startAsync(m_config.camera);
	m_cameraTimer.start(66);
}

void KRRRecordingCheckDialog::updateCamera()
{
	QString error;
	quint64 sequence = 0;
	const auto image = m_camera->frame(error, &sequence);
	if (error.isEmpty() && sequence == m_cameraSequence && m_cameraActivity.elapsed() < 10000) return;
	if (error.isEmpty() && sequence == m_cameraSequence) error = Ui("摄像头超时，请检查设备后重新检查。");
	if (!error.isEmpty())
	{
		m_cameraTimer.stop();
		m_cameraFailed = true;
		m_cameraImage = {};
		m_cameraStatus->setText(UiDiagnostic(error));
	}
	else
	{
		m_cameraActivity.restart();
		m_cameraImage = image;
		if (!m_cameraSequence)
			m_cameraStatus->setText(m_cameraPreview ? Ui("摄像头独立视频轨 · 实时") : Ui("摄像头 · 实时"));
		m_cameraSequence = sequence;
	}
	displayPreview();
	updateTrial();
}

void KRRRecordingCheckDialog::updateTrial()
{
	const bool sourceReady = m_config.source == "camera" || !m_image.isNull();
	m_trial->setEnabled(sourceReady && m_checks["ok"].toBool() && (!m_cameraStatus || !m_cameraImage.isNull()));
}

void KRRRecordingCheckDialog::updateReport()
{
	const auto captureInfo = m_checks["capture"].toObject();
	const auto capture = captureInfo["api"].toString();
	QString encoder = m_checks["encoder"].toString();
	if (encoder.isEmpty()) encoder = UiDiagnostic(m_checks["encoder_error"].toString());
	if (m_config.encoder == "auto" && encoder == "libx264") encoder += Ui("（自动回退到 CPU）");
	const auto root = QFileInfo(m_config.output).absolutePath();
	QString writeError;
	const auto writable = !QFileInfo::exists(root) ? Ui("待创建") : ProbeRecordingFolder(root, writeError, false) ? Ui("可写") : Ui("不可写");
	QString text = Ui("采集 %1 · 编码 %2 · 保存目录 %3")
		.arg(capture.isEmpty() ? "—" : capture, encoder.isEmpty() ? "—" : encoder,
			 writable);
	if (captureInfo["fallback_reason"] == "dxgi_blank_frame")
		text += "\n" + Ui("桌面复制返回空画面，已切换到兼容采集。");
	if (captureInfo["desktop_black"].toBool())
		text += "\n" + Ui("当前桌面画面全黑。请打开有内容的窗口，再重新检查。");
	if (!writeError.isEmpty()) text += QStringLiteral("\n") + root + QStringLiteral(" · ") + writeError;
	const auto error = m_lastStatus["error"].toString().trimmed();
	if (!error.isEmpty())
		text += "\n" + Ui("上次失败在 %1（录制至 %2 秒）").arg(
			UiDiagnostic(m_lastStatus["failure_stage"].toString()), QString::number(m_lastStatus["duration_sec"].toDouble(), 'f', 1))
			+ "\n" + UiDiagnostic(error.left(600));
	m_report->setText(text);
}
