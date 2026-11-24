#include "uilanguage.h"
#include "clipeditor.h"
#include "taskexit.h"
#include "submissiondialog.h"
#include "recordingmarkers.h"
#include "editwidgets.h"
#include "editsequencedialog.h"
#include "subtitledialog.h"
#include "smartcutdialog.h"
#include "recordingconfig.h"
#include "uitheme.h"
#include "playbackslider.h"
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QColorDialog>
#include <QFontComboBox>
#include <QInputDialog>
#include <QLineEdit>
#include <QRegularExpression>
#include <QDateTime>
#include <QCryptographicHash>
#include <QDir>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QListWidget>
#include <QMediaPlayer>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QScreen>
#include <QShortcut>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <numeric>
static int P(int n)
{
	return RecorderUi::Px(n);
}
static QString StampEdit()
{
	return QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss-zzz");
}
KRRClipEditor::KRRClipEditor(QWidget *parent) : QDialog(parent), m_analysis(this)
{
	qRegisterMetaType<KRREditProject>();
	qRegisterMetaType<QVector<float>>();
	setWindowTitle(Ui("WPS Recorder · 可视化剪辑"));
	setWindowFlag(Qt::WindowContextHelpButtonHint, false);
	setWindowFlag(Qt::WindowMaximizeButtonHint, true);
	auto available = QGuiApplication::primaryScreen()->availableGeometry().size();
	resize(QSize(P(1200), P(820)).boundedTo(available - QSize(P(30), P(40))));
	setMinimumSize(qMin(P(640), available.width() - P(20)), qMin(P(440), available.height() - P(40)));
	auto root = new QVBoxLayout(this);
	root->setContentsMargins(4, 4, 4, 4);
	root->setSpacing(4);
	auto header = new QHBoxLayout;
	auto import = new QPushButton(Ui("导入视频"), this);
	m_title = new QLabel(Ui("画面与时间轴同步剪辑"), this);
	m_title->setProperty("heading", true);
	m_title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
	m_undoButton = new QPushButton(Ui("撤销"), this);
	m_redoButton = new QPushButton(Ui("重做"), this);
	auto projectMenu = new QPushButton(Ui("工程 / 工具"), this);
	auto menu = new QMenu(projectMenu);
	projectMenu->setMenu(menu);
	auto exportButton = new QPushButton(Ui("导出成片"), this);
	exportButton->setObjectName("exportTimelineButton");
	exportButton->setProperty("role", "primary");
	header->addWidget(import);
	header->addWidget(m_title, 1);
	header->addWidget(m_undoButton);
	header->addWidget(m_redoButton);
	header->addWidget(projectMenu);
	auto smartCut = new QPushButton(Ui("智能剪辑"), this);
	smartCut->setObjectName("openSmartCut");
	header->addWidget(smartCut);
	connect(smartCut, &QPushButton::clicked, this, &KRRClipEditor::showSmartCut);
	auto submission = new QPushButton(Ui("提交档"), this);
	submission->setObjectName("editorSubmission");
	header->addWidget(submission);
	header->addWidget(exportButton);
	m_mediaControls = {smartCut, submission, exportButton};
	root->addLayout(header);
	connect(import, &QPushButton::clicked, this,
			[this]
			{
				auto file = QFileDialog::getOpenFileName(this, Ui("导入待剪辑视频"), m_folder,
														 Ui("视频 (*.mp4 *.mov *.mkv *.avi *.webm *.flv)"));
				if (!file.isEmpty())
					openFile(file, m_folder);
			});
	auto saveEdit = menu->addAction(Ui("保存剪辑工程…"));
	connect(saveEdit, &QAction::triggered, this, &KRRClipEditor::saveProject);
	connect(menu->addAction(Ui("打开剪辑工程…")), &QAction::triggered, this, &KRRClipEditor::loadProject);
	auto sequence = menu->addAction(Ui("多素材顺排…"));
	sequence->setObjectName("openEditSequence");
	connect(sequence, &QAction::triggered, this, [this]
	{
		m_player->pause();
		KRREditSequenceDialog dialog(m_project, m_folder, this);
		connect(this, &KRRClipEditor::smartCutInterrupted, &dialog, &KRREditSequenceDialog::interruptSmartCut);
		dialog.exec();
	});
	auto batch = menu->addAction(Ui("批量剪切…"));
	batch->setObjectName("openBatchCut");
	connect(batch, &QAction::triggered, this, &KRRClipEditor::showBatchCut);
	auto protect = menu->addAction(Ui("保留所选片段为必保"));
	protect->setObjectName("protectEditRange");
	connect(protect, &QAction::triggered, this, [this] { changeProtectedRanges(true); });
	auto unprotect = menu->addAction(Ui("解除片段必保"));
	connect(unprotect, &QAction::triggered, this, [this] { changeProtectedRanges(false); });
	auto snap = menu->addAction(Ui("吸附打点与片段边缘"));
	snap->setObjectName("editTimelineSnap");
	snap->setCheckable(true);
	connect(snap, &QAction::toggled, this, [this](bool enabled) { m_timeline->setSnapEnabled(enabled); });
	auto loadSrt = menu->addAction(Ui("导入 SRT 字幕…"));
	connect(loadSrt, &QAction::triggered, this,
			[this]
			{
				auto path = QFileDialog::getOpenFileName(this, Ui("导入 UTF-8 SRT 字幕"), m_folder, "SRT (*.srt)");
				if (!path.isEmpty())
					importCaptions(path);
			});
	auto saveSrt = menu->addAction(Ui("导出剪辑后的 SRT…"));
	connect(saveSrt, &QAction::triggered, this,
			[this]
			{
				if (m_project.input.isEmpty())
					return;
				auto path = QFileDialog::getSaveFileName(this, Ui("导出重排时间后的字幕"),
														 m_folder + "/edited-" + StampEdit() + ".srt", "SRT (*.srt)");
				QString error;
				if (!path.isEmpty() && !WriteEditSubtitles(path, m_project.outputCaptions(), error))
					m_status->setText(UiDiagnostic(error));
			});
	menu->addSeparator();
	auto speech = menu->addAction(Ui("字幕与配音…"));
	speech->setObjectName("editorSpeech");
	connect(speech, &QAction::triggered, this, &KRRClipEditor::showSubtitleWorkflow);
	m_mediaActions = {saveEdit, batch, protect, unprotect, snap, loadSrt, saveSrt, speech};
	connect(menu->addAction(Ui("多文件合并 / 批量拆分…")), &QAction::triggered, this, &KRRClipEditor::batchRequested);
	connect(m_undoButton, &QPushButton::clicked, this, [this] { undo(); });
	connect(m_redoButton, &QPushButton::clicked, this, [this] { undo(true); });
	connect(exportButton, &QPushButton::clicked, this, &KRRClipEditor::exportProject);
	connect(submission, &QPushButton::clicked, this, &KRRClipEditor::exportSubmission);
	auto panes = new QSplitter(Qt::Horizontal, this);
	panes->setObjectName("editWorkbenchPanes");
	panes->setChildrenCollapsible(false);
	m_assetsTabs = new QTabWidget(panes);
	m_assetsTabs->setMinimumWidth(P(116));
	auto assets = new QWidget(m_assetsTabs);
	auto assetsLayout = new QVBoxLayout(assets);
	assetsLayout->setContentsMargins(5, 5, 5, 5);
	m_materials = new QListWidget(assets);
	m_materials->setWordWrap(true);
	m_materials->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	assetsLayout->addWidget(m_materials, 1);
	auto addMaterial = new QPushButton(Ui("导入素材 / 顺排"), assets);
	connect(addMaterial, &QPushButton::clicked, sequence, &QAction::trigger);
	auto saveProjectButton = new QPushButton(Ui("保存工程"), assets);
	connect(saveProjectButton, &QPushButton::clicked, this, &KRRClipEditor::saveProject);
	assetsLayout->addWidget(addMaterial);
	assetsLayout->addWidget(saveProjectButton);
	m_assetsTabs->setTabToolTip(m_assetsTabs->addTab(assets, Ui("素材")), Ui("素材工程"));
	auto stillPage = new QWidget(m_assetsTabs);
	auto stillLayout = new QVBoxLayout(stillPage);
	stillLayout->setContentsMargins(5, 5, 5, 5);
	m_stills = new QListWidget(stillPage);
	stillLayout->addWidget(m_stills, 1);
	connect(m_stills, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) { editStill(m_stills->row(item)); });
	m_assetsTabs->setTabToolTip(m_assetsTabs->addTab(stillPage, Ui("静帧")), Ui("静止画面"));
	panes->addWidget(m_assetsTabs);
	auto preview = new QWidget(panes);
	auto previewLayout = new QVBoxLayout(preview);
	previewLayout->setContentsMargins(0, 0, 0, 0);
	previewLayout->setSpacing(4);
	auto previewHeading = new QHBoxLayout;
	auto previewTitle = new QLabel(Ui("成片预览"), preview);
	previewTitle->setProperty("heading", true);
	previewHeading->addWidget(previewTitle);
	previewHeading->addStretch(1);
	auto cropToggle = new QPushButton(RecorderUi::Icon("crop"), Ui("裁剪取景"), preview);
	cropToggle->setObjectName("editCropToggle");
	cropToggle->setCheckable(true);
	previewHeading->addWidget(cropToggle);
	previewLayout->addLayout(previewHeading);
	auto cropStrip = new QWidget(preview);
	auto cropRow = new QHBoxLayout(cropStrip);
	cropRow->setContentsMargins(0, 0, 0, 0);
	previewLayout->addWidget(cropStrip);
	cropStrip->hide();
	m_canvas = new KRREditCanvas(preview);
	m_canvas->setMinimumSize(0, P(96));
	m_canvas->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
	previewLayout->addWidget(m_canvas, 1);
	m_previewSeek = new KRRPlaybackSlider(preview);
	m_previewSeek->setObjectName("editPreviewSeek");
	m_previewSeek->setRange(0, 0);
	static_cast<KRRPlaybackSlider*>(m_previewSeek)->setHandleOnHover(true);
	previewLayout->addWidget(m_previewSeek);
	auto transport = new QHBoxLayout;
	auto before = new QPushButton(RecorderUi::Icon("previous"), {}, this);
	before->setObjectName("stepPreviousFrame");
	before->setAccessibleName(Ui("上一帧"));
	before->setToolTip(Ui("上一帧"));
	m_play = new QPushButton(Ui("播放"), this);
	auto after = new QPushButton(RecorderUi::Icon("next"), {}, this);
	after->setObjectName("stepNextFrame");
	after->setAccessibleName(Ui("下一帧"));
	after->setToolTip(Ui("下一帧"));
	m_time = new QLabel("00:00:00.000", this);
	m_time->setStyleSheet("font-family:Consolas;");
	m_cropPreview = new QCheckBox(Ui("预览裁剪结果"), this);
	m_cropPreview->setChecked(true);
	transport->addWidget(before);
	transport->addWidget(m_play);
	transport->addWidget(after);
	transport->addWidget(m_time);
	auto captionShortcut = new QPushButton(Ui("添加字幕"), this);
	transport->addWidget(captionShortcut);
	transport->addStretch();
	auto speed = new QComboBox(preview);
	speed->setObjectName("editPlaybackSpeed");
	for (double rate : {.5, .75, 1., 1.25, 1.5, 2.})
		speed->addItem(QString::number(rate) + "×", rate);
	speed->setCurrentIndex(2);
	transport->addWidget(speed);
	connect(speed, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, speed]
	{
		m_player->setPlaybackRate(speed->currentData().toDouble());
		syncAudio(true);
	});
	cropRow->addWidget(m_cropPreview);
	previewLayout->addLayout(transport);
	m_inspector = new QTabWidget(panes);
	m_inspector->setMinimumWidth(P(184));
	auto inspectorPage = [this](const QString &title)
	{
		auto page = new QWidget;
		auto body = new QVBoxLayout(page);
		body->setContentsMargins(5, 5, 5, 5);
		body->setSpacing(5);
		auto scroll = new QScrollArea;
		scroll->setWidgetResizable(true);
		scroll->setWidget(page);
		m_inspector->addTab(scroll, title);
		return body;
	};
	auto cropLayout = inspectorPage(Ui("画面"));
	auto cropNote = new QLabel(Ui("在预览画面直接拖框，或设置精确像素。"), this);
	cropNote->setWordWrap(true);
	cropNote->setProperty("muted", true);
	cropLayout->addWidget(cropNote);
	m_cropButton = new QPushButton(Ui("框选画面裁剪"), this);
	m_cropButton->setCheckable(true);
	cropRow->addWidget(m_cropButton);
	auto cropForm = new QFormLayout;
	auto cropNumber = [this, cropForm](const QString &label)
	{
		auto number = new QSpinBox(this);
		number->setRange(0, 8192);
		number->setSingleStep(2);
		number->setSuffix(" px");
		number->setKeyboardTracking(false);
		cropForm->addRow(label, number);
		return number;
	};
	m_cropX = cropNumber(Ui("左侧 X"));
	m_cropY = cropNumber(Ui("顶部 Y"));
	m_cropW = cropNumber(Ui("宽度"));
	m_cropH = cropNumber(Ui("高度"));
	m_cropW->setMinimum(2);
	m_cropH->setMinimum(2);
	m_cropX->setObjectName("cropX");
	m_cropY->setObjectName("cropY");
	m_cropW->setObjectName("cropWidth");
	m_cropH->setObjectName("cropHeight");
	cropLayout->addLayout(cropForm);
	auto applyCrop = new QPushButton(Ui("应用裁剪框"), this);
	cropLayout->addWidget(applyCrop);
	auto ratio = m_cropRatio = new QComboBox(this);
	ratio->setObjectName("cropAspectRatio");
	ratio->addItems({Ui("自由"), Ui("16:9 横屏"), Ui("9:16 竖屏"), Ui("1:1 方形"), "4:3", Ui("自定义…")});
	cropRow->addWidget(ratio, 1);
	auto resetCrop = new QPushButton(Ui("恢复完整画面"), this);
	cropRow->addWidget(resetCrop);
	connect(cropToggle, &QPushButton::toggled, this, [this, cropStrip](bool expanded)
	{
		cropStrip->setVisible(expanded);
		m_cropButton->setChecked(expanded && !m_project.input.isEmpty());
	});
	auto addStill = new QPushButton(Ui("插入静止画面"), stillPage);
	addStill->setObjectName("insertStill");
	stillLayout->addWidget(addStill);
	connect(addStill, &QPushButton::clicked, this, [this]{ editStill(); });
	cropLayout->addStretch();
	auto captionLayout = inspectorPage(Ui("字幕"));
	m_inspector->setTabToolTip(1, Ui("字幕与校对"));
	auto captionTop = new QHBoxLayout;
	auto automatic = new QPushButton(Ui("自动识别"), this);
	auto importSrt = new QPushButton(Ui("导入 SRT"), this);
	captionTop->addWidget(automatic);
	captionTop->addWidget(importSrt);
	captionLayout->addLayout(captionTop);
	auto speechWorkbench = new QPushButton(Ui("字幕与配音"), this);
	m_mediaControls << automatic << speechWorkbench;
	speechWorkbench->setObjectName("openSubtitleWorkflow");
	captionLayout->addWidget(speechWorkbench);
	connect(speechWorkbench, &QPushButton::clicked, this, &KRRClipEditor::showSubtitleWorkflow);
	auto style = new QPushButton(Ui("字幕样式"), this);
	style->setObjectName("subtitleStyle");
	captionLayout->addWidget(style);
	connect(style, &QPushButton::clicked, this, &KRRClipEditor::editSubtitleStyle);
	m_asrTrack = new QComboBox(this);
	m_language = new QComboBox(this);
	m_language->addItem(Ui("自动语言"), "auto");
	m_language->addItem(Ui("中文"), "zh");
	m_language->addItem("English", "en");
	for (const auto &entry : QList<QPair<QString, QString>>{{Ui("日本語"), "ja"},
															{"Français", "fr"},
															{"Deutsch", "de"},
															{"Español", "es"},
															{"한국어", "ko"},
															{"Italiano", "it"},
															{"Português", "pt"},
															{"Русский", "ru"}})
		m_language->addItem(entry.first, entry.second);
	m_asrModel = new QComboBox(this);
	m_asrModel->addItem(Ui("精细 · small Q5"), "ggml-small-q5_1.bin");
	m_asrModel->addItem(Ui("快速 · base"), "ggml-base.bin");
	if (!QFileInfo::exists(QCoreApplication::applicationDirPath() + "/whisper/ggml-small-q5_1.bin"))
		m_asrModel->setCurrentIndex(1);
	auto asrSettings = m_asrSettings = new QDialog(this);
	asrSettings->setObjectName("offlineSubtitleSettings");
	m_language->setObjectName("offlineSubtitleLanguage");
	m_asrModel->setObjectName("offlineSubtitleModel");
	asrSettings->setWindowTitle(Ui("离线自动字幕"));
	asrSettings->setWindowFlag(Qt::WindowContextHelpButtonHint, false);
	auto asrForm = new QFormLayout(asrSettings);
	asrForm->addRow(Ui("识别音轨"), m_asrTrack);
	asrForm->addRow(Ui("语言"), m_language);
	asrForm->addRow(Ui("识别模型"), m_asrModel);
	auto asrNote =
		new QLabel(Ui("small 更适合细致识别，base 更快。请选择包含讲话的音轨；识别后仍可人工校对。"), asrSettings);
	asrNote->setWordWrap(true);
	asrForm->addRow(asrNote);
	auto asrButtons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, asrSettings);
	asrButtons->button(QDialogButtonBox::Ok)->setText(Ui("开始识别"));
	asrButtons->button(QDialogButtonBox::Ok)->setProperty("role","primary");
	asrButtons->button(QDialogButtonBox::Cancel)->setText(Ui("取消"));
	asrForm->addRow(asrButtons);
	connect(asrButtons, &QDialogButtonBox::accepted, asrSettings, &QDialog::accept);
	connect(asrButtons, &QDialogButtonBox::rejected, asrSettings, &QDialog::reject);
	auto cancelAsr = new QPushButton(Ui("取消当前识别"), this);
	captionLayout->addWidget(cancelAsr);
	cancelAsr->hide();
	connect(cancelAsr, &QPushButton::clicked, &m_asr, &KRRMediaEditor::cancel);
	connect(&m_asr, &KRRMediaEditor::progress, this,
			[cancelAsr, automatic]
			{
				cancelAsr->show();
				automatic->setEnabled(false);
			});
	connect(&m_asr, &KRRMediaEditor::completed, this,
			[cancelAsr, automatic](const QString &, const QString &)
			{
				cancelAsr->hide();
				automatic->setEnabled(true);
			});
	connect(&m_asr, &KRRMediaEditor::progress, this, [this](const QString &message) { m_status->setText(message); });
	m_captionSearch = new QLineEdit(this);
	m_captionSearch->setObjectName("captionSearch");
	m_captionSearch->setPlaceholderText(Ui("搜索原文或译文"));
	m_captionSearch->setMaxLength(200);
	m_captionSearch->setClearButtonEnabled(true);
	captionLayout->addWidget(m_captionSearch);
	auto addCaptionTrack = new QPushButton(Ui("添加字幕轨"), this);
	captionLayout->addWidget(addCaptionTrack);
	connect(addCaptionTrack, &QPushButton::clicked, this, &KRRClipEditor::addSubtitleTrack);
	m_captionTrackVisible = new QCheckBox(Ui("显示所选字幕轨"), this);
	m_captionTrackVisible->setObjectName("editorSubtitleTrackVisible");
	captionLayout->addWidget(m_captionTrackVisible);
	connect(m_captionTrackVisible, &QCheckBox::toggled, this, [this](bool visible)
	{
		if (m_refreshing || m_track >= -1 || m_project.input.isEmpty()) return;
		checkpoint();
		const auto track = SelectedSubtitleTrack(m_track);
		if (visible) m_project.hiddenSubtitleTracks.remove(track);
		else m_project.hiddenSubtitleTracks.insert(track);
		refresh();
	});
	m_captionList = new QListWidget(this);
	m_captionList->setObjectName("captionSearchResults");
	m_captionList->setMinimumHeight(P(100));
	m_captionList->setMaximumHeight(P(160));
	captionLayout->addWidget(m_captionList, 1);
	connect(m_captionSearch, &QLineEdit::textChanged, this, &KRRClipEditor::filterCaptions);
	connect(m_captionSearch, &QLineEdit::returnPressed, this, [this]
	{
		if (m_captionList->count()) chooseCaption(m_captionList->item(0)->data(Qt::UserRole).toInt());
	});
	auto find = new QShortcut(QKeySequence::Find, this);
	connect(find, &QShortcut::activated, this, [this] { showSubtitles(); m_captionSearch->setFocus(); });
	auto captionTimes = new QFormLayout;
	m_captionBegin = new QDoubleSpinBox(this);
	m_captionEnd = new QDoubleSpinBox(this);
	for (auto n : {m_captionBegin, m_captionEnd})
	{
		n->setRange(0, 86400);
		n->setDecimals(3);
		n->setSingleStep(.1);
		n->setSuffix(" s");
	}
	captionTimes->addRow(Ui("开始"), m_captionBegin);
	captionTimes->addRow(Ui("结束"), m_captionEnd);
	captionLayout->addLayout(captionTimes);
	m_captionText = new QPlainTextEdit(this);
	m_captionText->setPlaceholderText(Ui("输入字幕文字，预览与导出均可显示"));
	m_captionText->setMaximumHeight(P(90));
	captionLayout->addWidget(m_captionText);
	auto captionButtons = new QHBoxLayout;
	auto addCaption = new QPushButton(Ui("添加"), this);
	auto updateCaption = new QPushButton(Ui("更新"), this);
	auto removeCaption = new QPushButton(Ui("删除"), this);
	removeCaption->setObjectName("removeSubtitleCue");
	captionButtons->addWidget(addCaption);
	captionButtons->addWidget(updateCaption);
	captionButtons->addWidget(removeCaption);
	captionLayout->addLayout(captionButtons);
	connect(captionShortcut, &QPushButton::clicked, this,
			[this, addCaption]
			{
				if (m_project.input.isEmpty())
					return;
				m_caption = -1;
				m_captionText->clear();
				m_inspector->setCurrentIndex(1);
				bool whole = m_begin == 0 && m_end == m_project.duration;
				m_captionBegin->setValue((whole ? m_position : m_begin) / 1000.0);
				m_captionEnd->setValue((whole ? qMin(m_project.duration, m_position + 3000) : m_end) / 1000.0);
				if (auto scroll = qobject_cast<QScrollArea *>(m_inspector->currentWidget()))
					scroll->ensureWidgetVisible(addCaption);
				m_captionText->setFocus();
			});
	auto soundLayout = inspectorPage(Ui("声音"));
	auto soundNote = new QLabel(Ui("点击下方声音轴选择轨道。单独保留/删除声音区间时用静音填补，视频位置不变。"), this);
	soundNote->setWordWrap(true);
	soundNote->setProperty("muted", true);
	soundLayout->addWidget(soundNote);
	auto importAudioButton = new QPushButton(Ui("导入外部音频"), this);
	importAudioButton->setObjectName("importExternalAudio");
	auto removeAudioButton = new QPushButton(Ui("移除选中的外部轨道"), this);
	soundLayout->addWidget(importAudioButton);
	soundLayout->addWidget(removeAudioButton);
	connect(importAudioButton, &QPushButton::clicked, this, &KRRClipEditor::importAudio);
	connect(removeAudioButton, &QPushButton::clicked, this, [this]{
		if (m_track < 0 || m_project.audio[m_track].file.isEmpty() || m_lockedTracks.contains(m_track)) return;
		m_player->pause();
		checkpoint();
		m_project.audio.removeAt(m_track);
		QSet<int> locks;
		for (int track : m_lockedTracks)
			if (track != m_track) locks.insert(track > m_track ? track - 1 : track);
		m_lockedTracks = std::move(locks);
		m_track = -1;
		refresh();
	});
	m_trackEnabled = new QCheckBox(Ui("启用当前声音轨道"), this);
	soundLayout->addWidget(m_trackEnabled);
	m_gain = new QSpinBox(this);
	m_gain->setRange(0, 100);
	m_gain->setSuffix(" %");
	m_gain->setSingleStep(5);
	auto soundForm = new QFormLayout;
	soundForm->addRow(Ui("轨道音量"), m_gain);
	soundLayout->addLayout(soundForm);
	auto resetAudio = new QPushButton(Ui("恢复此轨完整声音"), this);
	soundLayout->addWidget(resetAudio);
	soundLayout->addStretch();
	auto chapterLayout = inspectorPage(Ui("章节"));
	m_chapterPage = m_inspector->widget(m_inspector->count() - 1);
	m_inspector->removeTab(m_inspector->count() - 1);
	m_assetsTabs->addTab(m_chapterPage, Ui("章节"));
	m_chapterList = new QListWidget(this);
	m_chapterList->setObjectName("editChapters");
	m_chapterList->setWordWrap(true);
	chapterLayout->addWidget(m_chapterList, 1);
	m_chapterTitle = new QLineEdit(this);
	m_chapterTitle->setObjectName("chapterTitle");
	m_chapterTitle->setMaxLength(120);
	m_chapterTitle->setPlaceholderText(Ui("章节标题"));
	chapterLayout->addWidget(m_chapterTitle);
	auto addChapter = new QPushButton(Ui("在播放头处添加章节"), this);
	addChapter->setObjectName("addEditChapter");
	auto renameChapter = new QPushButton(Ui("更新章节标题"), this);
	renameChapter->setObjectName("renameEditChapter");
	auto removeChapter = new QPushButton(Ui("移除章节"), this);
	removeChapter->setObjectName("removeEditChapter");
	chapterLayout->addWidget(addChapter);
	chapterLayout->addWidget(renameChapter);
	chapterLayout->addWidget(removeChapter);
	auto chapterNarration = m_chapterNarration = new QPushButton(Ui("章节旁白…"), this);
	chapterNarration->setObjectName("openChapterNarration");
	chapterNarration->setToolTip(Ui("先添加章节，再导入章节旁白稿。"));
	chapterLayout->addWidget(chapterNarration);
	connect(chapterNarration, &QPushButton::clicked, this, [this] { showDubbingWorkflow(true); });
	connect(m_chapterList, &QListWidget::currentRowChanged, this, [this, renameChapter, removeChapter](int row)
	{
		const bool selected = row >= 0 && row < m_project.chapters.size();
		renameChapter->setEnabled(selected);
		removeChapter->setEnabled(selected);
		m_chapterTitle->setText(selected ? m_project.chapters[row].title : QString());
		if (selected && !m_refreshing) seek(m_project.chapters[row].at);
	});
	renameChapter->setEnabled(false);
	removeChapter->setEnabled(false);
	connect(m_chapterList, &QListWidget::itemDoubleClicked, this, [this](const QListWidgetItem* item)
	{
		seek(item->data(Qt::UserRole).toLongLong());
	});
	connect(addChapter, &QPushButton::clicked, this, [this] { editChapter(false); });
	connect(renameChapter, &QPushButton::clicked, this, [this] { editChapter(true); });
	connect(removeChapter, &QPushButton::clicked, this, [this]
	{
		const auto row = m_chapterList->currentRow();
		if (row < 0 || row >= m_project.chapters.size()) return;
		checkpoint();
		m_project.chapters.removeAt(row);
		refresh();
	});
	auto voiceLayout = inspectorPage(Ui("配音"));
	m_inspector->setTabToolTip(m_inspector->count() - 1, Ui("配音与旁白"));
	auto voice = new QPushButton(Ui("字幕逐句配音 / 自填旁白"), this);
	auto chapterVoice = new QPushButton(Ui("章节长稿与原音压低"), this);
	voiceLayout->addWidget(voice);
	voiceLayout->addWidget(chapterVoice);
	voiceLayout->addStretch();
	connect(voice, &QPushButton::clicked, this, [this] { showDubbingWorkflow(false); });
	connect(chapterVoice, &QPushButton::clicked, this, [this] { showDubbingWorkflow(true); });
	panes->addWidget(preview);
	panes->addWidget(m_inspector);
	panes->setStretchFactor(1, 1);
	panes->setSizes({220, 630, 330});
	auto workArea = new QSplitter(Qt::Vertical, this);
	workArea->setObjectName("studioEditVerticalSplit");
	workArea->setChildrenCollapsible(false);
	workArea->addWidget(panes);
	auto tracks = new QWidget(workArea);
	auto trackLayout = new QVBoxLayout(tracks);
	trackLayout->setContentsMargins(0, 0, 0, 0);
	trackLayout->setSpacing(3);
	workArea->addWidget(tracks);
	workArea->setStretchFactor(0, 2);
	workArea->setSizes({490, 210});
	root->addWidget(workArea, 1);
	auto actions = new QHBoxLayout;
	m_target = new QComboBox(this);
	m_target->setMinimumWidth(P(140));
	m_in = new QDoubleSpinBox(this);
	m_out = new QDoubleSpinBox(this);
	for (auto n : {m_in, m_out})
	{
		n->setDecimals(3);
		n->setRange(0, 86400);
		n->setSingleStep(.1);
		n->setSuffix(" s");
		n->setMaximumWidth(P(120));
	}
	m_in->hide();
	m_out->hide();
	m_lockTrack = new QPushButton(Ui("锁定轨道"), this);
	m_lockTrack->setObjectName("editLockTrack");
	m_lockTrack->setCheckable(true);
	auto split = new QPushButton(RecorderUi::Icon("scissors"), Ui("剪切"), this);
	split->setObjectName("editSplitClip");
	auto link = new QPushButton(RecorderUi::Icon("link"), Ui("联动"), this);
	link->setCheckable(true);
	link->setObjectName("editLinkTracks");
	link->setToolTip(Ui("视频操作时同步处理未锁定的声音与字幕轨"));
	auto chapter = new QPushButton(RecorderUi::Icon("flag"), Ui("打点"), this);
	m_keepClip = new QPushButton(Ui("仅保留片段"), this);
	m_deleteClip = new QPushButton(RecorderUi::Icon("trash"), Ui("删除"), this);
	m_deleteClip->setObjectName("editDeleteClip");
	m_exportClip = new QPushButton(Ui("导出片段"), this);
	auto addTrack = new QPushButton(Ui("+ 轨道"), this);
	addTrack->setObjectName("editAddTrack");
	auto trackMenu = new QMenu(addTrack);
	addTrack->setMenu(trackMenu);
	connect(trackMenu->addAction(Ui("添加音频轨")), &QAction::triggered, this, &KRRClipEditor::importAudio);
	connect(trackMenu->addAction(Ui("添加字幕轨")), &QAction::triggered, this, &KRRClipEditor::addSubtitleTrack);
	connect(link, &QPushButton::toggled, this, [this](bool enabled) { m_linkTracks = enabled; });
	connect(chapter, &QPushButton::clicked, this, [this]
	{
		if (m_project.input.isEmpty()) return;
		m_assetsTabs->setCurrentWidget(m_chapterPage);
		editChapter(false);
	});
	connect(m_lockTrack, &QPushButton::toggled, this, [this](bool locked)
	{
		if (locked) m_lockedTracks.insert(m_track);
		else m_lockedTracks.remove(m_track);
		m_timeline->setLockedTracks(m_lockedTracks);
		refreshClipActions();
	});
	actions->addWidget(m_target);
	for (auto button : {m_lockTrack, addTrack, split, m_deleteClip, link, chapter}) actions->addWidget(button);
	actions->addStretch(1);
	actions->addWidget(m_keepClip);
	actions->addWidget(m_exportClip);
	m_mediaControls << split << addTrack << link << chapter;
	trackLayout->addLayout(actions);
	auto timelineScroll = new QScrollArea(this);
	timelineScroll->setObjectName("editTimelineScroll");
	timelineScroll->setWidgetResizable(true);
	timelineScroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	timelineScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	m_timeline = new KRREditTimeline;
	timelineScroll->setWidget(m_timeline);
	timelineScroll->setMinimumHeight(P(120));
	trackLayout->addWidget(timelineScroll, 1);
	auto footer = new QHBoxLayout;
	m_summary = new QLabel(this);
	m_summary->setProperty("muted", true);
	m_result = new QPushButton(Ui("预览成片"), this);
	m_result->hide();
	m_zoom = new KRRPlaybackSlider(this);
	m_zoom->setRange(10, 200);
	m_zoom->setValue(10);
	m_zoom->setMaximumWidth(P(140));
	m_scroll = new KRRPlaybackSlider(this);
	m_scroll->setRange(0, 10000);
	m_scroll->setMaximumWidth(P(280));
	m_scroll->setEnabled(false);
	m_scroll->setToolTip(Ui("时间轴位置：放大后拖动查看前后片段；1× 时已显示完整视频。"));
	m_zoom->setToolTip(Ui("向右放大时间轴以精细剪辑；1× 显示完整视频。"));
	footer->addWidget(m_summary, 1);
	footer->addWidget(m_result);
	footer->addWidget(new QLabel(Ui("时间轴缩放"), this));
	footer->addWidget(m_zoom);
	footer->addWidget(new QLabel(Ui("位置"), this));
	footer->addWidget(m_scroll);
	trackLayout->addLayout(footer);
	m_status = new QLabel(Ui("点击标尺定位，剪切后点击片段进行拖动、删除或导出。"), this);
	m_status->setWordWrap(true);
	m_status->setProperty("muted", true);
	root->addWidget(m_status);
	m_player = new KRREditPlayer(this);
	m_stillTimer.setSingleShot(true);
	connect(&m_stillTimer, &QTimer::timeout, this, [this]{
		if (m_stillPreview < 0 || m_stillPreview >= m_project.stills.size()) return;
		if (m_project.arranged())
		{
			playArrangedSegment(m_arrangedSegment + 1);
			return;
		}
		const int index = m_stillPreview;
		const auto at = m_project.stills[index].at;
		clearStill();
		m_nextStill = nextStill(at + 1);
		if (m_project.nextKept(at) >= m_project.duration)
		{
			seek(m_project.kept.last().end - 1);
			m_atEnd = true;
			return;
		}
		m_player->setPosition(m_project.nextKept(at));
		m_player->play();
		syncAudio(true);
	});
	connect(m_player, &KRREditPlayer::frameReady, this,
			[this](const QImage &frame)
			{
				qint64 time = m_player->position();
				if (m_player->state() == QMediaPlayer::PlayingState && m_project.nextKept(time) == time &&
					time < m_project.duration)
					m_canvas->setFrame(frame);
			});
	connect(m_player, &KRREditPlayer::positionChanged, this,
			[this](qint64 time)
			{
				if (m_player->state() != QMediaPlayer::PlayingState || m_project.input.isEmpty())
					return;
				if (m_project.arranged())
				{
					const auto clips = m_project.segments();
					if (!m_arrangedPlaying || m_arrangedSegment < 0 || m_arrangedSegment >= clips.size()) return;
					const auto clip = clips[m_arrangedSegment];
					if (time >= clip.end) { playArrangedSegment(m_arrangedSegment + 1); return; }
					if (time < clip.begin) return;
					m_position = time;
					const auto at = editPosition();
					m_canvas->setPosition(at);
					m_timeline->setPosition(at);
					m_time->setText(EditTime(at));
					syncAudio(false);
					return;
				}
				if (m_nextStill >= 0 && time >= m_project.stills[m_nextStill].at)
				{
					previewStill(m_nextStill, true);
					return;
				}
				auto next = m_project.nextKept(time);
				if (next >= m_project.duration)
				{
					m_player->pause();
					seek(m_project.kept.last().end - 1);
					m_atEnd = true;
					return;
				}
				if (next != time)
				{
					if (m_nextStill >= 0 && m_project.stills[m_nextStill].at <= next)
					{
						previewStill(m_nextStill, true);
						return;
					}
					m_player->setPosition(next);
					m_position = next;
					syncAudio(true);
					return;
				}
				m_position = time;
				m_canvas->setPosition(time);
				m_timeline->setPosition(time);
				m_time->setText(EditTime(time));
				syncAudio(false);
			});
	connect(m_player, &KRREditPlayer::stateChanged, this,
			[this](QMediaPlayer::State state)
			{
				if (state == QMediaPlayer::StoppedState && m_project.arranged() && m_arrangedPlaying && m_stillPreview < 0)
				{
					const int current = m_arrangedSegment;
					QTimer::singleShot(0, this, [this, current]
					{
						if (m_arrangedPlaying && m_arrangedSegment == current) playArrangedSegment(current + 1);
					});
				}
				if (state == QMediaPlayer::StoppedState && m_nextStill >= 0 && !m_project.kept.isEmpty() &&
					m_project.stills[m_nextStill].at == m_project.kept.last().end)
				{
					previewStill(m_nextStill, true);
					return;
				}
				m_play->setText(state == QMediaPlayer::PlayingState ? Ui("暂停") : Ui("播放"));
				syncAudio(true);
			});
	connect(m_player, &KRREditPlayer::failed, this, [this](const QString &error) { m_status->setText(UiDiagnostic(error)); });
	connect(m_play, &QPushButton::clicked, this, &KRRClipEditor::togglePlayback);
	connect(before, &QPushButton::clicked, this, [this] { stepFrame(-1); });
	connect(after, &QPushButton::clicked, this, [this] { stepFrame(1); });
	connect(m_cropPreview, &QCheckBox::toggled, m_canvas, &KRREditCanvas::setCropPreview);
	connect(m_cropButton, &QPushButton::toggled, this, [this](bool editing) {
		m_player->pause();
		clearStill();
		m_canvas->setCropEditing(editing);
		m_cropPreview->setChecked(!editing);
	});
	auto cropApply = [this](const QRect &crop, const QSize &aspect) {
		if (!crop.isValid() || m_project.input.isEmpty() || (crop == m_project.crop && aspect == m_project.cropRatio))
			return;
		checkpoint();
		m_project.crop = crop;
		m_project.cropRatio = aspect;
		refresh();
	};
	connect(m_canvas, &KRREditCanvas::cropRequested, this, [=](const QRect &r) { cropApply(r, m_project.cropRatio); });
	connect(m_canvas, &KRREditCanvas::subtitlePositionRequested, this, [this](int track, const QPointF &position)
	{
		if (isBusy() || track < 0 || track >= m_project.subtitlePositions.size()) return;
		if (m_project.subtitlePositions[track] != position)
		{
			checkpoint();
			m_project.subtitlePositions[track] = position;
		}
		chooseTrack(SubtitleTrackSelection(track));
	});
	connect(applyCrop, &QPushButton::clicked, this, [this] { m_cropButton->setChecked(false); m_cropPreview->setChecked(true); });
	auto changePixels = [=](QSpinBox *field, int value) {
		if (m_refreshing || m_project.input.isEmpty()) return;
		auto desired = m_project.crop;
		if (field == m_cropX)
			desired.moveLeft(qBound(0,value,m_project.size.width()-desired.width())/2*2);
		else if (field == m_cropY)
			desired.moveTop(qBound(0,value,m_project.size.height()-desired.height())/2*2);
		else if (field == m_cropW)
			desired.setWidth(value);
		else
			desired.setHeight(value);
		m_cropButton->setChecked(true);
		cropApply(FitEditCrop(desired,m_project.size,m_project.cropRatio,field==m_cropH),m_project.cropRatio);
	};
	for (auto number : {m_cropX,m_cropY,m_cropW,m_cropH})
	{
		connect(number, qOverload<int>(&QSpinBox::valueChanged), this, [=](int value) {
			if (m_typingCrop != number) changePixels(number,value);
		});
		connect(number->findChild<QLineEdit*>(), &QLineEdit::textEdited, this, [=](QString text) {
			if (text.endsWith(number->suffix())) text.chop(number->suffix().size());
			bool ok=false;
			const int value=text.trimmed().toInt(&ok);
			if (!ok || value < number->minimum() || value > number->maximum()) return;
			m_typingCrop=number;
			changePixels(number,value);
		});
		connect(number, &QSpinBox::editingFinished, this, [this,number] {
			if (m_typingCrop != number) return;
			m_typingCrop=nullptr;
			refresh();
		});
	}
	connect(resetCrop, &QPushButton::clicked, this, [=] {
		cropApply(QRect(0,0,m_project.size.width()/2*2,m_project.size.height()/2*2),{});
	});
	connect(ratio, qOverload<int>(&QComboBox::activated), this, [=](int index) {
		if (m_project.input.isEmpty()) return;
		const QSize sizes[] = {{},{16,9},{9,16},{1,1},{4,3}};
		QSize aspect = index < 5 ? sizes[index] : QSize();
		if (index == 5)
		{
			bool accepted = false;
			const auto text = QInputDialog::getText(this, Ui("自定义裁剪比例"), Ui("输入宽:高（例如 21:9）"),
				QLineEdit::Normal, "21:9", &accepted);
			const auto match = QRegularExpression("^\\s*(\\d{1,4})\\s*[:：/]\\s*(\\d{1,4})\\s*$").match(text);
			if (!accepted || !match.hasMatch() || match.captured(1).toInt() == 0 || match.captured(2).toInt() == 0)
			{
				if (accepted) m_status->setText(Ui("请输入两个正整数作为宽高比例。"));
				refresh();
				return;
			}
			const int w = match.captured(1).toInt(), h = match.captured(2).toInt();
			const int divisor = std::gcd(w,h);
			aspect = QSize(w/divisor,h/divisor);
		}
		if (index == 0)
		{
			cropApply(m_project.crop,{});
			return;
		}
		const auto frame = m_project.size;
		auto fitted = FitEditCrop(QRect(QPoint(),frame),frame,aspect);
		if (!fitted.isValid()) { m_status->setText(Ui("当前画面无法容纳该比例。")); refresh(); return; }
		fitted.moveTo((frame.width()-fitted.width())/4*2,(frame.height()-fitted.height())/4*2);
		m_cropButton->setChecked(true);
		cropApply(fitted,aspect);
	});
	connect(m_timeline, &KRREditTimeline::seekRequested, this, &KRRClipEditor::seekTimeline);
	connect(m_timeline, &KRREditTimeline::selectionRequested, this, &KRRClipEditor::select);
	connect(m_timeline, &KRREditTimeline::clipSelected, this, [this](qint64 begin, qint64 end)
	{
		select(begin, end);
		m_clipSelected = true;
		refreshClipActions();
	});
	connect(m_timeline, &KRREditTimeline::positionChanged, this, [this](qint64 time)
	{
		if (!m_previewSeek->isSliderDown()) m_previewSeek->setValue(static_cast<int>(time));
	});
	auto previewDebounce = new QTimer(m_previewSeek);
	previewDebounce->setSingleShot(true);
	previewDebounce->setInterval(36);
	connect(m_previewSeek, &QSlider::sliderMoved, previewDebounce, qOverload<>(&QTimer::start));
	connect(m_previewSeek, &QSlider::actionTriggered, previewDebounce,
		[this, previewDebounce] { if (!m_previewSeek->isSliderDown()) previewDebounce->start(); });
	connect(previewDebounce, &QTimer::timeout, this, [this] { seekTimeline(m_previewSeek->value()); });
	connect(m_previewSeek, &QSlider::sliderReleased, this, [this, previewDebounce]
	{
		previewDebounce->stop();
		seekTimeline(m_previewSeek->value());
	});
	connect(m_timeline, &KRREditTimeline::trackRequested, this, &KRRClipEditor::chooseTrack);
	connect(m_timeline, &KRREditTimeline::trackToggleRequested, this,
			[this](int t)
			{
				checkpoint();
				m_project.audio[t].enabled = !m_project.audio[t].enabled;
				refresh();
			});
	connect(m_timeline, &KRREditTimeline::captionRequested, this, &KRRClipEditor::chooseCaption);
	connect(m_timeline, &KRREditTimeline::clipMoved, this, &KRRClipEditor::moveClip);
	connect(m_timeline, &KRREditTimeline::stillRequested, this, [this](int i) {
		previewStill(i, false);
		editStill(i);
	});
	connect(m_target, qOverload<int>(&QComboBox::activated), this,
		[this](int i) { chooseTrack(m_target->itemData(i).toInt()); });
	connect(m_in, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
			[this](double v)
			{
				if (!m_refreshing)
					select(qRound64(v * 1000), m_end);
			});
	connect(m_out, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
			[this](double v)
			{
				if (!m_refreshing)
					select(m_begin, qRound64(v * 1000));
			});
	connect(split, &QPushButton::clicked, this, &KRRClipEditor::splitSelectedTrack);
	connect(m_keepClip, &QPushButton::clicked, this, [this] { applyClipSelection(true); });
	connect(m_deleteClip, &QPushButton::clicked, this, [this] { applyClipSelection(false); });
	connect(m_exportClip, &QPushButton::clicked, this, &KRRClipEditor::exportSelectedClip);
	snap->setChecked(true);
	auto viewport = [this]
	{
		double zoom = m_zoom->value() / 10.0;
		m_scroll->setEnabled(zoom > 1 && !m_project.input.isEmpty());
		const auto duration = m_project.editDuration();
		qint64 offset = qRound64((duration - duration / zoom) * m_scroll->value() / 10000.0);
		m_timeline->setViewport(zoom, offset);
	};
	connect(m_zoom, &QSlider::valueChanged, this, viewport);
	connect(m_scroll, &QSlider::valueChanged, this, viewport);
	connect(m_captionList, &QListWidget::currentRowChanged, this,
			[this](int row)
			{
				if (!m_refreshing && row >= 0)
					chooseCaption(m_captionList->item(row)->data(Qt::UserRole).toInt());
			});
	connect(addCaption, &QPushButton::clicked, this, [this] { editCaption(false); });
	connect(updateCaption, &QPushButton::clicked, this, [this] { editCaption(true); });
	connect(removeCaption, &QPushButton::clicked, this,
			[this]
			{
				if (m_caption < 0 || m_caption >= m_project.captions.size() ||
					m_lockedTracks.contains(SubtitleTrackSelection(m_project.captions[m_caption].track)))
					return;
				checkpoint();
				if (!m_project.captions[m_caption].dubbingFile.isEmpty()) m_project.markDubbingTrackStale();
				m_project.captions.removeAt(m_caption);
				m_caption = -1;
				refresh();
			});
	connect(importSrt, &QPushButton::clicked, this,
			[this]
			{
				auto f = QFileDialog::getOpenFileName(this, Ui("导入 UTF-8 SRT 字幕"), m_folder, "SRT (*.srt)");
				if (!f.isEmpty())
					importCaptions(f);
			});
	connect(automatic, &QPushButton::clicked, this,
			[this, asrSettings]
			{
				if (!m_project.input.isEmpty() && asrSettings->exec() == QDialog::Accepted)
					autoCaptions();
			});
	connect(m_trackEnabled, &QCheckBox::toggled, this,
			[this](bool enabled)
			{
				if (m_refreshing || m_track < 0 || m_lockedTracks.contains(m_track))
					return;
				checkpoint();
				m_project.audio[m_track].enabled = enabled;
				refresh();
			});
	connect(m_gain, &QSpinBox::editingFinished, this,
			[this]
			{
				if (m_track < 0 || m_lockedTracks.contains(m_track) || std::abs(m_project.audio[m_track].gain - m_gain->value() / 100.0) < .001)
					return;
				checkpoint();
				m_project.audio[m_track].gain = m_gain->value() / 100.0;
				refresh();
			});
	connect(resetAudio, &QPushButton::clicked, this,
			[this]
			{
				if (m_track < 0 || m_lockedTracks.contains(m_track))
					return;
				checkpoint();
				auto &track = m_project.audio[m_track];
				track.kept = track.file.isEmpty() ? QVector<KRREditRange>{{0,m_project.duration}}
					: QVector<KRREditRange>{{track.offset,track.offset+track.length}};
				refresh();
			});
	connect(m_result, &QPushButton::clicked, this,
			[this]
			{
				m_player->pause();
				emit exported(m_lastExport);
				if (isWindow()) hide();
			});
	m_draftTimer.setSingleShot(true);
	m_draftTimer.setInterval(650);
	connect(&m_draftTimer, &QTimer::timeout, this, [this] { saveDraft(); });
	m_frameTimer.setSingleShot(true);
	m_frameTimer.setInterval(35);
	HideConsoleProcess(m_frameProcess);
	connect(&m_frameTimer, &QTimer::timeout, this, &KRRClipEditor::requestFrame);
	connect(&m_frameProcess, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
			[this](int code, QProcess::ExitStatus status)
			{
				QByteArray bytes = m_frameProcess.readAllStandardOutput();
				if (code == 0 && status == QProcess::NormalExit && std::abs(m_frameTime - m_position) < 35 && m_stillPreview < 0 &&
					m_player->state() != QMediaPlayer::PlayingState)
				{
					auto image = QImage::fromData(bytes, "PNG");
					if (!image.isNull())
						m_canvas->setFrame(image);
				}
				if (m_frameTime != m_position && !m_project.input.isEmpty())
					m_frameTimer.start();
			});
	connect(&m_analysis, &KRREditAnalysis::sourceReady, this,
			[this](quint64 token, const KRREditProject &p)
			{
				if (token != m_analysis.generation())
					return;
				m_sourceLoading = false;
				m_project = m_pendingProject ? *m_pendingProject : p;
				if (!m_pendingProject)
				{
					for (const auto& entry : ReadRecordingMarkers(p.input))
					{
						const auto marker = entry.toObject();
						const auto at = static_cast<qint64>(marker["at_ms"].toDouble());
						if (at > 0 && at < p.duration && !m_project.splits.contains(at)) m_project.splits.append(at);
						auto title = marker["label"].toString().trimmed().left(120);
						if (title.isEmpty()) title = Ui("标记 %1").arg(m_project.chapters.size() + 1);
						QString error;
						m_project.addChapter(at, title, error);
						if (m_project.splits.size() == 256) break;
					}
					std::sort(m_project.splits.begin(), m_project.splits.end());
				}
				m_pendingProject.reset();
				m_undo.clear();
				m_redo.clear();
				m_dirty = false;
				m_begin = 0;
				m_end = p.duration;
				m_track = -1;
				m_caption = -1;
				m_position = 0;
				for (int i = 0; i < p.audio.size(); ++i)
				{
					auto player = new QMediaPlayer(this);
					player->setVolume(0);
					m_audioPlayers.append(player);
				}
				m_player->setSource(p.input, p.size, p.fps, p.duration);
				m_zoom->setValue(10);
				m_scroll->setValue(0);
				refresh();
				m_captionBegin->setValue(0);
				m_captionEnd->setValue(qMin(3.0, p.duration / 1000.0));
				seek(m_openAt < 0 ? m_project.kept.front().begin : m_openAt);
				m_openAt = -1;
				if (m_requestedCut) QTimer::singleShot(0, this, &KRRClipEditor::runRequestedSmartCut);
				if (m_pendingSubtitleWorkflow)
				{
					m_pendingSubtitleWorkflow = false;
					QTimer::singleShot(0, this, &KRRClipEditor::showSubtitleWorkflow);
				}
			});
	connect(&m_analysis, &KRREditAnalysis::thumbnail, this,
			[this](quint64 token, int i, const QImage &image)
			{
				if (token == m_analysis.generation())
					m_timeline->setThumbnail(i, image);
			});
	connect(&m_analysis, &KRREditAnalysis::waveform, this,
			[this](quint64 token, int i, const QVector<float> &wave)
			{
				if (token == m_analysis.generation())
					m_timeline->setWaveform(i, wave);
			});
	connect(&m_analysis, &KRREditAnalysis::audioReady, this,
			[this](quint64 token, int t, const QString &proxy)
			{
				if (token == m_analysis.generation() && t < m_audioPlayers.size())
				{
					m_audioPlayers[t]->setMedia(QUrl::fromLocalFile(proxy));
					syncAudio(true);
				}
			});
	connect(&m_analysis, &KRREditAnalysis::progress, this,
			[this](quint64 token, const QString &text)
			{
				if (token == m_analysis.generation())
					m_status->setText(text);
			});
	connect(&m_analysis, &KRREditAnalysis::failed, this,
			[this](quint64 token, const QString &e)
			{
				if (token != m_analysis.generation()) return;
				m_sourceLoading = m_pendingSubtitleWorkflow = false;
				m_status->setText(e);
				refreshMediaControls();
			});
	connect(&m_asr, &KRRMediaEditor::busyChanged, this, &KRRClipEditor::refreshMediaControls);
	connect(&m_exporter, &KRRMediaEditor::busyChanged, this, &KRRClipEditor::refreshMediaControls);
	connect(&m_asr, &KRRMediaEditor::completed, this,
			[this](const QString &output, const QString &error)
			{
				m_asrInProgress = false;
				refreshMediaControls();
				const bool reopen = m_reopenAfterAsr;
				const bool reopenSmart = m_returnToSmartCut;
				m_reopenAfterAsr = false;
				m_returnToSmartCut = false;
				if (!error.isEmpty())
					m_status->setText(UiDiagnostic(error));
				else
				{
					const bool wasEmpty = m_project.captions.isEmpty();
					importCaptions(output, true);
					if (wasEmpty && !m_project.captions.isEmpty() && m_asrRequestedLanguage != "auto")
						m_project.subtitleLanguage = m_asrRequestedLanguage;
					if (reopen && !m_project.captions.isEmpty() && isVisible())
						QTimer::singleShot(0, this, &KRRClipEditor::showSubtitleWorkflow);
					else if (reopenSmart && !m_project.captions.isEmpty() && isVisible())
						QTimer::singleShot(0, this, &KRRClipEditor::showSmartCut);
				}
			});
	connect(&m_exporter, &KRRMediaEditor::completed, this,
			[this](const QString &output, const QString &error)
			{
				if (!error.isEmpty())
					m_status->setText(UiDiagnostic(error));
				else
				{
					m_lastExport = output;
					m_result->show();
					m_status->setText(Ui("导出完成：") + output + Ui("。点击“预览成片”查看结果。"));
				}
			});
	refresh();
	RecorderUi::PolishButtons(this);
}
KRRClipEditor::~KRRClipEditor()
{
	m_analysis.stop();
	clearMedia();
}
void KRRClipEditor::clearMedia()
{
	m_arrangedPlaying = false;
	m_arrangedSegment = -1;
	clearStill();
	m_nextStill = -1;
	m_frameTimer.stop();
	if (m_frameProcess.state() != QProcess::NotRunning)
	{
		m_frameProcess.kill();
		m_frameProcess.waitForFinished(2000);
	}
	if (m_player)
	{
		m_player->stop();
		m_player->clear();
	}
	for (auto player : m_audioPlayers)
	{
		player->stop();
		player->setMedia(QMediaContent());
		delete player;
	}
	m_audioPlayers.clear();
}
bool KRRClipEditor::confirmReplace()
{
	if (isBusy())
	{
		m_status->setText(Ui("请等待字幕识别或导出结束后再替换素材。"));
		return false;
	}
	if (!m_dirty)
		return true;
	QMessageBox dialog(QMessageBox::Question, Ui("切换工程"),
		Ui("当前工程有修改，切换前会自动保存草稿。"), QMessageBox::NoButton, this);
	auto proceed = dialog.addButton(Ui("保存并切换"), QMessageBox::AcceptRole);
	dialog.addButton(QMessageBox::Cancel);
	dialog.exec();
	return dialog.clickedButton() == proceed && saveDraft();
}
bool KRRClipEditor::openFile(const QString &input, const QString &folder, qint64 at)
{
	m_requestedCut.reset();
	if (input.isEmpty())
		return false;
	QString absolute = QFileInfo(input).absoluteFilePath();
	if (absolute == m_project.input)
	{
		m_folder = folder;
		if (m_sourceReleased)
		{
			m_sourceReleased = false;
			m_player->setSource(m_project.input, m_project.size, m_project.fps, m_project.duration);
			refresh();
			if (at < 0) at = m_position;
		}
		if (at >= 0) seek(at);
		return true;
	}
	if (!confirmReplace())
		return false;
	m_folder = folder;
	m_analysis.stop();
	m_sourceLoading = m_pendingSubtitleWorkflow = false;
	clearMedia();
	m_sourceReleased = false;
	m_project = {};
	m_lockedTracks.clear();
	m_clipSelected = false;
	m_captionSearch->clear();
	m_pendingProject.reset();
	m_projectPath.clear();
	m_draftTimer.stop();
	m_canvas->setFrame({});
	m_canvas->setProject(nullptr);
	m_timeline->setProject(nullptr);
	m_timeline->clearAssets();
	m_dirty = false;
	m_lastExport.clear();
	m_result->hide();
	refresh();
	m_cache = std::make_unique<QTemporaryDir>(DataDirectory() + "/edit-cache-XXXXXX");
	if (!m_cache->isValid())
	{
		m_status->setText(Ui("无法创建剪辑缓存目录。"));
		return false;
	}
	m_status->setText(Ui("正在读取素材信息…"));
	m_openAt = at;
	m_sourceLoading = true;
	m_analysis.start(absolute, m_cache->path());
	restoreDraft(absolute);
	return true;
}

