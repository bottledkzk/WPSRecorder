#include "uilanguage.h"
#include "mainwindow.h"
#include "taskexit.h"
#ifdef WPS_WEB_STUDIO
#include "webstudio.h"
#endif
#include "recordingwidgets.h"
#include "recordinghotkeys.h"
#include <QSet>
#include "recordingcheck.h"
#include "recordingcheckdialog.h"
#include "recordingassistantdialog.h"
#include "recordingrecovery.h"
#include "recordingmarkers.h"
#include "playerwindow.h"
#include "playbackslider.h"
#include "previewchannel.h"
#include "controlserver.h"
#include "framecapture.h"
#include "regionselector.h"
#include "annotationoverlay.h"
#include "audiocapture.h"
#include "cameracapture.h"
#include "compositor.h"
#include "uitheme.h"
#include "clipeditor.h"
#include "submissiondialog.h"
#include "speechsettingsdialog.h"
#include "previewvideo.h"

#include <windows.h>
#include <QCheckBox>
#include <QCloseEvent>
#include <QColorDialog>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QLineEdit>
#include <QMediaPlayer>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QScopedValueRollback>
#include <QActionGroup>
#include <windowsx.h>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QUrl>
#include <QVBoxLayout>
#include <QDateTimeEdit>
#include <QDialogButtonBox>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QKeySequenceEdit>
#include <QListWidget>
#include <QScrollArea>
#include <QTabWidget>
#include <QToolButton>
#include <QStyle>
#include <QMenu>
#include <QStackedWidget>
#include <QFrame>
#include <algorithm>
#include <QPainter>
#include <QLocalSocket>
#include <QImageReader>
#include <QUuid>
#include <QButtonGroup>
#include <QScreen>
#include <QGuiApplication>
#include <QThreadPool>
#include <QPointer>

