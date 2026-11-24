#include "subtitledialog.h"
#include "previewvideo.h"
#include "chapternarration.h"
#include "recordingconfig.h"
#include "speechservices.h"
#include "speechsettingsdialog.h"
#include "subtitlereviewdialog.h"
#include "uilanguage.h"
#include "uitheme.h"

#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMediaPlayer>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTableWidget>
#include <QToolButton>
#include <QUuid>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>

int KRRSubtitleDialog::dubbingTrackIndex() const
{
	for (int i = 0; i < m_project.audio.size(); ++i)
		if (m_project.audio[i].sentenceDubbing && m_project.audio[i].chapterDubbing == m_chapterMode) return i;
	return -1;
}

KRRSubtitleDialog::KRRSubtitleDialog(const KRREditProject& project, const QString& folder, QWidget* owner,
	bool chapter, qint64 position)
	: QDialog(owner), m_chapterMode(chapter), m_project(project), m_folder(folder),
	m_cache(DataDirectory() + "/subtitles-XXXXXX"), m_takes(cues().size())
{
	setObjectName(m_chapterMode ? "chapterNarrationWorkbench" : "subtitleWorkbench");
	setWindowTitle(Ui(m_chapterMode ? "章节旁白" : "字幕与配音"));
	setWindowFlag(Qt::WindowContextHelpButtonHint, false);
	resize(RecorderUi::Px(1040), RecorderUi::Px(690));
	auto layout = new QVBoxLayout(this);
	m_controls = new QWidget(this);
	layout->addWidget(m_controls, 1);
	auto columns = new QHBoxLayout(m_controls);
	columns->setContentsMargins(0, 0, 0, 0);
	const auto button = [this](const char* text, const char* name)
	{
		auto item = new QPushButton(Ui(text), this);
		item->setObjectName(name);
		return item;
	};
	auto captions = new QWidget(this);
	auto content = new QVBoxLayout(captions);
	if (m_chapterMode) columns->addWidget(captions, 1);
	else
	{
		m_sources = new QTabWidget(this);
		m_sources->setObjectName("speechSourceTabs");
		m_sources->addTab(captions, Ui("字幕配音"));
		columns->addWidget(m_sources, 1);
	}
	auto top = new QHBoxLayout;
	m_asr = button("离线识别…", "subtitleTranscribe");
	m_direction = new QComboBox(this);
	m_direction->setObjectName("subtitleDirection");
	m_direction->addItem(Ui(m_chapterMode ? "中文旁白" : "中文 → English"), m_chapterMode ? "zh" : "en");
	m_direction->addItem(Ui(m_chapterMode ? "English 旁白" : "English → 中文"), m_chapterMode ? "en" : "zh");
	int han = 0, latin = 0;
	for (const auto& line : cues())
		for (const auto ch : line.text)
			if (ch.unicode() >= 0x3400 && ch.unicode() <= 0x9fff) ++han;
			else if (ch.isLetter() && ch.unicode() < 128) ++latin;
	const bool english = m_project.subtitleLanguage == "en" ||
		(m_project.subtitleLanguage != "zh" && (m_project.translationLanguage == "zh" ||
			(m_project.translationLanguage.isEmpty() && latin > han * 3)));
	m_sourceLanguage = english ? QStringLiteral("en") : QStringLiteral("zh");
	m_direction->setCurrentIndex((m_chapterMode ?
		(m_project.chapterNarrationLanguage == "en" || (m_project.chapterNarrationLanguage.isEmpty() && latin > han * 3)) :
		english) ? 1 : 0);
	m_translateAll = button("翻译全部", "translateAll");
	m_translateOne = button("翻译选句", "translateLine");
	m_review = button("AI 校对…", "reviewSubtitles");
	m_export = button("导出字幕…", "exportBilingual");
	auto translateToggle = new QToolButton(this);
	translateToggle->setText(Ui("翻译（可选）"));
	translateToggle->setObjectName("showTranslationOptions");
	translateToggle->setCheckable(true);
	translateToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
	auto translateOptions = new QWidget(this);
	auto words = new QVBoxLayout(translateOptions);
	words->setContentsMargins(0, 0, 0, 0);
	auto translateActions = new QHBoxLayout;
	for (auto item : QList<QWidget*>{m_direction, m_translateAll, m_translateOne, m_review}) translateActions->addWidget(item);
	translateActions->addStretch();
	if (!m_chapterMode) words->addLayout(translateActions);
	else delete translateActions;
	m_glossary = new QLineEdit(this);
	m_glossary->setObjectName("subtitleGlossary");
	m_glossary->setMaxLength(1000);
	m_glossary->setText(m_project.subtitleGlossary);
	m_glossary->setPlaceholderText(Ui("可选术语，例如 Qt Creator 保持原样"));
	words->addWidget(m_glossary);
	if (!m_chapterMode)
	{
		m_subtitleTrack = new QComboBox(this);
		m_subtitleTrack->setObjectName("subtitleTrackFilter");
		for (int i = 0; i < m_project.subtitlePositions.size(); ++i)
			m_subtitleTrack->addItem(Ui("字幕轨 %1").arg(i + 1), i);
		top->addWidget(m_subtitleTrack);
		top->addWidget(m_asr);
		top->addWidget(translateToggle);
		top->addStretch();
		top->addWidget(m_export);
	}
	if (m_chapterMode)
	{
		for (auto* item : {m_asr, m_translateAll, m_translateOne, m_review, m_export}) item->hide();
		translateToggle->hide();
		auto import = button("导入长稿…", "importNarrationScript");
		top->addWidget(import);
		top->addStretch();
		connect(import, &QPushButton::clicked, this, &KRRSubtitleDialog::importChapterScript);
		connect(m_direction, qOverload<int>(&QComboBox::currentIndexChanged), this, [this]
		{
			m_project.chapterNarrationLanguage = m_direction->currentData().toString();
			updateVoice();
		});
	}
	content->addLayout(top);
	content->addWidget(translateOptions);
	m_search = new QLineEdit(this);
	m_search->setPlaceholderText(Ui("搜索原文或译文"));
	m_search->setClearButtonEnabled(true);
	content->addWidget(m_search);
	QString glossaryError;
	ParsePronunciations(m_project.pronunciationGuide, m_pronunciations, glossaryError);
	m_table = new QTableWidget(0, 4, this);
	m_table->setObjectName("subtitleLines");
	m_table->setHorizontalHeaderLabels({Ui("起止时间"), Ui(m_chapterMode ? "旁白稿" : "原文"),
		Ui(m_chapterMode ? "章节" : "译文"), Ui("配音")});
	auto header = m_table->horizontalHeader();
	header->setSectionResizeMode(QHeaderView::Stretch);
	header->setSectionResizeMode(0, QHeaderView::Fixed);
	header->setSectionResizeMode(3, QHeaderView::Fixed);
	const auto cellFont = m_table->fontMetrics();
	const auto headerFont = header->fontMetrics();
	const auto time = EditTime(qMax<qint64>(m_project.duration, 359'999'999));
	const int padding = RecorderUi::Px(18);
	const int timeWidth = cellFont.horizontalAdvance(time + "–" + time);
	const int statusWidth = qMax(cellFont.horizontalAdvance(Ui("当前读法待生成")),
		cellFont.horizontalAdvance(Ui("超时 %1 秒").arg("8888.88")));
	header->resizeSection(0, qMax(timeWidth, headerFont.horizontalAdvance(m_table->horizontalHeaderItem(0)->text())) + padding);
	header->resizeSection(3, qMax(statusWidth, headerFont.horizontalAdvance(m_table->horizontalHeaderItem(3)->text())) + padding);
	m_table->verticalHeader()->setDefaultSectionSize(RecorderUi::Px(32));
	m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
	m_table->setSelectionMode(QAbstractItemView::SingleSelection);
	m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
	for (int i = 0; i < cues().size(); ++i)
	{
		const auto& line = cues()[i];
		const auto duration = line.dubbingFile.isEmpty() ? 0 : SpeechWaveDuration(line.dubbingFile);
		if (duration > 0) m_takes[i] = {line.dubbingFile, line.dubbingText, duration, false};
	}
	resetRows();
	content->addWidget(m_table, 1);

	auto edits = new QHBoxLayout;
	m_original = new QPlainTextEdit(this);
	m_translation = new QPlainTextEdit(this);
	m_original->setObjectName("subtitleOriginal");
	m_translation->setObjectName("subtitleTranslation");
	m_original->setPlaceholderText(Ui(m_chapterMode ? "旁白稿" : "原文"));
	m_translation->setPlaceholderText(Ui("译文"));
	for (auto field : {m_original, m_translation})
	{
		field->setFixedHeight(RecorderUi::Px(66));
		edits->addWidget(field);
	}
	content->addLayout(edits);
	m_narration = new QLineEdit(this);
	m_narration->setObjectName("sentenceNarration");
	m_narration->setMaxLength(2000);
	m_narration->setPlaceholderText(Ui("可选配音稿。留空读所选原文或译文"));
	m_narration->setAccessibleName(Ui("本句配音稿"));
	auto sentence = new QHBoxLayout;
	sentence->addWidget(m_narration, 1);
	m_lineReading = button("本句读音规则…", "subtitleLinePronunciation");
	sentence->addWidget(m_lineReading);
	content->addLayout(sentence);
	auto actions = new QHBoxLayout;
	m_make = button("生成选句", "synthesizeLine");
	m_batch = button("生成待配句", "synthesizeBatch");
	m_add = button("生成配音轨", "addDubTrack");
	actions->addWidget(m_make);
	actions->addWidget(m_batch);
	actions->addStretch();
	actions->addWidget(m_add);
	content->addLayout(actions);

	auto showTranslation = [=](bool open)
	{
		translateToggle->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
		translateOptions->setVisible(open);
		m_translation->setVisible(open);
		m_table->setColumnHidden(2, !open && !m_chapterMode);
	};
	connect(translateToggle, &QToolButton::toggled, this, showTranslation);
	translateToggle->setChecked(!m_chapterMode && std::any_of(cues().cbegin(), cues().cend(),
		[](const auto& line) { return !line.translation.isEmpty(); }));
	showTranslation(translateToggle->isChecked());

	if (m_sources)
	{
		auto page = new QWidget(this);
		auto body = new QVBoxLayout(page);
		auto scriptLabel = new QLabel(Ui("旁白稿"), page);
		m_freeText = new QPlainTextEdit(page);
		m_freeText->setObjectName("freeNarrationText");
		m_freeText->setPlaceholderText(Ui("输入最多 4000 字的旁白…"));
		scriptLabel->setBuddy(m_freeText);
		body->addWidget(scriptLabel);
		body->addWidget(m_freeText, 1);
		m_freeStart = new QDoubleSpinBox(page);
		m_freeStart->setObjectName("freeNarrationStart");
		m_freeStart->setDecimals(3);
		m_freeStart->setRange(0., qMax<qint64>(0, (project.arranged() ? project.outputDuration() : project.duration) - 1) / 1000.);
		m_freeStart->setValue(position / 1000.);
		m_freeStart->setSuffix(Ui(" 秒"));
		auto placement = new QFormLayout;
		placement->addRow(project.arranged() ? Ui("起点（成片时间）") : Ui("起点（源片时间）"), m_freeStart);
		body->addLayout(placement);
		m_makeNarration = button("生成旁白", "synthesizeNarration");
		body->addWidget(m_makeNarration, 0, Qt::AlignLeft);
		m_sources->addTab(page, Ui("自填旁白"));
	}
	else { m_narration->hide(); m_lineReading->hide(); }

	auto pane = new QWidget(this);
	pane->setMinimumWidth(RecorderUi::Px(268));
	pane->setMaximumWidth(RecorderUi::Px(320));
	columns->addWidget(pane);
	auto side = new QVBoxLayout(pane);
	side->setContentsMargins(0, 0, 0, 0);
	auto settings = new QGroupBox(Ui("配音设置"), pane);
	side->addWidget(settings);
	side->addStretch();
	auto voices = new QFormLayout(settings);
	voices->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	m_engine = new QComboBox(this);
	m_engine->setObjectName("sentenceEngine");
	m_engine->addItem(Ui("Audio8 Preview 0.1B · 本地"), "audio8");
	RefreshTtsServiceChoices(m_engine);
	m_read = new QComboBox(this);
	m_read->setObjectName("sentenceLanguage");
	m_read->addItem(Ui("读原文"), "original");
	m_read->addItem(Ui("读译文"), "translation");
	const bool hasTranslatedTake = std::any_of(cues().cbegin(), cues().cend(), [this](const auto& line)
	{
		return !line.dubbingText.isEmpty() && line.dubbingText == SubtitleSpokenText(line, true, m_pronunciations) &&
			line.dubbingText != SubtitleSpokenText(line, false, m_pronunciations);
	});
	m_read->setCurrentIndex(hasTranslatedTake ? 1 : 0);
	auto languages = new QStackedWidget(settings);
	if (!m_chapterMode)
	{
		m_freeLanguage = new QComboBox(this);
		m_freeLanguage->setObjectName("freeNarrationLanguage");
		m_freeLanguage->addItem(Ui("中文"), "zh");
		m_freeLanguage->addItem("English", "en");
		m_freeLanguage->setCurrentIndex(m_freeLanguage->findData(m_sourceLanguage));
		languages->addWidget(m_read);
		languages->addWidget(m_freeLanguage);
	}
	else
	{
		m_read->hide();
		languages->addWidget(m_direction);
	}
	m_voice = new QComboBox(this);
	m_voice->setObjectName("sentenceVoice");
	m_voice->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	m_voice->setMinimumContentsLength(12);
	m_engine->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	m_engine->setMinimumContentsLength(14);
	m_speed = new QDoubleSpinBox(this);
	m_speed->setObjectName("sentenceSpeed");
	m_speed->setRange(.5, 2);
	m_speed->setSingleStep(.1);
	m_speed->setValue(1);
	m_speed->setSuffix(" ×");
	voices->addRow(Ui("朗读"), languages);
	voices->addRow(Ui("配音模型"), m_engine);
	voices->addRow(Ui("音色"), m_voice);
	voices->addRow(Ui("语速"), m_speed);
	m_dubVolume = new QSpinBox(this);
	m_dubVolume->setObjectName("dubbingVolume");
	m_dubVolume->setRange(0, 100);
	m_dubVolume->setSuffix("%");
	const auto priorTrack = dubbingTrackIndex();
	m_dubVolume->setValue(priorTrack < 0 ? 100 : qRound(m_project.audio[priorTrack].gain * 100));
	voices->addRow(Ui("配音音量"), m_dubVolume);
	connect(m_dubVolume, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value)
	{
		const int track = dubbingTrackIndex();
		if (track >= 0) m_project.audio[track].gain = value / 100.;
		if (m_player && m_previewEnd < 0) m_player->setVolume(value);
	});
	if (m_chapterMode)
	{
		m_duckPercent = new QSpinBox(this);
		m_duckPercent->setObjectName("narrationOriginalVolume");
		m_duckPercent->setAccessibleName(Ui("旁白期间原声"));
		m_duckPercent->setRange(0, 100);
		m_duckPercent->setSuffix("%");
		const int existing = dubbingTrackIndex();
		m_duckPercent->setValue(existing < 0 ? 35 : qRound(m_project.audio[existing].originalGain * 100));
		connect(m_duckPercent, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value)
		{
			const int track = dubbingTrackIndex();
			if (track >= 0) m_project.audio[track].originalGain = value / 100.;
		});
		voices->addRow(Ui("旁白期间原声"), m_duckPercent);
	}
	m_audio8Directory = button("本地模型目录…", "audio8Directory");
	voices->addRow(m_audio8Directory);
	m_voiceHint = new QLabel(this);
	m_voiceHint->setTextFormat(Qt::PlainText);
	m_voiceHint->setWordWrap(true);
	voices->addRow(m_voiceHint);
	auto engineTools = new QHBoxLayout;
	m_readings = button("读音词表…", "subtitlePronunciations");
	auto services = button("服务设置…", "speechSettings");
	engineTools->addWidget(m_readings);
	engineTools->addWidget(services);
	voices->addRow(engineTools);
	auto auditionTools = new QVBoxLayout;
	m_originalPlay = button("听原片", "playSourceLine");
	m_voicePlay = button("试听本句", "playDubLine");
	m_voiceSample = button("试听音色", "previewSpeechVoice");
	auditionTools->addWidget(m_voiceSample);
	auditionTools->addWidget(m_originalPlay);
	auditionTools->addWidget(m_voicePlay);
	voices->addRow(Ui("试听"), auditionTools);

	m_status = new QLabel(this);
	m_status->setObjectName("subtitleStatus");
	m_status->setTextFormat(Qt::PlainText);
	m_status->setWordWrap(true);
	layout->addWidget(m_status);
	auto footer = new QHBoxLayout;
	footer->addStretch();
	m_cancel = button("取消当前任务", "cancelSpeechWork");
	auto close = button("取消", "discardSubtitleDraft");
	m_apply = button("应用到时间轴", "applySubtitleDraft");
	m_apply->setProperty("role", "primary");
	footer->addWidget(m_cancel);
	footer->addWidget(close);
	footer->addWidget(m_apply);
	layout->addLayout(footer);
	m_player = new QMediaPlayer(this);
	m_player->setObjectName("subtitleAudioPlayer");
	m_player->setNotifyInterval(50);
	m_originalPreview = std::make_unique<KRRCompatiblePreview>();
	connect(m_originalPreview.get(), &KRRCompatiblePreview::ready, this, [this](const QString& file)
	{
		if (m_job != Job::Idle || narrationMode() || m_row < 0 || m_waitingOriginalRow != m_row) return;
		const auto line = cues()[m_row];
		showStatus(Ui("原声试听已就绪。"));
		playFile(file, line.begin, line.end);
	});
	connect(m_originalPreview.get(), &KRRCompatiblePreview::failed, this, [this](const QString& error)
	{
		if (m_waitingOriginalRow >= 0) showStatus(error);
		m_waitingOriginalRow = -1;
	});
	connect(m_player, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus status)
	{
		if (m_previewEnd >= 0 && m_row >= 0 &&
			(status == QMediaPlayer::LoadedMedia || status == QMediaPlayer::BufferedMedia) &&
			m_player->position() < cues()[m_row].begin)
			m_player->setPosition(cues()[m_row].begin);
	});
	connect(m_player, &QMediaPlayer::positionChanged, this,
		[this](qint64 t) { if (m_previewEnd >= 0 && t >= m_previewEnd) m_player->pause(); });
	connect(m_player, &QMediaPlayer::stateChanged, this, &KRRSubtitleDialog::updateAudition);
	connect(m_player, qOverload<QMediaPlayer::Error>(&QMediaPlayer::error), this,
		[this](QMediaPlayer::Error error)
	{
		if (error == QMediaPlayer::NoError || m_originalPreview->isPreparing()) return;
		const auto originalFile = m_originalPreview->file();
		if (m_previewEnd >= 0 && m_row >= 0 && !originalFile.isEmpty() &&
			m_player->media().request().url().toLocalFile() == originalFile && m_originalPreview->retryWithEncoding())
		{
			m_waitingOriginalRow = m_row;
			showStatus(Ui("正在准备原声试听…"));
		}
		else showStatus(m_player->errorString());
	});

	connect(m_table, &QTableWidget::currentCellChanged, this, [this](int row) { loadRow(row); });
	connect(m_search, &QLineEdit::textChanged, this, &KRRSubtitleDialog::filterVisibleRows);
	if (m_subtitleTrack)
		connect(m_subtitleTrack, qOverload<int>(&QComboBox::currentIndexChanged), this, &KRRSubtitleDialog::filterVisibleRows);
	connect(m_original, &QPlainTextEdit::textChanged, this, &KRRSubtitleDialog::storeRow);
	connect(m_translation, &QPlainTextEdit::textChanged, this, &KRRSubtitleDialog::storeRow);
	connect(m_narration, &QLineEdit::textChanged, this, &KRRSubtitleDialog::storeRow);
	connect(m_glossary, &QLineEdit::textChanged, this, [this](const QString& text) { m_project.subtitleGlossary = text; });
	connect(m_review, &QPushButton::clicked, this, &KRRSubtitleDialog::review);
	connect(m_readings, &QPushButton::clicked, this, &KRRSubtitleDialog::editPronunciations);
	connect(m_lineReading, &QPushButton::clicked, this, &KRRSubtitleDialog::editLinePronunciation);
	connect(services, &QPushButton::clicked, this, [this]
	{
		const auto engine = m_engine->currentData().toString();
		ConfigureSpeechServices(this, IsTtsService(engine) ? engine : "tts");
		RefreshTtsServiceChoices(m_engine);
		updateVoice();
	});
	connect(m_audio8Directory, &QPushButton::clicked, this, [this]
	{
		if (ConfigureAudio8Directory(this)) updateVoice();
	});
	connect(m_voice, &QComboBox::currentTextChanged, this, &KRRSubtitleDialog::updateControls);
	connect(m_speed, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &KRRSubtitleDialog::updateControls);
	if (m_sources)
	{
		connect(m_sources, &QTabWidget::currentChanged, this, [this, languages](int index)
		{
			m_previewEnd = -1;
			m_player->stop();
			m_player->setMedia({});
			languages->setCurrentIndex(index);
			updateVoice();
			showStatus(narrationMode() ? Ui("输入旁白稿，生成后可试听并加入时间轴。") : QString());
		});
		connect(m_freeLanguage, qOverload<int>(&QComboBox::currentIndexChanged), this, &KRRSubtitleDialog::updateVoice);
		connect(m_freeText, &QPlainTextEdit::textChanged, this, &KRRSubtitleDialog::updateControls);
		connect(m_makeNarration, &QPushButton::clicked, this, &KRRSubtitleDialog::generateNarration);
	}
	connect(m_engine, qOverload<int>(&QComboBox::currentIndexChanged), this, &KRRSubtitleDialog::updateVoice);
	connect(m_read, qOverload<int>(&QComboBox::currentIndexChanged), this, [this]
	{
		for (int i = 0; i < cues().size(); ++i) refreshRow(i);
		updateVoice();
		updateControls();
	});
	connect(m_asr, &QPushButton::clicked, this, [this] { if (saveDraft()) { m_requestedAsr = true; accept(); } });
	connect(m_translateAll, &QPushButton::clicked, this, [this] { translate(false); });
	connect(m_translateOne, &QPushButton::clicked, this, [this] { translate(true); });
	connect(m_originalPlay, &QPushButton::clicked, this, [this] { play(true); });
	connect(m_make, &QPushButton::clicked, this, [this] { generate(true); });
	connect(m_voicePlay, &QPushButton::clicked, this, [this] { play(false); });
	connect(m_voiceSample, &QPushButton::clicked, this, &KRRSubtitleDialog::previewVoice);
	connect(m_batch, &QPushButton::clicked, this, [this] { generate(false); });
	connect(m_add, &QPushButton::clicked, this, &KRRSubtitleDialog::buildTrack);
	connect(m_export, &QPushButton::clicked, this, &KRRSubtitleDialog::exportSubtitles);
	connect(m_apply, &QPushButton::clicked, this, [this]
	{
		if (narrationMode() ? applyNarration() : saveDraft()) accept();
	});
	connect(m_cancel, &QPushButton::clicked, this, [this]
	{
		m_cancelled = true;
		m_worker.cancel();
		showStatus(Ui("正在取消…"));
	});
	connect(close, &QPushButton::clicked, this, &KRRSubtitleDialog::reject);
	connect(&m_worker, &KRRMediaEditor::progress, this, [this](const auto& text)
		{ if (!m_cancelled) showStatus(text); });
	connect(&m_worker, &KRRMediaEditor::translated, this, [this](const QVector<QString>& lines)
	{
		if (m_cancelled || lines.size() != m_translationRows.size()) return;
		for (int i = 0; i < lines.size(); ++i)
		{
			if (lines[i].isEmpty()) continue;
			const auto row = m_translationRows[i];
			cues()[row].translation = lines[i];
			invalidateStaleTake(row);
			refreshRow(row);
		}
		m_project.translationLanguage = m_direction->currentData().toString();
		m_project.subtitleLanguage = m_project.translationLanguage == "en" ? "zh" : "en";
		loadRow(m_row);
		updateVoice();
	});
	connect(&m_worker, &KRRMediaEditor::completed, this, &KRRSubtitleDialog::completed);

	filterVisibleRows();
	updateVoice();
	updateControls();
	showStatus(m_project.dubbingTrackNeedsRefresh(m_chapterMode) ? Ui("配音轨待更新。重做受影响的句子后，点击「更新配音轨」。") :
		m_chapterMode ? Ui("导入章节长稿，逐句生成并试听。生成配音轨时按实际时长重新对齐。") :
		m_table->rowCount() ? QString() : Ui("可离线识别生成字幕，导入 SRT，或直接填写旁白。"));
	RecorderUi::PolishButtons(this);
}