bool KRRClipEditor::openSmartCutRequest(const QString& input, const QString& folder, qint64 at,
	const KRRSmartCutOptions& options, bool chapters, QWidget* candidateHost)
{
	if (isBusy() || !openFile(input, folder, at)) return false;
	m_requestedCut = options;
	m_requestedChapters = chapters;
	m_candidateHost = candidateHost;
	if (!m_project.input.isEmpty()) QTimer::singleShot(0, this, &KRRClipEditor::runRequestedSmartCut);
	return true;
}

bool KRRClipEditor::openSmartCutProjectRequest(const QString& path, const QString& input, const QString& folder,
	qint64 at, const KRRSmartCutOptions& options, bool chapters, QWidget* candidateHost)
{
	QString error;
	QJsonObject source;
	if (path != m_projectPath && ReadEditProjectJson(path, source, error) &&
		QFileInfo(source["input"].toString()).absoluteFilePath() == QFileInfo(input).absoluteFilePath())
	{
		if (!openProjectFile(path, folder)) return false;
		m_openAt = at;
		m_requestedCut = options;
		m_requestedChapters = chapters;
		m_candidateHost = candidateHost;
		return true;
	}
	return openSmartCutRequest(input, folder, at, options, chapters, candidateHost);
}

void KRRClipEditor::openSequenceMontage(const QStringList& inputs, const QString& folder, const QString& goal)
{
	m_player->pause();
	KRREditSequenceDialog dialog(m_project, folder, this);
	connect(this, &KRRClipEditor::smartCutInterrupted, &dialog, &KRREditSequenceDialog::interruptSmartCut);
	dialog.importFiles(inputs, goal);
	dialog.exec();
}