static QString Stamp() { return QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss-zzz"); }

static QString ResolveBundledRecordingImage(const QString& path)
{
	if (path.isEmpty() || QFileInfo(path).isFile()) return path;
	const auto file = QFileInfo(path).fileName();
	if (!QStringList{"alpine-lake.png", "intermission.png", "study-at-dusk.png"}.contains(file) ||
		!QDir::fromNativeSeparators(path).endsWith("/studio/assets/" + file, Qt::CaseInsensitive)) return path;
	const auto bundled = QCoreApplication::applicationDirPath() + "/studio/assets/" + file;
	return QFileInfo(bundled).isFile() ? bundled : path;
}
static QString DefaultRecordingFolder()
{
	const auto movies = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
	return movies.isEmpty() ? QString{} : QDir(movies).filePath("WPSRecorder");
}
static QString TimeText(double seconds)
{
	int total = static_cast<int>(seconds);
	return QString("%1:%2:%3").arg(total / 3600, 2, 10, QLatin1Char('0'))
		.arg(total / 60 % 60, 2, 10, QLatin1Char('0')).arg(total % 60, 2, 10, QLatin1Char('0'));
}
KRRMainWindow::KRRMainWindow(QWidget* parent) : QMainWindow(parent)
{
	setWindowTitle("WPS Recorder · " + Ui("录制工作台"));
	setWindowFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowSystemMenuHint | Qt::WindowMinMaxButtonsHint);
	resize(RecorderUi::Px(1240), RecorderUi::Px(765));
	setMinimumSize(RecorderUi::Px(860), RecorderUi::Px(550));
	auto central = new QWidget(this);
	auto root = new QVBoxLayout(central);
	buildStudioHeader(root);
	m_mode = new QComboBox(this);
	m_mode->setObjectName("studioSourceType");
	m_mode->addItems({Ui("屏幕录制"), Ui("区域录制"), Ui("窗口录制"), Ui("摄像头录制"), Ui("图片源")});
	m_cameraToggle = RecorderUi::Toggle("camera", Ui("画中画"), this);
	m_micToggle = RecorderUi::Toggle("mic", Ui("麦克风"), this);
	m_cameraToggle->setObjectName("cameraToggle");
	m_micToggle->setObjectName("micToggle");
	m_lectureButton = new QPushButton(RecorderUi::Icon("pen"), Ui("讲解"), this);
	m_lectureButton->setCheckable(true);
	connect(m_lectureButton, &QPushButton::clicked, this, &KRRMainWindow::showAnnotation);
	m_settingsDialog = new QDialog(this); m_settingsDialog->setWindowTitle(Ui("设置")); m_settingsDialog->resize(RecorderUi::Px(760), RecorderUi::Px(610)); m_settingsDialog->setMinimumSize(RecorderUi::Px(680), RecorderUi::Px(480));
	auto settingsRoot = new QVBoxLayout(m_settingsDialog); auto settingsBody = new QHBoxLayout;
	m_settingsNav = new QListWidget(m_settingsDialog); m_settingsNav->setObjectName("settingsNav"); m_settingsNav->addItems({Ui("画面来源"), Ui("画质与编码"), Ui("声音与分轨"), Ui("摄像头样式"), Ui("鼠标与讲解"), Ui("存储与倒计时"), Ui("界面语言"), Ui("模型服务"), Ui("会话管理")}); m_settingsNav->setFixedWidth(RecorderUi::Px(CurrentUiLanguage() == "en" ? 190 : 160)); m_settingsNav->setSpacing(RecorderUi::Px(4)); m_settingsNav->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	m_settingsNav->item(6)->setData(Qt::UserRole, "interface-language");
	m_settingsPages = new QStackedWidget(m_settingsDialog); settingsBody->addWidget(m_settingsNav); settingsBody->addWidget(m_settingsPages, 1); settingsRoot->addLayout(settingsBody, 1);
	auto done = new QPushButton(Ui("完成"), m_settingsDialog); done->setFixedWidth(RecorderUi::Px(100)); done->setProperty("role", "primary"); done->setIcon(RecorderUi::Icon("done")); auto footer = new QHBoxLayout; footer->addStretch(); footer->addWidget(done); settingsRoot->addLayout(footer);
	connect(done, &QPushButton::clicked, this, [this] { if (validateStorage()) { saveSettings(); sendScene(); m_settingsDialog->accept(); } });
	auto configureModels = new QPushButton(Ui("模型服务"), m_settingsDialog);
	configureModels->setObjectName("openModelServices");
	footer->insertWidget(0, configureModels);
	connect(configureModels, &QPushButton::clicked, this, [this] { ConfigureSpeechServices(m_settingsDialog, "assistant"); });
	configureModels->hide();
	auto configureShortcuts = new QPushButton(Ui("快捷键"), m_settingsDialog);
	footer->insertWidget(1, configureShortcuts);
	connect(configureShortcuts, &QPushButton::clicked, this, &KRRMainWindow::configureHotkeys);
	connect(m_settingsNav, &QListWidget::currentRowChanged, m_settingsPages, &QStackedWidget::setCurrentIndex); m_settingsNav->setCurrentRow(0);
	auto settingsPage = [this](const QString& title, const QString& description)
	{
		auto page = new QWidget(m_settingsDialog); auto layout = new QVBoxLayout(page); layout->setContentsMargins(12, 10, 12, 10); layout->setSpacing(7);
		page->setAccessibleName(title);
		Q_UNUSED(description);
		auto form = new QFormLayout; form->setVerticalSpacing(6); form->setHorizontalSpacing(12); form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow); layout->addLayout(form); layout->addStretch();
		auto scroll = new QScrollArea(m_settingsDialog); scroll->setWidgetResizable(true); scroll->setWidget(page); m_settingsPages->addWidget(scroll); return form;
	};
	auto form = settingsPage(Ui("画面来源"), Ui("选择要记录的屏幕、窗口或区域。"));
	auto videoForm = settingsPage(Ui("画质与编码"), Ui("自动模式优先使用可用的 GPU 编码器。"));
	auto audioForm = settingsPage(Ui("声音与分轨"), Ui("分别保留系统声音与麦克风，便于后期处理。"));
	auto cameraForm = settingsPage(Ui("摄像头样式"), Ui("调整画中画位置与外观，参数以画面百分比表示。"));
	auto effectForm = settingsPage(Ui("鼠标与讲解"), Ui("画笔、聚光灯、白板和遮挡可在讲解工具中实时使用。"));
	auto saveForm = settingsPage(Ui("存储与倒计时"), Ui("默认文件夹用于后续录制、独立摄像头视频和截图。可选择本地目录或已配置的同步目录。"));
	auto languageForm = settingsPage(Ui("界面语言"), {});
	auto modelPage = settingsPage(Ui("模型服务"), {});
	modelPage->addRow(configureModels);
	configureModels->show();
	auto conversationPage = settingsPage(Ui("会话管理"), {});
	auto archive = new QPushButton(Ui("已归档的对话"), this);
	conversationPage->addRow(archive);
	connect(archive, &QPushButton::clicked, this, &KRRMainWindow::showConversationSettings);
	auto language = new QComboBox(m_settingsDialog);
	language->setObjectName("interfaceLanguage");
	language->addItem(QStringLiteral("简体中文"), "zh-CN");
	language->addItem("English", "en");
	language->setCurrentIndex(language->findData(CurrentUiLanguage()));
	languageForm->addRow(Ui("界面语言"), language);
	auto applyLanguage = new QPushButton(Ui("应用语言"), m_settingsDialog);
	applyLanguage->setObjectName("applyInterfaceLanguage");
	applyLanguage->setEnabled(false);
	applyLanguage->setProperty("role", "primary");
	languageForm->addRow(applyLanguage);
	connect(language, qOverload<int>(&QComboBox::currentIndexChanged), this, [language, applyLanguage] {
		applyLanguage->setEnabled(language->currentData().toString() != CurrentUiLanguage());
	});
	connect(applyLanguage, &QPushButton::clicked, this, [this, language] {
		QJsonObject status;
		QString error;
		SendControl({{"command", "status"}}, status, error, 300);
		if (m_active || m_countdownRemaining > 0 || status["state"] == "recording")
		{
			QMessageBox::information(m_settingsDialog, Ui("切换界面语言"), Ui("请结束录制或倒计时后再切换界面语言。"));
			return;
		}
		if (QMessageBox::question(m_settingsDialog, Ui("切换界面语言"),
			Ui("将重新打开录制工作台以应用新语言。现在切换？"),
			QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes)
			emit languageChangeRequested(language->currentData().toString());
	});
	m_directory = new QLineEdit(this);
	m_directory->setObjectName("defaultSaveDirectory"); m_directory->setPlaceholderText(Ui("选择默认保存文件夹"));
	auto directoryRow = new QWidget(this);
	auto directoryLayout = new QHBoxLayout(directoryRow);
	directoryLayout->setContentsMargins(0, 0, 0, 0);
	directoryLayout->addWidget(m_directory);
	auto browse = new QPushButton(Ui("选择"), this);
	directoryLayout->addWidget(browse);
	saveForm->addRow(Ui("默认保存文件夹"), directoryRow);
	auto storageTools = new QWidget(this); auto storageLayout = new QHBoxLayout(storageTools); storageLayout->setContentsMargins(0,0,0,0);
	auto openStorage = new QPushButton(Ui("打开文件夹"), this); auto resetStorage = new QPushButton(Ui("恢复默认"), this); storageLayout->addWidget(openStorage); storageLayout->addWidget(resetStorage); storageLayout->addStretch(); saveForm->addRow("", storageTools);
	connect(openStorage, &QPushButton::clicked, this, [this] { if (validateStorage()) QDesktopServices::openUrl(QUrl::fromLocalFile(m_directory->text())); });
	connect(m_directory, &QLineEdit::textEdited, this, [this] { m_defaultDirectory = false; });
	connect(resetStorage, &QPushButton::clicked, this, [this]
	{
		m_defaultDirectory = true;
		m_directory->setText(DefaultRecordingFolder());
	});
	connect(browse, &QPushButton::clicked, this, &KRRMainWindow::browseRecordingFolder);
	m_resolution = new QComboBox(this);
	m_resolution->addItem(Ui("自动（跟随来源）"), "auto");
	m_resolution->addItems({ "1920x1080", "1280x720", "2560x1440", "3840x2160", "640x360"});
	m_resolution->setEditable(true);
	videoForm->addRow(Ui("输出分辨率"), m_resolution);
	m_fps = new QComboBox(this);
	m_fps->setObjectName("recordingFps");
	for (int fps : RecordingFrameRates) m_fps->addItem(QString::number(fps), fps);
	videoForm->addRow(Ui("帧率 fps"), m_fps);
	m_bitrate = new QSpinBox(this); m_bitrate->setRange(100, 100000); m_bitrate->setSingleStep(500);
	m_bitrate->setObjectName("recordingBitrate");
	m_bitrate->setKeyboardTracking(false);
	connect(m_bitrate, &QSpinBox::editingFinished, this, [this] { m_recommendedBitrate->setChecked(false); });
	videoForm->addRow(Ui("视频码率 kbps"), m_bitrate);
	m_recommendedBitrate = new QCheckBox(Ui("使用推荐码率"), this);
	m_recommendedBitrate->setObjectName("useRecommendedBitrate");
	m_bitrateHint = new QLabel(this);
	m_bitrateHint->setProperty("muted", true);
	auto videoDefaults = new QPushButton(Ui("默认"), this);
	videoDefaults->setObjectName("recordingRecommendedDefault");
	videoForm->addRow(videoDefaults);
	m_recommendedBitrate->hide();
	m_bitrateHint->hide();
	connect(videoDefaults, &QPushButton::clicked, this, [this] { m_recommendedBitrate->setChecked(true); updateBitrateRecommendation(); });
	m_format = new QComboBox(this); m_format->addItems({"mp4", "mov", "mkv", "avi", "flv"});
	videoForm->addRow(Ui("文件格式"), m_format);
	m_outputColor = new QComboBox(this);
	m_outputColor->setObjectName("recordingOutputColor");
	m_outputColor->addItem("SDR", "sdr");
	m_outputColor->addItem("HDR (HEVC)", "hdr");
	m_outputColor->addItem(Ui("SDR 转 HDR"), "sdr-to-hdr");
	m_outputColor->addItem(Ui("HDR 转 SDR"), "hdr-to-sdr");
	videoForm->addRow(Ui("色彩"), m_outputColor);
	auto colors=new QGroupBox(Ui("屏幕颜色与亮度"),this);auto colorForm=new QFormLayout(colors);
	m_screenColor=new QComboBox(this);m_screenColor->addItem(Ui("自动 · 匹配系统SDR白电平"),"auto");m_screenColor->addItem(Ui("高光压缩 · 保留亮部层次"),"highlights");colorForm->addRow("HDR→SDR",m_screenColor);
	auto colorMore=new QToolButton(this);colorMore->setText(Ui("亮度微调 ▾"));colorMore->setCheckable(true);colorForm->addRow(colorMore);
	auto advancedColor=new QWidget(this);auto advancedForm=new QFormLayout(advancedColor);advancedForm->setContentsMargins(0,0,0,0);
	m_screenExposure=new QDoubleSpinBox(this);m_screenExposure->setRange(-2,2);m_screenExposure->setSingleStep(.25);m_screenExposure->setSuffix(" EV");advancedForm->addRow(Ui("曝光补偿"),m_screenExposure);
	m_screenWhite=new QSpinBox(this);m_screenWhite->setRange(0,1000);m_screenWhite->setSingleStep(10);m_screenWhite->setSpecialValueText(Ui("跟随系统"));m_screenWhite->setSuffix(" nit");m_screenWhite->setToolTip(Ui("0为读取Windows的SDR内容亮度；手动增大参考白可降低HDR录制亮度。通常保持自动。"));advancedForm->addRow(Ui("HDR参考白"),m_screenWhite);
	colorForm->addRow(advancedColor);advancedColor->hide();connect(colorMore,&QToolButton::toggled,this,[advancedColor,colorMore](bool show){advancedColor->setVisible(show);colorMore->setText(show?Ui("收起微调 ▴"):Ui("亮度微调 ▾"));});
	auto colorNote=new QLabel(Ui("用于屏幕/选区录制，不改变系统HDR或摄像头。偏亮时可降低曝光，或选择高光压缩。"),this);colorNote->setWordWrap(true);colorNote->setProperty("muted",true);colorForm->addRow(colorNote);

	m_audio = new QComboBox(this);
	m_audio->addItem(Ui("无音频"), "none"); m_audio->addItem(Ui("麦克风"), "mic");
	m_audio->addItem(Ui("系统播放声"), "system"); m_audio->addItem(Ui("麦克风 + 系统声"), "mix");
	audioForm->addRow(Ui("音频来源"), m_audio);
	m_settingsMic = RecorderUi::Toggle("mic", Ui("麦克风"), this);
	audioForm->addRow(m_settingsMic);
	m_source = new QComboBox(this);
	m_source->addItem(Ui("全屏 / 选择屏幕"), "screen"); m_source->addItem(Ui("框选区域"), "region"); m_source->addItem(Ui("固定窗口（独立表面）"), "window");
	m_source->addItem(Ui("仅摄像头"), "camera"); m_source->addItem(Ui("图片"), "image");
	form->addRow(Ui("画面来源"), m_source);
	m_monitor = new QComboBox(this);
	refreshMonitors();
	form->addRow(Ui("屏幕"), m_monitor);
	m_encoder = new QComboBox(this); m_encoder->addItem(Ui("自动：优先 GPU"), "auto"); m_encoder->addItem("CPU · x264", "libx264"); m_encoder->addItem("NVIDIA · NVENC", "h264_nvenc"); m_encoder->addItem("AMD · AMF", "h264_amf"); m_encoder->addItem("Intel · QSV", "h264_qsv");
	videoForm->addRow(Ui("视频编码"), m_encoder);videoForm->addRow(colors);
	m_separateAudio = new QCheckBox(Ui("系统声 / 麦克风独立音轨"), this); audioForm->addRow("", m_separateAudio);
	m_denoise = new QCheckBox(Ui("音频降噪"), this); audioForm->addRow("", m_denoise);
	auto selectRegion = new QPushButton(Ui("框选录制区域"), this);
	form->addRow(Ui("区域"), selectRegion);
	m_regionLabel = new QLabel(Ui("尚未框选"), this); m_regionLabel->setWordWrap(true);
	form->addRow("", m_regionLabel);
	connect(selectRegion, &QPushButton::clicked, this, &KRRMainWindow::selectRecordingRegion);
	m_window = new QComboBox(this); m_window->setMinimumContentsLength(12); m_window->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	auto refresh = new QPushButton(Ui("刷新窗口列表"), this);
	form->addRow(Ui("窗口"), m_window); form->addRow("", refresh);
	connect(refresh, &QPushButton::clicked, this, &KRRMainWindow::refreshWindows);
	m_countdown = new QComboBox(this); m_countdown->addItem(Ui("立即开始"), 0); m_countdown->addItem(Ui("3 秒"), 3); m_countdown->addItem(Ui("5 秒"), 5);
	saveForm->addRow(Ui("录制倒计时"), m_countdown);
	m_cursor = new QCheckBox(Ui("显示鼠标指针"), this); effectForm->addRow("", m_cursor);
	m_clicks = new QCheckBox(Ui("按下鼠标时高亮"), this); effectForm->addRow("", m_clicks);
	m_radius = new QSpinBox(this); m_radius->setRange(5, 100); effectForm->addRow(Ui("高亮半径 px"), m_radius);
	auto color = new QPushButton(Ui("选择高亮颜色"), this); effectForm->addRow("", color);
	connect(color, &QPushButton::clicked, this, [this]
	{
		QColor selected = QColorDialog::getColor(QColor(m_clickColor), this);
		if (selected.isValid()) m_clickColor = selected.name();
	});
	m_camera = new QComboBox(this); for (auto value : KRRCameraCapture::Devices()) { auto item = value.toObject(); m_camera->addItem(item["name"].toString(), item["index"].toInt()); }
	if (!m_camera->count()) m_camera->addItem(Ui("未检测到摄像头"), -1);
	cameraForm->addRow(Ui("摄像头"), m_camera);
	auto checkCamera = new QPushButton(Ui("测试摄像头"), this); cameraForm->addRow("", checkCamera);
	checkCamera->setObjectName("testCamera");
	connect(checkCamera, &QPushButton::clicked, this, [this]
	{
		if (m_active) { showError(Ui("请在录制结束后测试摄像头")); return; }
		KRRCameraCapture capture; QString error;
		if (!capture.start(m_camera->currentData().toInt(), error)) { showError(error); return; }
		QDialog dialog(this); dialog.setWindowTitle(Ui("摄像头实时预览")); dialog.setWindowFlag(Qt::WindowContextHelpButtonHint, false);
		dialog.setObjectName("cameraTestPreview");
		auto layout = new QVBoxLayout(&dialog); auto preview = new QLabel(&dialog); preview->setFixedSize(RecorderUi::Px(560), RecorderUi::Px(340)); preview->setAlignment(Qt::AlignCenter); preview->setStyleSheet("background:#202a3a;border-radius:8px;"); layout->addWidget(preview);
		auto status = new QLabel(&dialog); status->setProperty("muted", true); layout->addWidget(status);
		status->setObjectName("cameraTestStatus");
		auto close = new QDialogButtonBox(QDialogButtonBox::Close, &dialog); close->button(QDialogButtonBox::Close)->setText(Ui("关闭预览")); layout->addWidget(close); connect(close, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
		QTimer refresh; auto update = [&] { auto image = capture.frame(error); if (!error.isEmpty()) { status->setText(UiDiagnostic(error)); refresh.stop(); return; } if (m_cameraMirror->isChecked()) image = image.mirrored(true, false); preview->setPixmap(QPixmap::fromImage(image.scaled(preview->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation))); status->setText(Ui("实时预览 · 已接收 %1 帧 · %2×%3 · 关闭后释放摄像头").arg(capture.frameSequence()).arg(image.width()).arg(image.height())); };
		connect(&refresh, &QTimer::timeout, &dialog, update); update(); refresh.start(66); dialog.exec(); refresh.stop(); capture.stop();
	});
	m_cameraEnabled = RecorderUi::Toggle("camera", Ui("摄像头画中画"), this);
	cameraForm->addRow(m_cameraEnabled);
	m_cameraMirror = new QCheckBox(Ui("水平镜像"), this); m_cameraMirror->setChecked(true); cameraForm->addRow("", m_cameraMirror);
	m_cameraTrack = new QCheckBox(Ui("另存独立摄像头视频"), this); cameraForm->addRow("", m_cameraTrack);
	m_showCameraPreview = new QCheckBox(Ui("录制时显示摄像头悬浮预览"), this); m_showCameraPreview->setChecked(true); cameraForm->addRow("", m_showCameraPreview);
	m_cameraShape = new QComboBox(this); m_cameraShape->addItem(Ui("圆角"), "rounded"); m_cameraShape->addItem(Ui("圆形"), "circle"); m_cameraShape->addItem(Ui("矩形"), "rectangle"); cameraForm->addRow(Ui("样式"), m_cameraShape);
	auto percent = [this, cameraForm](const QString& label, int value) { auto number = new QSpinBox(this); number->setRange(0, 100); number->setSuffix(" %"); number->setValue(value); cameraForm->addRow(label, number); return number; };
	m_cameraX = percent(Ui("水平位置"), 75); m_cameraY = percent(Ui("垂直位置"), 65); m_cameraW = percent(Ui("宽度"), 22); m_cameraH = percent(Ui("高度"), 30); m_cameraW->setMinimum(5); m_cameraH->setMinimum(5);
	auto border = new QPushButton(Ui("选择边框颜色"), this); cameraForm->addRow("", border); connect(border, &QPushButton::clicked, this, [this] { auto color = QColorDialog::getColor(QColor(m_cameraBorder), this); if (color.isValid()) { m_cameraBorder = color.name(); sendScene(); } });
	m_image = new QLineEdit(this); form->addRow(Ui("图片源"), m_image); auto chooseImage = new QPushButton(Ui("选择图片"), this); form->addRow("", chooseImage);
	connect(chooseImage, &QPushButton::clicked, this, [this] { auto path = QFileDialog::getOpenFileName(this, Ui("选择图片"), {}, Ui("图片 (*.png *.jpg *.bmp)")); if (!path.isEmpty()) { m_image->setText(path); m_source->setCurrentIndex(4); sendScene(); } });

	m_editDialog = new QDialog(this); m_editDialog->setWindowTitle(Ui("批量合并与区间拆分")); m_editDialog->resize(RecorderUi::Px(820), RecorderUi::Px(560)); auto editRoot = new QVBoxLayout(m_editDialog);
	auto assets = new QWidget(m_editDialog); auto assetLayout = new QVBoxLayout(assets);
	assetLayout->addWidget(new QLabel(Ui("合并片段 · 拖动调整顺序"), this)); m_clips = new QListWidget(this); m_clips->setDragDropMode(QAbstractItemView::InternalMove); assetLayout->addWidget(m_clips, 1);
	auto addFiles = new QPushButton(Ui("添加视频"), this); auto removeClip = new QPushButton(Ui("移出所选片段"), this); assetLayout->addWidget(addFiles); assetLayout->addWidget(removeClip);
	connect(addFiles, &QPushButton::clicked, this, [this] { for (const auto& path : QFileDialog::getOpenFileNames(this, Ui("添加片段"), m_directory->text(), Ui("视频 (*.mp4 *.mov *.mkv *.webm *.avi)"))) { auto item = new QListWidgetItem(QFileInfo(path).fileName(), m_clips); item->setData(Qt::UserRole, path); item->setToolTip(path); } });
	connect(removeClip, &QPushButton::clicked, this, [this] { delete m_clips->takeItem(m_clips->currentRow()); });
	editRoot->addWidget(assets, 1);
	auto recording = new QFrame(this);
	recording->setObjectName("recordingCard");
	auto recordingLayout = new QVBoxLayout(recording);
	recordingLayout->setContentsMargins(8, 6, 8, 8);
	recordingLayout->setSpacing(4);

	auto statusRow = new QHBoxLayout;
	m_state = new QLabel(Ui("准备就绪"), recording);
	m_time = new QLabel("00:00:00", recording);
	m_state->setObjectName("stateLabel");
	m_state->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
	m_time->setObjectName("timeLabel");
	auto light = new QLabel(QStringLiteral("●"), recording);
	light->setObjectName("recordingLight");
	statusRow->addWidget(light);
	statusRow->addWidget(m_time);
	statusRow->addWidget(m_state, 1, Qt::AlignRight);
	recordingLayout->addLayout(statusRow);
	m_start = new QPushButton(RecorderUi::Icon("record"), Ui("开始录制"), recording);
	m_start->setProperty("role", "startRecording");
	m_stop = new QPushButton(RecorderUi::Icon("stop"), Ui("停止并保存"), recording);
	m_stop->setProperty("role", "record");
	auto recordActions = new QHBoxLayout;
	recordActions->setSpacing(0);
	recordActions->addWidget(m_start, 1);
	recordActions->addWidget(m_stop, 1);
	auto quickRecord = new QToolButton(recording);
	quickRecord->setObjectName("studioQuickRecord");
	quickRecord->setText(QStringLiteral("⌄"));
	quickRecord->setAccessibleName(Ui("录制快捷选项"));
	quickRecord->setPopupMode(QToolButton::InstantPopup);
	auto recordMenu = new QMenu(quickRecord);
	auto delayMenu = recordMenu->addMenu(Ui("倒计时"));
	auto delayActions = new QActionGroup(delayMenu);
	delayActions->setExclusive(true);
	for (int i = 0; i < m_countdown->count(); ++i)
	{
		auto action = delayMenu->addAction(m_countdown->itemText(i));
		delayActions->addAction(action);
		action->setCheckable(true);
		action->setChecked(i == m_countdown->currentIndex());
		connect(action, &QAction::triggered, this, [this, i] { m_countdown->setCurrentIndex(i); saveSettings(); });
		connect(m_countdown, qOverload<int>(&QComboBox::currentIndexChanged), action,
			[action, i](int index) { action->setChecked(index == i); });
	}
	connect(recordMenu->addAction(Ui("预约录制")), &QAction::triggered, this, &KRRMainWindow::scheduleTask);
	quickRecord->setMenu(recordMenu);
	recordActions->addWidget(quickRecord);
	recordingLayout->addLayout(recordActions);
	m_stop->hide();
	auto buttons = new QHBoxLayout;
	m_pause = new QPushButton(RecorderUi::Icon("pause"), Ui("暂停"), recording);
	auto screenshot = new QPushButton(RecorderUi::Icon("camera"), Ui("截图"), recording);
	m_addMarker = new QPushButton(RecorderUi::Icon("flag"), Ui("打点"), recording);
	m_addMarker->setObjectName("addRecordingMarker");
	for (auto button : {m_pause, screenshot, m_addMarker}) buttons->addWidget(button);
	recordingLayout->addLayout(buttons);
	m_checkButton = new QPushButton(Ui("录前检查"), this);
	m_checkButton->hide();
	m_recoverButton = new QPushButton(Ui("查看可恢复素材"), recording);
	m_recoverButton->setObjectName("recoverLastRecording");
	m_recoverButton->hide();
	recordingLayout->addWidget(m_recoverButton);
	recordingLayout->addStretch(1);
	connect(m_addMarker, &QPushButton::clicked, this, &KRRMainWindow::addRecordingMarker);
	connect(m_recoverButton, &QPushButton::clicked, this, &KRRMainWindow::showRecoveries);
	m_metrics = new QLabel("", this); statusBar()->addPermanentWidget(m_metrics);

	connect(m_start, &QPushButton::clicked, this, &KRRMainWindow::beginCountdown);
	connect(m_checkButton, &QPushButton::clicked, this, &KRRMainWindow::showRecordingCheck);
	connect(m_stop, &QPushButton::clicked, this, &KRRMainWindow::stopRecording);
	connect(m_pause, &QPushButton::clicked, this, [this]
	{
		QJsonObject response; QString error;
		if (!SendControl({{"command", m_paused ? "resume" : "pause"}}, response, error, 2000)) showError(error);
		updateStatus();
	});
	connect(screenshot, &QPushButton::clicked, this, &KRRMainWindow::takeScreenshot);

	auto preview = new QFrame(this); preview->setObjectName("previewCard");
	auto previewLayout = new QVBoxLayout(preview);
	previewLayout->setContentsMargins(6, 6, 6, 6);
	m_reviewName = new QLabel(Ui("视频回看"), preview);
	m_reviewName->setProperty("heading", true);
	m_reviewName->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
	auto reviewHeading = new QHBoxLayout;
	reviewHeading->addWidget(m_reviewName, 1);
	auto editCurrent = new QPushButton(RecorderUi::Icon("scissors"), Ui("进入剪辑"), preview);
	connect(editCurrent, &QPushButton::clicked, this, [this] { editFile(m_preview); });
	reviewHeading->addWidget(editCurrent);
	previewLayout->addLayout(reviewHeading);
	m_video = new KRRPreviewVideo(this); m_video->setMinimumSize(150, 120);
	m_video->setObjectName("inlinePlaybackVideo");
	m_player = new QMediaPlayer(this); m_video->attach(m_player);
	m_compatiblePreview = std::make_unique<KRRCompatiblePreview>();
	connect(m_compatiblePreview.get(), &KRRCompatiblePreview::ready, this, [this](const QString& file)
	{
		m_message->setText(Ui("兼容预览已就绪。"));
		m_player->setMedia(QUrl::fromLocalFile(file));
		m_player->play();
	});
	connect(m_compatiblePreview.get(), &KRRCompatiblePreview::failed, this, [this](const QString& error)
	{
		m_message->setText(error);
	});
	m_fullscreen = new KRRPlayerWindow(m_player, m_video, this);
	connect(m_fullscreen, &KRRPlayerWindow::recordingRequested, this, [this](const QString& path)
	{
		if (!m_active && path != m_preview && QFileInfo(path).isFile()) openPreview(path);
	});
	m_previewReader = std::make_unique<KRRPreviewReader>();

	m_previewStack = new QStackedWidget(this);
	auto empty = new QWidget(this); auto emptyLayout = new QVBoxLayout(empty); emptyLayout->setAlignment(Qt::AlignCenter); emptyLayout->setSpacing(RecorderUi::Px(14));
	auto emptyTitle = new QLabel(Ui("选择录像或打开本地视频"), empty);
	emptyTitle->setProperty("muted", true);
	emptyLayout->addWidget(emptyTitle, 0, Qt::AlignHCenter);
	auto emptyOpen = new QPushButton(Ui("打开已有视频"), this); emptyOpen->setFixedWidth(RecorderUi::Px(156)); emptyLayout->addWidget(emptyOpen, 0, Qt::AlignHCenter);
	connect(emptyOpen, &QPushButton::clicked, this, [this] { auto path = QFileDialog::getOpenFileName(this, Ui("打开视频"), m_directory->text(), Ui("视频 (*.mp4 *.mov *.mkv *.webm *.avi *.flv)")); if (!path.isEmpty()) openPreview(path); });
	m_previewStack->addWidget(empty); m_previewStack->addWidget(m_video); previewLayout->addWidget(m_previewStack, 1);
	auto live = new QWidget(this);
	auto liveLayout = new QVBoxLayout(live);
	liveLayout->setContentsMargins(0, 0, 0, 0);
	m_liveFrame = new KRRLiveCanvas(live);
	m_liveFrame->setText(Ui("正在等待录制画面…"));
	m_liveFrame->setObjectName("liveRecordingFrame");
	m_liveFrame->setAlignment(Qt::AlignCenter);
	m_liveFrame->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
	m_liveFrame->setStyleSheet("background:#101216;color:#e4e7ed;");
	m_floatBar=new KRRRecordingToolbar(this);
	m_floatBar->setSwitchToMinimal(true);
	connect(m_floatBar, &KRRRecordingToolbar::hideRequested, this, [this] { m_floatButton->setChecked(false); });
	connect(m_floatButton, &QPushButton::toggled, this, [this](bool enabled) {
		if (!enabled) m_floatBar->hide();
		else
		{
			QString error;
			if (!m_floatBar->present(error, m_active)) m_message->setText(error);
		}
	});
	connect(m_floatBar,&KRRRecordingToolbar::pauseRequested,m_pause,&QPushButton::click);
	connect(m_floatBar,&KRRRecordingToolbar::stopRequested,m_stop,&QPushButton::click);
	connect(m_floatBar,&KRRRecordingToolbar::startRequested,m_start,&QPushButton::click);
	connect(m_floatBar,&KRRRecordingToolbar::annotationRequested,this,&KRRMainWindow::showAnnotation);
	connect(m_floatBar,&KRRRecordingToolbar::screenshotRequested,this,&KRRMainWindow::takeScreenshot);
	connect(m_floatBar,&KRRRecordingToolbar::markerRequested,this,&KRRMainWindow::addRecordingMarker);
	connect(m_floatBar, &KRRRecordingToolbar::returnRequested, this, &KRRMainWindow::switchRecordingMode);
	connect(m_liveFrame,&KRRLiveCanvas::placementChanged,this,[this](const QRectF &rect){
		QSignalBlocker bx(m_cameraX),by(m_cameraY),bw(m_cameraW),bh(m_cameraH);
		int w=qBound(5,qRound(rect.width()*100),100),h=qBound(5,qRound(rect.height()*100),100);
		m_cameraX->setValue(qBound(0,qRound(rect.x()*100),100-w));m_cameraY->setValue(qBound(0,qRound(rect.y()*100),100-h));m_cameraW->setValue(w);m_cameraH->setValue(h);m_effectTimer.start();
	});
	m_liveStatus = new QLabel(live);
	m_liveStatus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
	m_liveStatus->setProperty("muted", true);
	m_cameraPreview = new QDialog(nullptr, Qt::Window | Qt::WindowStaysOnTopHint | Qt::WindowTitleHint | Qt::WindowSystemMenuHint | Qt::WindowCloseButtonHint); m_cameraPreview->setWindowTitle(Ui("摄像头 · 实时监看")); m_cameraPreview->setWindowFlag(Qt::WindowContextHelpButtonHint,false); m_cameraPreview->resize(RecorderUi::Px(300),RecorderUi::Px(265));
	auto cameraLayout = new QVBoxLayout(m_cameraPreview); m_cameraFrame = new QLabel(Ui("等待摄像头画面…"),m_cameraPreview); m_cameraFrame->setAlignment(Qt::AlignCenter); m_cameraFrame->setMinimumSize(160,100); m_cameraFrame->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Ignored); m_cameraFrame->setStyleSheet("background:#152133;color:white;border-radius:8px;"); cameraLayout->addWidget(m_cameraFrame,1); m_cameraStatus = new QLabel(m_cameraPreview); m_cameraStatus->setWordWrap(true); cameraLayout->addWidget(m_cameraStatus);
	auto hideCamera = new QPushButton(Ui("隐藏监看"),m_cameraPreview); cameraLayout->addWidget(hideCamera); connect(hideCamera,&QPushButton::clicked,this,[this]{m_showCameraPreview->setChecked(false);});
	connect(m_showCameraPreview,&QCheckBox::toggled,this,[this](bool show){if(!show)m_cameraPreview->hide();});
	connect(m_cameraPreview,&QDialog::finished,this,[this]{m_showCameraPreview->setChecked(false);});
	m_previewSocket = new QLocalSocket(this); m_previewTimeout.setSingleShot(true);
	connect(&m_previewTimeout,&QTimer::timeout,m_previewSocket,&QLocalSocket::abort);
	connect(m_previewSocket,&QLocalSocket::connected,this,[this]{m_previewSocket->write("{\"command\":\"preview\"}\n");});
	connect(m_previewSocket,&QLocalSocket::readyRead,this,[this]{
		m_previewBytes += m_previewSocket->readAll();
		if(m_previewBytes.size()>8000000){m_previewSocket->abort();return;}
		if(!m_previewBytes.contains('\n'))return;
		m_previewTimeout.stop(); auto object=QJsonDocument::fromJson(m_previewBytes).object(); m_previewSocket->disconnectFromServer();
		if(!m_active || !object["active"].toBool())return;
		QImage image=QImage::fromData(QByteArray::fromBase64(object["frame"].toString().toLatin1()),"JPEG");
		QImage camera=QImage::fromData(QByteArray::fromBase64(object["camera"].toString().toLatin1()),"JPEG");
		displayLivePreview(image, camera, static_cast<qint64>(object["sequence"].toDouble()));
	});
	connect(&m_previewTimer,&QTimer::timeout,this,&KRRMainWindow::requestLivePreview);m_previewTimer.setTimerType(Qt::PreciseTimer);m_previewTimer.start(33);

	m_seek = new KRRPlaybackSlider(this); m_seek->setObjectName("inlinePlaybackSeek");m_seek->setToolTip(Ui("点击进度条直接跳转，也可拖动播放点")); m_seek->setRange(0, 0); static_cast<KRRPlaybackSlider*>(m_seek)->setHandleOnHover(true); previewLayout->addWidget(m_seek);
	m_playbackControls = new QWidget(this); auto playback = new QHBoxLayout(m_playbackControls); playback->setContentsMargins(0, 0, 0, 0);
	auto open = new QPushButton(Ui("打开视频"), this); open->hide();
	auto play = new QPushButton(RecorderUi::Icon("play"), {}, this); play->setToolTip(Ui("播放 / 暂停"));
	auto full = new QPushButton(RecorderUi::Icon("full"), {}, this); full->setToolTip(Ui("无边框全屏预览")); full->setObjectName("expandPreviewButton");
	auto volume = new KRRPlaybackSlider(this); volume->setRange(0, 100); volume->setValue(70); volume->setMaximumWidth(110);
	auto speed = new QComboBox(this); speed->addItems({"0.5x", "0.75x", "1.0x", "1.25x", "1.5x", "2.0x"}); speed->setCurrentIndex(2);
	auto previous = new QPushButton(RecorderUi::Icon("previous"), {}, this);
	auto next = new QPushButton(RecorderUi::Icon("next"), {}, this);
	previous->setToolTip(Ui("上一节")); next->setToolTip(Ui("下一节"));
	auto jump = [this](bool forward)
	{
		const int at = m_player->position();
		int target = forward ? m_player->duration() : 0;
		for (int i = 1; i < m_playbackMarkers->count(); ++i)
		{
			const int mark = m_playbackMarkers->itemData(i).toInt();
			if (forward && mark > at + 250) target = qMin(mark, target);
			if (!forward && mark < at - 250) target = qMax(mark, target);
		}
		if (m_player->duration() > 0) m_player->setPosition(qMin<qint64>(target, m_player->duration() - 1));
	};
	connect(previous, &QPushButton::clicked, this, [jump] { jump(false); });
	connect(next, &QPushButton::clicked, this, [jump] { jump(true); });
	playback->addWidget(previous); playback->addWidget(play); playback->addWidget(next);
	m_reviewTime = new QLabel("00:00:00 / 00:00:00", this);
	playback->addWidget(m_reviewTime);
	m_deletePreview = new QPushButton(Ui("删除"), this);
	m_deletePreview->setObjectName("deleteRecentRecording");
	m_deletePreview->setToolTip(Ui("把刚录制的视频移到回收站"));
	m_deletePreview->setEnabled(false);
	playback->addWidget(m_deletePreview); playback->addStretch();
	m_playbackMarkers = new QComboBox(this);
	m_playbackMarkers->setObjectName("recordingMarkers");
	m_playbackMarkers->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	m_playbackMarkers->setMinimumContentsLength(11);
	m_playbackMarkers->hide();
	m_renameMarker = new QPushButton(Ui("命名标记"), this);
	m_renameMarker->setObjectName("renameRecordingMarker");
	m_renameMarker->hide();
	connect(m_renameMarker, &QPushButton::clicked, this, &KRRMainWindow::renamePlaybackMarker);
	connect(m_playbackMarkers, qOverload<int>(&QComboBox::currentIndexChanged), m_renameMarker,
			[this](int index) { m_renameMarker->setEnabled(index > 0); });
	connect(m_playbackMarkers, qOverload<int>(&QComboBox::activated), this, [this](int index)
	{
		if (index > 0 && m_player->duration() > 0)
		{
			m_player->setPosition(qMin(m_player->duration() - 1, m_playbackMarkers->itemData(index).toLongLong()));
			const auto caption = m_playbackMarkers->itemText(index);
			statusBar()->showMessage(caption, 2000);
		}
	});
	m_trialTracks = new QComboBox(this);
	m_trialTracks->setObjectName("trialAudioTracks");
	m_trialTracks->hide();
	m_trialReplay = new QPushButton(Ui("试听此音轨"), this);
	m_trialReplay->setObjectName("replayTrialTrack");
	m_trialReplay->hide();
	playback->addWidget(m_trialTracks);
	playback->addWidget(m_trialReplay);
	connect(m_trialReplay, &QPushButton::clicked, this, &KRRMainWindow::playTrialTrack);
	HideConsoleProcess(m_trialPlayback);
	connect(&m_trialPlayback, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
		[this](int code, QProcess::ExitStatus status)
	{
		const auto wav = m_trialPlayback.property("output").toString();
		if (m_preview != m_trialPath) return;
		m_trialReplay->setEnabled(true);
		if (code != 0 || status != QProcess::NormalExit || !QFileInfo(wav).isFile())
		{
			m_message->setText(Ui("无法试听所选试录音轨。"));
			return;
		}
		m_player->setMedia(QUrl::fromLocalFile(wav));
		m_player->play();
	});
	connect(&m_trialPlayback, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error)
	{
		if (error != QProcess::FailedToStart || m_preview != m_trialPath) return;
		m_trialReplay->setEnabled(true);
		m_message->setText(Ui("无法试听所选试录音轨。"));
	});
	connect(m_deletePreview, &QPushButton::clicked, this, &KRRMainWindow::deletePreview);
	volume->setToolTip(Ui("音量")); playback->addWidget(volume); playback->addWidget(speed); playback->addWidget(full);
	previewLayout->addWidget(m_playbackControls); m_playbackControls->hide(); m_seek->hide();
	m_recent = new QListWidget(this);
	m_recent->setObjectName("studioLibrary");
	connect(m_recent, &QListWidget::itemClicked, this, [this](QListWidgetItem* item)
	{
		const auto file = item->data(Qt::UserRole).toString();
		if (file != m_preview) openPreview(file);
	});
	connect(open, &QPushButton::clicked, this, [this]
	{
		QString path = QFileDialog::getOpenFileName(this, Ui("打开视频"), m_directory->text(), Ui("视频 (*.mp4 *.mov *.mkv *.avi *.flv)"));
		if (!path.isEmpty()) openPreview(path);
	});
	connect(play, &QPushButton::clicked, this, [this]
	{
		if (m_player->state() == QMediaPlayer::PlayingState) m_player->pause();
		else
		{
			if (m_player->duration() > 0 && m_player->position() >= m_player->duration() - 50)
				m_player->setPosition(0);
			m_player->play();
		}
	});
	connect(full, &QPushButton::clicked, this, [this]
	{
		m_fullscreen->enter(true);
	});
	connect(volume, &QSlider::valueChanged, m_player, &QMediaPlayer::setVolume);
	m_player->setVolume(volume->value());
	connect(speed, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, speed]
	{
		ChangePreviewRate(m_player, speed->currentText().chopped(1).toDouble());
	});
	BindPlaybackSeek(m_seek,m_player);
	connect(m_player, &QMediaPlayer::stateChanged, play, [play](QMediaPlayer::State state)
	{
		play->setIcon(RecorderUi::Icon(state == QMediaPlayer::PlayingState ? "pause" : "play"));
	});
	connect(m_player, &QMediaPlayer::positionChanged, this, [this](qint64 position)
	{
		if (!m_seek->isSliderDown()) m_seek->setValue(static_cast<int>(position));
		m_reviewTime->setText(TimeText(position / 1000.) + " / " + TimeText(m_player->duration() / 1000.));
	});
	connect(m_player, &QMediaPlayer::durationChanged, this, [this](qint64 duration)
	{
		m_seek->setMaximum(static_cast<int>(qMin<qint64>(duration, INT_MAX)));
		m_reviewTime->setText(TimeText(m_player->position() / 1000.) + " / " + TimeText(duration / 1000.));
		if (duration > 0 && m_reviewPendingSeek >= 0)
		{
			const auto target = m_reviewPendingSeek;
			m_reviewPendingSeek = -1;
			QTimer::singleShot(0, this, [this, target, file = m_preview]
			{
				if (file == m_preview && m_player->duration() > 0)
					m_player->setPosition(qMin(m_player->duration() - 1, target));
			});
		}
		const auto playingFile = m_player->media().request().url().toLocalFile();
		const auto compatibleFile = m_compatiblePreview->file();
		if (playingFile == m_preview || (!compatibleFile.isEmpty() && playingFile == compatibleFile))
			m_trimEnd->setValue(duration / 1000.0);
		refreshMediaActions();
	});
	connect(m_player, &QMediaPlayer::videoAvailableChanged, this, &KRRMainWindow::refreshMediaActions);
	connect(m_player, &QMediaPlayer::mediaStatusChanged, this, &KRRMainWindow::refreshMediaActions);
	connect(m_player, qOverload<QMediaPlayer::Error>(&QMediaPlayer::error), this, [this]
	{
		if (m_player->error() == QMediaPlayer::NoError || m_compatiblePreview->isPreparing()) return;
		const auto compatibleFile = m_compatiblePreview->file();
		if (!m_active && !compatibleFile.isEmpty() && m_player->media().request().url().toLocalFile() == compatibleFile &&
			m_compatiblePreview->retryWithEncoding())
			m_message->setText(Ui("正在准备兼容预览…"));
		else
			m_message->setText(Ui("播放器：") + m_player->errorString());
	});
	auto editing = new QGroupBox(Ui("时间轴选区"), this);
	auto editingLayout = new QHBoxLayout(editing);
	m_trimBegin = new QDoubleSpinBox(this); m_trimEnd = new QDoubleSpinBox(this);
	for (auto number : {m_trimBegin, m_trimEnd}) { number->setRange(0, 864000); number->setDecimals(2); number->setSuffix(Ui(" 秒")); }
	auto trim = new QPushButton(Ui("裁剪导出"), this); auto split = new QPushButton(Ui("按区间拆分"), this); auto merge = new QPushButton(Ui("多段合并"), this);
	editingLayout->addWidget(m_trimBegin); editingLayout->addWidget(new QLabel(Ui("至"), this)); editingLayout->addWidget(m_trimEnd);
	editingLayout->addWidget(trim); editingLayout->addWidget(split); editingLayout->addWidget(merge);
	editRoot->insertWidget(0, editing);
	m_exportDialog = new QDialog(this); m_exportDialog->setWindowTitle(Ui("导出视频或音频")); m_exportDialog->resize(RecorderUi::Px(660), RecorderUi::Px(450)); auto exportLayout = new QVBoxLayout(m_exportDialog);
	auto exportRow = new QFormLayout; exportRow->setVerticalSpacing(18);
	m_exportFormat = new QComboBox(this); m_exportFormat->addItems({"mp4", "mov", "mkv", "webm", "gif", "m4a", "mp3"});
	m_subtitles = new QLineEdit(this); m_subtitles->setPlaceholderText(Ui("可选：嵌入 SRT 字幕"));
	auto subtitleFile = new QPushButton(Ui("选择字幕"), this); auto exportFile = new QPushButton(Ui("导出视频"), this);
	exportRow->addRow(Ui("文件格式"), m_exportFormat); exportRow->addRow(Ui("字幕文件"), m_subtitles); exportRow->addRow("", subtitleFile); exportLayout->addLayout(exportRow); exportLayout->addStretch(); exportLayout->addWidget(exportFile);
	m_copyStreams = new QCheckBox(Ui("快速转封装：完整视频，不重新编码"), m_exportDialog); exportRow->addRow("", m_copyStreams);
	m_exportEncoder = new QComboBox(m_exportDialog); for (int i = 0; i < m_encoder->count(); ++i) m_exportEncoder->addItem(m_encoder->itemText(i), m_encoder->itemData(i)); exportRow->insertRow(1, Ui("视频编码"), m_exportEncoder);
	m_exportDenoise = new QCheckBox(Ui("清理背景噪声并统一音量"), m_exportDialog); exportRow->addRow("", m_exportDenoise);
	connect(subtitleFile, &QPushButton::clicked, this, [this] { auto path = QFileDialog::getOpenFileName(this, Ui("嵌入字幕"), m_directory->text(), "SRT (*.srt)"); if (!path.isEmpty()) m_subtitles->setText(path); });
	connect(exportFile, &QPushButton::clicked, this, &KRRMainWindow::exportVideo);

	auto addCurrent = new QPushButton(Ui("当前视频加入合并列表"), m_editDialog);
	connect(addCurrent, &QPushButton::clicked, this, [this]
	{
		if (m_preview.isEmpty()) return;
		auto item = new QListWidgetItem(QFileInfo(m_preview).fileName(), m_clips);
		item->setData(Qt::UserRole, m_preview);
	});
	editRoot->insertWidget(1, addCurrent);
	connect(trim, &QPushButton::clicked, this, [this]
	{
		if (m_preview.isEmpty()) { showError(Ui("请先打开视频")); return; }
		QString path = QFileDialog::getSaveFileName(this, Ui("导出裁剪视频"), m_directory->text() + "/clip-" + Stamp() + ".mp4", "MP4 (*.mp4)");
		if (!path.isEmpty() && !m_editor.trim(m_preview, path, m_trimBegin->value(), m_trimEnd->value())) showError(Ui("请等待当前导出完成"));
		else if (!path.isEmpty()) { m_editorInputs = QStringList{m_preview}; m_message->setText(Ui("正在导出裁剪视频…")); }
	});
	connect(split, &QPushButton::clicked, this, [this]
	{
		if (m_preview.isEmpty()) { showError(Ui("请先打开视频")); return; }
		bool accepted = false;
		QString ranges = QInputDialog::getText(this, Ui("拆分区间"), Ui("用秒填写，例如 0-5;5-10"), QLineEdit::Normal, "0-5;5-10", &accepted);
		if (!accepted) return;
		QList<QPair<double, double>> intervals;
		for (const QString& range : ranges.split(';', Qt::SkipEmptyParts))
		{
			QStringList parts = range.split('-'); bool beginOk = false, endOk = false;
			double begin = parts.value(0).toDouble(&beginOk), end = parts.value(1).toDouble(&endOk);
			if (parts.size() != 2 || !beginOk || !endOk) { showError(Ui("区间格式无效")); return; }
			intervals.append({begin, end});
		}
		QString directory = QFileDialog::getExistingDirectory(this, Ui("选择拆分文件保存目录"), m_directory->text());
		if (!directory.isEmpty() && !m_editor.split(m_preview, directory, intervals)) showError(Ui("请等待当前导出完成"));
		else if (!directory.isEmpty()) { m_editorInputs = QStringList{m_preview}; m_message->setText(Ui("正在拆分视频…")); }
	});
	connect(merge, &QPushButton::clicked, this, [this]
	{
		QStringList inputs;
		for (int i = 0; i < m_clips->count(); ++i) inputs << m_clips->item(i)->data(Qt::UserRole).toString();
		if (inputs.size() < 2) inputs = QFileDialog::getOpenFileNames(this, Ui("按文件选择顺序合并"), m_directory->text(), Ui("视频 (*.mp4 *.mov *.mkv *.avi *.flv *.webm)"));
		if (inputs.size() < 2) return;
		QString path = QFileDialog::getSaveFileName(this, Ui("导出合并视频"), m_directory->text() + "/merged-" + Stamp() + ".mp4", "MP4 (*.mp4)");
		KRRExportOptions options; options.encoder = m_encoder->currentData().toString();
		if (!path.isEmpty() && !m_editor.merge(inputs, path, options)) showError(Ui("请等待当前导出完成"));
		else if (!path.isEmpty()) { m_editorInputs = inputs; m_message->setText(Ui("正在合并视频…")); }
	});
	connect(&m_editor, &KRRMediaEditor::busyChanged, this, &KRRMainWindow::refreshMediaActions);
	connect(&m_editor, &KRRMediaEditor::completed, this, [this](const QString& output, const QString& error)
	{
		m_editorInputs.clear();
		if (!error.isEmpty()) { showError(error); return; }
		m_message->setText(Ui("导出完成：") + output);
		if (QFileInfo(output).suffix().toLower() == "srt") { m_subtitles->setText(output); m_message->setText(Ui("字幕已生成，可校对后嵌入导出：") + output); }
		else if (QFileInfo(output).isFile()) openPreview(output);
	});
	m_message = new QLabel(Ui("准备就绪"), this);
	m_message->setProperty("muted", true);
	m_message->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
	buildStudio(root, live, preview, recording);
	root->setContentsMargins(4, 4, 4, 0);
	root->setSpacing(4);
	setCentralWidget(central);
	for (auto dialog : {m_settingsDialog, m_editDialog, m_exportDialog})
		dialog->setWindowFlag(Qt::WindowContextHelpButtonHint, false);
	if (!QFileInfo::exists(RecorderUiSettingsPath()))
	{
		QSettings legacy(QCoreApplication::applicationDirPath() + "/settings.ini", QSettings::IniFormat); legacy.setIniCodec("UTF-8");
		QSettings migrated(RecorderUiSettingsPath(), QSettings::IniFormat); migrated.setIniCodec("UTF-8");
		for (const auto& key : legacy.allKeys()) migrated.setValue(key, legacy.value(key));
	}
	QSettings settings(RecorderUiSettingsPath(), QSettings::IniFormat);
	settings.setIniCodec("UTF-8");
	const auto defaultFolder = DefaultRecordingFolder();
	m_directory->setText(settings.value("directory", defaultFolder).toString());
	m_defaultDirectory = settings.value("defaultDirectory", m_directory->text().isEmpty() ||
		QDir::cleanPath(m_directory->text()).compare(QDir::cleanPath(defaultFolder), Qt::CaseInsensitive) == 0).toBool();
	const auto storedResolution = settings.value("resolution", "auto").toString();
	// Versions before 2.2.4 saved the Simplified Chinese label instead of the auto ID.
	if (storedResolution == "auto" || storedResolution == QStringLiteral("自动（跟随来源）"))
		m_resolution->setCurrentIndex(0);
	else
		m_resolution->setCurrentText(storedResolution);
	m_screenColor->setCurrentIndex(qMax(0,m_screenColor->findData(settings.value("screenColor","auto"))));m_screenExposure->setValue(settings.value("screenExposureEV",0).toDouble());m_screenWhite->setValue(settings.value("screenWhiteNits",0).toInt());colorMore->setChecked(m_screenWhite->value()!=0||m_screenExposure->value()!=0);
	m_outputColor->setCurrentIndex(qMax(0, m_outputColor->findData(settings.value("outputColor", "sdr"))));
	m_fps->setCurrentIndex(m_fps->findData(RecordingFrameRatePreset(settings.value("fps", 30).toInt())));
	m_bitrate->setValue(settings.value("bitrate", 6000).toInt());
	m_recommendedBitrate->setChecked(settings.value("recommendedBitrate", !settings.contains("bitrate")).toBool());
	m_micGain->setValue(settings.value("micGain", 100).toInt());
	m_systemGain->setValue(settings.value("systemGain", 100).toInt());
	m_additionalAudio = QJsonDocument::fromJson(settings.value("studioAudioSources").toByteArray()).array();
	rebuildAudioSources();
	m_scenePresets = QJsonDocument::fromJson(settings.value("studioScenes").toByteArray()).array();
	refreshSceneList();
	m_audio->setCurrentIndex(settings.value("audio", 0).toInt()); m_source->setCurrentIndex(settings.value("source", 0).toInt());
	m_format->setCurrentText(settings.value("format", "mp4").toString()); m_countdown->setCurrentIndex(settings.value("countdown", 1).toInt());
	m_cursor->setChecked(settings.value("cursor", true).toBool()); m_clicks->setChecked(settings.value("clicks", false).toBool());
	m_radius->setValue(settings.value("radius", 26).toInt()); m_clickColor = settings.value("color", "#ffcc33").toString();
	m_region = settings.value("region").toRect();
	m_monitor->setCurrentIndex(qBound(0, settings.value("monitor", 0).toInt(), m_monitor->count() - 1));
	m_encoder->setCurrentIndex(qMax(0, m_encoder->findData(settings.value("encoder", "auto"))));
	m_cameraEnabled->setChecked(settings.value("cameraEnabled", false).toBool()); m_cameraTrack->setChecked(settings.value("cameraTrack", false).toBool());
	m_showCameraPreview->setChecked(settings.value("showCameraPreview",true).toBool());
	m_floatButton->setChecked(settings.value("showFloatingBar", false).toBool());
	m_cameraMirror->setChecked(settings.value("cameraMirror", true).toBool()); m_cameraShape->setCurrentIndex(settings.value("cameraShape", 0).toInt());
	m_cameraX->setValue(settings.value("cameraX", 75).toInt()); m_cameraY->setValue(settings.value("cameraY", 65).toInt()); m_cameraW->setValue(settings.value("cameraW", 22).toInt()); m_cameraH->setValue(settings.value("cameraH", 30).toInt());
	m_cameraBorder = settings.value("cameraBorder", "#ffffff").toString(); m_image->setText(ResolveBundledRecordingImage(settings.value("image").toString()));
	m_separateAudio->setChecked(settings.value("separateAudio", false).toBool()); m_denoise->setChecked(settings.value("denoise", false).toBool());
	m_hotkeys = RecordingHotkeys::FromSettings(settings.value("hotkeys",
		QStringList{"Ctrl+Alt+R", "Ctrl+Alt+P", "Ctrl+Alt+S", "Ctrl+Alt+1", "Ctrl+Alt+2", "Ctrl+Alt+3", "Ctrl+Alt+D"}).toStringList());
	m_lastOutput = settings.value("lastOutput").toString();
	QStringList loaded;
	for (const auto& path : settings.value("recent").toStringList())
	{
		const QFileInfo file(path);
		const auto absolute = file.absoluteFilePath();
		if (!file.isFile() || loaded.contains(absolute, Qt::CaseInsensitive)) continue;
		loaded.append(absolute);
		auto item = new QListWidgetItem(file.fileName(), m_recent);
		item->setData(Qt::UserRole, absolute);
		item->setToolTip(absolute);
	}
	m_overlay = new KRRAnnotationOverlay;
	connect(m_overlay, &KRRAnnotationOverlay::activeChanged, this, [this](bool active)
	{
		m_lectureButton->setChecked(active);
		m_floatBar->setAnnotationActive(active);
		const auto hint = Ui("讲解工具已展开，默认操作模式。Esc 返回操作，再按退出；F8 收起/展开；Ctrl+Alt+Esc 唤回工具。遮挡固定于画面。");
		if (active) m_message->setText(hint);
		else if (m_message->text() == hint) m_message->clear();
	});
	connect(m_overlay, &KRRAnnotationOverlay::effectsChanged, this, [this](const QJsonObject& effects)
	{
		m_effects = effects;
		if (!m_effectTimer.isActive()) m_effectTimer.start(32);
	});
	connect(m_screenColor,qOverload<int>(&QComboBox::currentIndexChanged),this,[this]{m_effectTimer.start(100);});
	connect(m_screenExposure,qOverload<double>(&QDoubleSpinBox::valueChanged),this,[this]{m_effectTimer.start(100);});
	connect(m_screenWhite,qOverload<int>(&QSpinBox::valueChanged),this,[this]{m_effectTimer.start(100);});
	m_effectTimer.setSingleShot(true); connect(&m_effectTimer, &QTimer::timeout, this, &KRRMainWindow::sendScene);
	m_sceneSocket = new QLocalSocket(this);
	m_sceneTimeout.setSingleShot(true);
	connect(m_sceneSocket, &QLocalSocket::connected, this, [this] { m_sceneSocket->write(m_sceneRequest); });
	auto readSceneReply = [this]
	{
		const auto response = QJsonDocument::fromJson(m_sceneSocket->readLine()).object();
		finishScene(response["ok"].toBool() ? QString() : response["error"].toString());
	};
	connect(m_sceneSocket, &QLocalSocket::readyRead, this, [this, readSceneReply]
	{
		if (m_sceneSocket->canReadLine()) readSceneReply();
	});
	connect(m_sceneSocket, &QLocalSocket::errorOccurred, this, [this, readSceneReply]
	{
		if (m_sceneSocket->canReadLine()) readSceneReply();
		else if (m_sceneTimeout.isActive()) finishScene(m_sceneSocket->errorString());
	});
	connect(&m_sceneTimeout, &QTimer::timeout, this, [this]
	{
		finishScene(Ui("录制服务响应超时"));
	});
	for (auto number : {m_cameraX, m_cameraY, m_cameraW, m_cameraH}) connect(number, qOverload<int>(&QSpinBox::valueChanged), this, [this] { m_effectTimer.start(100); });
	connect(m_cameraMirror, &QCheckBox::toggled, this, [this] { m_effectTimer.start(100); });
	connect(m_cameraToggle, &QPushButton::clicked, m_cameraEnabled, &QPushButton::setChecked);
	connect(m_floatBar, &KRRRecordingToolbar::cameraRequested, m_cameraEnabled, &QPushButton::setChecked);
	connect(m_cameraEnabled, &QPushButton::toggled, this, [this](bool enabled) {
		if (!enabled && m_source->currentData() != "camera" && !m_cameraTrack->isChecked()) m_cameraPreview->hide();
		syncCaptureButtons();
		m_effectTimer.start(50);
		saveSettings();
	});
	connect(m_micToggle, &QPushButton::clicked, this, &KRRMainWindow::toggleMic);
	connect(m_settingsMic, &QPushButton::clicked, this, &KRRMainWindow::toggleMic);
	connect(m_floatBar, &KRRRecordingToolbar::micRequested, this, &KRRMainWindow::toggleMic);
	connect(m_audio, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { syncCaptureButtons(); });
	syncCaptureButtons();
	connect(m_cameraShape, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { m_effectTimer.start(100); });
	if (m_region.isValid()) m_regionLabel->setText(QString("%1, %2 · %3 × %4").arg(m_region.x()).arg(m_region.y()).arg(m_region.width()).arg(m_region.height()));

	m_mode->setCurrentIndex(m_source->currentIndex());
	connect(m_mode, qOverload<int>(&QComboBox::activated), this, [this](int index) {
		if (index == 2) refreshWindows();
		if ((index == 1 && !m_region.isValid()) || (index == 4 && !QFileInfo(m_image->text()).isFile()) ||
			(index == 2 && !m_window->count()))
			chooseSource(true);
		else m_source->setCurrentIndex(index);
		m_mode->setCurrentIndex(m_source->currentIndex());
		syncQuickSources();
		sendScene();
		saveSettings();
	});
	connect(m_source, qOverload<int>(&QComboBox::currentIndexChanged), m_mode, &QComboBox::setCurrentIndex);
	auto sourceFields = [this, form, selectRegion, refresh, chooseImage](int index)
	{
		for (auto field : QList<QWidget*>{m_monitor, selectRegion, m_regionLabel, m_window, refresh, m_image, chooseImage})
		{
			bool visible = (index == 0 && field == m_monitor) || (index == 1 && (field == selectRegion || field == m_regionLabel)) || (index == 2 && (field == m_window || field == refresh)) || (index == 4 && (field == m_image || field == chooseImage));
			field->setVisible(visible); if (auto label = form->labelForField(field)) label->setVisible(visible);
		}
		syncCaptureButtons();
		m_effectTimer.start(50);
	};
	connect(m_source, qOverload<int>(&QComboBox::currentIndexChanged), this, sourceFields); sourceFields(m_source->currentIndex());
	connect(m_monitor, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { m_effectTimer.start(50); });
	connect(m_window, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { m_effectTimer.start(50); });
	for (auto field : {m_fps, m_monitor, m_source, m_window})
		connect(field, qOverload<int>(&QComboBox::currentIndexChanged), this, &KRRMainWindow::updateBitrateRecommendation);
	connect(m_resolution, &QComboBox::currentTextChanged, this, &KRRMainWindow::updateBitrateRecommendation);
	connect(m_image, &QLineEdit::textChanged, this, &KRRMainWindow::updateBitrateRecommendation);
	connect(m_recommendedBitrate, &QCheckBox::toggled, this, &KRRMainWindow::updateBitrateRecommendation);
	updateBitrateRecommendation();
	refreshWindows();
	syncQuickSources();
	connect(qApp,&QGuiApplication::screenAdded,this,[this](QScreen*){refreshMonitors();});
	connect(qApp,&QGuiApplication::screenRemoved,this,[this](QScreen*){refreshMonitors();});
	for(auto screen : QGuiApplication::screens())connect(screen,&QScreen::geometryChanged,this,[this]{refreshMonitors();});
	m_statusSocket = new QLocalSocket(this);
	m_statusSocket->setReadBufferSize(65536);
	m_statusDeadline.setSingleShot(true);
	connect(m_statusSocket, &QLocalSocket::connected, this, [this]
	{
		m_statusSocket->write("{\"command\":\"status\"}\n");
	});
	connect(m_statusSocket, &QLocalSocket::readyRead, this, [this]
	{
		if (m_statusSocket->canReadLine())
			finishStatusPoll(QJsonDocument::fromJson(m_statusSocket->readLine()).object());
	});
	connect(m_statusSocket, &QLocalSocket::errorOccurred, this, [this]
	{
		if (m_statusSocket->canReadLine())
			finishStatusPoll(QJsonDocument::fromJson(m_statusSocket->readLine()).object());
		else finishStatusPoll({});
	});
	connect(&m_statusDeadline, &QTimer::timeout, this, [this] { finishStatusPoll({}); });
	connect(&m_pollTimer, &QTimer::timeout, this, &KRRMainWindow::pollRecordingStatus);
	m_pollTimer.start(500);
	m_levelSocket = new QLocalSocket(this);
	m_levelSocket->setReadBufferSize(16384);
	m_levelDeadline.setSingleShot(true);
	connect(&m_levelDeadline, &QTimer::timeout, m_levelSocket, &QLocalSocket::abort);
	connect(m_levelSocket, &QLocalSocket::connected, this, [this]
	{
		m_levelSocket->write("{\"command\":\"meter\"}\n");
	});
	connect(m_levelSocket, &QLocalSocket::readyRead, this, [this]
	{
		if (!m_levelSocket->canReadLine()) return;
		m_levelDeadline.stop();
		const auto levels = QJsonDocument::fromJson(m_levelSocket->readLine()).object();
		if (m_active && levels["state"] == "recording") updateMeters(levels);
		m_levelSocket->abort();
	});
	connect(&m_countdownTimer, &QTimer::timeout, this, [this]
	{
		if (--m_countdownRemaining <= 0) { m_countdownTimer.stop(); startRecording(); }
		else m_state->setText(Ui("%1 秒后开始…").arg(m_countdownRemaining));
	});
	HideConsoleProcess(m_stopProcess);
	connect(&m_stopProcess, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this](int code, QProcess::ExitStatus)
	{
		const auto error = QString::fromUtf8(m_stopProcess.readAllStandardError()).trimmed();
		if (code != 0) m_message->setText(error.isEmpty() ? Ui("停止录制未收到确认，请重试。") : UiDiagnostic(error));
		updateStatus();
	});
	connect(&m_stopProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error)
	{
		if (error == QProcess::FailedToStart)
		{
			m_message->setText(m_stopProcess.errorString());
			updateStatus();
		}
	});
	m_clipEditor = new KRRClipEditor(m_editPane);
	m_clipEditor->setWindowFlags(Qt::Widget);
	m_editPane->layout()->addWidget(m_clipEditor);
	m_clipEditor->show();
	connect(m_clipEditor,&KRRClipEditor::exported,this,[this](const QString& file){openPreview(file);raise();activateWindow();});
	connect(m_clipEditor,&KRRClipEditor::batchRequested,this,[this]{m_editDialog->show();m_editDialog->raise();m_editDialog->activateWindow();});
	m_clipEditor->setRecording(m_active);
	buildStudioAssistants();
	initializeReviewProbe();
	m_studioReady = true;
	m_visualSources = QJsonDocument::fromJson(settings.value("studioSources").toByteArray()).array();
	m_sourceIndex = settings.value("studioSourceIndex", -1).toInt();
	m_sceneIndex = settings.value("studioSceneIndex", -1).toInt();
	const auto dockOrder = settings.value("studioDockOrder").toStringList();
	for (int i = 0; i < dockOrder.size() && i < m_recordDocks->count(); ++i)
		if (auto dock = m_recordDocks->findChild<QWidget*>(dockOrder[i], Qt::FindDirectChildrenOnly))
			m_recordDocks->insertWidget(i, dock);
	m_recordDocks->restoreState(settings.value("studioDockSizes").toByteArray());
	if (m_recordLayout->restoreState(settings.value("studioRecordSizes").toByteArray()))
		m_recordLayout->setOrientation(Qt::Vertical);
	restoreGeometry(settings.value("studioGeometry").toByteArray());
	if (m_sceneIndex >= 0 && m_sceneIndex < m_scenePresets.size())
	{
		const auto scene = m_scenePresets[m_sceneIndex].toObject();
		if (scene["layeredSources"].toBool())
		{
			const QScopedValueRollback<bool> loading(m_updatingSources, true);
			const bool automatic = m_resolution->currentText() == m_resolution->itemText(0);
			auto config = configuration().toJson();
			const auto saved = scene["config"].toObject();
			for (const auto& key : QStringList{"source", "monitor", "monitorDevice", "window", "image", "x", "y", "rw", "rh",
				"cursor", "clicks", "camera", "cameraEnabled", "cameraTrack", "cameraMirror", "cameraShape", "cameraRect",
				"cameraBorder", "screenColor", "screenWhiteNits", "screenExposureEV"})
				if (saved.contains(key)) config[key] = saved[key];
			applySceneConfiguration(KRRRecordingConfig::FromJson(config));
			if (automatic) m_resolution->setCurrentIndex(0);
		}
	}
	rememberSource();
	if (m_scenePresets.isEmpty())
	{
		auto initial = configuration().toJson();
		initial.remove("output");
		m_scenePresets.append(QJsonObject{{"name", Ui("场景 1")}, {"config", initial}});
		m_sceneIndex = 0;
	}
	refreshSceneList();
	if (m_sceneIndex >= 0 && m_sceneIndex < m_scenes->count()) m_scenes->setCurrentRow(m_sceneIndex);
	updateStatus();
	registerHotkeys();
	refreshMediaActions();
	// Opening the GUI must not overwrite a recording already started by the CLI.
	m_effectTimer.stop();
	if (m_floatButton->isChecked() && !m_floatBar->isVisible())
	{
		QString error;
		if (!m_floatBar->present(error, m_active)) m_message->setText(error);
	}
	RecorderUi::PolishButtons(this);
}