KRRSubtitleDialog::~KRRSubtitleDialog()
{
	m_player->blockSignals(true);
	delete m_player;
	m_player = nullptr;
	m_originalPreview->clear();
}

void KRRSubtitleDialog::reject()
{
	if (m_job != Job::Idle)
	{
		m_cancel->click();
		return;
	}
	m_player->stop();
	m_player->setMedia({});
	QDialog::reject();
}

void KRRSubtitleDialog::showStatus(const QString& text)
{
	m_status->setText(UiDiagnostic(text));
}

void KRRSubtitleDialog::resetRows()
{
	const QSignalBlocker block(m_table);
	m_table->setRowCount(cues().size());
	for (int i = 0; i < cues().size(); ++i)
	{
		for (int c = 0; c < 4; ++c)
			if (!m_table->item(i, c)) m_table->setItem(i, c, new QTableWidgetItem);
		refreshRow(i);
	}
}

void KRRSubtitleDialog::filterVisibleRows()
{
	const auto query = m_search->text().trimmed();
	const auto track = m_subtitleTrack ? m_subtitleTrack->currentData().toInt() : -1;
	int first = -1;
	for (int i = 0; i < cues().size(); ++i)
	{
		const auto& cue = cues()[i];
		const bool show = (track < 0 || cue.track == track) &&
			(cue.text.contains(query, Qt::CaseInsensitive) || cue.translation.contains(query, Qt::CaseInsensitive));
		m_table->setRowHidden(i, !show);
		if (show && first < 0) first = i;
	}
	if (m_row < 0 || m_table->isRowHidden(m_row)) m_table->setCurrentCell(first, 0);
}