void KRRClipEditor::interruptSmartCut()
{
	m_requestedCut.reset();
	emit smartCutInterrupted();
}
void KRRClipEditor::checkpoint()
{
	m_undo.append(m_project);
	if (m_undo.size() > 50)
		m_undo.removeFirst();
	m_redo.clear();
	m_dirty = true;
	m_draftTimer.start();
}
void KRRClipEditor::undo(bool redo)
{
	auto &source = redo ? m_redo : m_undo;
	auto &destination = redo ? m_undo : m_redo;
	if (source.isEmpty())
		return;
	destination.append(m_project);
	m_project = source.takeLast();
	m_dirty = true;
	m_draftTimer.start();
	m_caption = -1;
	clearStill();
	m_nextStill = -1;
	refresh();
	seek(m_position);
}
void KRRClipEditor::setRecording(bool active)
{
	if (active)
		m_pendingSubtitleWorkflow = false;
	refreshMediaControls();
}

void KRRClipEditor::refreshMediaControls()
{
	const bool loaded = !m_project.input.isEmpty();
	const bool available = loaded && !isBusy();
	for (auto control : m_mediaControls) control->setEnabled(available);
	for (auto action : m_mediaActions) action->setEnabled(available);
	m_chapterNarration->setEnabled(available && (!m_project.chapters.isEmpty() || !m_project.chapterNarration.isEmpty()));
	m_play->setEnabled(loaded);
	m_inspector->setEnabled(loaded);
}