KRRRecordingConfig KRRMainWindow::configuration() const
{
	KRRRecordingConfig config;
	config.output = QDir(m_directory->text()).absoluteFilePath("recording-" + Stamp() + "." + m_format->currentText());
	QStringList resolution = m_resolution->currentText().split('x');
	config.width = resolution.value(0).toInt(); config.height = resolution.value(1).toInt();
	config.fps = m_fps->currentData().toInt(); config.bitrate = m_bitrate->value(); config.audio = m_audio->currentData().toString();
	if (m_micGain) config.micGain = m_micGain->value() / 100.;
	if (m_systemGain) config.systemGain = m_systemGain->value() / 100.;
	config.audioSources = m_additionalAudio;
	config.source = m_source->currentData().toString(); config.region = m_region;
	config.window = static_cast<quintptr>(m_window->currentData().toULongLong());
	config.cursor = m_cursor->isChecked(); config.clicks = m_clicks->isChecked();
	config.clickColor = m_clickColor; config.clickRadius = m_radius->value();
	config.monitor = config.source == "screen" ? m_monitor->currentIndex() : qMax(0, m_monitor->currentIndex());
	config.monitorDevice = m_monitor->currentData(Qt::UserRole + 1).toString();
	config.encoder = m_encoder->currentData().toString();
	config.separateAudio = m_separateAudio->isChecked(); config.denoise = m_denoise->isChecked();
	config.camera = m_camera->currentIndex() < 0 ? m_unavailableCameraIndex : m_camera->currentData().toInt();
	config.cameraEnabled = m_cameraEnabled->isChecked(); config.cameraTrack = m_cameraTrack->isChecked();
	config.cameraMirror = m_cameraMirror->isChecked(); config.cameraShape = m_cameraShape->currentData().toString(); config.cameraBorder = m_cameraBorder;
	config.cameraRect = QRectF(m_cameraX->value() / 100.0, m_cameraY->value() / 100.0, m_cameraW->value() / 100.0, m_cameraH->value() / 100.0);
	config.image = ResolveBundledRecordingImage(m_image->text()); config.effects = m_effects;
	config.outputColor = m_outputColor->currentData().toString();
	config.screenColor=m_screenColor->currentData().toString();config.screenExposureEV=m_screenExposure->value();config.screenWhiteNits=m_screenWhite->value();
	if (m_resolution->currentText() == m_resolution->itemText(0))
	{
		QSize size = config.source == "image" ? QImageReader(config.image).size() : config.source == "camera" ? QSize(1280,720) : KRRFrameCapture::CaptureArea(config).size();
		config.width = size.width() / 2 * 2; config.height = size.height() / 2 * 2;
	}
	if (m_recommendedBitrate->isChecked())
		config.bitrate = RecommendedRecordingBitrate(config.width, config.height, config.fps);
	if (config.camera < 0 && !config.cameraEnabled && !config.cameraTrack && config.source != "camera") config.camera = 0;
	return config;
}
void KRRMainWindow::browseRecordingFolder()
{
	if (m_active || m_countdownRemaining) return;
	const auto path = QFileDialog::getExistingDirectory(this, Ui("保存录制文件"), m_directory->text());
	if (!path.isEmpty())
	{
		m_defaultDirectory = false;
		m_directory->setText(QDir::toNativeSeparators(path));
		saveSettings();
	}
}