void KRRSubtitleDialog::importChapterScript()
{
	if (m_job != Job::Idle) return;
	m_player->stop();
	QDialog editor(this);
	editor.setWindowTitle(Ui("导入章节长稿"));
	editor.resize(RecorderUi::Px(700), RecorderUi::Px(520));
	auto layout = new QVBoxLayout(&editor);
	auto help = new QLabel(Ui("按模板填写稿件，也可打开 UTF-8 文本。标题用「## 章节标题」或序号，空章节跳过。导入会替换本工作台的旁白稿。"), &editor);
	help->setWordWrap(true);
	layout->addWidget(help);
	auto manuscript = new QPlainTextEdit(ChapterNarrationScript(m_project), &editor);
	manuscript->setObjectName("chapterNarrationScript");
	layout->addWidget(manuscript, 1);
	auto status = new QLabel(&editor);
	status->setWordWrap(true);
	status->setTextFormat(Qt::PlainText);
	layout->addWidget(status);
	auto footer = new QHBoxLayout;
	auto open = new QPushButton(Ui("打开 UTF-8 文本…"), &editor);
	auto cancel = new QPushButton(Ui("取消"), &editor);
	auto apply = new QPushButton(Ui("分句并对齐"), &editor);
	apply->setObjectName("applyChapterScript");
	footer->addWidget(open);
	footer->addStretch();
	footer->addWidget(cancel);
	footer->addWidget(apply);
	layout->addLayout(footer);
	QVector<KRREditCaption> parsed;
	connect(open, &QPushButton::clicked, &editor, [&, this]
	{
		const auto path = QFileDialog::getOpenFileName(&editor, Ui("打开 UTF-8 文本…"), m_folder, "UTF-8 (*.txt *.md)");
		if (path.isEmpty()) return;
		QFile file(path);
		if (!file.open(QIODevice::ReadOnly) || file.size() > 240000)
		{
			status->setText(Ui("稿件无法读取或超过长度限制。"));
			return;
		}
		const auto text = QString::fromUtf8(file.readAll());
		if (text.contains(QChar::ReplacementCharacter))
		{
			status->setText(Ui("请将文件保存为 UTF-8 后重新打开。"));
			return;
		}
		manuscript->setPlainText(text);
		status->clear();
	});
	connect(cancel, &QPushButton::clicked, &editor, &QDialog::reject);
	connect(apply, &QPushButton::clicked, &editor, [&]
	{
		QString error;
		if (!ReadChapterNarration(m_project, manuscript->toPlainText(), parsed, error)) { status->setText(error); return; }
		editor.accept();
	});
	if (editor.exec() != QDialog::Accepted) return;
	const auto previous = cues();
	const auto same = [](const auto& a, const auto& b) { return a.chapterAt == b.chapterAt && a.text == b.text; };
	if (parsed.size() == previous.size() && std::equal(parsed.begin(), parsed.end(), previous.begin(), same)) return;
	QVector<Take> preserved(parsed.size());
	QVector<bool> used(previous.size(), false);
	for (int i = 0; i < parsed.size(); ++i)
	{
		for (int j = 0; j < previous.size(); ++j)
		{
			if (used[j] || !same(parsed[i], previous[j])) continue;
			preserved[i] = m_takes[j];
			parsed[i].dubbingFile = previous[j].dubbingFile;
			parsed[i].dubbingText = previous[j].dubbingText;
			parsed[i].dubbingNeedsRefresh = previous[j].dubbingNeedsRefresh;
			used[j] = true;
			break;
		}
	}
	m_player->setMedia({});
	for (int j = 0; j < previous.size(); ++j)
		if (!used[j] && m_takes[j].temporary) QFile::remove(m_takes[j].path);
	cues() = parsed;
	m_takes = preserved;
	m_project.markDubbingTrackStale(true);
	resetRows();
	m_table->setCurrentCell(0, 0);
	loadRow(0);
	showStatus(Ui("已导入 %1 句。当前为预估位置，生成配音轨时按音频时长对齐。").arg(parsed.size()));
}