void KRRClipEditor::refresh()
{
	m_refreshing = true;
	bool loaded = !m_project.input.isEmpty();
	if (property("activeInput").toString() != m_project.input)
	{
		setProperty("activeInput", m_project.input);
		emit sourceChanged(m_project.input);
	}
	m_title->setText(loaded ? QFileInfo(m_project.input).fileName() + (m_dirty ? " *" : "") : Ui("画面与时间轴同步剪辑"));
	m_title->setToolTip(m_project.input);
	m_undoButton->setEnabled(!m_undo.isEmpty());
	m_redoButton->setEnabled(!m_redo.isEmpty());
	refreshMediaControls();
	refreshChapters();
	m_target->setEnabled(loaded);
	m_in->setEnabled(loaded);
	m_out->setEnabled(loaded);
	int asrTrack = m_asrTrack->currentIndex();
	m_target->clear();
	m_target->addItem(Ui("V1 主视频轨"), -1);
	m_asrTrack->clear();
	for (int i = 0; i < m_project.audio.size(); ++i)
	{
		const auto& t = m_project.audio[i];
		m_target->addItem(t.title, i);
		if (t.file.isEmpty()) m_asrTrack->addItem(t.title, t.stream);
	}
	m_asrTrack->setCurrentIndex(qBound(0, asrTrack, m_asrTrack->count() - 1));
	for (int i = 0; i < m_project.subtitlePositions.size(); ++i)
		m_target->addItem(Ui("字幕 %1").arg(i + 1), SubtitleTrackSelection(i));
	if (m_target->findData(m_track) < 0) m_track = -1;
	m_target->setCurrentIndex(m_target->findData(m_track));
	m_timeline->setTrack(m_track);
	m_canvas->setProject(loaded ? &m_project : nullptr);
	m_previewSeek->setRange(0, static_cast<int>(qMin<qint64>(m_project.editDuration(), INT_MAX)));
	m_previewSeek->setValue(static_cast<int>(editPosition()));
	QVector<int> marks;
	for (const auto& chapter : (m_project.arranged() ? m_project.outputChapters() : m_project.chapters))
		marks.append(static_cast<int>(chapter.at));
	static_cast<KRRPlaybackSlider*>(m_previewSeek)->setMarkers(marks);
	m_materials->clear();
	if (loaded)
	{
		auto material = new QListWidgetItem(QFileInfo(m_project.input).fileName() + "\n" + EditTime(m_project.duration)
			+ QString("  ·  %1×%2").arg(m_project.size.width()).arg(m_project.size.height()), m_materials);
		material->setToolTip(m_project.input);
	}
	for (const auto& track : m_project.audio)
		if (!track.file.isEmpty()) new QListWidgetItem(QFileInfo(track.file).fileName(), m_materials);
	m_stills->clear();
	for (const auto& still : m_project.stills)
		new QListWidgetItem(EditTime(still.at) + "  " + still.caption.simplified(), m_stills);
	m_timeline->setProject(loaded ? &m_project : nullptr);
	m_timeline->setLockedTracks(m_lockedTracks);
	m_in->setMaximum(m_project.editDuration() / 1000.0);
	m_out->setMaximum(m_project.editDuration() / 1000.0);
	m_in->setValue(m_begin / 1000.0);
	m_out->setValue(m_end / 1000.0);
	m_timeline->setSelection(m_begin, m_end);
	const QSize ratio = m_project.cropRatio;
	const int ratioIndex = ratio.isEmpty() ? 0 : ratio == QSize(16,9) ? 1 : ratio == QSize(9,16) ? 2 :
		ratio == QSize(1,1) ? 3 : ratio == QSize(4,3) ? 4 : 5;
	m_cropRatio->setItemText(5, ratioIndex == 5 ? Ui("自定义 · %1:%2").arg(ratio.width()).arg(ratio.height()) : Ui("自定义…"));
	m_cropRatio->setCurrentIndex(ratioIndex);
	while (m_audioPlayers.size() > m_project.audio.size())
		delete m_audioPlayers.takeLast();
	for (int i = 0; i < m_project.audio.size(); ++i)
	{
		const auto &track = m_project.audio[i];
		if (track.file.isEmpty()) continue;
		if (m_audioPlayers.size() <= i) m_audioPlayers.append(new QMediaPlayer(this));
		const auto url = QUrl::fromLocalFile(track.file);
		if (m_audioPlayers[i]->media().request().url() != url)
			m_audioPlayers[i]->setMedia(url);
	}
	if (m_typingCrop != m_cropX) m_cropX->setValue(m_project.crop.x());
	if (m_typingCrop != m_cropY) m_cropY->setValue(m_project.crop.y());
	if (m_typingCrop != m_cropW) m_cropW->setValue(m_project.crop.width());
	if (m_typingCrop != m_cropH) m_cropH->setValue(m_project.crop.height());
	filterCaptions();
	m_captionBegin->setMaximum(m_project.editDuration() / 1000.0);
	m_captionEnd->setMaximum(m_project.editDuration() / 1000.0);
	m_trackEnabled->setEnabled(m_track >= 0 && !m_lockedTracks.contains(m_track));
	m_captionTrackVisible->setEnabled(loaded && m_track < -1);
	m_captionTrackVisible->setChecked(loaded && m_track < -1 && !m_project.hiddenSubtitleTracks.contains(SelectedSubtitleTrack(m_track)));
	m_gain->setEnabled(m_track >= 0 && !m_lockedTracks.contains(m_track));
	if (m_track >= 0)
	{
		m_trackEnabled->setChecked(m_project.audio[m_track].enabled);
		m_gain->setValue(qRound(m_project.audio[m_track].gain * 100));
	}
	m_trackEnabled->setText(m_track >= 0 && m_project.audio[m_track].needsRefresh ? Ui("配音轨待更新") : Ui("启用当前声音轨道"));
	m_summary->setText(loaded ? Ui("源 %1  →  成片 %2  ·  %3×%4")
									.arg(EditTime(m_project.duration), EditTime(m_project.outputDuration()))
									.arg(m_project.crop.width())
									.arg(m_project.crop.height())
							  : QString{});
	m_narrationDucking = NarrationDucking(m_project);
	m_refreshing = false;
	refreshClipActions();
	syncAudio(false);
}
void KRRClipEditor::select(qint64 begin, qint64 end)
{
	if (m_project.input.isEmpty())
		return;
	m_clipSelected = false;
	m_begin = qBound<qint64>(0, begin, m_project.editDuration());
	m_end = qBound<qint64>(0, end, m_project.editDuration());
	if (m_end < m_begin)
		std::swap(m_begin, m_end);
	m_refreshing = true;
	m_in->setValue(m_begin / 1000.0);
	m_out->setValue(m_end / 1000.0);
	m_refreshing = false;
	m_timeline->setSelection(m_begin, m_end);
	m_captionBegin->setValue(m_begin / 1000.0);
	m_captionEnd->setValue(m_end / 1000.0);
	refreshClipActions();
}
void KRRClipEditor::filterCaptions()
{
	const QSignalBlocker guard(m_captionList);
	m_captionList->clear();
	for (int i : SearchEditCaptions(m_project.captions, m_captionSearch->text()))
	{
		const auto& caption = m_project.captions[i];
		auto item = new QListWidgetItem(EditTime(caption.begin) + "  " + caption.displayText().simplified(), m_captionList);
		item->setData(Qt::UserRole, i);
		item->setToolTip(caption.displayText());
		if (i == m_caption) m_captionList->setCurrentItem(item);
	}
}
void KRRClipEditor::chooseTrack(int track)
{
	if (track == -1000 && !m_project.input.isEmpty() && !m_project.arranged())
	{
		checkpoint();
		m_project.arrange();
		m_track = -1;
		refresh();
		const auto at = editPosition();
		select(at, at);
		seekTimeline(at);
		return;
	}
	if (track < SubtitleTrackSelection(m_project.subtitlePositions.size() - 1) || track >= m_project.audio.size())
		return;
	const bool changed = m_track != track;
	if (changed) m_clipSelected = false;
	m_track = track;
	if (track >= 0)
		m_inspector->setCurrentIndex(2);
	else if (track < -1)
		m_inspector->setCurrentIndex(1);
	if (changed) refresh();
}