void KRRMainWindow::updateBitrateRecommendation()
{
	const auto config = configuration();
	const int recommended = RecommendedRecordingBitrate(config.width, config.height, config.fps);
	m_bitrateHint->setText(Ui("推荐 %1 kbps").arg(recommended));
	m_bitrate->setEnabled(!m_active);
	if (m_recommendedBitrate->isChecked()) m_bitrate->setValue(recommended);
}
void KRRMainWindow::editFile(const QString& file)
{
	m_player->pause();
	if (!file.isEmpty() && file != m_clipEditor->inputFile() && !m_clipEditor->openFile(file, m_directory->text())) return;
	selectWorkspace(Edit);
	m_clipEditor->show();
}
void KRRMainWindow::beginCountdown()
{
	if (m_active || m_startPending || m_countdownRemaining || m_checkingRecording) return;
	if(!validateStorage())return;
	QString error;
	const auto config = configuration();
	if (!config.validate(error))
	{
		RememberRecordingOutcome({{"error", error}, {"failure_stage", "validate"},
			{"requested_encoder", config.encoder}, {"audio_mode", config.audio}});
		showError(error);
		return;
	}
	m_clipEditor->interruptSmartCut();
	saveSettings();
	m_countdownRemaining = m_countdown->currentData().toInt();
	if (!m_countdownRemaining) { startRecording(); return; }
	refreshMediaActions();
	m_start->setEnabled(false); m_stop->setEnabled(true);
	m_start->hide(); m_stop->show();
	m_state->setText(Ui("%1 秒后开始…").arg(m_countdownRemaining));
	m_countdownTimer.start(1000);
}
KRRAssistantContext KRRMainWindow::assistantContext() const
{
	KRRAssistantContext context;
	context.folder = m_directory->text();
	for (int i = 0; i < m_recent->count(); ++i)
		context.recent.append(m_recent->item(i)->data(Qt::UserRole).toString());
	context.settings = {
		{"resolution", m_resolution->currentText() == m_resolution->itemText(0) ? QString("auto") : m_resolution->currentText()},
		{"fps", m_fps->currentData().toInt()}, {"bitrate_kbps", m_bitrate->value()},
		{"audio", m_audio->currentData().toString()}, {"encoder", m_encoder->currentData().toString()},
		{"format", m_format->currentText()}, {"separate_audio", m_separateAudio->isChecked()},
		{"denoise", m_denoise->isChecked()}, {"cursor", m_cursor->isChecked()}
	};
	return context;
}