void KRRSubtitleDialog::refreshRow(int row)
{
	const auto& line = cues()[row];
	const auto& take = m_takes[row];
	m_table->item(row, 0)->setText(EditTime(line.begin) + "–" + EditTime(line.end));
	m_table->item(row, 0)->setTextAlignment(Qt::AlignCenter);
	m_table->item(row, 1)->setText(line.text.simplified());
	m_table->item(row, 2)->setText(m_chapterMode ? ChapterNarrationTitle(m_project, line.chapterAt) : line.translation.simplified());
	auto status = take.path.isEmpty() ? QString("—") :
		!m_chapterMode && take.duration > line.end - line.begin ? Ui("超时 %1 秒").arg((take.duration - line.end + line.begin) / 1000.0, 0, 'f', 2) :
		Ui("%1 秒").arg(take.duration / 1000.0, 0, 'f', 2);
	if (line.dubbingNeedsRefresh) status = Ui("待重做");
	else if (m_read && !take.path.isEmpty() && !usableTake(row)) status = Ui("当前读法待生成");
	m_table->item(row, 3)->setText(status);
	m_table->item(row, 3)->setTextAlignment(Qt::AlignCenter);
}

void KRRSubtitleDialog::loadRow(int row)
{
	m_waitingOriginalRow = -1;
	m_row = row;
	m_loading = true;
	m_player->stop();
	m_previewEnd = -1;
	const auto line = cues().value(row);
	m_original->setPlainText(line.text);
	m_translation->setPlainText(line.translation);
	m_narration->setText(line.narrationText);
	m_lineReading->setEnabled(row >= 0);
	m_lineReading->setText(line.pronunciation.isEmpty() ? Ui("本句读音规则…") : Ui("本句读音规则 · 已设置"));
	m_loading = false;
	updateControls();
}