void KRRClipEditor::addSubtitleTrack()
{
	if (m_project.input.isEmpty() || isBusy() || m_project.subtitlePositions.size() >= 16) return;
	checkpoint();
	const int track = m_project.subtitlePositions.size();
	m_project.subtitlePositions.append(QPointF(.5, qMax(.12, .9375 - track * .12)));
	chooseTrack(SubtitleTrackSelection(track));
}

void KRRClipEditor::refreshClipActions()
{
	const bool loaded = !m_project.input.isEmpty();
	const bool locked = m_lockedTracks.contains(m_track);
	const bool selected = loaded && m_clipSelected && m_end > m_begin;
	const QSignalBlocker block(m_lockTrack);
	m_lockTrack->setEnabled(loaded);
	m_lockTrack->setChecked(locked);
	m_lockTrack->setText(locked ? Ui("解锁轨道") : Ui("锁定轨道"));
	m_keepClip->setEnabled(selected && !locked);
	m_deleteClip->setEnabled(selected && !locked);
	m_exportClip->setEnabled(selected && !isBusy());
}

void KRRClipEditor::applyClipSelection(bool retain)
{
	if (!m_clipSelected || isBusy() || m_lockedTracks.contains(m_track)) return;
	applyRange(retain);
}

void KRRClipEditor::exportSelectedClip()
{
	if (!m_clipSelected || isBusy()) return;
	KRRTimelineExportOptions options;
	options.previewStart = m_project.arranged() ? m_begin : m_project.outputTime(m_begin);
	const auto end = m_project.arranged() ? m_end : m_project.outputTime(m_end - 1) + 1;
	if (end <= options.previewStart || options.previewStart >= m_project.outputDuration()) return;
	options.previewLength = end - options.previewStart;
	exportTimeline(options);
}

namespace
{
struct LinkedSection
{
	qint64 from, to, destination;
};

void RemapLinkedTracks(const KRREditProject& before, KRREditProject& after, const QSet<int>& locked)
{
	QVector<LinkedSection> sections;
	const auto original = before.segments();
	qint64 destination = 0;
	for (const auto& segment : after.segments())
	{
		const auto length = segment.end - segment.begin;
		qint64 from = 0;
		for (const auto& source : original)
		{
			const bool match = segment.still >= 0 ? segment.still == source.still :
				source.still < 0 && segment.begin >= source.begin && segment.begin < source.end;
			if (match)
			{
				if (segment.still < 0) from += segment.begin - source.begin;
				sections.append({from, from + length, destination});
				break;
			}
			from += source.end - source.begin;
		}
		destination += length;
	}
	if (before.editDuration() > before.outputDuration())
		sections.append({before.outputDuration(), before.editDuration(), destination});
	for (int track = 0; track < after.audio.size(); ++track)
	{
		if (locked.contains(track)) continue;
		QVector<KRREditTrack::Clip> placed;
		for (const auto& section : sections)
			for (const auto& clip : before.audio[track].placed)
			{
				const auto start = qMax(section.from, clip.begin), end = qMin(section.to, clip.end);
				const auto shift = section.destination - section.from;
				if (start < end) placed.append({start + shift, end + shift, clip.mediaStart + start - clip.begin});
			}
		after.audio[track].placed = std::move(placed);
	}
	after.captions.clear();
	for (const auto& cue : before.captions)
	{
		if (locked.contains(SubtitleTrackSelection(cue.track)))
		{
			after.captions.append(cue);
			continue;
		}
		for (const auto& section : sections)
		{
			const auto begin = qMax(cue.begin, section.from), end = qMin(cue.end, section.to);
			if (begin >= end) continue;
			auto piece = cue;
			piece.begin = section.destination + begin - section.from;
			piece.end = section.destination + end - section.from;
			after.captions.append(piece);
		}
	}
	if (!before.captions.isEmpty()) after.markDubbingTrackStale();
}
}

void KRRClipEditor::applyRange(bool retain)
{
	if (m_project.input.isEmpty() || m_lockedTracks.contains(m_track))
		return;
	auto candidate = m_project;
	QString error;
	qint64 begin = m_begin, end = m_end;
	if (m_track == -1 && m_clipSelected && !candidate.arranged())
	{
		begin = candidate.outputTime(begin);
		end = candidate.outputTime(end - 1) + 1;
		candidate.arrange();
	}
	const auto linkedBefore = m_track == -1 && m_linkTracks && candidate.arranged() ? candidate : KRREditProject{};
	if (m_track < -1)
	{
		if (m_end <= m_begin) return;
		QVector<KRREditCaption> remaining;
		const int track = SelectedSubtitleTrack(m_track);
		for (auto cue : candidate.captions)
		{
			if (cue.track != track)
			{
				remaining.append(cue);
				continue;
			}
			const auto append = [&](qint64 begin, qint64 end)
			{
				if (begin >= end) return;
				auto piece = cue;
				piece.begin = begin;
				piece.end = end;
				remaining.append(piece);
			};
			if (retain) append(qMax(cue.begin, m_begin), qMin(cue.end, m_end));
			else
			{
				append(cue.begin, qMin(cue.end, m_begin));
				append(qMax(cue.begin, m_end), cue.end);
			}
		}
		candidate.captions = remaining;
		candidate.markDubbingTrackStale();
		if (!candidate.validate(error)) { m_status->setText(UiDiagnostic(error)); return; }
		m_caption = -1;
	}
	else if (!candidate.editRange(begin, end, m_track, retain, error))
	{
		m_status->setText(UiDiagnostic(error));
		return;
	}
	if (!linkedBefore.input.isEmpty()) RemapLinkedTracks(linkedBefore, candidate, m_lockedTracks);
	if (!candidate.validate(error)) { m_status->setText(UiDiagnostic(error)); return; }
	checkpoint();
	m_project = candidate;
	m_clipSelected = false;
	refresh();
	if (m_project.arranged()) seekTimeline(qMin(m_begin, m_project.outputDuration() - 1));
	else seek(m_position);
	m_status->setText(m_track == -1 ? Ui("已更新视频保留区间，预览和导出会同步跳过已删除部分。") :
		m_track < -1 ? Ui("已更新所选字幕轨。") : Ui("已更新此声音轴；被剪掉的部分静音，其他轨道位置不变。"));
}
void KRRClipEditor::seek(qint64 time, bool pause)
{
	if (m_project.input.isEmpty())
		return;
	if (pause)
	{
		m_arrangedPlaying = false;
		m_player->pause();
	}
	clearStill();
	m_nextStill = -1;
	m_atEnd = false;
	qint64 bounded = qBound<qint64>(0, time, m_project.duration - 1);
	// Round to a frame index first. Never floor a timestamp that was already rounded to milliseconds.
	const qint64 last = qMax<qint64>(0, static_cast<qint64>(std::ceil(m_project.duration * m_project.fps / 1000.0)) - 1);
	const qint64 frame = qBound<qint64>(0, qRound64(bounded * m_project.fps / 1000.0), last);
	m_position = qMin(m_project.duration - 1, static_cast<qint64>(std::floor(frame * 1000.0 / m_project.fps + 1e-7)));
	// Paused scrubbing uses FFmpeg. Queuing native seeks for every drag can race Play.
	if (m_player->state() == QMediaPlayer::PlayingState)
		m_player->setPosition(m_position);
	const auto at = editPosition();
	m_canvas->setPosition(at);
	m_timeline->setPosition(at);
	m_time->setText(EditTime(at));
	syncAudio(true);
	if (m_player->state() != QMediaPlayer::PlayingState)
		m_frameTimer.start();
}
qint64 KRRClipEditor::editPosition() const
{
	if (!m_project.arranged()) return m_position;
	if (m_stillPreview < 0) return m_project.outputTime(m_position);
	qint64 at = 0;
	for (const auto& clip : m_project.segments())
	{
		if (clip.still == m_stillPreview) return at;
		at += clip.end - clip.begin;
	}
	return qMax<qint64>(0, at - 1);
}

void KRRClipEditor::seekTimeline(qint64 time)
{
	if (!m_project.arranged()) { seek(time); return; }
	const auto clips = m_project.segments();
	qint64 at = 0;
	for (int i = 0; i < clips.size(); ++i)
	{
		const auto& clip = clips[i];
		const auto length = clip.end - clip.begin;
		if (time < at + length || i == clips.size() - 1)
		{
			m_arrangedSegment = i;
			if (clip.still >= 0)
			{
				m_arrangedPlaying = false;
				previewStill(clip.still, false);
			}
			else seek(clip.begin + qBound<qint64>(0, time - at, length - 1));
			return;
		}
		at += length;
	}
}