bool KRRMainWindow::applyAssistantSettings(const QJsonObject& before, const QJsonObject& changes, QString& error)
{
	if (m_active || m_countdownRemaining || AssistantRecordingState()["state"] == "recording")
	{
		error = Ui("录制结束后可应用设置。");
		return false;
	}
	if (before != assistantContext().settings)
	{
		error = Ui("当前设置已变化，请重新生成预览。");
		return false;
	}
	const auto preview = PreviewAssistantSettings(before, changes);
	if (!preview["ok"].toBool() || preview["diff"].toArray().isEmpty())
	{
		error = Ui("该设置预览已失效。");
		return false;
	}
	const bool usedRecommendation = m_recommendedBitrate->isChecked();
	auto put = [this](const QJsonObject& values)
	{
		if (values.contains("resolution"))
		{
			const auto resolution = values["resolution"].toString();
			if (resolution == "auto") m_resolution->setCurrentIndex(0);
			else m_resolution->setCurrentText(resolution);
		}
		if (values.contains("fps")) m_fps->setCurrentIndex(m_fps->findData(values["fps"].toInt()));
		if (values.contains("bitrate_kbps"))
		{
			m_recommendedBitrate->setChecked(false);
			m_bitrate->setValue(values["bitrate_kbps"].toInt());
		}
		if (values.contains("audio")) m_audio->setCurrentIndex(m_audio->findData(values["audio"].toString()));
		if (values.contains("encoder")) m_encoder->setCurrentIndex(m_encoder->findData(values["encoder"].toString()));
		if (values.contains("format")) m_format->setCurrentText(values["format"].toString());
		if (values.contains("separate_audio")) m_separateAudio->setChecked(values["separate_audio"].toBool());
		if (values.contains("denoise")) m_denoise->setChecked(values["denoise"].toBool());
		if (values.contains("cursor")) m_cursor->setChecked(values["cursor"].toBool());
	};
	put(preview["changes"].toObject());
	if (saveSettings())
	{
		return true;
	}
	QJsonObject rollback;
	for (const auto& key : changes.keys()) rollback[key] = before[key];
	put(rollback);
	m_recommendedBitrate->setChecked(usedRecommendation);
	saveSettings();
	error = Ui("无法保存助手建议的设置。");
	return false;
}


void KRRMainWindow::showRecordingCheck()
{
	if (m_active || m_countdownRemaining || m_checkingRecording)
	{
		m_message->setText(Ui("停止录制后可检查设备和试录。"));
		return;
	}
	if (m_defaultDirectory && !validateStorage()) return;
	QScopedValueRollback<bool> checking(m_checkingRecording, true);
	const auto config = configuration();
	QString source = m_source->currentText();
	if (config.source == "window") source += " · " + m_window->currentText();
	else if (config.source == "screen") source += " · " + m_monitor->currentText();
	else if (config.source == "camera") source += " · " + m_camera->currentText();
	else if (config.source == "region") source += " · " + m_regionLabel->text();
	else if (config.source == "image") source += " · " + QFileInfo(config.image).fileName();
	KRRRecordingCheckDialog dialog(config, source, m_lastRecordingStatus, this);
	bool trial = false;
	connect(&dialog, &KRRRecordingCheckDialog::trialRequested, this, [&trial] { trial = true; });
	dialog.exec();
	if (trial) startTrial(config);
}

void KRRMainWindow::startTrial(KRRRecordingConfig config)
{
	if (m_active || m_countdownRemaining || m_trialPlayback.state() != QProcess::NotRunning) return;
	if (!QDir().mkpath(QFileInfo(config.output).absolutePath()))
	{
		showError(Ui("无法创建试录文件，请检查保存目录。"));
		return;
	}
	auto folder = std::make_unique<QTemporaryDir>(QFileInfo(config.output).absolutePath() + "/.wps-trial-XXXXXX");
	if (!folder->isValid()) { showError(Ui("无法创建试录文件，请检查保存目录。")); return; }
	config.output = folder->filePath("trial.mp4");
	config.maxDuration = 6;
	config.separateAudio = true;
	config.cameraTrack = false;
	QString error;
	QJsonObject response;
	if (!EnsureControlService(error) || !SendControl({{"command", "start"}, {"config", config.toJson()}}, response, error, 3000))
	{
		showError(error);
		return;
	}
	m_player->stop();
	m_player->setMedia({});
	m_trialPath = config.output;
	m_trialAudioMode = config.audio;
	m_trialWorkspace = std::move(folder);
	m_recordingSessionOutput = config.output;
	m_lastRecordingStatus = {};
	updateStatus();
}

void KRRMainWindow::playTrialTrack()
{
	if (!m_trialWorkspace || m_preview != m_trialPath || m_trialPlayback.state() != QProcess::NotRunning) return;
	QString selection = m_trialTracks->currentData().toString();
	m_player->stop();
	if (selection == "video")
	{
		m_player->setMedia(QUrl::fromLocalFile(m_trialPath));
		m_player->play();
		return;
	}
	const QString wav = m_trialWorkspace->filePath("listen-" + selection + ".wav");
	if (QFileInfo(wav).isFile())
	{
		m_player->setMedia(QUrl::fromLocalFile(wav));
		m_player->play();
		return;
	}
	m_trialPlayback.setProperty("output", wav);
	m_trialReplay->setEnabled(false);
	m_trialPlayback.start(FindFFmpeg(), {"-nostdin", "-v", "error", "-n", "-i", m_trialPath,
										 "-map", "0:a:" + selection, "-c:a", "pcm_s16le", wav});
}

void KRRMainWindow::showRecoveries()
{
	if (m_active)
	{
		m_message->setText(Ui("停止录制后可恢复素材。"));
		return;
	}
	QDialog dialog(this);
	dialog.setObjectName("recordingRecovery");
	dialog.setWindowTitle(Ui("恢复录制素材"));
	dialog.resize(RecorderUi::Px(660), RecorderUi::Px(400));
	auto layout = new QVBoxLayout(&dialog);
	auto folder = new QLineEdit(m_directory->text(), &dialog);
	folder->setReadOnly(true);
	auto browse = new QPushButton(Ui("选择录制目录"), &dialog);
	auto top = new QHBoxLayout;
	top->addWidget(folder, 1);
	top->addWidget(browse);
	layout->addLayout(top);
	auto items = new QListWidget(&dialog);
	items->setObjectName("recordingRecoveryItems");
	layout->addWidget(items, 1);
	auto details = new QLabel(&dialog);
	details->setTextFormat(Qt::PlainText);
	details->setWordWrap(true);
	layout->addWidget(details);
	auto buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
	auto materials = buttons->addButton(Ui("打开原始素材目录"), QDialogButtonBox::ActionRole);
	auto save = buttons->addButton(Ui("恢复并预览…"), QDialogButtonBox::ActionRole);
	save->setObjectName("saveRecovery");
	save->setProperty("role", "primary");
	layout->addWidget(buttons);
	QVector<KRRRecoveryItem> found;
	const auto updateDetails = [&]
	{
		int index = items->currentRow();
		save->setEnabled(index >= 0);
		materials->setEnabled(index >= 0);
		if (index < 0)
		{
			details->setText(Ui("这个目录没有可恢复的录制。可选择当时使用的保存目录。"));
			return;
		}
		const auto& selected = found[index];
		QStringList tracks{Ui("视频")};
		if (selected.audio.contains("mic")) tracks << Ui("麦克风");
		if (selected.audio.contains("system")) tracks << Ui("系统声音");
		if (selected.hasCamera) tracks << Ui("独立摄像头素材");
		const auto stage = selected.stage.isEmpty() ? Ui("上次录制意外中断") : UiDiagnostic(selected.stage);
		details->setText(tracks.join(" · ") + "\n" + stage + "\n" + UiDiagnostic(selected.error.trimmed().left(600)));
	};
	const auto list = [&]
	{
		items->clear();
		found = RecordingRecoveryItems(folder->text());
		for (const auto& item : found)
			items->addItem(item.name + "  ·  " + QLocale().toString(item.modified, QLocale::ShortFormat));
		items->setCurrentRow(found.isEmpty() ? -1 : 0);
		updateDetails();
	};
	connect(items, &QListWidget::currentRowChanged, &dialog, updateDetails);
	connect(materials, &QPushButton::clicked, &dialog, [&]
	{
		const auto workspace = found.value(items->currentRow()).workspace;
		if (!workspace.isEmpty()) QDesktopServices::openUrl(QUrl::fromLocalFile(workspace));
	});
	connect(browse, &QPushButton::clicked, &dialog, [&]
	{
		const auto directory = QFileDialog::getExistingDirectory(&dialog, Ui("选择录制目录"), folder->text());
		if (directory.isEmpty()) return;
		folder->setText(directory);
		list();
	});
	connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	connect(save, &QPushButton::clicked, &dialog, [&]
	{
		const auto selected = found.value(items->currentRow());
		if (selected.workspace.isEmpty()) return;
		const auto name = QFileInfo(selected.name).completeBaseName();
		const auto suggested = QDir(folder->text()).filePath(name + "-recovered-" + Stamp() + ".mp4");
		QFileDialog file(&dialog, Ui("另存恢复件"), suggested, "MP4 (*.mp4);;MKV (*.mkv)");
		file.setAcceptMode(QFileDialog::AcceptSave);
		file.setFileMode(QFileDialog::AnyFile);
		file.setDefaultSuffix("mp4");
		file.setOption(QFileDialog::DontConfirmOverwrite);
		connect(&file, &QFileDialog::filterSelected, &file, [&file](const QString& filter)
		{
			file.setDefaultSuffix(filter.startsWith("MKV") ? "mkv" : "mp4");
		});
		if (file.exec() != QDialog::Accepted) return;
		const auto output = file.selectedFiles().value(0);
		if (QFileInfo::exists(output) || QFileInfo::exists(RecordingMarkersPath(output)))
		{
			details->setText(Ui("目标文件已存在，请选择新文件名。"));
			return;
		}
		if (!m_editor.recoverRecording(selected.workspace, output))
		{
			m_message->setText(Ui("请等待当前任务完成"));
			return;
		}
		m_message->setText(Ui("正在恢复，完成后自动打开预览…"));
		dialog.accept();
	});
	RecorderUi::PolishButtons(&dialog);
	const auto recentWorkspace = m_lastRecordingStatus["recovery_workspace"].toString();
	if (!recentWorkspace.isEmpty()) folder->setText(QFileInfo(recentWorkspace).absolutePath());
	else if (!m_recordingSessionOutput.isEmpty()) folder->setText(QFileInfo(m_recordingSessionOutput).absolutePath());
	list();
	dialog.exec();
}
void KRRMainWindow::addRecordingMarker()
{
	QJsonObject response;
	QString error;
	if (!SendControl({{"command", "mark"}}, response, error, 1000))
	{
		m_message->setText(UiDiagnostic(error));
		m_floatBar->markerFeedback(false);
		return;
	}
	const auto time = response["marker"].toObject()["at_ms"].toDouble() / 1000.0;
	const auto message = Ui("已打点 · %1").arg(TimeText(time));
	m_message->setText(message);
	statusBar()->showMessage(message, 2500);
	m_floatBar->markerFeedback(true);
}

void KRRMainWindow::updatePlaybackMarkers()
{
	const auto markers = m_preview == m_trialPath ? QJsonArray{} : ReadRecordingMarkers(m_preview);
	QStringList recent;
	for (int i = 0; i < m_recent->count(); ++i)
		recent << m_recent->item(i)->data(Qt::UserRole).toString();
	m_fullscreen->setRecordings(markers, recent, m_preview);
	const QScopedValueRollback<bool> updating(m_refreshingChapters, true);
	const QSignalBlocker block(m_chapters);
	m_playbackMarkers->clear();
	m_chapters->clear();
	m_renameMarker->setEnabled(false);
	QVector<int> points;
	if (markers.isEmpty())
	{
		static_cast<KRRPlaybackSlider*>(m_seek)->setMarkers(points);
		return;
	}
	m_playbackMarkers->addItem(Ui("跳转到标记"));
	for (int index = 0; index < markers.size(); ++index)
	{
		const auto item = markers.at(index).toObject();
		const auto time = static_cast<qint64>(item["at_ms"].toDouble());
		QString label = item["label"].toString();
		if (label.isEmpty()) label = Ui("标记 %1").arg(index + 1);
		m_playbackMarkers->addItem(EditTime(time) + "  " + label, time);
		points.append(static_cast<int>(qBound<qint64>(0, time, INT_MAX)));
		auto row = new QListWidgetItem(label, m_chapters);
		row->setFlags(row->flags() | Qt::ItemIsEditable);
		row->setData(Qt::UserRole + 2, label);
		row->setToolTip(EditTime(time) + "  ·  " + label);
		row->setData(Qt::UserRole, time);
		row->setData(Qt::UserRole + 1, index + 1);
	}
	static_cast<KRRPlaybackSlider*>(m_seek)->setMarkers(points);
	for (int i = 0; i < m_recent->count(); ++i)
		if (m_recent->item(i)->data(Qt::UserRole).toString() == m_preview)
			m_recent->item(i)->setData(Qt::UserRole + 1, QVariant());
}
void KRRMainWindow::addPlaybackMarker()
{
	if (m_preview.isEmpty() || m_preview == m_trialPath || m_player->duration() <= 0 || !QFileInfo(m_preview).isFile()) return;
	auto marks = ReadRecordingMarkers(m_preview);
	if (marks.size() >= 500) { m_message->setText(Ui("此录像的打点数量已达上限。")); return; }
	const auto at = m_player->position();
	int index = 0;
	while (index < marks.size() && marks[index].toObject()["at_ms"].toDouble() <= double(at)) ++index;
	const auto label = Ui("标记 %1").arg(marks.size() + 1);
	marks.insert(index, QJsonObject{{"at_ms", static_cast<double>(at)}, {"label", label}});
	QString error;
	if (!WriteRecordingMarkers(RecordingMarkersPath(m_preview), marks, error)) { showError(error); return; }
	updatePlaybackMarkers();
	updateReviewLibrary();
	m_chapters->setCurrentRow(index);
	m_chapters->editItem(m_chapters->currentItem());
}