void KRRSubtitleDialog::storeRow()
{
	if (m_loading || m_row < 0 || m_row >= cues().size()) return;
	auto& line = cues()[m_row];
	const auto original = m_original->toPlainText().trimmed();
	const auto translated = m_translation->toPlainText().trimmed();
	const auto narration = m_narration->text().trimmed();
	if (line.text != original || line.translation != translated || line.narrationText != narration)
	{
		const bool changedOriginal = line.text != original;
		line.text = original;
		line.narrationText = narration;
		line.translation = changedOriginal && line.translation == translated ? QString() : translated;
		if (line.translation != translated)
		{
			m_loading = true;
			m_translation->clear();
			m_loading = false;
		}
		if (!m_chapterMode && m_project.translationLanguage.isEmpty() && !translated.isEmpty())
		{
			m_project.translationLanguage = m_direction->currentData().toString();
			m_project.subtitleLanguage = m_project.translationLanguage == "en" ? "zh" : "en";
		}
		invalidateStaleTake(m_row);
		refreshRow(m_row);
	}
	updateControls();
}

QString KRRSubtitleDialog::spoken(int row) const
{
	if (row < 0 || row >= cues().size()) return {};
	return SubtitleSpokenText(cues()[row], !m_chapterMode && m_read->currentData() == "translation", m_pronunciations);
}

bool KRRSubtitleDialog::usableTake(int row) const
{
	return !m_takes[row].path.isEmpty() && m_takes[row].text == spoken(row);
}

void KRRSubtitleDialog::invalidateStaleTake(int row)
{
	auto& line = cues()[row];
	auto& take = m_takes[row];
	InvalidateStoredSubtitleTake(m_project, line, m_pronunciations);
	if (take.path.isEmpty()) return;
	if (SubtitleTakeMatches(line, take.text, m_pronunciations))
	{
		line.dubbingNeedsRefresh = false;
		return;
	}
	if (m_player->media().request().url() == QUrl::fromLocalFile(take.path))
	{
		m_player->stop();
		m_player->setMedia({});
	}
	if (take.temporary) QFile::remove(take.path);
	take = {};
	line.dubbingNeedsRefresh = true;
	m_project.markDubbingTrackStale(m_chapterMode);
}

void KRRSubtitleDialog::editPronunciations()
{
	auto text = m_project.pronunciationGuide;
	for (;;)
	{
		bool accepted = false;
		text = QInputDialog::getMultiLineText(this, Ui("读音词表"),
			Ui("每行填写「术语 => 读音」，例如 GPU => G P U。规则只用于朗读。"), text, &accepted);
		if (!accepted || text == m_project.pronunciationGuide) return;
		KRRPronunciations readings;
		QString error;
		if (!ParsePronunciations(text, readings, error))
		{
			QMessageBox::warning(this, Ui("检查读音词表"), error);
			continue;
		}
		m_pronunciations = readings;
		m_project.pronunciationGuide = text;
		for (int i = 0; i < cues().size(); ++i)
		{
			invalidateStaleTake(i);
			refreshRow(i);
		}
		auto& other = m_chapterMode ? m_project.captions : m_project.chapterNarration;
		for (auto& line : other) InvalidateStoredSubtitleTake(m_project, line, m_pronunciations);
		updateControls();
		showStatus(m_project.dubbingTrackNeedsRefresh(m_chapterMode) ? Ui("配音轨待更新。重做受影响的句子后，点击「更新配音轨」。") :
			Ui("读音词表已更新。可生成一句试听。"));
		return;
	}
}

void KRRSubtitleDialog::editLinePronunciation()
{
	if (m_job != Job::Idle || m_row < 0 || m_row >= cues().size()) return;
	auto& cue = cues()[m_row];
	bool accepted = false;
	auto text = QInputDialog::getMultiLineText(this, Ui("本句读音规则"),
		Ui("每行填写「术语 => 读音」。例如 WPS => W P S。"), cue.pronunciation, &accepted).trimmed();
	if (!accepted || text == cue.pronunciation) return;
	KRRPronunciations local;
	QString error;
	if (!ParsePronunciations(text, local, error)) { showStatus(error); return; }
	cue.pronunciation = text;
	invalidateStaleTake(m_row);
	refreshRow(m_row);
	loadRow(m_row);
}

void KRRSubtitleDialog::review()
{
	if (m_row < 0 || m_job != Job::Idle) return;
	m_player->stop();
	m_player->setMedia({});
	const auto target = m_project.translationLanguage.isEmpty() ? m_direction->currentData().toString() : m_project.translationLanguage;
	KRREditProject candidate;
	int firstChanged = -1;
	if (!ShowSubtitleReview(this, m_project, m_row, target, candidate, firstChanged)) return;
	const auto previous = cues();
	m_project = candidate;
	for (int i = 0; i < cues().size(); ++i)
	{
		if (previous[i].translation == cues()[i].translation &&
			previous[i].narrationText == cues()[i].narrationText) continue;
		invalidateStaleTake(i);
		refreshRow(i);
	}
	m_table->setCurrentCell(firstChanged, 0);
	loadRow(firstChanged);
	showStatus(m_project.dubbingTrackNeedsRefresh(m_chapterMode) ? Ui("配音轨待更新。重做受影响的句子后，点击「更新配音轨」。") :
		Ui("已采纳勾选的建议。核对后可应用到时间轴。"));
}

KRRVoiceOptions KRRSubtitleDialog::voiceOptions() const
{
	KRRVoiceOptions options;
	options.engine = m_engine->currentData().toString();
	options.voice = m_voice->isEditable() ? m_voice->currentText().trimmed() : m_voice->currentData().toString();
	options.speed = m_speed->value();
	const auto target = m_direction->currentData().toString();
	const auto original = m_project.subtitleLanguage == "zh" || m_project.subtitleLanguage == "en" ?
		m_project.subtitleLanguage : m_sourceLanguage;
	options.language = narrationMode() ? m_freeLanguage->currentData().toString() :
		m_chapterMode ? target : m_read->currentData() == "translation" ?
		(m_project.translationLanguage.isEmpty() ? target : m_project.translationLanguage) :
		original;
	return options;
}