void KRRClipEditor::playArrangedSegment(int index, qint64 source)
{
	const auto clips = m_project.segments();
	if (index < 0 || index >= clips.size())
	{
		m_arrangedPlaying = false;
		m_player->pause();
		syncAudio(false);
		m_atEnd = true;
		return;
	}
	m_arrangedSegment = index;
	m_arrangedPlaying = true;
	m_atEnd = false;
	const auto clip = clips[index];
	if (clip.still >= 0) { previewStill(clip.still, true); return; }
	clearStill();
	m_position = source >= clip.begin && source < clip.end ? source : clip.begin;
	m_player->setPosition(m_position);
	m_player->play();
	syncAudio(true);
}
void KRRClipEditor::stepFrame(int direction)
{
	if (m_project.input.isEmpty()) return;
	const qint64 frame = qRound64(editPosition() * m_project.fps / 1000.0);
	seekTimeline(static_cast<qint64>(std::floor(qMax<qint64>(0, frame + direction) * 1000.0 / m_project.fps + 1e-7)));
}
void KRRClipEditor::requestFrame()
{
	if (m_project.input.isEmpty() || m_stillPreview >= 0 || m_player->state() == QMediaPlayer::PlayingState)
		return;
	if (m_frameProcess.state() != QProcess::NotRunning)
	{
		m_frameProcess.kill();
		return;
	}
	m_frameProcess.readAllStandardOutput();
	m_frameProcess.readAllStandardError();
	m_frameTime = m_position;
	const double seconds = std::floor(qRound64(m_position * m_project.fps / 1000.0) * 1000000.0 / m_project.fps) / 1000000.0;
	m_frameProcess.start(FindFFmpeg(), {"-v", "error", "-nostdin", "-ss", QString::number(seconds, 'f', 6),
										"-i", m_project.input, "-frames:v", "1", "-vf",
										"scale=w='min(1920,iw)':h='min(1080,ih)':force_original_aspect_ratio=decrease:flags=lanczos",
										"-f", "image2pipe",
										"-vcodec", "png", "pipe:1"});
}
void KRRClipEditor::togglePlayback()
{
	if (m_project.input.isEmpty())
		return;
	if (m_stillPreview >= 0)
	{
		if (m_stillTimer.isActive()) { m_stillTimer.stop(); m_play->setText(Ui("播放")); }
		else previewStill(m_stillPreview, true);
		return;
	}
	if (m_player->state() == QMediaPlayer::PlayingState)
	{
		m_player->pause();
		seek(m_position);
		return;
	}
	if (m_project.arranged())
	{
		const auto clips = m_project.segments();
		int index = 0;
		if (!m_atEnd)
			for (int i = 0; i < clips.size(); ++i)
				if (clips[i].still < 0 && clips[i].begin <= m_position && m_position < clips[i].end) { index = i; break; }
		playArrangedSegment(index, m_atEnd ? -1 : m_position);
		return;
	}
	qint64 next = m_project.nextKept(m_position);
	if (next >= m_project.duration || m_atEnd)
		next = m_project.kept.front().begin;
	m_position = next;
	m_nextStill = nextStill(next);
	m_atEnd = false;
	if (m_nextStill >= 0 && m_project.stills[m_nextStill].at == next)
	{
		previewStill(m_nextStill,true);
		return;
	}
	m_player->setPosition(next);
	m_player->play();
	syncAudio(true);
}
void KRRClipEditor::syncAudio(bool seekAudio)
{
	if (!m_player)
		return;
	bool playing = m_player->state() == QMediaPlayer::PlayingState && !m_player->isBuffering();
	const auto timeline = editPosition();
	const double original = m_narrationDucking.gainAt(timeline);
	for (int i = 0; i < m_audioPlayers.size() && i < m_project.audio.size(); ++i)
	{
		auto player = m_audioPlayers[i];
		const auto& track = m_project.audio[i];
		if (track.needsRefresh)
		{
			player->pause();
			player->setVolume(0);
			continue;
		}
		bool audible = false;
		for (auto r : track.kept)
			audible |= m_position >= r.begin && m_position < r.end;
		const bool external = !track.file.isEmpty();
		bool inClip = !external || (m_position >= track.offset && m_position < track.offset+track.length);
		audible &= inClip;
		auto target = qMax<qint64>(0,m_position-track.offset);
		if (m_project.arranged())
		{
			inClip = audible = false;
			for (const auto& clip : track.placed)
				if (timeline >= clip.begin && timeline < clip.end)
				{
					target = clip.mediaStart + timeline - clip.begin;
					inClip = audible = true;
					break;
				}
		}
		const double gain = track.gain * (external ? 1 : original);
		player->setVolume(track.enabled && audible ? qMin(100, qRound(gain * 100)) : 0);
		if (player->playbackRate() != m_player->playbackRate()) player->setPlaybackRate(m_player->playbackRate());
		if (player->media().isNull())
			continue;
		if (playing && inClip && (seekAudio || std::abs(player->position() - target) > 120))
			player->setPosition(target);
		if (playing && inClip && player->state() != QMediaPlayer::PlayingState)
			player->play();
		else if ((!playing || !inClip) && player->state() == QMediaPlayer::PlayingState)
			player->pause();
	}
}
void KRRClipEditor::clearStill()
{
	m_stillTimer.stop();
	m_stillPreview = -1;
	m_canvas->setStill({});
	m_play->setText(m_player && m_player->state() == QMediaPlayer::PlayingState ? Ui("暂停") : Ui("播放"));
}
int KRRClipEditor::nextStill(qint64 from) const
{
	for (const auto &segment : m_project.segments())
		if (segment.still >= 0 && segment.begin >= from) return segment.still;
	return -1;
}
void KRRClipEditor::previewStill(int index, bool playing)
{
	if (index < 0 || index >= m_project.stills.size()) return;
	m_player->pause();
	m_frameTimer.stop();
	const auto &still = m_project.stills[index];
	const auto image = LoadEditStill(still.image);
	if (image.isNull()) { m_status->setText(Ui("无法读取静止画面。")); return; }
	m_cropButton->setChecked(false);
	m_stillPreview = index;
	m_position = still.at;
	m_timeline->setPosition(editPosition());
	m_canvas->setStill(image,still.caption);
	m_time->setText(EditTime(still.at) + Ui(" · 静止画面"));
	m_play->setText(playing ? Ui("暂停") : Ui("播放"));
	if (playing) m_stillTimer.start(qRound(still.length / m_player->playbackRate()));
}
void KRRClipEditor::editStill(int index)
{
	if (m_project.input.isEmpty()) return;
	const auto at = index >= 0 ? m_project.stills[index].at : m_project.nextKept(m_position);
	if (index < 0)
	{
		if (at >= m_project.duration && !m_atEnd) { m_status->setText(Ui("请在保留的画面区间插入。")); return; }
		for (int i=0;i<m_project.stills.size();++i)
			if (m_project.stills[i].at == at) { editStill(i); return; }
		if (m_project.stills.size() >= 24) { m_status->setText(Ui("一个工程最多可插入 24 段静止画面。")); return; }
	}
	m_player->pause();
	m_stillTimer.stop();
	auto draft = index >= 0 ? m_project.stills[index] : KRREditStill{at,3000,{},{}};
	QDialog dialog(this);
	dialog.setWindowTitle(Ui("静止画面"));
	auto form = new QFormLayout(&dialog);
	auto preview = new QLabel(&dialog);
	preview->setFixedSize(P(300),P(169));
	preview->setAlignment(Qt::AlignCenter);
	form->addRow(preview);
	auto update = [&] { preview->setPixmap(QPixmap::fromImage(LoadEditStill(draft.image)).scaled(preview->size(),Qt::KeepAspectRatio,Qt::SmoothTransformation)); };
	update();
	auto image = new QPushButton(Ui("选择本地图片…"),&dialog);
	auto presets = new QComboBox(&dialog);
	presets->addItem(Ui("默认画面"), QString{});
	presets->addItem(Ui("山湖暮色"), ":/edit/alpine-lake.png");
	presets->addItem(Ui("暮光书桌"), ":/edit/study-at-dusk.png");
	if (!draft.image.isEmpty() && presets->findData(draft.image) < 0)
		presets->addItem(QFileInfo(draft.image).fileName(), draft.image);
	presets->setCurrentIndex(qMax(0, presets->findData(draft.image)));
	connect(presets, qOverload<int>(&QComboBox::activated), &dialog, [&](int i) { draft.image = presets->itemData(i).toString(); update(); });
	auto grab = new QPushButton(Ui("保存当前帧…"), &dialog);
	const auto currentFrame = m_canvas->currentFrame();
	grab->setEnabled(!currentFrame.isNull());
	connect(grab, &QPushButton::clicked, &dialog, [&]
	{
		const auto file = QFileDialog::getSaveFileName(&dialog, Ui("保存当前帧"), m_folder + "/still-" + StampEdit() + ".png", "PNG (*.png)");
		if (file.isEmpty()) return;
		QSaveFile imageFile(file);
		if (!imageFile.open(QIODevice::WriteOnly) || !currentFrame.save(&imageFile, "PNG") || !imageFile.commit())
		{
			QMessageBox::warning(&dialog, Ui("静止画面"), Ui("无法保存当前帧。"));
			return;
		}
		draft.image = file;
		presets->addItem(QFileInfo(file).fileName(), file);
		presets->setCurrentIndex(presets->count() - 1);
		update();
	});
	auto choices = new QHBoxLayout;
	choices->addWidget(presets, 1); choices->addWidget(image); choices->addWidget(grab);
	form->addRow(choices);
	connect(image,&QPushButton::clicked,&dialog,[&]{
		auto path = QFileDialog::getOpenFileName(&dialog,Ui("选择静止图片"),m_folder,Ui("图片 (*.png *.jpg *.jpeg *.bmp *.webp)"));
		if (path.isEmpty()) return;
		if (LoadEditStill(path).isNull()) { QMessageBox::warning(&dialog,Ui("无法打开图片"),Ui("请选择可读取且不超过 8192 像素的图片。")); return; }
		draft.image = path;
		presets->addItem(QFileInfo(path).fileName(), path);
		presets->setCurrentIndex(presets->count() - 1);
		update();
	});

	auto seconds = new QDoubleSpinBox(&dialog);
	seconds->setRange(.1,600); seconds->setDecimals(2); seconds->setSuffix(Ui(" 秒")); seconds->setValue(draft.length/1000.);
	form->addRow(Ui("时长"),seconds);
	auto caption = new QLineEdit(draft.caption,&dialog); caption->setMaxLength(2000);
	form->addRow(Ui("静止画面字幕"),caption);
	auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&dialog);
	buttons->button(QDialogButtonBox::Ok)->setText(Ui("保存"));
	buttons->button(QDialogButtonBox::Cancel)->setText(Ui("取消"));
	bool remove = false;
	if (index >= 0)
		connect(buttons->addButton(Ui("移除此静止画面"),QDialogButtonBox::DestructiveRole),&QPushButton::clicked,&dialog,[&]{remove=true;dialog.accept();});
	connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);
	connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
	form->addRow(buttons);
	if (dialog.exec() != QDialog::Accepted) return;
	checkpoint();
	draft.length=qRound64(seconds->value()*1000);
	draft.caption=caption->text().trimmed();
	if (remove) m_project.stills.removeAt(index);
	else if (index >= 0) m_project.stills[index]=draft;
	else m_project.stills.append(draft);
	std::sort(m_project.stills.begin(),m_project.stills.end(),[](const auto &a,const auto &b){return a.at<b.at;});
	clearStill();
	refresh();
	if (remove) seek(at);
	else for (int i=0;i<m_project.stills.size();++i)
		if (m_project.stills[i].at == at) { previewStill(i,false); break; }
}
void KRRClipEditor::importAudio()
{
	if (m_project.input.isEmpty() || m_project.audio.size() >= 16) return;
	const auto file = QFileDialog::getOpenFileName(this,Ui("导入外部音频"),m_folder,
		Ui("音频 (*.wav *.mp3 *.m4a *.aac *.flac *.ogg *.opus *.wma);;所有文件 (*)"));
	if (file.isEmpty()) return;
	const bool arranged = m_project.arranged();
	const auto at = editPosition();
	KRREditTrack track;
	QString error;
	if (!ProbeEditAudio(file, arranged ? 0 : at, m_project.duration, track, error))
	{
		m_status->setText(UiDiagnostic(error));
		return;
	}
	if (arranged)
		track.placed = {{at, qMin<qint64>(86400000, at + track.length), 0}};
	m_player->pause();
	checkpoint();
	m_project.audio.append(track);
	m_track = m_project.audio.size()-1;
	refresh();
	select(at, at + track.length);
	m_status->setText(Ui("已从播放头导入，可选区保留或静音、调音量、移除轨道。超出成片末尾的部分不导出。"));
}
void KRRClipEditor::editSubtitleStyle()
{
	if (m_project.input.isEmpty()) return;
	m_player->pause();
	m_stillTimer.stop();
	const auto original = m_project.subtitleStyle;
	const int track = qMax(0, SelectedSubtitleTrack(m_track));
	const auto originalPosition = m_project.subtitlePositions[track];
	auto draft = original;
	auto draftPosition = originalPosition;
	QDialog dialog(this);
	dialog.setWindowTitle(Ui("字幕样式"));
	auto form = new QFormLayout(&dialog);
	auto font = new QFontComboBox(&dialog);
	auto pixels = new QSpinBox(&dialog);
	pixels->setRange(0,320); pixels->setSpecialValueText(Ui("自动（默认）"));
	auto opacity = new QSpinBox(&dialog);
	opacity->setRange(0,100); opacity->setSuffix(" %");
	auto color = new QPushButton(Ui("选择颜色"),&dialog);
	auto horizontal = new QSpinBox(&dialog);
	auto vertical = new QSpinBox(&dialog);
	for (auto field : {horizontal, vertical}) { field->setRange(0, 100); field->setSuffix(" %"); }
	auto sample = new QLabel(Ui("字幕样式 · Subtitle"),&dialog);
	sample->setAlignment(Qt::AlignCenter); sample->setMinimumHeight(P(70));
	form->addRow(sample);
	form->addRow(Ui("字体"),font); form->addRow(Ui("字号（成片像素）"),pixels); form->addRow(Ui("不透明度"),opacity); form->addRow(Ui("文字颜色"),color);
	form->addRow(Ui("字幕轨水平位置"), horizontal);
	form->addRow(Ui("字幕轨垂直位置"), vertical);
	auto update = [&] {
		m_project.subtitleStyle=draft;
		m_project.subtitlePositions[track] = draftPosition;
		m_canvas->update();
		QFont display(draft.font);
		display.setPixelSize(qBound(P(12),draft.pixels ? draft.pixels : P(26),P(42)));
		sample->setFont(display);
		sample->setStyleSheet(QString("background:#162132;color:rgba(%1,%2,%3,%4);border-radius:8px;")
			.arg(draft.color.red()).arg(draft.color.green()).arg(draft.color.blue()).arg(draft.color.alpha()));
	};
	auto sync = [&]{
		QSignalBlocker f(font),p(pixels),o(opacity),h(horizontal),v(vertical);
		font->setCurrentFont(QFont(draft.font)); pixels->setValue(draft.pixels); opacity->setValue(qRound(draft.color.alphaF()*100));
		horizontal->setValue(qRound(draftPosition.x() * 100));
		vertical->setValue(qRound(draftPosition.y() * 100));
		update();
	};
	connect(font,&QFontComboBox::currentFontChanged,&dialog,[&](const QFont &value){draft.font=value.family();update();});
	connect(pixels,qOverload<int>(&QSpinBox::valueChanged),&dialog,[&](int value){draft.pixels=value==0?0:qMax(8,value);update();});
	connect(opacity,qOverload<int>(&QSpinBox::valueChanged),&dialog,[&](int value){draft.color.setAlpha(qRound(value*255./100));update();});
	connect(horizontal, qOverload<int>(&QSpinBox::valueChanged), &dialog, [&](int value){ draftPosition.setX(value / 100.0); update(); });
	connect(vertical, qOverload<int>(&QSpinBox::valueChanged), &dialog, [&](int value){ draftPosition.setY(value / 100.0); update(); });
	connect(color,&QPushButton::clicked,&dialog,[&]{
		auto c=QColorDialog::getColor(draft.color,&dialog);
		if(c.isValid()){c.setAlpha(draft.color.alpha());draft.color=c;update();}
	});
	auto import = new QPushButton(Ui("导入 ASS / JSON 样式…"),&dialog);
	auto reset = new QPushButton(Ui("恢复默认"),&dialog);
	auto row = new QHBoxLayout; row->addWidget(import); row->addWidget(reset); form->addRow(row);
	connect(import,&QPushButton::clicked,&dialog,[&]{
		const auto path=QFileDialog::getOpenFileName(&dialog,Ui("导入字幕样式"),m_folder,Ui("字幕样式 (*.ass *.json)"));
		QString error;
		if(path.isEmpty()) return;
		if(!ReadEditSubtitleStyle(path,draft,error)) QMessageBox::warning(&dialog,Ui("无法导入样式"),UiDiagnostic(error));
		else sync();
	});
	connect(reset,&QPushButton::clicked,&dialog,[&]{draft={};draftPosition=QPointF(.5, .9375);sync();});
	auto buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&dialog);
	connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);
	connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
	form->addRow(buttons); sync();
	const bool accepted = dialog.exec()==QDialog::Accepted;
	m_project.subtitleStyle=original;
	m_project.subtitlePositions[track] = originalPosition;
	if (accepted && draft.valid() && (draft.toJson() != original.toJson() || draftPosition != originalPosition))
	{
		checkpoint();
		m_project.subtitleStyle = draft;
		m_project.subtitlePositions[track] = draftPosition;
	}
	refresh();
}
void KRRClipEditor::chooseCaption(int index)
{
	if (index < 0 || index >= m_project.captions.size())
		return;
	m_caption = index;
	auto c = m_project.captions[index];
	m_track = SubtitleTrackSelection(c.track);
	m_timeline->setTrack(m_track);
	m_target->setCurrentIndex(m_target->findData(m_track));
	m_inspector->setCurrentIndex(1);
	select(c.begin, c.end);
	seekTimeline(c.begin);
	m_captionText->setPlainText(c.text);
	m_refreshing = true;
	for (int row = 0; row < m_captionList->count(); ++row)
		if (m_captionList->item(row)->data(Qt::UserRole).toInt() == index)
		{
			m_captionList->setCurrentRow(row);
			break;
		}
	m_refreshing = false;
}
void KRRClipEditor::moveClip(const KRREditClipMove& move)
{
	if (isBusy() || m_project.input.isEmpty() || m_lockedTracks.contains(move.track)) return;
	auto candidate = m_project;
	qint64 placedAt, placedEnd;
	int caption = -1;
	if (move.track == -1)
	{
		candidate.arrange();
		auto& video = candidate.orderedVideo;
		if (move.clip < 0 || move.clip >= video.size() || move.destination < 0 ||
			move.destination >= video.size() || move.destination == move.clip) return;
		const auto clip = video[move.clip];
		const auto before = m_linkTracks ? candidate : KRREditProject{};
		video.move(move.clip, move.destination);
		if (m_linkTracks) RemapLinkedTracks(before, candidate, m_lockedTracks);
		placedAt = candidate.outputTime(clip.begin);
		placedEnd = candidate.outputTime(clip.end - 1) + 1;
	}
	else
	{
		if (move.clip < 0 || move.clip >= move.ranges.size()) return;
		placedAt = move.ranges[move.clip].begin;
		placedEnd = move.ranges[move.clip].end;
		if (move.track >= 0 && move.track < candidate.audio.size())
		{
			candidate.arrange();
			auto& clips = candidate.audio[move.track].placed;
			if (clips.size() != move.ranges.size()) return;
			for (int i = 0; i < clips.size(); ++i)
			{
				clips[i].begin = move.ranges[i].begin;
				clips[i].end = move.ranges[i].end;
			}
			std::sort(clips.begin(), clips.end(), [](const auto& a, const auto& b) { return a.begin < b.begin; });
		}
		else if (move.track < -1 && move.indexes.size() == move.ranges.size())
		{
			for (int i = 0; i < move.indexes.size(); ++i)
			{
				const int index = move.indexes[i];
				if (index < 0 || index >= candidate.captions.size() ||
					candidate.captions[index].track != SelectedSubtitleTrack(move.track)) return;
				auto& cue = candidate.captions[index];
				cue.begin = move.ranges[i].begin;
				cue.end = move.ranges[i].end;
			}
			caption = move.indexes[move.clip];
			candidate.markDubbingTrackStale();
		}
		else return;
	}
	QString error;
	if (!candidate.validate(error)) { m_status->setText(UiDiagnostic(error)); return; }
	checkpoint();
	m_project = std::move(candidate);
	refresh();
	if (caption >= 0) chooseCaption(caption);
	else
	{
		select(placedAt, placedEnd);
		seekTimeline(placedAt);
	}
	m_clipSelected = true;
	refreshClipActions();
}