void KRRMainWindow::savePlaybackMarker(QListWidgetItem* item)
{
	if (m_refreshingChapters || m_preview.isEmpty() || !item) return;
	const auto previous = item->data(Qt::UserRole + 2).toString();
	const auto label = item->text().simplified().left(120);
	if (label == previous) return;
	auto marks = ReadRecordingMarkers(m_preview);
	const int index = item->data(Qt::UserRole + 1).toInt() - 1;
	if (index < 0 || index >= marks.size()) return;
	auto mark = marks[index].toObject();
	mark["label"] = label;
	marks.replace(index, mark);
	QString error;
	const bool written = WriteRecordingMarkers(RecordingMarkersPath(m_preview), marks, error);
	{
		const QSignalBlocker block(m_chapters);
		item->setText(written ? label : previous);
		item->setData(Qt::UserRole + 2, written ? label : previous);
		item->setToolTip(EditTime(item->data(Qt::UserRole).toLongLong()) + "  ·  " + item->text());
	}
	if (!written) { showError(error); return; }
	m_playbackMarkers->setItemText(index + 1, item->toolTip());
	QStringList recent;
	for (int i = 0; i < m_recent->count(); ++i) recent << m_recent->item(i)->data(Qt::UserRole).toString();
	m_fullscreen->setRecordings(marks, recent, m_preview);
}

void KRRMainWindow::renamePlaybackMarker()
{
	const auto video = m_preview;
	const int index = m_playbackMarkers->currentIndex() - 1;
	auto markers = ReadRecordingMarkers(video);
	if (m_active || index < 0 || index >= markers.size()) return;
	auto marker = markers[index].toObject();
	bool accepted = false;
	const auto label = QInputDialog::getText(this, Ui("命名标记"), Ui("标记名称"), QLineEdit::Normal,
		marker["label"].toString(), &accepted).simplified();
	if (!accepted || m_active || video != m_preview) return;
	if (label.size() > 120)
	{
		m_message->setText(Ui("标记名称最多 120 个字符。"));
		return;
	}
	marker["label"] = label;
	markers.replace(index, marker);
	QString error;
	if (!WriteRecordingMarkers(RecordingMarkersPath(video), markers, error)) { showError(error); return; }
	updatePlaybackMarkers();
	m_playbackMarkers->setCurrentIndex(index + 1);
	updateReviewLibrary();
}
void KRRMainWindow::startRecording()
{
	if (m_startPending) return;
	releaseIdlePreview();
	const auto config = configuration();
	m_startPending = true;
	m_stopDuringStart = false;
	m_start->hide();
	m_stop->show();
	m_stop->setEnabled(true);
	m_floatBar->setStatus(true, false, false, TimeText(0));
	const QPointer<KRRMainWindow> self(this);
	QThreadPool::globalInstance()->start(QRunnable::create([self, config]
	{
		QString error;
		QJsonObject response;
		const bool started = EnsureControlService(error) && SendControl({{"command", "start"}, {"config", config.toJson()}}, response, error, 3000);
		if (!self) return;
		QMetaObject::invokeMethod(self.data(), [self, config, started, error]
		{
			if (!self) return;
			auto& w = *self;
			w.m_startPending = false;
			if (started)
			{
				w.m_recordingSessionOutput = config.output;
				w.applyRecordingStatus(true, {{"state", "recording"}, {"phase", "starting"}, {"output", config.output}});
				if (w.m_stopDuringStart) w.stopRecording();
			}
			else
			{
				w.applyRecordingStatus(false, {});
				w.m_message->setText(UiDiagnostic(error.isEmpty() ? QString("Recording could not start") : error));
			}
			w.m_stopDuringStart = false;
			w.refreshMediaActions();
		}, Qt::QueuedConnection);
	}));
}
void KRRMainWindow::stopRecording()
{
	if (m_startPending) { m_stopDuringStart = true; return; }
	if (m_countdownRemaining > 0)
	{
		m_countdownTimer.stop(); m_countdownRemaining = 0; updateStatus(); refreshMediaActions(); return;
	}
	if (m_active && m_stopProcess.state() == QProcess::NotRunning)
	{
		m_stopProcess.start(QCoreApplication::applicationDirPath() + "/wpsrecorder_cli.exe", {"--stop"});
		m_stop->setEnabled(false);
		m_pause->setEnabled(false);
		m_state->setText(Ui("正在保存…"));
		m_floatBar->setStatus(true, m_paused, true, m_time->text());
	}
}
void KRRMainWindow::updateStatus()
{
	cancelStatusPoll();
	if (m_countdownRemaining > 0) return;
	QJsonObject response; QString error;
	const bool connected = SendControl({{"command", "status"}}, response, error, 500);
	applyRecordingStatus(connected, response);
}

void KRRMainWindow::pollRecordingStatus()
{
	if (m_startPending || m_countdownRemaining > 0 || m_statusSocket->state() != QLocalSocket::UnconnectedState) return;
	m_statusDeadline.start(500);
	m_statusSocket->connectToServer(ControlServerName());
}

void KRRMainWindow::cancelStatusPoll()
{
	m_statusDeadline.stop();
	const QSignalBlocker blocker(m_statusSocket);
	m_statusSocket->abort();
}

void KRRMainWindow::finishStatusPoll(const QJsonObject& response)
{
	cancelStatusPoll();
	applyRecordingStatus(response["ok"].toBool(), response);
}