void KRRSubtitleDialog::updateVoice()
{
	const auto options = voiceOptions();
	const bool external = IsTtsService(options.engine);
	const bool audio8 = options.engine == "audio8";
	const QString revision = external ? SpeechServiceFingerprint(SpeechService(options.engine)) :
		audio8 ? Audio8Directory() : QString();
	if (options.engine != m_voiceEngine || options.language != m_voiceLanguage || revision != m_voiceRevision)
	{
		const QSignalBlocker block(m_voice);
		m_voiceEngine = options.engine;
		m_voiceLanguage = options.language;
		m_voiceRevision = revision;
		m_voice->clear();
		m_voice->setEditable(external);
		if (audio8)
		{
			for (const auto& name : Audio8Voices())
				m_voice->addItem(name == "default" ? Ui("默认音色") : Ui("普通话女声"), name);
		}
		else if (external)
		{
			const auto primary = DefaultVoice(options.engine, options.language);
			if (!primary.isEmpty()) m_voice->addItem(primary, primary);
			m_voice->lineEdit()->setMaxLength(255);
			m_voice->lineEdit()->setPlaceholderText(Ui("输入服务音色 ID，可留默认"));
		}
	}
	m_voiceAvailable = external || m_voice->count() > 0;
	QString hint;
	m_audio8Directory->setVisible(audio8);
	if (audio8)
	{
		hint = m_voiceAvailable ? Ui("Audio8 按句加载。较长句子可能需要几十秒。") :
			Ui("还未配置 Audio8 本地运行包，请打开“本地模型目录”。");
	}
	else if (external)
		hint = Ui("发送待朗读文字至 %1，音色 ID 可修改。").arg(SpeechServiceHost(options.engine));
	m_voiceHint->setText(hint);
	m_voiceHint->setVisible(!hint.isEmpty());
	updateControls();
}

void KRRSubtitleDialog::updateControls()
{
	const bool idle = m_job == Job::Idle;
	const bool free = narrationMode();
	m_controls->setEnabled(idle);
	m_apply->setText(free ? Ui("加入时间轴") : Ui("应用到时间轴"));
	m_cancel->setVisible(!idle);
	if (free)
	{
		const auto text = narrationText();
		const bool ready = narrationReady();
		m_apply->setEnabled(idle && ready && m_project.audio.size() < 16);
		m_makeNarration->setEnabled(m_voiceAvailable && m_cache.isValid() && !text.isEmpty() && text.size() <= 4000);
		m_makeNarration->setText(ready ? Ui("重新生成旁白") : Ui("生成旁白"));
		if (!ready && m_player->media().request().url() == QUrl::fromLocalFile(m_freeTake.path) &&
			m_player->state() == QMediaPlayer::PlayingState)
			m_player->stop();
		updateAudition();
		return;
	}
	const bool selected = m_row >= 0;
	bool translated = false, ready = false;
	bool allReady = !cues().isEmpty();
	for (int i = 0; i < cues().size(); ++i)
	{
		translated |= !cues()[i].translation.isEmpty();
		const auto& take = m_takes[i];
		ready |= usableTake(i) && take.duration <= cues()[i].end - cues()[i].begin;
		allReady &= usableTake(i);
	}
	const bool take = selected && usableTake(m_row);
	m_apply->setEnabled(idle);
	m_asr->setEnabled(!m_project.audio.isEmpty());
	m_translateAll->setEnabled(!cues().isEmpty());
	m_translateOne->setEnabled(selected);
	m_review->setEnabled(selected);
	m_original->setEnabled(selected);
	m_translation->setEnabled(selected);
	m_narration->setEnabled(selected);
	m_make->setEnabled(m_voiceAvailable && selected && !spoken(m_row).isEmpty() && m_cache.isValid());
	m_make->setText(selected && (take || cues()[m_row].dubbingNeedsRefresh) ? Ui("重做本句") : Ui("生成选句"));
	m_export->setEnabled(!cues().isEmpty());
	m_export->setText(translated ? Ui("导出双语字幕…") : Ui("导出字幕…"));
	m_batch->setEnabled(m_voiceAvailable && !cues().isEmpty() && m_cache.isValid());
	m_add->setEnabled((m_chapterMode ? allReady : ready) && (m_project.audio.size() < 16 || dubbingTrackIndex() >= 0));
	m_add->setText(m_chapterMode ? Ui("对齐并生成旁白轨…") :
		dubbingTrackIndex() >= 0 ? Ui("更新配音轨") : Ui("生成配音轨"));
	m_add->setToolTip(m_project.dubbingTrackNeedsRefresh(m_chapterMode) ? Ui("旧配音轨已暂停播放与导出，更新后恢复。") : QString());
	updateAudition();
}

bool KRRSubtitleDialog::narrationMode() const
{
	return m_sources && m_sources->currentIndex() == 1;
}

QString KRRSubtitleDialog::narrationText() const
{
	return m_pronunciations.apply(m_freeText->toPlainText().trimmed());
}

bool KRRSubtitleDialog::narrationReady() const
{
	if (m_freeTake.path.isEmpty() || m_freeTake.text != narrationText()) return false;
	const auto voice = voiceOptions();
	return m_voiceRevision == m_narratedRevision && voice.engine == m_narratedVoice.engine && voice.voice == m_narratedVoice.voice &&
		voice.speed == m_narratedVoice.speed && voice.language == m_narratedVoice.language;
}

void KRRSubtitleDialog::generateNarration()
{
	const auto text = narrationText();
	if (m_job != Job::Idle || !m_voiceAvailable || text.isEmpty() || text.size() > 4000 || !m_cache.isValid()) return;
	m_player->stop();
	m_player->setMedia({});
	m_queuedVoice = voiceOptions();
	const auto path = m_cache.filePath("narration-" + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".wav");
	if (!m_worker.synthesize(text, path, m_queuedVoice)) return;
	m_job = Job::Narrate;
	m_cancelled = false;
	showStatus(Ui("正在生成旁白，请稍候…"));
	updateControls();
}

bool KRRSubtitleDialog::applyNarration()
{
	if (!narrationReady() || m_project.audio.size() >= 16) return false;
	const auto start = qRound64(m_freeStart->value() * 1000.);
	const auto end = start + m_freeTake.duration;
	const bool arranged = m_project.arranged();
	if (end > (arranged ? m_project.outputDuration() : m_project.duration) || m_freeTake.duration > m_project.duration)
	{
		showStatus(Ui("旁白到 %1，超出可用时长。请提前起点、缩短稿件或加快语速。").arg(EditTime(end)));
		return false;
	}
	if (!arranged && !EditRangesContain(m_project.kept, {start, start + 1}))
	{
		showStatus(Ui("起点在已剪掉的画面中，请选择一个保留区间。"));
		return false;
	}
	KRREditTrack track;
	track.title = Ui("旁白");
	track.gain = m_dubVolume->value() / 100.;
	track.stream = -1;
	track.file = m_freeTake.path;
	track.offset = arranged ? 0 : start;
	track.length = m_freeTake.duration;
	track.kept = {{track.offset, track.offset + track.length}};
	if (arranged) track.placed = {{start, end, 0}};
	m_project.audio.append(track);
	if (saveDraft()) return true;
	m_project.audio.removeLast();
	return false;
}