void KRRClipEditor::splitSelectedTrack()
{
	if (isBusy() || m_project.input.isEmpty() || m_lockedTracks.contains(m_track)) return;
	auto candidate = m_project;
	candidate.arrange();
	const auto at = m_project.arranged() ? editPosition() : candidate.outputTime(m_position);
	bool changed = false;
	auto splitAudio = [&](int track)
	{
		if (m_lockedTracks.contains(track)) return;
		auto& clips = candidate.audio[track].placed;
		for (int i = 0; i < clips.size(); ++i)
		{
			const auto clip = clips[i];
			if (clip.begin >= at || at >= clip.end) continue;
			clips[i].end = at;
			clips.insert(i + 1, {at, clip.end, clip.mediaStart + at - clip.begin});
			changed = true;
			break;
		}
	};
	auto splitCaptions = [&](int track)
	{
		if (m_lockedTracks.contains(track)) return;
		for (int i = 0; i < candidate.captions.size(); ++i)
		{
			auto cue = candidate.captions[i];
			if (cue.track != SelectedSubtitleTrack(track) || cue.begin >= at || at >= cue.end) continue;
			candidate.captions[i].end = at;
			cue.begin = at;
			candidate.captions.insert(i + 1, cue);
			candidate.markDubbingTrackStale();
			changed = true;
			break;
		}
	};
	if (m_track == -1 || (m_linkTracks && !m_lockedTracks.contains(-1)))
	{
		auto& clips = candidate.orderedVideo;
		for (int i = 0; i < clips.size(); ++i)
		{
			const auto clip = clips[i];
			if (clip.begin >= m_position || m_position >= clip.end) continue;
			clips[i].end = m_position;
			clips.insert(i + 1, {m_position, clip.end});
			changed = true;
			break;
		}
	}
	if (m_track >= 0 || m_linkTracks)
		for (int i = 0; i < candidate.audio.size(); ++i)
			if (m_linkTracks || i == m_track) splitAudio(i);
	if (m_track < -1 || m_linkTracks)
		for (int i = 0; i < candidate.subtitlePositions.size(); ++i)
			if (m_linkTracks || SubtitleTrackSelection(i) == m_track) splitCaptions(SubtitleTrackSelection(i));
	if (!changed) return;
	QString error;
	if (!candidate.validate(error)) { m_status->setText(UiDiagnostic(error)); return; }
	checkpoint();
	m_project = candidate;
	refresh();
	select(at, at);
	seekTimeline(at);
	m_status->setText(Ui("已在播放头处切分。点击片段后可拖动、删除或导出。"));
}

void KRRClipEditor::changeProtectedRanges(bool protect)
{
	if (m_project.input.isEmpty() || isBusy()) return;
	auto candidate = m_project;
	const KRREditRange selection{m_begin, m_end};
	if (selection.end - selection.begin < 20)
	{
		m_status->setText(Ui("请选择至少 0.02 秒的有效区间。"));
		return;
	}
	if (protect)
	{
		candidate.protectedRanges.append(selection);
		candidate.protectedRanges = MergeEditRanges(candidate.protectedRanges);
	}
	else
	{
		QVector<KRREditRange> remaining;
		for (const auto& range : candidate.protectedRanges)
		{
			if (range.begin < selection.begin) remaining.append({range.begin, qMin(range.end, selection.begin)});
			if (range.end > selection.end) remaining.append({qMax(range.begin, selection.end), range.end});
		}
		candidate.protectedRanges = remaining;
	}
	QString error;
	if (!candidate.validate(error)) { m_status->setText(UiDiagnostic(error)); return; }
	if (candidate.toJson() == m_project.toJson()) return;
	checkpoint();
	m_project = candidate;
	refresh();
	m_status->setText(Ui("当前有 %1 个必保区间。").arg(m_project.protectedRanges.size()));
}

void KRRClipEditor::applyCut(const KRREditProject& candidate)
{
	if (candidate.toJson() == m_project.toJson()) return;
	const bool addedChapters = candidate.chapters.size() > m_project.chapters.size();
	checkpoint();
	m_project = candidate;
	m_track = -1;
	m_caption = -1;
	refresh();
	select(m_project.kept.first().begin, m_project.kept.first().end);
	seek(m_project.kept.first().begin);
	if (addedChapters) m_assetsTabs->setCurrentWidget(m_chapterPage);
	m_status->setText(Ui("已应用剪辑。可用一次撤销恢复。成片 %1。").arg(EditTime(m_project.outputDuration())));
}

void KRRClipEditor::refreshChapters()
{
	const auto selected = m_chapterList->currentItem();
	const auto selectedAt = selected ? selected->data(Qt::UserRole).toLongLong() : -1;
	const auto projected = m_project.outputChapters();
	m_chapterList->clear();
	int index = 0, row = -1;
	for (const auto& chapter : m_project.chapters)
	{
		const bool kept = EditRangesContain(m_project.kept, {chapter.at, chapter.at + 1});
		const auto output = kept && index < projected.size() ? Ui("成片 %1").arg(EditTime(projected[index++].at)) : Ui("已剪除");
		auto item = new QListWidgetItem(chapter.title + '\n' +
			Ui("源 %1").arg(EditTime(chapter.at)) + "  ·  " + output, m_chapterList);
		item->setData(Qt::UserRole, chapter.at);
		if (chapter.at == selectedAt) row = m_chapterList->count() - 1;
	}
	m_chapterList->setCurrentRow(row);
}

void KRRClipEditor::editChapter(bool rename)
{
	if (m_project.input.isEmpty()) return;
	const auto title = m_chapterTitle->text().trimmed();
	const auto row = m_chapterList->currentRow();
	if (rename && (row < 0 || row >= m_project.chapters.size())) return;
	auto candidate = m_project;
	const auto at = rename ? candidate.chapters[row].at : qMin(m_position, candidate.duration - 1);
	if (rename) candidate.chapters.removeAt(row);
	QString error;
	if (!candidate.addChapter(at, title, error)) { m_status->setText(error); return; }
	checkpoint();
	m_project = candidate;
	refresh();
	for (int i = 0; i < m_project.chapters.size(); ++i)
		if (m_project.chapters[i].at == at) { m_chapterList->setCurrentRow(i); break; }
}

void KRRClipEditor::showBatchCut()
{
	if (m_project.input.isEmpty() || isBusy()) return;
	m_player->pause();
	syncAudio(false);
	KRREditProject candidate;
	if (BatchCut(this, m_project, {m_begin, m_end}, candidate)) applyCut(candidate);
}

void KRRClipEditor::showSmartCut()
{
	reviewSmartCut(nullptr, false);
}

void KRRClipEditor::runRequestedSmartCut()
{
	if (!m_requestedCut) return;
	const auto request = *m_requestedCut;
	m_requestedCut.reset();
	if (m_candidateHost)
	{
		auto host = m_candidateHost.data();
		m_candidateHost.clear();
		const auto revision = SmartCutRevision(m_project);
		DockSmartCut(this, host, m_project, {m_begin, m_end}, request, m_requestedChapters,
			[this, revision](const KRREditProject& result, bool subtitles)
		{
			if (isBusy() || SmartCutRevision(m_project) != revision)
			{
				QMessageBox::information(this, Ui("智能剪辑"), Ui("当前工程已变化，请按最新工程重新生成草稿。"));
				return;
			}
			if (!subtitles)
			{
				applyCut(result);
				emit studioCutApplied();
			}
		});
		return;
	}
	reviewSmartCut(&request, m_requestedChapters);
}

void KRRClipEditor::reviewSmartCut(const KRRSmartCutOptions* request, bool chapters)
{
	if (m_project.input.isEmpty() || isBusy()) return;
	m_player->pause();
	syncAudio(false);
	KRREditProject candidate;
	bool requestSubtitles = false;
	if (!ReviewSmartCut(this, m_project, {m_begin, m_end}, candidate, requestSubtitles, request, chapters)) return;
	if (!requestSubtitles) { applyCut(candidate); return; }
	if (m_asrSettings->exec() == QDialog::Accepted)
	{
		autoCaptions();
		m_returnToSmartCut = m_asrInProgress;
	}
}
void KRRClipEditor::editCaption(bool update)
{
	if (m_project.input.isEmpty())
		return;
	auto candidate = m_project;
	KRREditCaption c{qRound64(m_captionBegin->value() * 1000), qRound64(m_captionEnd->value() * 1000),
					 m_captionText->toPlainText().trimmed()};
	c.track = qMax(0, SelectedSubtitleTrack(m_track));
	if (update)
	{
		if (m_caption < 0 || m_caption >= candidate.captions.size())
		{
			m_status->setText(Ui("请先选择要更新的字幕。"));
			return;
		}
		const auto& before = candidate.captions[m_caption];
		c.track = before.track;
		if (before.text == c.text) c.translation = before.translation;
		c.narrationText = before.narrationText;
		c.pronunciation = before.pronunciation;
		c.dubbingFile = before.dubbingFile;
		c.dubbingText = before.dubbingText;
		c.dubbingNeedsRefresh = before.dubbingNeedsRefresh;
		if (!c.dubbingFile.isEmpty() && (before.begin != c.begin || before.end != c.end))
		{
			c.dubbingFile.clear();
			c.dubbingText.clear();
			c.dubbingNeedsRefresh = true;
			candidate.markDubbingTrackStale();
		}
		KRRPronunciations readings;
		QString detail;
		if (ParsePronunciations(candidate.pronunciationGuide, readings, detail))
			InvalidateStoredSubtitleTake(candidate, c, readings);
		candidate.captions[m_caption] = c;
	}
	else
		candidate.captions.append(c);
	if (m_lockedTracks.contains(SubtitleTrackSelection(c.track))) return;
	QString error;
	if (!candidate.captionFits(c, update ? m_caption : candidate.captions.size() - 1))
	{
		m_status->setText(Ui("字幕时间无效，或与同轨字幕重叠。"));
		return;
	}
	if (!candidate.validate(error))
	{
		m_status->setText(UiDiagnostic(error));
		return;
	}
	checkpoint();
	m_project = candidate;
	int editedIndex = update ? m_caption : m_project.captions.size() - 1;
	int selectedIndex = 0;
	for (int i = 0; i < m_project.captions.size(); ++i)
	{
		const auto &other = m_project.captions[i];
		if (other.begin < c.begin || (other.begin == c.begin && i < editedIndex))
			++selectedIndex;
	}
	std::stable_sort(m_project.captions.begin(), m_project.captions.end(),
					 [](const KRREditCaption &a, const KRREditCaption &b) { return a.begin < b.begin; });
	refresh();
	chooseCaption(selectedIndex);
	m_status->setText(Ui("字幕已加入预览与时间轴；可修改文字和开始/结束时间。"));
}
void KRRClipEditor::importCaptions(const QString &path, bool sourceTimes)
{
	if (m_project.input.isEmpty())
		return;
	QVector<KRREditCaption> captions;
	QString error;
	if (!ReadEditSubtitles(path, captions, error))
	{
		m_status->setText(UiDiagnostic(error));
		return;
	}
	const auto duration = sourceTimes ? m_project.duration : m_project.editDuration();
	for (auto& cue : captions)
	{
		cue.begin = qMax<qint64>(0, cue.begin);
		cue.end = qMin(cue.end, duration);
	}
	if (sourceTimes && m_project.arranged())
	{
		QVector<KRREditCaption> placed;
		qint64 at = 0;
		for (const auto& segment : m_project.segments())
		{
			if (segment.still < 0)
				for (auto cue : captions)
				{
					const auto begin = qMax(cue.begin, segment.begin), end = qMin(cue.end, segment.end);
					if (begin >= end) continue;
					cue.begin = at + begin - segment.begin;
					cue.end = at + end - segment.begin;
					placed.append(cue);
				}
			at += segment.end - segment.begin;
		}
		captions = std::move(placed);
	}
	captions.erase(std::remove_if(captions.begin(), captions.end(), [](const auto& cue) { return cue.begin >= cue.end; }), captions.end());
	std::stable_sort(captions.begin(), captions.end(), [](const auto& a, const auto& b) { return a.begin < b.begin; });
	for (int i = 0; i + 1 < captions.size(); ++i) captions[i].end = qMin(captions[i].end, captions[i + 1].begin);
	captions.erase(std::remove_if(captions.begin(), captions.end(), [](const auto& cue) { return cue.begin >= cue.end; }), captions.end());
	if (captions.isEmpty())
	{
		m_status->setText(Ui("字幕文件在当前时间轴内没有可导入的内容。"));
		return;
	}
	auto candidate = m_project;
	const int selected = qMax(0, SelectedSubtitleTrack(m_track));
	int track = -1;
	for (int i = 0; i < candidate.subtitlePositions.size(); ++i)
	{
		const int index = (selected + i) % candidate.subtitlePositions.size();
		// A single imported file stays on one track, including when that track already has cues.
		const bool occupied = std::any_of(candidate.captions.cbegin(), candidate.captions.cend(), [&](const auto& cue)
		{
			if (index != cue.track) return false;
			const auto next = std::upper_bound(captions.cbegin(), captions.cend(), cue.begin,
				[](qint64 at, const auto& item) { return at < item.end; });
			return next != captions.cend() && next->begin < cue.end;
		});
		if (!occupied) { track = index; break; }
	}
	if (track < 0)
	{
		track = candidate.subtitlePositions.size();
		if (track >= 16)
		{
			m_status->setText(Ui("现有字幕轨在此区间都有内容。请腾出一个字幕轨后再导入。"));
			return;
		}
		candidate.subtitlePositions.append(QPointF(.5, qMax(.12, .9375 - track * .12)));
	}
	for (auto cue : captions)
	{
		cue.track = track;
		candidate.captions.append(cue);
	}
	if (!candidate.validate(error))
	{
		m_status->setText(UiDiagnostic(error));
		return;
	}
	checkpoint();
	m_project = candidate;
	std::stable_sort(m_project.captions.begin(), m_project.captions.end(),
					 [](const KRREditCaption &a, const KRREditCaption &b) { return a.begin < b.begin; });
	refresh();
	m_status->setText(Ui("已在字幕轨 %1 添加 %2 条字幕。自动识别文本请人工校对。").arg(track + 1).arg(captions.size()));
}
void KRRClipEditor::autoCaptions()
{
	if (m_project.audio.isEmpty())
	{
		m_status->setText(Ui("源视频没有声音轨道。"));
		return;
	}
	if (!m_cache || m_asr.isBusy())
	{
		m_status->setText(Ui("请等待当前识别结束。"));
		return;
	}
	QString directory = QCoreApplication::applicationDirPath() + "/whisper",
			executable = directory + "/whisper-cli.exe", model = directory + "/" + m_asrModel->currentData().toString();
	if (!QFileInfo::exists(executable))
		executable = QFileDialog::getOpenFileName(this, Ui("选择本地 whisper-cli.exe"), {}, "Whisper (whisper-cli.exe)");
	if (executable.isEmpty())
		return;
	if (!QFileInfo::exists(model))
		model = QFileDialog::getOpenFileName(this, Ui("选择本地多语言 GGML 模型"), {}, "GGML (*.bin)");
	if (model.isEmpty())
		return;
	QString output = m_cache->filePath("transcript-" + StampEdit() + ".srt");
	m_asrRequestedLanguage = m_language->currentData().toString();
	m_asrInProgress = m_asr.transcribe(m_project.input, output, executable, model, m_asrRequestedLanguage,
									 m_asrTrack->currentData().toInt());
	m_status->setText(Ui("正在本地识别选定声音轨道，完成后字幕自动加入时间轴…"));
}
void KRRClipEditor::showSubtitles()
{
	m_inspector->setCurrentIndex(1);
}
void KRRClipEditor::showSubtitleWorkflow()
{
	showSubtitles();
	showDubbingWorkflow(false);
}