void KRRMainWindow::applyRecordingStatus(bool connected, const QJsonObject& response)
{
	if (m_startPending || m_countdownRemaining > 0 || m_applyingRecordingStatus) return;
	const QScopedValueRollback<bool> applying(m_applyingRecordingStatus, true);
	if (connected)
	{
		m_unreachablePolls = 0;
		if (!response["output"].toString().isEmpty()) m_lastRecordingStatus = response;
	}
	else if (m_active && ++m_unreachablePolls < 3)
	{
		m_state->setText(Ui("正在等待录制后台响应…"));
		return;
	}
	const bool active = connected && response["state"] == "recording";
	const bool saving = active && (response["phase"] == "finalizing" || m_stopProcess.state() != QProcess::NotRunning);
	if (active && !m_active)
		releaseIdlePreview();
	const auto output = response["output"].toString();
	if (active) m_recordingSessionOutput = output;
	const bool completed = connected && !active && !output.isEmpty() && output == m_recordingSessionOutput;
	const bool interrupted = (m_active && !active && !completed) ||
		(!connected && !m_recordingSessionOutput.isEmpty() && m_unreachablePolls >= 3);
	QString scheduled = response["scheduled_start"].toString();
	if (!scheduled.isEmpty()) m_scheduledOutput = response["scheduled_output"].toString();
	m_paused = response["paused"].toBool();
	m_state->setText(active ? (saving ? Ui("正在保存…") : m_paused ? Ui("已暂停") : Ui("正在录制")) : Ui("准备就绪"));
	if (auto light = findChild<QLabel*>("recordingLight"))
		light->setStyleSheet(active && !m_paused ? "color:#ff6473" : "color:#60d0a6");
	if (m_state->property("recording").toBool() != active) { m_state->setProperty("recording", active); m_state->style()->unpolish(m_state); m_state->style()->polish(m_state); }
	if (!active && !scheduled.isEmpty()) m_state->setText(Ui("已预约：") + QDateTime::fromString(scheduled, Qt::ISODate).toLocalTime().toString("MM-dd HH:mm:ss"));
	m_time->setText(TimeText(response["duration_sec"].toDouble()));
	m_start->setVisible(!active); m_stop->setVisible(active);
	m_start->setEnabled(!active); m_stop->setEnabled(active && !saving); m_pause->setEnabled(active && !saving && response["phase"] == "recording");
	const bool canMark = active && !saving && response["phase"] == "recording" && response["output"].toString() != m_trialPath;
	m_addMarker->setEnabled(canMark);
	m_floatBar->setMarkerEnabled(canMark);
	m_floatBar->setStatus(active, m_paused, saving, TimeText(response["duration_sec"].toDouble()));
	m_floatBar->setCaptureButtons(m_cameraEnabled->isChecked(), m_micToggle->isChecked(), m_cameraToggle->isEnabled(), !saving);
	if (m_floatButton->isChecked() && active != m_active)
	{
		QString floatError;
		if ((active && output == m_trialPath) || !m_floatBar->present(floatError, active)) m_floatBar->hide();
		if (!floatError.isEmpty()) m_message->setText(floatError);
	}
	m_checkButton->setEnabled(!active && scheduled.isEmpty());
	m_recoverButton->setVisible(!active && (interrupted || !m_lastRecordingStatus["recovery_workspace"].toString().isEmpty()));
	m_pause->setText(m_paused ? Ui("继续") : Ui("暂停"));
	m_pause->setIcon(RecorderUi::Icon(m_paused ? "play" : "pause"));
	for (QWidget* field : QList<QWidget*>{m_directory, m_resolution, m_fps, m_recommendedBitrate, m_format, m_audio, m_separateAudio, m_denoise, m_camera, m_cameraTrack, m_encoder}) field->setEnabled(!active);
	m_bitrate->setEnabled(!active);
	m_metrics->setText(studioMetrics(response, active));
	m_studioStatus->setText(m_state->text() + "    " + m_time->text());
	if ((active || completed) && output != m_trialPath) m_lastOutput = output;
	if(active)
	{
		if (!m_active)
		{
			m_liveFrame->setText(Ui("正在等待录制画面…"));
			m_liveSequence = -1;
			m_liveCameraReady = false;
			m_lastPreview.restart();
		}
		QString liveState = saving ? Ui("正在保存 · 显示最后画面") : m_paused ? Ui("已暂停 · 显示暂停前画面") : Ui("● 正在录制");
		if(!m_paused && !saving && m_lastPreview.isValid() && m_lastPreview.elapsed()>2000)liveState+=Ui(" · 监看等待更新");
		m_liveStatus->setText(liveState + Ui(" · 合成画面监看 · 第 %1 帧").arg(qMax<qint64>(0,m_liveSequence)));
		m_cameraStatus->setText(liveState + Ui("\n摄像头实时画面 · 主窗口查看合成效果"));
		if (m_effectTimer.isActive() == false && response.contains("capture_control_error") && !response["capture_control_error"].toString().isEmpty())
		{
			m_message->setText(UiDiagnostic(response["capture_control_error"].toString()));
			if (!response["pip_enabled"].toBool() && m_cameraEnabled->isChecked()) m_cameraEnabled->setChecked(false);
			if (!response["mic_enabled"].toBool() && m_micToggle->isChecked()) toggleMic(false);
		}
		m_liveFrame->setPlacement(QRectF(m_cameraX->value()/100.,m_cameraY->value()/100.,m_cameraW->value()/100.,m_cameraH->value()/100.),m_cameraEnabled->isChecked()&&m_source->currentData().toString()!="camera"&&!m_paused&&!saving);
	}
	if (completed || (m_active && !active))
	{
		const bool trial = !m_trialPath.isEmpty() && m_recordingSessionOutput == m_trialPath;
		const bool failed = !response["failure_stage"].toString().isEmpty();
		const bool playable = completed && response["output_published"].toBool(!failed) && QFileInfo(output).isFile();
		if (trial && (!completed || failed) && m_trialWorkspace) m_trialWorkspace->setAutoRemove(false);
		if (!completed) m_message->setText(Ui("录制后台已中断，可检查并恢复已录制的素材。"));
		else if (!response["error"].toString().isEmpty())
		{
			QString notice = Ui("录制提示：") + UiDiagnostic(response["error"].toString().trimmed());
			if (playable && response["failure_stage"] == "capture-source") notice += " " + Ui("已保存：") + output;
			m_message->setText(notice);
		}
		else if (playable && trial) m_message->setText(Ui("试录完成。可回看画面并分别试听声音。"));
		else if (playable) m_message->setText(Ui("已保存：") + output);
		m_overlay->dismiss();
		m_previewSocket->abort();m_previewReader->reset();m_cameraPreview->hide();m_liveFrame->setFrame({});m_liveFrame->clear();m_cameraFrame->clear();
		if (playable)
		{
			const bool recordPage = m_workspace->currentIndex() == Record;
			if (recordPage && !m_fullscreen->isVisible()) openPreview(output);
			else
			{
				auto row = new QListWidgetItem(QFileInfo(output).fileName());
				row->setData(Qt::UserRole, output);
				row->setToolTip(output);
				m_recent->insertItem(0, row);
				while (m_recent->count() > 50) delete m_recent->takeItem(m_recent->count() - 1);
				updatePlaybackMarkers();
				if (m_workspace->currentIndex() == Review) updateReviewLibrary();
				saveSettings();
			}
		}
		if (completed) m_recordingSessionOutput.clear();
		if (m_monitor->currentIndex() < 0) QTimer::singleShot(0, this, &KRRMainWindow::refreshMonitors);
	}
	if (m_active != active || completed)
	{
		m_active = active;
		syncQuickSources();
		if (!active)
		{
			m_scenePending = false;
			finishScene({});
		}
		if (m_clipEditor) m_clipEditor->setRecording(active);
		refreshMediaActions();
	}
}
void KRRMainWindow::displayLivePreview(const QImage& image, const QImage& camera, qint64 sequence)
{
	m_liveCameraReady = !camera.isNull();
	if (!image.isNull()) {
		const auto oldSize = m_liveFrame->frameSize();
		if (sequence != m_liveSequence) m_lastPreview.restart();
		m_liveSequence = sequence; m_liveFrame->setFrame(image);
		if (oldSize != image.size() || m_liveZoom == 0) refreshLiveZoom();
	}
	if (!camera.isNull() && m_showCameraPreview->isChecked()) {
		m_cameraFrame->setPixmap(QPixmap::fromImage(camera.scaled(m_cameraFrame->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation)));
		if (!m_cameraPreview->isVisible()) {
			if (SetWindowDisplayAffinity(reinterpret_cast<HWND>(m_cameraPreview->winId()), 0x11)) {
				auto screen=QGuiApplication::screenAt(frameGeometry().center()); if(!screen)screen=QGuiApplication::primaryScreen(); auto safe=screen->availableGeometry();
				m_cameraPreview->move(safe.right()-m_cameraPreview->width()-24,safe.bottom()-m_cameraPreview->height()-48);m_cameraPreview->show();
			} else { m_showCameraPreview->setChecked(false); m_message->setText(Ui("此会话无法排除摄像头悬浮窗，请在主窗口监看。")); }
		}
	} else m_cameraPreview->hide();
}
void KRRMainWindow::requestLivePreview()
{
	if (!m_active) { updateIdlePreview(); return; }
	if (m_workspace->currentIndex() != Record && !m_showCameraPreview->isChecked()) return;
	KRRPreviewFrame frame;
	if (m_previewReader->read(frame)) {
		if (m_previewSocket->state()!=QLocalSocket::UnconnectedState) m_previewSocket->abort();
		if (frame.sequence != m_liveSequence) displayLivePreview(frame.frame, frame.camera, frame.sequence);
		return;
	}
	// Compatibility fallback for older services or unavailable shared memory. Do not queue requests.
	if (m_previewSocket->state()!=QLocalSocket::UnconnectedState || (m_lastPreview.isValid() && m_lastPreview.elapsed()<160)) return;
	m_previewBytes.clear();m_previewSocket->connectToServer(ControlServerName());m_previewTimeout.start(1200);
}
void KRRMainWindow::refreshMonitors()
{
	if(!m_monitor)return;
	QString previous=m_monitor->currentData(Qt::UserRole+1).toString();bool blocked=m_monitor->blockSignals(true);m_monitor->clear();
	for(const auto& value:KRRFrameCapture::Monitors())
	{
		auto screen=value.toObject();int index=m_monitor->count();
		m_monitor->addItem(QString("%1 %2 · %3×%4 · %5%").arg(index+1).arg(screen["friendly_name"].toString() + (screen["hdr_active"].toBool()?" · HDR":"") + (screen["primary"].toBool()?Ui("（主显示器）"):"")).arg(screen["width"].toInt()).arg(screen["height"].toInt()).arg(screen["scale_percent"].toInt()),index);
		m_monitor->setItemData(index,screen["name"].toString(),Qt::UserRole+1);
		if(screen["name"].toString()==previous)m_monitor->setCurrentIndex(index);
	}
	if (m_active && m_monitor->findData(previous, Qt::UserRole + 1) < 0) m_monitor->setCurrentIndex(-1);
	m_monitor->blockSignals(blocked);
	if (m_bitrateHint && m_image && m_recommendedBitrate) updateBitrateRecommendation();
	if (m_image) syncQuickSources();
}
bool KRRMainWindow::validateStorage()
{
	const auto path = QDir::fromNativeSeparators(m_directory->text().trimmed());
	QString error;
	if (ProbeRecordingFolder(path, error))
	{
		m_directory->setText(QDir::cleanPath(path));
		return true;
	}
	if (m_defaultDirectory)
	{
		const auto candidate = QDir(DataDirectory()).filePath("Recordings");
		QString detail;
		if (candidate.compare(path, Qt::CaseInsensitive) != 0 && ProbeRecordingFolder(candidate, detail))
		{
			m_directory->setText(QDir::cleanPath(candidate));
			saveSettings();
			const auto notice = Ui("默认目录无法写入，本次保存到：%1").arg(m_directory->text());
			m_message->setText(notice);
			statusBar()->showMessage(notice, 9000);
			return true;
		}
	}
	const int integrity = ProcessIntegrityRid();
	const bool restricted = IsContainerProcess() || (integrity >= 0 && integrity < SECURITY_MANDATORY_MEDIUM_RID);
	if (restricted)
	{
		showError(Ui("当前程序以受限权限运行，无法写入 %1（%2）。请从 Windows 开始菜单或用户程序目录启动已安装的 WPS Recorder。")
			.arg(path.isEmpty() ? Ui("未设置") : path, error));
		return false;
	}
	showError(Ui("无法写入保存目录：%1\n%2\n请在“存储与倒计时”中选择可写文件夹。")
		.arg(path.isEmpty() ? Ui("未设置") : path, error));
	return false;
}
void KRRMainWindow::refreshMediaActions()
{
	const QFileInfo file(m_preview);
	const bool ready = !m_active && !m_countdownRemaining && !m_editor.isBusy() &&
		!m_preview.isEmpty() && m_preview != m_trialPath && m_player->duration() > 0 &&
		m_player->isVideoAvailable() && m_player->mediaStatus() != QMediaPlayer::InvalidMedia && file.isFile() && file.size() > 0;
	for (auto button : {m_submission, m_transcribe, m_otherExport})
		button->setEnabled(ready);
	if (m_clipEditor && m_workspace->currentIndex() == Edit)
		m_otherExport->setEnabled(!m_clipEditor->inputFile().isEmpty() && !m_clipEditor->isBusy());
}
static BOOL CALLBACK AddWindow(HWND window, LPARAM parameter)
{
	if (!IsWindowVisible(window) || IsIconic(window) || GetWindowTextLengthW(window) == 0) return TRUE;
	DWORD process = 0; GetWindowThreadProcessId(window, &process);
	if (process == GetCurrentProcessId() || (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)) return TRUE;
	wchar_t className[128]{}; GetClassNameW(window, className, 128);
	if (QString::fromWCharArray(className) == "Progman" || QString::fromWCharArray(className) == "WorkerW") return TRUE;
	wchar_t title[512]{}; GetWindowTextW(window, title, 512);
	auto combo = reinterpret_cast<QComboBox*>(parameter);
	combo->addItem(QString::fromWCharArray(title), QVariant::fromValue<qulonglong>(reinterpret_cast<quintptr>(window)));
	return TRUE;
}
void KRRMainWindow::refreshWindows()
{
	const auto selected = m_window->currentData();
	const QSignalBlocker windowSignals(m_window);
	m_window->clear();
	EnumWindows(AddWindow, reinterpret_cast<LPARAM>(m_window));
	if (selected.isValid() || m_source->currentData() == "window") m_window->setCurrentIndex(m_window->findData(selected));
	syncQuickSources();
}
void KRRMainWindow::takeScreenshot()
{
	KRRFrameCapture capture; QString error;
	const auto config = configuration();
	QImage image = capture.capture(config, error);
	if (image.isNull()) { showError(error); return; }
	ApplyRecordingPreviewEffects(image, config);
	QDir().mkpath(m_directory->text());
	QString path = QDir(m_directory->text()).filePath("screenshot-" + Stamp() + ".png");
	if (!image.save(path)) showError(Ui("无法保存截图，请检查目录和磁盘空间"));
	else m_message->setText(Ui("截图已保存：") + path);
}
void KRRMainWindow::openPreview(const QString& selected)
{
	const QString file = QFileInfo(selected).absoluteFilePath();
	if (!QFileInfo(file).isFile()) return;
	selectWorkspace(Review);
	m_player->stop();
	m_player->setMedia({});
	m_compatiblePreview->clear();
	m_video->clear();
	m_preview = file;
	m_reviewPendingSeek = -1;
	m_trimBegin->setValue(0);
	m_trimEnd->setValue(0);
	if (!m_fullscreen->isVisible()) m_previewStack->setCurrentIndex(1);
	m_playbackControls->show(); m_seek->show();
	if (NeedsCompatiblePreview(file))
	{
		m_message->setText(Ui("正在准备兼容预览…"));
		m_compatiblePreview->prepare(file);
	}
	else
	{
		m_player->setMedia(QUrl::fromLocalFile(file));
		m_player->play();
	}
	updatePlaybackMarkers();
	const bool trial = file == m_trialPath;
	m_trialTracks->setVisible(trial);
	m_trialReplay->setVisible(trial);
	refreshMediaActions();
	if (trial)
	{
		m_trialReplay->setEnabled(m_trialPlayback.state() == QProcess::NotRunning);
		m_trialTracks->clear();
		m_trialTracks->addItem(Ui("试录画面"), "video");
		if (m_trialAudioMode == "mic" || m_trialAudioMode == "mix") m_trialTracks->addItem(Ui("麦克风"), "0");
		if (m_trialAudioMode == "system" || m_trialAudioMode == "mix")
			m_trialTracks->addItem(Ui("系统声音"), m_trialAudioMode == "mix" ? "1" : "0");
		m_deletePreview->setEnabled(false);
		return;
	}
	for (int i = m_recent->count() - 1; i >= 0; --i) if (m_recent->item(i)->data(Qt::UserRole).toString() == file) delete m_recent->takeItem(i);
	auto item = new QListWidgetItem(QFileInfo(file).fileName()); item->setData(Qt::UserRole, file); item->setToolTip(file); m_recent->insertItem(0, item);
	while (m_recent->count() > 50) delete m_recent->takeItem(m_recent->count() - 1);
	updatePlaybackMarkers();
	describePreview(file);
	updateReviewLibrary();
	m_deletePreview->setEnabled(QFileInfo(file).isFile() && file == m_lastOutput);
	saveSettings();
}
void KRRMainWindow::deletePreview()
{
	const QString file = m_preview;
	if (m_active || file.isEmpty() || file != m_lastOutput || !QFileInfo(file).isFile())
		return;
	if (QMessageBox::question(this, Ui("删除录制"), Ui("将此视频移到回收站并放弃关联的剪辑草稿？\n\n") + QFileInfo(file).fileName(),
							  QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
		return;
	if (m_preview != file || m_active) return;
	const auto absolute = QFileInfo(file).absoluteFilePath();
	const bool usedForExport = m_editor.isBusy() && std::any_of(m_editorInputs.cbegin(), m_editorInputs.cend(),
		[&](const QString& input) { return QFileInfo(input).absoluteFilePath().compare(absolute, Qt::CaseInsensitive) == 0; });
	if (usedForExport || (m_clipEditor && !m_clipEditor->releaseIdleSource(file)))
	{
		showError(Ui("请先结束正在使用该视频的剪辑或导出。"));
		return;
	}
	m_player->stop();
	m_player->setMedia(QMediaContent());
	m_compatiblePreview->clear();
	m_video->clear();
	if (!QFile::moveToTrash(file))
	{
		openPreview(file);
		showError(Ui("无法移到回收站。请检查文件是否正在被其他程序使用。"));
		return;
	}
	if (m_clipEditor) m_clipEditor->discardReleasedSource(file);
	m_preview.clear();
	refreshMediaActions();
	m_lastOutput.clear();
	for (int i = m_recent->count() - 1; i >= 0; --i)
		if (m_recent->item(i)->data(Qt::UserRole).toString() == file) delete m_recent->takeItem(i);
	for (int i = m_clips->count() - 1; i >= 0; --i)
		if (m_clips->item(i)->data(Qt::UserRole).toString() == file) delete m_clips->takeItem(i);
	m_previewStack->setCurrentIndex(0);
	describePreview({});
	updatePlaybackMarkers();
	updateReviewLibrary();
	m_playbackControls->hide();
	m_seek->hide();
	m_deletePreview->setEnabled(false);
	m_message->setText(Ui("已移到回收站"));
	saveSettings();
}
void KRRMainWindow::syncCaptureButtons()
{
	const auto audio = m_audio->currentData().toString();
	const bool mic = audio == "mic" || audio == "mix";
	const bool pipAvailable = m_camera->currentData().toInt() >= 0 && m_source->currentData() != "camera";
	m_cameraToggle->setEnabled(pipAvailable);
	m_cameraEnabled->setEnabled(pipAvailable);
	m_cameraToggle->setChecked(m_cameraEnabled->isChecked());
	m_micToggle->setChecked(mic);
	m_settingsMic->setChecked(mic);
	m_floatBar->setCaptureButtons(m_cameraEnabled->isChecked(), mic, pipAvailable, true);
}
void KRRMainWindow::toggleMic(bool enabled)
{
	const auto audio = m_audio->currentData().toString();
	const bool system = audio == "mix" || audio == "system";
	m_audio->setCurrentIndex(m_audio->findData(enabled ? (system ? "mix" : "mic") : (system ? "system" : "none")));
	sendScene();
	saveSettings();
}
void KRRMainWindow::showError(const QString& message)
{
	const auto localized = UiDiagnostic(message);
	m_message->setText(localized); QMessageBox::warning(this, "WPS Recorder", localized);
}
KRRMainWindow::~KRRMainWindow()
{
	m_previewTimer.stop();
	m_meterTimer.stop();
	releaseIdlePreview();
	for (auto process : {&m_reviewProbe, &m_reviewThumbnail})
	{
		process->blockSignals(true);
		if (process->state() != QProcess::NotRunning) { process->kill(); process->waitForFinished(1000); }
	}
	m_pollTimer.stop();
	cancelStatusPoll();
	if (m_trialPlayback.state() != QProcess::NotRunning)
	{
		m_trialPlayback.blockSignals(true);
		m_trialPlayback.kill();
		m_trialPlayback.waitForFinished(1000);
	}
	// Media Foundation calls the attached surface while releasing the player.
	m_player->blockSignals(true);
	delete m_player;
	m_player = nullptr;
	m_compatiblePreview->clear();
	if (m_trialWorkspace && m_recordingSessionOutput == m_trialPath) m_trialWorkspace->setAutoRemove(false);
	unregisterHotkeys();
	delete m_overlay;
	delete m_cameraPreview;
	delete m_floatBar;
}
void KRRMainWindow::selectRecordingRegion()
{
	QWidget *owner = this;
	if (m_settingsDialog->isVisible()) owner = m_settingsDialog;
	KRRRegionSelector selector(m_region, owner);
	if (selector.exec() != QDialog::Accepted) return;
	m_region = selector.selectedRegion();
	m_source->setCurrentIndex(1);
	m_regionLabel->setText(QString("%1, %2 · %3 × %4").arg(m_region.x()).arg(m_region.y())
		.arg(m_region.width()).arg(m_region.height()));
	updateBitrateRecommendation();
	syncQuickSources();
	sendScene();
	saveSettings();
}
void KRRMainWindow::showSettings(int page)
{
	refreshMonitors();
	m_settingsNav->setCurrentRow(page); m_settingsDialog->show(); m_settingsDialog->raise(); m_settingsDialog->activateWindow();
}
void KRRMainWindow::sendScene()
{
	m_idleConfigClock.invalidate();
	QString error;
	if (!ValidateEffects(m_effects, error)) { m_message->setText(UiDiagnostic(error) + Ui("，请清空部分笔迹后继续")); return; }
	if (!m_active) return;
	if (m_source->currentData() == "screen" && m_monitor->currentIndex() < 0) return;
	if (m_sceneTimeout.isActive())
	{
		m_scenePending = true;
		return;
	}
	auto config = configuration().toJson(); QJsonObject scene;
	for (const auto& key : QStringList{"screenColor", "screenWhiteNits", "screenExposureEV", "source", "monitor", "monitorDevice", "x", "y", "rw", "rh", "window", "image", "cameraEnabled", "cameraShape", "cameraMirror", "cameraRect", "cameraBorder", "effects", "cursor", "clicks", "micGain", "systemGain", "audioSources"}) scene[key] = config[key];
	scene["micEnabled"] = config["audio"] == "mic" || config["audio"] == "mix";
	scene["systemEnabled"] = config["audio"] == "system" || config["audio"] == "mix";
	m_scenePending = false;
	m_sceneRequest = QJsonDocument(QJsonObject{{"command", "scene"}, {"scene", scene}}).toJson(QJsonDocument::Compact) + '\n';
	m_sceneTimeout.start(1500);
	m_sceneSocket->connectToServer(ControlServerName());
}
void KRRMainWindow::finishScene(const QString& error)
{
	m_sceneTimeout.stop();
	const QSignalBlocker blocker(m_sceneSocket);
	m_sceneSocket->abort();
	if (!error.isEmpty() && m_active)
		m_message->setText(Ui("场景更新失败：") + UiDiagnostic(error));
	if (m_scenePending && m_active) m_effectTimer.start(0);
}
void KRRMainWindow::showAnnotation()
{
	if (m_overlay->isVisible()) { m_overlay->dismiss(); return; }
	QString error;
	if (!m_overlay->present(KRRFrameCapture::CaptureArea(configuration()), m_effects, error)) { m_lectureButton->setChecked(false); showError(error); }
}
void KRRMainWindow::registerHotkeys()
{
	unregisterHotkeys();
	if (m_hotkeyEditing) return;
	QStringList failures;
	const auto window = reinterpret_cast<HWND>(winId());
	for (int i = 0; i < RecordingHotkeys::Count; ++i)
		if (!m_hotkeys[i].isEmpty() && !RecordingHotkeys::Register(window, i, m_hotkeys[i])) failures << m_hotkeys[i];
	const auto warning = Ui("部分全局热键不可用，请在设置中调整：");
	if (!failures.isEmpty()) m_message->setText(warning + failures.join(", "));
	else if (m_message->text().startsWith(warning)) m_message->clear();
}

void KRRMainWindow::unregisterHotkeys()
{
	const auto window = reinterpret_cast<HWND>(winId());
	for (int i = 0; i < RecordingHotkeys::Count; ++i)
		UnregisterHotKey(window, RecordingHotkeys::FirstId + i);
}

void KRRMainWindow::setHotkeyEditing(bool editing)
{
	if (m_hotkeyEditing == editing) return;
	m_hotkeyEditing = editing;
	registerHotkeys();
}

bool KRRMainWindow::setHotkeys(const QStringList& keys, QString& error)
{
	if (keys.size() != RecordingHotkeys::Count)
	{
		error = Ui("快捷键列表不完整。");
		return false;
	}
	QStringList normalized;
	QSet<quint64> assigned;
	for (const auto& value : keys)
	{
		if (value.trimmed().isEmpty()) { normalized << QString(); continue; }
		const auto sequence = QKeySequence(value, QKeySequence::PortableText);
		UINT modifiers = 0, key = 0;
		if (!RecordingHotkeys::Binding(sequence, modifiers, key))
		{
			error = Ui("请使用含 Ctrl、Alt 或 Win 的单个快捷键组合。");
			return false;
		}
		const quint64 binding = (static_cast<quint64>(modifiers) << 32) | key;
		if (assigned.contains(binding))
		{
			error = Ui("热键重复，请为每项选择不同组合");
			return false;
		}
		assigned.insert(binding);
		normalized << sequence.toString(QKeySequence::PortableText);
	}
	if (normalized == m_hotkeys) return true;

	unregisterHotkeys();
	const auto window = reinterpret_cast<HWND>(winId());
	for (int i = 0; i < normalized.size(); ++i)
	{
		if (normalized[i].isEmpty() || RecordingHotkeys::Register(window, i, normalized[i]) || normalized[i] == m_hotkeys[i])
			continue;
		registerHotkeys();
		error = Ui("无法注册快捷键：%1").arg(normalized[i]);
		return false;
	}
	const auto previous = m_hotkeys;
	m_hotkeys = normalized;
	if (!saveSettings())
	{
		m_hotkeys = previous;
		registerHotkeys();
		error = Ui("无法保存快捷键。");
		return false;
	}
	registerHotkeys();
	return true;
}

QStringList KRRMainWindow::hotkeyLabels() const
{
	return {Ui("开始录制"), Ui("暂停 / 继续"), Ui("停止保存"), Ui("下一屏幕"),
		Ui("切换摄像头"), Ui("切换图片"), Ui("画笔 / 操作"), Ui("录制打点"), Ui("极简模式")};
}

void KRRMainWindow::configureHotkeys()
{
	QDialog dialog(this);
	dialog.setObjectName("globalHotkeysDialog");
	dialog.setWindowTitle(Ui("自定义全局热键"));
	dialog.setMinimumWidth(RecorderUi::Px(430));
	auto layout = new QFormLayout(&dialog);
	QList<QKeySequenceEdit*> edits;
	auto hint = new QLabel(Ui("留空即停用该项全局热键。"), &dialog);
	hint->setWordWrap(true);
	hint->setProperty("muted", true);
	layout->addRow(hint);
	const auto names = hotkeyLabels();
	for (int i = 0; i < names.size(); ++i)
	{
		auto edit = new QKeySequenceEdit(QKeySequence(m_hotkeys.value(i), QKeySequence::PortableText), &dialog);
		edit->setObjectName(QString("globalHotkey%1").arg(i));
		edit->setAccessibleName(names[i]);
		if (auto input = edit->findChild<QLineEdit*>()) input->setPlaceholderText(Ui("未设置"));
		auto clear = new QPushButton(Ui("清除"), &dialog);
		clear->setObjectName(QString("clearGlobalHotkey%1").arg(i));
		clear->setToolTip(Ui("停用此全局热键"));
		clear->setAccessibleName(Ui("清除热键：%1").arg(names[i]));
		clear->setEnabled(!edit->keySequence().isEmpty());
		connect(clear, &QPushButton::clicked, edit, &QKeySequenceEdit::clear);
		connect(edit, &QKeySequenceEdit::keySequenceChanged, clear, [clear](const QKeySequence &key) {
			clear->setEnabled(!key.isEmpty());
		});
		auto row = new QHBoxLayout;
		row->addWidget(edit, 1);
		row->addWidget(clear);
		layout->addRow(names[i], row);
		edits << edit;
	}
	auto clearAll = new QPushButton(Ui("全部清空"), &dialog);
	clearAll->setObjectName("clearAllGlobalHotkeys");
	connect(clearAll, &QPushButton::clicked, &dialog, [edits] {
		for (auto *edit : edits) edit->clear();
	});
	layout->addRow(clearAll);
	auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog); layout->addRow(buttons); connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	setHotkeyEditing(true);
	const auto accepted = dialog.exec() == QDialog::Accepted;
	setHotkeyEditing(false);
	if (!accepted) return;
	QStringList keys;
	for (auto edit : edits) keys << edit->keySequence().toString(QKeySequence::PortableText);
	QString error;
	if (!setHotkeys(keys, error)) showError(error);
}
bool KRRMainWindow::nativeEvent(const QByteArray& eventType, void* message, long* result)
{
	auto native = static_cast<MSG*>(message);
	if (native->message == WM_CLOSE)
	{
#ifdef WPS_WEB_STUDIO
		if (!m_webStudio || !m_webStudio->requestFrontendClose())
#endif
			QTimer::singleShot(0, this, [this] { requestUserClose(); });
		*result = 0;
		return true;
	}
	if (native->message == WM_NCHITTEST && m_studioHeader && isVisible())
	{
		const auto pos = mapFromGlobal(QPoint(GET_X_LPARAM(native->lParam), GET_Y_LPARAM(native->lParam)));
		if (!isMaximized() && !isFullScreen() && rect().contains(pos))
		{
			const int edge = RecorderUi::Px(4);
			const bool left = pos.x() < edge, right = pos.x() >= width() - edge;
			const bool top = pos.y() < edge, bottom = pos.y() >= height() - edge;
			if (top || bottom || left || right)
			{
				*result = top ? (left ? HTTOPLEFT : right ? HTTOPRIGHT : HTTOP)
					: bottom ? (left ? HTBOTTOMLEFT : right ? HTBOTTOMRIGHT : HTBOTTOM)
					: left ? HTLEFT : HTRIGHT;
				return true;
			}
		}
		const auto local = m_studioHeader->mapFrom(this, pos);
		if (m_studioHeader->rect().contains(local))
		{
			auto child = m_studioHeader->childAt(local);
			if (!child || qobject_cast<QLabel*>(child))
			{
				*result = HTCAPTION;
				return true;
			}
		}
	}
	if (native->message != WM_HOTKEY) return QMainWindow::nativeEvent(eventType, message, result);
	int action = static_cast<int>(native->wParam) - RecordingHotkeys::FirstId;
	if (action < 0 || action >= RecordingHotkeys::Count || m_hotkeyEditing || m_hotkeys.value(action).isEmpty())
		return QMainWindow::nativeEvent(eventType, message, result);
	if (action == 0 && m_start->isEnabled()) beginCountdown();
	else if (action == 1 && m_pause->isEnabled()) m_pause->click();
	else if (action == 2 && m_stop->isEnabled()) stopRecording();
	else if (action == 3) { m_source->setCurrentIndex(0); if (m_monitor->count()) m_monitor->setCurrentIndex((m_monitor->currentIndex() + 1) % m_monitor->count()); sendScene(); }
	else if (action == 4) { m_source->setCurrentIndex(m_source->currentData() == "camera" ? 0 : 3); sendScene(); }
	else if (action == 5) { m_source->setCurrentIndex(4); sendScene(); }
	else if (action == 6) { if (!m_overlay->isVisible()) showAnnotation(); else m_overlay->toggleInput(); }
	else if (action == 7 && m_addMarker->isEnabled()) addRecordingMarker();
	else if (action == RecordingHotkeys::Minimal) switchRecordingMode();
	*result = 0; return true;
}
void KRRMainWindow::exportVideo()
{
	if (m_preview.isEmpty()) { showError(Ui("请先录制或打开视频")); return; }
	QString extension = m_exportFormat->currentText();
	QString output = QFileDialog::getSaveFileName(this, Ui("导出视频"), m_directory->text() + "/export-" + Stamp() + "." + extension, extension.toUpper() + " (*." + extension + ")");
	if (output.isEmpty()) return;
	KRRExportOptions options; options.encoder = m_exportEncoder->currentData().toString(); options.denoise = m_exportDenoise->isChecked(); options.subtitles = m_subtitles->text();
	options.copyStreams = m_copyStreams->isChecked();
	if (!m_editor.exportMedia(m_preview, output, m_trimBegin->value(), m_trimEnd->value(), options)) showError(Ui("请等待当前任务完成"));
	else
	{
		m_editorInputs = QStringList{m_preview};
		m_message->setText(Ui("正在导出… 可选择已配置的 OneDrive 等同步目录作为保存位置。"));
	}
}
void KRRMainWindow::transcribeVideo()
{
    if(m_preview.isEmpty()){showError(Ui("请先录制或打开包含音频的视频。"));return;}
	if (m_active) { m_message->setText(Ui("停止录制后可处理字幕和配音。")); return; }
	if (!m_clipEditor->openFile(m_preview, m_directory->text())) return;
	m_player->pause();
	selectWorkspace(Edit);
	m_clipEditor->show();
	m_clipEditor->showSubtitleWorkflow();
}
void KRRMainWindow::showSubmission()
{
	if (m_active || m_editor.isBusy())
	{
		m_message->setText(Ui("请在录制或后台任务结束后生成提交档。"));
		return;
	}
	if (m_preview.isEmpty() || m_preview == m_trialPath)
	{
		m_message->setText(Ui("请先打开正式录制的视频。"));
		return;
	}
	KRREditProject project;
	QString error;
	if (!ProbeEditSource(m_preview, project, error))
	{
		showError(error);
		return;
	}
	const auto begin = qRound64(m_trimBegin->value() * 1000);
	const auto end = m_trimEnd->value() > 0 ? qMin(project.duration, qRound64(m_trimEnd->value() * 1000)) : project.duration;
	if (end - begin < 20)
	{
		showError(Ui("提交选区无效，请检查入点和出点。"));
		return;
	}
	project.kept = {{begin, end}};
	if (!m_subtitles->text().isEmpty() && !ReadEditSubtitles(m_subtitles->text(), project.captions, error))
	{
		showError(error);
		return;
	}
	const auto at = m_player->position() - begin;
	m_player->pause();
	KRRSubmission plan;
	if (ChooseSubmission(this, project, m_directory->text(), at, plan) &&
		m_editor.renderTimeline(plan.project, plan.output, "auto", plan.separateAudio, plan.burnCaptions, plan.options))
	{
		m_editorInputs = QStringList{plan.project.input};
		m_message->setText(Ui("正在导出提交成片…"));
	}
}
void KRRMainWindow::scenePreset(bool save)
{
	const QStringList keys{"screenColor", "screenWhiteNits", "screenExposureEV", "source", "monitor", "x", "y", "rw", "rh", "window", "image", "cameraEnabled", "cameraShape", "cameraMirror", "cameraRect", "cameraBorder", "effects", "cursor", "clicks"};
	QString path = save ? QFileDialog::getSaveFileName(this, Ui("保存场景预设"), m_directory->text() + "/scene.json", "JSON (*.json)") : QFileDialog::getOpenFileName(this, Ui("加载场景预设"), m_directory->text(), "JSON (*.json)");
	if (path.isEmpty()) return;
	QFile file(path);
	if (save)
	{
		auto config = configuration().toJson(); QJsonObject scene; for (const auto& key : keys) scene[key] = config[key];
		if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(QJsonObject{{"version", 1}, {"scene", scene}}).toJson()) < 0) { showError(Ui("无法保存场景预设")); return; }
		m_message->setText(Ui("场景已保存：") + path); return;
	}
	if (!file.open(QIODevice::ReadOnly) || file.size() > 2 * RecordingEffectsByteLimit) { showError(Ui("无法读取场景预设")); return; }
	QJsonParseError parse; auto object = QJsonDocument::fromJson(file.readAll(), &parse).object();
	if (parse.error != QJsonParseError::NoError || object["version"].toInt() != 1 || !object["scene"].isObject()) { showError(Ui("需要 version:1 和 scene 对象")); return; }
	auto scene = object["scene"].toObject(), config = configuration().toJson();
	for (auto it = scene.begin(); it != scene.end(); ++it) { if (!keys.contains(it.key())) { showError(Ui("未知场景字段：") + it.key()); return; } config[it.key()] = it.value(); }
	auto candidate = KRRRecordingConfig::FromJson(config); QString error;
	if (!candidate.validate(error)) { showError(error); return; }
	applySceneConfiguration(candidate);
	saveSettings();
	m_message->setText(Ui("场景已加载：") + path);
}
void KRRMainWindow::applySceneConfiguration(const KRRRecordingConfig& candidate)
{
	m_source->setCurrentIndex(m_source->findData(candidate.source));
	const int monitorIndex = candidate.monitorDevice.isEmpty() ? candidate.monitor :
		m_monitor->findData(candidate.monitorDevice, Qt::UserRole + 1);
	m_monitor->setCurrentIndex(monitorIndex);
	m_image->setText(candidate.image);
	m_region = candidate.region;
	const int windowIndex = m_window->findData(QVariant::fromValue<qulonglong>(candidate.window));
	if (windowIndex >= 0 || candidate.source == "window") m_window->setCurrentIndex(windowIndex);
	m_cameraEnabled->setChecked(candidate.cameraEnabled); m_cameraMirror->setChecked(candidate.cameraMirror); m_cameraShape->setCurrentIndex(m_cameraShape->findData(candidate.cameraShape));
	m_cameraX->setValue(qRound(candidate.cameraRect.x() * 100)); m_cameraY->setValue(qRound(candidate.cameraRect.y() * 100)); m_cameraW->setValue(qRound(candidate.cameraRect.width() * 100)); m_cameraH->setValue(qRound(candidate.cameraRect.height() * 100)); m_cameraBorder = candidate.cameraBorder;
	m_screenColor->setCurrentIndex(m_screenColor->findData(candidate.screenColor));m_screenExposure->setValue(candidate.screenExposureEV);m_screenWhite->setValue(qRound(candidate.screenWhiteNits));
	m_cursor->setChecked(candidate.cursor); m_clicks->setChecked(candidate.clicks);
	if (!m_active)
	{
		m_resolution->setCurrentText(QString("%1x%2").arg(candidate.width).arg(candidate.height));
		m_fps->setCurrentIndex(m_fps->findData(candidate.fps));
		m_bitrate->setValue(candidate.bitrate);
		m_audio->setCurrentIndex(m_audio->findData(candidate.audio));
		m_separateAudio->setChecked(candidate.separateAudio);
		m_encoder->setCurrentIndex(m_encoder->findData(candidate.encoder));
		m_outputColor->setCurrentIndex(qMax(0, m_outputColor->findData(candidate.outputColor)));
		m_unavailableCameraIndex = candidate.camera;
		m_camera->setCurrentIndex(m_camera->findData(candidate.camera));
		m_cameraTrack->setChecked(candidate.cameraTrack);
		m_denoise->setChecked(candidate.denoise);
		if (m_additionalAudio != candidate.audioSources)
		{
			m_additionalAudio = candidate.audioSources;
			rebuildAudioSources();
		}
	}
	m_micGain->setValue(qRound(candidate.micGain * 100));
	m_systemGain->setValue(qRound(candidate.systemGain * 100));
	if (m_region.isValid()) m_regionLabel->setText(QString("%1, %2 · %3 × %4").arg(m_region.x()).arg(m_region.y()).arg(m_region.width()).arg(m_region.height()));
	m_overlay->dismiss(); m_effects = candidate.effects; syncQuickSources(); sendScene();
}
bool KRRMainWindow::saveSettings()
{
	QSettings settings(RecorderUiSettingsPath(), QSettings::IniFormat);
	settings.setIniCodec("UTF-8");
	settings.setValue("screenColor",m_screenColor->currentData());settings.setValue("screenExposureEV",m_screenExposure->value());settings.setValue("screenWhiteNits",m_screenWhite->value());
	settings.setValue("outputColor", m_outputColor->currentData());
	settings.setValue("showFloatingBar", m_floatButton->isChecked());
	if (m_studioReady)
	{
		rememberSource();
		settings.setValue("studioSources", QJsonDocument(m_visualSources).toJson(QJsonDocument::Compact));
		settings.setValue("studioSourceIndex", m_sourceIndex);
		settings.setValue("studioSceneIndex", m_sceneIndex);
	}
	if (m_recordDocks)
	{
		QStringList order;
		for (int i = 0; i < m_recordDocks->count(); ++i) order << m_recordDocks->widget(i)->objectName();
		settings.setValue("studioDockOrder", order);
		settings.setValue("studioDockSizes", m_recordDocks->saveState());
		settings.setValue("studioRecordSizes", m_recordLayout->saveState());
	}
	settings.setValue("studioGeometry", saveGeometry());
	auto minimalProfile = configuration();
	minimalProfile.output.clear();
	minimalProfile.effects = {};
	settings.setValue("minimalRecordingProfile", QJsonDocument(minimalProfile.toJson()).toJson(QJsonDocument::Compact));
	settings.setValue("directory", m_directory->text());
	settings.setValue("defaultDirectory", m_defaultDirectory);
	settings.setValue("resolution", m_resolution->currentText() == m_resolution->itemText(0) ? QString("auto") : m_resolution->currentText());
	settings.setValue("fps", m_fps->currentData()); settings.setValue("bitrate", m_bitrate->value());
	settings.setValue("recommendedBitrate", m_recommendedBitrate->isChecked());
	settings.setValue("audio", m_audio->currentIndex()); settings.setValue("source", m_source->currentIndex());
	settings.setValue("micGain", m_micGain->value()); settings.setValue("systemGain", m_systemGain->value());
	settings.setValue("studioAudioSources", QJsonDocument(m_additionalAudio).toJson(QJsonDocument::Compact));
	settings.setValue("format", m_format->currentText()); settings.setValue("countdown", m_countdown->currentIndex());
	settings.setValue("cursor", m_cursor->isChecked()); settings.setValue("clicks", m_clicks->isChecked());
	settings.setValue("radius", m_radius->value()); settings.setValue("color", m_clickColor); settings.setValue("region", m_region);
	settings.setValue("monitor", m_monitor->currentIndex()); settings.setValue("encoder", m_encoder->currentData());
	settings.setValue("cameraEnabled", m_cameraEnabled->isChecked()); settings.setValue("cameraTrack", m_cameraTrack->isChecked()); settings.setValue("cameraMirror", m_cameraMirror->isChecked()); settings.setValue("cameraShape", m_cameraShape->currentIndex());
	settings.setValue("showCameraPreview",m_showCameraPreview->isChecked());
	settings.setValue("cameraX", m_cameraX->value()); settings.setValue("cameraY", m_cameraY->value()); settings.setValue("cameraW", m_cameraW->value()); settings.setValue("cameraH", m_cameraH->value()); settings.setValue("cameraBorder", m_cameraBorder);
	settings.setValue("image", m_image->text()); settings.setValue("separateAudio", m_separateAudio->isChecked()); settings.setValue("denoise", m_denoise->isChecked()); settings.setValue("hotkeys", m_hotkeys); settings.setValue("lastOutput", m_lastOutput);
	QStringList recent; for (int i = 0; i < m_recent->count(); ++i) recent << m_recent->item(i)->data(Qt::UserRole).toString(); settings.setValue("recent", recent);
	settings.sync();
	if (settings.status() == QSettings::NoError) return true;
	m_message->setText(Ui("无法保存设置：") + settings.fileName());
	return false;
}
void KRRMainWindow::changeEvent(QEvent* event)
{
	QMainWindow::changeEvent(event);
	if (m_windowMaximize && event->type() == QEvent::WindowStateChange)
		m_windowMaximize->setText(isMaximized() ? QStringLiteral("❐") : QStringLiteral("□"));
}
bool KRRMainWindow::requestUserClose()
{
	const QScopedValueRollback<bool> userClosing(m_userClosing, true);
	return close();
}