void KRRSubtitleDialog::translate(bool selected)
{
	if (m_job != Job::Idle || (selected && m_row < 0)) return;
	const auto host = SpeechServiceHost("translate");
	if (host.isEmpty())
	{
		showStatus(Ui("请在模型服务中配置字幕翻译。"));
		return;
	}
	const auto target = m_direction->currentData().toString();
	if (selected && !m_project.translationLanguage.isEmpty() && m_project.translationLanguage != target)
	{
		showStatus(Ui("切换翻译方向时请翻译全部，避免在同一轨混用语言。"));
		return;
	}
	QVector<QString> source;
	m_translationRows.clear();
	const int track = m_subtitleTrack->currentData().toInt();
	for (int i = 0; i < cues().size(); ++i)
	{
		if (selected ? i != m_row : cues()[i].track != track) continue;
		m_translationRows.append(i);
		source.append(cues()[i].text);
	}
	if (source.isEmpty()) return;
	QString error;
	if (!m_project.validate(error)) { showStatus(error); return; }
	if (!m_worker.translateSubtitles(source, target, m_glossary->text(), selected ? 0 : -1)) return;
	m_player->stop();
	m_player->setMedia({});
	m_job = Job::Translate;
	m_cancelled = false;
	updateControls();
	showStatus(Ui("正向 %1 提交字幕文本…").arg(host));
}

void KRRSubtitleDialog::generate(bool selected)
{
	if (m_job != Job::Idle || !m_voiceAvailable) return;
	m_pending.clear();
	const auto voice = voiceOptions();
	for (int i = 0; i < cues().size(); ++i)
	{
		if (selected && i != m_row) continue;
		if (m_subtitleTrack && cues()[i].track != m_subtitleTrack->currentData().toInt()) continue;
		const auto text = spoken(i);
		if (text.isEmpty()) continue;
		if (selected ? i == m_row : !usableTake(i))
		{
			if (text.size() > 2000)
			{
				showStatus(Ui("第 %1 句应用读音规则后超过 2000 字，请缩短配音稿。").arg(i + 1));
				return;
			}
			m_pending.append(i);
		}
	}
	if (m_pending.isEmpty()) { showStatus(Ui("没有待生成的句子。")); return; }
	m_queuedVoice = voice;
	m_player->stop();
	m_player->setMedia({});
	m_job = Job::Generate;
	m_cancelled = false;
	m_queued = 0;
	m_long = 0;
	updateControls();
	startNextTake();
}

void KRRSubtitleDialog::startNextTake()
{
	const int row = m_pending[m_queued];
	const auto path = m_cache.filePath(QUuid::createUuid().toString(QUuid::WithoutBraces) + ".wav");
	m_worker.synthesize(spoken(row), path, m_queuedVoice);
	showStatus(Ui("正在配音第 %1 句（%2 / %3）…").arg(row + 1).arg(m_queued + 1).arg(m_pending.size()));
}

void KRRSubtitleDialog::completed(const QString& output, const QString& error)
{
	if (m_job == Job::Translate && error.isEmpty() && !m_cancelled)
		showStatus(Ui("翻译已完成。可校对、重译单句或导出双语字幕。"));
	if (m_job == Job::Generate && error.isEmpty() && !m_cancelled)
	{
		const int row = m_pending[m_queued];
		const auto duration = SpeechWaveDuration(output);
		if (duration > 0)
		{
			if (!m_takes[row].path.isEmpty()) m_project.markDubbingTrackStale(m_chapterMode);
			if (m_takes[row].temporary) QFile::remove(m_takes[row].path);
			m_takes[row] = {output, spoken(row), duration, true};
			cues()[row].dubbingNeedsRefresh = false;
			refreshRow(row);
			m_long += !m_chapterMode && duration > cues()[row].end - cues()[row].begin;
			if (++m_queued < m_pending.size()) { startNextTake(); return; }
			showStatus(m_long ? Ui("有 %1 句配音超时。可试听后缩短文字或加快语速重做。").arg(m_long) :
				Ui("配音已生成，可逐句试听或加入时间轴。"));
		}
		else showStatus(Ui("配音服务返回的音频无效。"));
	}
	if (m_job == Job::Narrate && error.isEmpty() && !m_cancelled)
	{
		const auto duration = SpeechWaveDuration(output);
		if (duration > 0)
		{
			if (!m_freeTake.path.isEmpty()) QFile::remove(m_freeTake.path);
			m_freeTake = {output, narrationText(), duration, true};
			m_narratedVoice = m_queuedVoice;
			m_narratedRevision = m_voiceRevision;
			showStatus(Ui("已生成 %1 秒旁白。可试听或加入时间轴。").arg(duration / 1000., 0, 'f', 2));
		}
		else showStatus(Ui("配音服务返回的音频无效。"));
	}
	if (m_job == Job::Build && error.isEmpty() && !m_cancelled)
	{
		KRREditTrack track;
		QString detail;
		if (ProbeEditAudio(output, 0, m_project.duration, track, detail))
		{
			track.title = Ui(m_chapterMode ? "章节旁白" : "逐句配音");
			track.sentenceDubbing = true;
			track.gain = m_dubVolume->value() / 100.;
			if (m_chapterMode)
			{
				track.chapterDubbing = true;
				track.speech = m_placedSpeech;
				track.originalGain = m_duckPercent->value() / 100.;
				cues() = m_placedNarration;
				resetRows();
			}
			const int existing = dubbingTrackIndex();
			if (m_project.arranged())
			{
				if (m_chapterMode)
				{
					qint64 at = 0;
					for (const auto& segment : m_project.segments())
					{
						const auto end = qMin(segment.end, track.length);
						if (segment.still < 0 && segment.begin < end)
							track.placed.append({at, at + end - segment.begin, segment.begin});
						at += segment.end - segment.begin;
					}
				}
				else track.placed = {{0, track.length, 0}};
			}
			if (existing < 0) m_project.audio.append(track);
			else
			{
				const auto& previous = m_project.audio[existing];
				track.enabled = previous.enabled;
				track.kept = previous.kept;
				if (m_project.arranged())
				{
					track.placed.clear();
					for (auto clip : previous.placed)
					{
						clip.end = qMin(clip.end, clip.begin + track.length - clip.mediaStart);
						if (clip.begin < clip.end) track.placed.append(clip);
					}
				}
				m_project.audio[existing] = track;
			}
			showStatus(m_chapterMode ? Ui("章节旁白已对齐。应用后可预听压低原声的效果。") :
				Ui("配音轨已生成。应用到时间轴后可剪切和调整音量。"));
		}
		else showStatus(detail);
	}
	if (m_job == Job::VoiceSample && error.isEmpty() && !m_cancelled)
	{
		showStatus(QString());
		playFile(output);
	}
	if (m_cancelled) showStatus(Ui("任务已取消。"));
	else if (!error.isEmpty()) showStatus(error);
	m_job = Job::Idle;
	updateControls();
}

void KRRSubtitleDialog::play(bool original)
{
	if (m_job != Job::Idle) return;
	const bool free = narrationMode();
	if (free ? (original || !narrationReady()) : m_row < 0) return;
	const auto line = original ? cues()[m_row] : KRREditCaption{};
	auto path = original ? m_project.input : free ? m_freeTake.path : m_takes[m_row].path;
	if (original && PrefersMediaFoundationPlayback())
	{
		if (m_originalPreview->file().isEmpty())
		{
			m_waitingOriginalRow = m_row;
			showStatus(Ui("正在准备原声试听…"));
			if (!m_originalPreview->isPreparing()) m_originalPreview->prepare(path, true);
			return;
		}
		path = m_originalPreview->file();
	}
	playFile(path, line.begin, original ? line.end : -1);
}

void KRRSubtitleDialog::playFile(const QString& path, qint64 begin, qint64 end)
{
	m_waitingOriginalRow = -1;
	if (path.isEmpty()) return;
	const auto url = QUrl::fromLocalFile(path);
	if (url == m_player->media().request().url() && m_player->state() == QMediaPlayer::PlayingState)
	{
		m_player->stop();
		return;
	}
	m_player->stop();
	m_previewEnd = end;
	m_player->setVolume(end >= 0 ? 100 : m_dubVolume->value());
	if (m_player->media().request().url() != url) m_player->setMedia(url);
	m_player->setPosition(begin);
	m_player->play();
}