void KRRClipEditor::showDubbingWorkflow(bool chapter)
{
	if (m_project.input.isEmpty())
	{
		if (!chapter && m_sourceLoading) m_pendingSubtitleWorkflow = true;
		return;
	}
	if (chapter && m_project.chapters.isEmpty() && m_project.chapterNarration.isEmpty())
	{
		m_status->setText(Ui("先在播放头处添加章节，再导入章节长稿。"));
		return;
	}
	if (isBusy())
	{
		m_status->setText(Ui("请等待当前识别或导出结束。"));
		return;
	}
	m_player->pause();
	syncAudio(false);
	KRRSubtitleDialog dialog(m_project, m_folder, this, chapter, editPosition());
	connect(this, &KRRClipEditor::smartCutInterrupted, &dialog, &KRRSubtitleDialog::reviewInterrupted);
	if (chapter) connect(this, &KRRClipEditor::smartCutInterrupted, &dialog, &KRRSubtitleDialog::reject);
	if (dialog.exec() != QDialog::Accepted) return;
	const auto& draft = dialog.draft();
	const bool changed = draft.toJson() != m_project.toJson();
	if (changed)
	{
		const bool addedNarration = draft.audio.size() > m_project.audio.size() && !draft.audio.last().sentenceDubbing;
		checkpoint();
		m_project = draft;
		if (addedNarration)
		{
			const auto& track = m_project.audio.last();
			const auto start = m_project.arranged() ? track.placed.first().begin : track.offset;
			chooseTrack(m_project.audio.size() - 1);
			seekTimeline(start);
			select(start, start + track.length);
		}
		else refresh();
	}
	if (dialog.requestedAsr())
	{
		if (m_asrSettings->exec() == QDialog::Accepted)
		{
			autoCaptions();
			m_reopenAfterAsr = m_asrInProgress;
		}
	}
	else if (changed && chapter)
		m_status->setText(m_project.dubbingTrackNeedsRefresh(true) ? Ui("章节旁白稿已应用。生成新旁白轨后可继续试听。") :
			Ui("章节旁白设置已应用。保存剪辑工程可继续编辑。"));
	else if (changed)
		m_status->setText(m_project.dubbingTrackNeedsRefresh() ? Ui("字幕已应用。配音轨待更新，可在字幕与配音中重做。") :
			Ui("改动已应用到时间轴。保存剪辑工程可继续编辑。"));
}
namespace
{
QString DraftFor(const QString& identity)
{
	const auto key = QCryptographicHash::hash(QDir::cleanPath(identity).toCaseFolded().toUtf8(), QCryptographicHash::Sha256).toHex();
	return DataDirectory() + "/drafts/" + QString::fromLatin1(key) + ".json";
}

bool WriteProjectFile(const QString& path, const QJsonObject& object)
{
	QSaveFile file(path);
	const auto data = QJsonDocument(object).toJson();
	return file.open(QIODevice::WriteOnly) && file.write(data) == data.size() && file.commit();
}
}

QStringList KRRClipEditor::draftFiles()
{
	const auto entries = QDir(DataDirectory() + "/drafts").entryInfoList({"*.json"}, QDir::Files | QDir::NoSymLinks, QDir::Time);
	QStringList files;
	for (const auto& file : entries) files.append(file.absoluteFilePath());
	return files;
}

QStringList KRRClipEditor::projectMaterials() const
{
	QStringList files;
	if (!m_project.input.isEmpty()) files << m_project.input;
	for (const auto& audio : m_project.audio) if (!audio.file.isEmpty()) files << audio.file;
	for (const auto& still : m_project.stills) if (!still.image.isEmpty() && !still.image.startsWith(":")) files << still.image;
	files.removeDuplicates();
	return files;
}

QString KRRClipEditor::draftPath() const
{
	return DraftFor(m_projectPath.isEmpty() ? m_project.input : m_projectPath);
}

bool KRRClipEditor::saveDraft()
{
	if (!m_dirty || m_project.input.isEmpty()) return true;
	auto content = m_project.toJson();
	content["draftOf"] = m_projectPath;
	const auto path = draftPath();
	if (QDir().mkpath(QFileInfo(path).absolutePath()) && WriteProjectFile(path, content)) return true;
	m_status->setText(Ui("无法自动保存剪辑草稿。"));
	return false;
}

void KRRClipEditor::restoreDraft(const QString& input)
{
	QJsonObject content;
	QString error;
	KRREditProject project;
	if (ReadEditProjectJson(DraftFor(input), content, error) && KRREditProject::FromJson(content, project, error)
		&& QFileInfo(project.input).absoluteFilePath() == input)
		m_pendingProject = std::make_unique<KRREditProject>(std::move(project));
}

void KRRClipEditor::saveProject()
{
	if (m_project.input.isEmpty()) return;
	auto path = m_projectPath;
	if (path.isEmpty()) path = QFileDialog::getSaveFileName(this, Ui("保存剪辑工程"),
		m_folder + "/edit-" + StampEdit() + ".json", Ui("剪辑工程 (*.json)"));
	if (path.isEmpty()) return;
	if (!WriteProjectFile(path, m_project.toJson()))
	{
		m_status->setText(Ui("无法保存剪辑工程。"));
		return;
	}
	QFile::remove(draftPath());
	m_projectPath = path;
	m_dirty = false;
	refresh();
	emit sourceChanged(m_project.input);
	m_status->setText(Ui("工程已保存：") + path);
}

void KRRClipEditor::loadProject()
{
	const auto path = QFileDialog::getOpenFileName(this, Ui("打开剪辑工程"), m_folder, Ui("剪辑工程 (*.json)"));
	if (!path.isEmpty()) openProjectFile(path, m_folder);
}

bool KRRClipEditor::openProjectFile(const QString& path, const QString& folder)
{
	QJsonObject object;
	QString error;
	if (!ReadEditProjectJson(path, object, error))
	{
		QMessageBox::warning(this, Ui("打开剪辑工程"), error);
		return false;
	}
	if (object["kind"].toString() == "sequence")
	{
		m_player->pause();
		KRREditSequenceDialog dialog(m_project, folder, this);
		connect(this, &KRRClipEditor::smartCutInterrupted, &dialog, &KRREditSequenceDialog::interruptSmartCut);
		dialog.openProject(path);
		dialog.exec();
		return true;
	}
	KRREditProject project;
	if (!KRREditProject::FromJson(object, project, error))
	{
		QMessageBox::warning(this, Ui("打开剪辑工程"), error.isEmpty() ? Ui("工程 JSON 无效。") : error);
		return false;
	}
	if (!confirmReplace()) return false;
	auto savedPath = QFileInfo(path).absoluteFilePath();
	if (QFileInfo(savedPath).absolutePath() == QDir(DataDirectory() + "/drafts").absolutePath())
		{
		const auto original = object["draftOf"].toString();
		savedPath = !original.isEmpty() && DraftFor(original).compare(QDir::fromNativeSeparators(savedPath), Qt::CaseInsensitive) == 0
			? original : QString{};
	}
	else
	{
		QJsonObject recovered;
		KRREditProject candidate;
		if (ReadEditProjectJson(DraftFor(savedPath), recovered, error)
			&& KRREditProject::FromJson(recovered, candidate, error)
			&& candidate.input == project.input) project = std::move(candidate);
	}
	m_dirty = false;
	m_project.input.clear();
	if (!openFile(project.input, folder)) return false;
	m_pendingProject = std::make_unique<KRREditProject>(std::move(project));
	m_projectPath = savedPath;
	return true;
}
void KRRClipEditor::exportProject()
{
	exportTimeline({});
}

void KRRClipEditor::exportTimeline(const KRRTimelineExportOptions& options)
{
	QString error;
	if (!m_project.validate(error))
	{
		m_status->setText(UiDiagnostic(error));
		return;
	}
	if (m_exporter.isBusy())
	{
		m_status->setText(Ui("正在导出，请稍候。"));
		return;
	}
	QDialog dialog(this);
	dialog.setWindowTitle(Ui("导出剪辑成片"));
	auto layout = new QFormLayout(&dialog);
	auto format = new QComboBox(&dialog);
	format->addItems({"mp4", "mov", "mkv"});
	auto encoder = new QComboBox(&dialog);
	encoder->addItem(Ui("自动：优先 GPU"), "auto");
	encoder->addItem("CPU · x264", "libx264");
	encoder->addItem("NVIDIA · NVENC", "h264_nvenc");
	auto separate = new QCheckBox(Ui("保留独立声音轨道"), &dialog);
	separate->setChecked(true);
	auto burn = new QCheckBox(Ui("将字幕写入画面（所有播放器可见）"), &dialog);
	burn->setChecked(true);
	burn->setEnabled(options.previewLength == 0);
	layout->addRow(Ui("格式"), format);
	layout->addRow(Ui("编码"), encoder);
	layout->addRow(separate);
	layout->addRow(burn);
	auto note = new QLabel(Ui("关闭字幕写入时，字幕保存为可选字幕轨。工程按源时间剪辑，导出自动拼接保留区间。"), &dialog);
	note->setWordWrap(true);
	layout->addRow(note);
	auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
	buttons->button(QDialogButtonBox::Ok)->setText(Ui("选择位置并导出"));
	buttons->button(QDialogButtonBox::Cancel)->setText(Ui("取消"));
	layout->addRow(buttons);
	connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	RecorderUi::PolishButtons(&dialog);
	if (dialog.exec() != QDialog::Accepted)
		return;
	auto output = QFileDialog::getSaveFileName(this, Ui("保存剪辑成片"),
											   m_folder + "/edited-" + StampEdit() + "." + format->currentText(),
											   Ui("视频 (*.") + format->currentText() + ")");
	if (output.isEmpty())
		return;
	m_player->pause();
	if (m_exporter.renderTimeline(m_project, output, encoder->currentData().toString(), separate->isChecked(),
								  burn->isChecked(), options))
		m_status->setText(Ui("正在导出点击时的剪辑计划，可继续编辑；原视频保持完整。"));
}
void KRRClipEditor::exportSubmission()
{
	QString error;
	if (!m_project.validate(error) || m_exporter.isBusy())
	{
		m_status->setText(error.isEmpty() ? Ui("正在导出，请稍候。") : UiDiagnostic(error));
		return;
	}
	qint64 at = 0;
	const auto sourceAt = m_stillPreview >= 0 ? m_project.stills.value(m_stillPreview).at : m_project.nextKept(m_position);
	for (const auto& segment : m_project.segments())
	{
		if (segment.begin > sourceAt || (segment.still >= 0 && segment.still == m_stillPreview)) break;
		at += segment.still >= 0 ? segment.end - segment.begin : qMin(sourceAt, segment.end) - segment.begin;
		if (segment.still < 0 && sourceAt < segment.end) break;
	}
	m_player->pause();
	syncAudio(false);
	KRRSubmission plan;
	if (ChooseSubmission(this, m_project, m_folder, at, plan) &&
		m_exporter.renderTimeline(plan.project, plan.output, "auto", plan.separateAudio, plan.burnCaptions, plan.options))
		m_status->setText(Ui("正在导出提交成片…"));
}
bool KRRClipEditor::releaseIdleSource(const QString &path)
{
	if (m_project.input.compare(QFileInfo(path).absoluteFilePath(), Qt::CaseInsensitive) != 0) return true;
	if (isBusy() || isVisible()) return false;
	m_analysis.stop();
	clearMedia();
	m_sourceReleased = true;
	return true;
}
void KRRClipEditor::discardReleasedSource(const QString& path)
{
	if (!m_sourceReleased || m_project.input.compare(QFileInfo(path).absoluteFilePath(), Qt::CaseInsensitive) != 0) return;
	m_sourceReleased = m_dirty = false;
	m_project = {};
	m_cache.reset();
	m_undo.clear();
	m_redo.clear();
	refresh();
}
void KRRClipEditor::closeEvent(QCloseEvent *event)
{
	if (!prepareClose())
	{
		event->ignore();
		return;
	}
	pausePlayback();
	event->accept();
}
void KRRClipEditor::keyPressEvent(QKeyEvent *event)
{
	if (event->key() == Qt::Key_Space)
	{
		togglePlayback();
		event->accept();
	}
	else if (event->matches(QKeySequence::Undo))
	{
		undo();
		event->accept();
	}
	else if (event->matches(QKeySequence::Redo))
	{
		undo(true);
		event->accept();
	}
	else if (event->key() == Qt::Key_Left)
	{
		stepFrame(-1);
		event->accept();
	}
	else if (event->key() == Qt::Key_Right)
	{
		stepFrame(1);
		event->accept();
	}
	else
		QDialog::keyPressEvent(event);
}
void KRRClipEditor::reject()
{
	if (isWindow() && !prepareClose()) return;
	pausePlayback();
	if (isWindow()) QDialog::reject();
}
void KRRClipEditor::pausePlayback()
{
	m_player->pause();
	m_arrangedPlaying = false;
	m_stillTimer.stop();
	syncAudio(false);
}
bool KRRClipEditor::confirmApplicationClose()
{
	return saveDraft();
}
void KRRClipEditor::cancelTasks()
{
	m_draftTimer.stop();
	m_reopenAfterAsr = false;
	m_returnToSmartCut = false;
	m_exporter.cancel();
	m_asr.cancel();
	interruptSmartCut();
}
bool KRRClipEditor::prepareClose()
{
	const auto mode = isBusy() ? ChooseTaskExit(this) : KRRTaskExit::Save;
	if (mode == KRRTaskExit::Return) return false;
	if (mode == KRRTaskExit::Save && !saveDraft() && ChooseTaskExit(this, true) != KRRTaskExit::Discard)
		return false;
	cancelTasks();
	return true;
}