bool KRRMainWindow::canEnterMinimalMode() const
{
#ifdef WPS_WEB_STUDIO
	if (m_webStudio && m_webStudio->workInProgress()) return false;
#endif
	return !m_active && !m_startPending && !m_countdownRemaining && !m_editor.isBusy() && !m_clipEditor->isBusy();
}

void KRRMainWindow::showBackgroundBar()
{
	hide();
	releaseIdlePreview();
	m_floatBar->setSwitchToMinimal(false);
	m_floatButton->setChecked(true);
	QString error;
	if (!m_floatBar->isVisible() && !m_floatBar->present(error, m_active))
	{
		restoreWorkbench();
		showError(error);
	}
}

void KRRMainWindow::restoreWorkbench()
{
	if (isMinimized()) showNormal();
	else show();
	selectWorkspace(Record);
	m_floatBar->setSwitchToMinimal(true);
	raise();
	activateWindow();
	emit workbenchRestored();
}

void KRRMainWindow::switchRecordingMode()
{
	if (!isVisible()) { restoreWorkbench(); return; }
#ifdef WPS_WEB_STUDIO
	if (m_webStudio && m_webStudio->requestFrontendMinimal()) return;
#endif
	emit minimalRecorderRequested();
}

void KRRMainWindow::closeEvent(QCloseEvent* event)
{
	if (m_confirmingExit)
	{
		event->ignore();
		return;
	}
	const QScopedValueRollback<bool> confirming(m_confirmingExit, true);
	bool busy = m_editor.isBusy() || m_clipEditor->isBusy();
#ifdef WPS_WEB_STUDIO
	if (m_webStudio) busy |= m_webStudio->workInProgress();
#endif
	const auto mode = busy || m_userClosing ? ChooseTaskExit(this, false, m_userClosing, busy) : KRRTaskExit::Save;
	if (mode == KRRTaskExit::Background)
	{
		event->ignore();
		saveSettings();
		emit backgroundRequested();
		return;
	}
	if (mode == KRRTaskExit::Return)
	{
		event->ignore();
		return;
	}
	if ((m_active || m_startPending) &&
		QMessageBox::question(this, Ui("录制仍在进行"), Ui("关闭界面后，后台录制会继续。仍要关闭吗？")) != QMessageBox::Yes)
	{
		event->ignore(); return;
	}
	if (mode == KRRTaskExit::Save)
	{
		bool saved = m_clipEditor->confirmApplicationClose();
#ifdef WPS_WEB_STUDIO
		if (m_webStudio) saved = m_webStudio->saveExitDraft() && saved;
#endif
		if (!saved && ChooseTaskExit(this, true) != KRRTaskExit::Discard)
		{
			event->ignore();
			return;
		}
	}
	m_editor.cancel();
	m_clipEditor->cancelTasks();
#ifdef WPS_WEB_STUDIO
	if (m_webStudio) m_webStudio->cancelExitWork();
#endif
	saveSettings();
	event->accept();
}