void KRRSubtitleDialog::previewVoice()
{
	if (m_job != Job::Idle || !m_voiceAvailable || !m_cache.isValid()) return;
	auto options = voiceOptions();
	options.speed = 1.;
	const auto key = QCryptographicHash::hash((options.engine + "|" + options.language + "|" + options.voice
		+ "|" + m_voiceRevision).toUtf8(), QCryptographicHash::Sha256).toHex();
	m_voiceSamplePath = m_cache.filePath("voice-" + QString::fromLatin1(key) + ".wav");
	if (QFileInfo(m_voiceSamplePath).isFile())
	{
		playFile(m_voiceSamplePath);
		return;
	}
	m_player->stop();
	m_player->setMedia({});
	const auto sample = options.language == "zh" ? QStringLiteral("你好！欢迎使用录制工作台。") : QStringLiteral("Hello! Welcome to the recording studio.");
	if (!m_worker.synthesize(sample, m_voiceSamplePath, options)) return;
	m_job = Job::VoiceSample;
	m_cancelled = false;
	updateControls();
}

void KRRSubtitleDialog::updateAudition()
{
	const bool free = narrationMode();
	m_originalPlay->setVisible(!free);
	m_originalPlay->setEnabled(!free && m_row >= 0);
	const bool takeReady = free ? narrationReady() : m_row >= 0 && usableTake(m_row);
	const bool playing = m_player->state() == QMediaPlayer::PlayingState;
	const bool sample = playing && m_player->media().request().url().toLocalFile() == m_voiceSamplePath;
	m_voiceSample->setEnabled(m_voiceAvailable && m_cache.isValid());
	m_voiceSample->setText(sample ? Ui("停止音色试听") : Ui("试听音色"));
	m_voicePlay->setEnabled(takeReady);
	m_originalPlay->setText(playing && m_previewEnd >= 0 ? Ui("停止原片试听") : Ui("听原片"));
	m_voicePlay->setText(playing && !sample && m_previewEnd < 0 ? Ui("停止配音试听") :
		free ? Ui("试听旁白") : Ui("试听本句"));
}

void KRRSubtitleDialog::exportSubtitles()
{
	if (m_project.outputCaptions(KRRSubtitleMode::Translation).isEmpty())
	{
		const auto path = QFileDialog::getSaveFileName(this, Ui("导出原文字幕"),
			QDir(m_folder).filePath("subtitles.srt"), "SRT (*.srt)");
		if (path.isEmpty()) return;
		QString error;
		showStatus(WriteEditSubtitles(path, m_project.outputCaptions(), error) ? Ui("字幕已导出。") : error);
		return;
	}
	const auto parent = QFileDialog::getExistingDirectory(this, Ui("选择双语字幕保存位置"), m_folder);
	if (parent.isEmpty()) return;
	const auto folder = QDir(parent).filePath("subtitles-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss-zzz"));
	const auto error = ExportSubtitleFiles(m_project, folder);
	showStatus(error.isEmpty() ? Ui("已导出原文、译文与双语 SRT：") + folder : error);
}

void KRRSubtitleDialog::buildTrack()
{
	if (m_project.audio.size() >= 16 && dubbingTrackIndex() < 0) return;
	auto placed = cues();
	if (m_chapterMode)
	{
		QVector<qint64> lengths;
		for (int i = 0; i < m_takes.size(); ++i) lengths.append(usableTake(i) ? m_takes[i].duration : 0);
		QString error;
		if (!PlaceChapterNarration(m_project, placed, lengths, error)) { showStatus(error); return; }
	}
	QVector<KRRDubLine> lines;
	int excluded = 0;
	const bool compTime = m_project.arranged() && !m_chapterMode;
	const auto duration = compTime ? qMin(m_project.outputDuration(), m_project.duration) : m_project.duration;
	for (int i = 0; i < m_takes.size(); ++i)
	{
		const auto& line = placed[i];
		if (m_subtitleTrack && line.track != m_subtitleTrack->currentData().toInt()) continue;
		const auto& take = m_takes[i];
		const auto end = qMin(line.end, duration);
		if (!usableTake(i) || take.duration > end - line.begin) { ++excluded; continue; }
		lines.append({line.begin, end, take.path});
	}
	if (lines.isEmpty()) return;
	if (excluded || dubbingTrackIndex() >= 0)
	{
		auto message = Ui("将加入 %1 句，另有 %2 句未生成或超时。继续？").arg(lines.size()).arg(excluded);
		if (dubbingTrackIndex() >= 0) message += "\n" + Ui("新配音轨将使用本次生成的句子。原 WAV 文件会保留。");
		if (QMessageBox::question(this, Ui("加入已完成的配音"), message) != QMessageBox::Yes) return;
	}
	QString output;
	if (m_chapterMode)
	{
		const auto suggested = QDir(m_folder).filePath("chapters-" +
			QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss-zzz") + ".wav");
		QFileDialog file(this, Ui("保存对齐后的配音轨"), suggested, "WAV (*.wav)");
		file.setAcceptMode(QFileDialog::AcceptSave);
		file.setFileMode(QFileDialog::AnyFile);
		file.setDefaultSuffix("wav");
		file.setOption(QFileDialog::DontConfirmOverwrite);
		if (file.exec() != QDialog::Accepted) return;
		output = file.selectedFiles().value(0);
		if (QFileInfo(output).suffix().toLower() != "wav") { showStatus(Ui("独立配音请保存为 WAV。")); return; }
		m_placedNarration = placed;
		m_placedSpeech.clear();
		for (const auto& line : placed) m_placedSpeech.append({line.begin, line.end});
		m_placedSpeech = MergeEditRanges(m_placedSpeech);
	}
	else
		output = m_cache.filePath("dubbing-" + QUuid::createUuid().toString(QUuid::WithoutBraces) + ".wav");
	if (!m_worker.buildDubbing(duration, lines, output)) return;
	m_player->stop();
	m_job = Job::Build;
	m_cancelled = false;
	updateControls();
	showStatus(compTime ? Ui("正在按成片时间轴对齐配音…") : Ui("正在按源时间轴对齐配音…"));
}

bool KRRSubtitleDialog::saveDraft()
{
	if (m_chapterMode && !cues().isEmpty()) m_project.chapterNarrationLanguage = m_direction->currentData().toString();
	QString error;
	if (!m_project.validate(error)) { showStatus(error); return false; }
	const QDir storage(DataDirectory());
	QHash<QString, QString> saved;
	const auto save = [&](const QString& source, const QString& subfolder)
	{
		if (saved.contains(source)) return true;
		const auto folder = storage.filePath(subfolder);
		const auto target = QDir(folder).filePath(QFileInfo(source).fileName());
		if (!QDir().mkpath(folder) || !QFile::copy(source, target))
		{
			for (const auto& file : saved) QFile::remove(file);
			showStatus(Ui("无法保存配音。请检查应用数据目录。"));
			return false;
		}
		saved.insert(source, target);
		return true;
	};
	for (const auto& take : m_takes)
		if (take.temporary && !save(take.path, "sentence-audio")) return false;
	if (m_cache.isValid())
	{
		for (const auto& track : m_project.audio)
			if (!track.file.isEmpty() && QFileInfo(track.file).absolutePath() == m_cache.path() &&
				!save(track.file, "speech-audio")) return false;
	}
	for (auto& track : m_project.audio)
		track.file = saved.value(track.file, track.file);
	for (int i = 0; i < m_takes.size(); ++i)
	{
		auto& line = cues()[i];
		line.dubbingFile = saved.value(m_takes[i].path, m_takes[i].path);
		line.dubbingText = m_takes[i].text;
	}
	return true;
}
