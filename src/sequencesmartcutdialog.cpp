#include "sequencesmartcutdialog.h"
#include "editplayer.h"
#include "editwidgets.h"
#include "recordingconfig.h"
#include "speechservices.h"
#include "speechsettingsdialog.h"
#include "uilanguage.h"
#include "uitheme.h"
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMediaPlayer>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include "playbackslider.h"
#include <QSpinBox>
#include <QSplitter>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

KRRSequenceSmartCutDialog::KRRSequenceSmartCutDialog(const KRREditSequence& source, QWidget* parent, const QString& goal)
	: QDialog(parent), m_source(source), m_temporary(DataDirectory() + "/montage-preview-XXXXXX"), m_worker(this)
{
	setObjectName("sequenceSmartCutDialog");
	setWindowTitle(Ui("AI 混剪助手"));
	setWindowFlag(Qt::WindowContextHelpButtonHint, false);
	setWindowFlag(Qt::WindowMaximizeButtonHint, true);
	resize(1160, 750);
	auto root = new QVBoxLayout(this);
	auto button = [this](QHBoxLayout* row, const char* name, const char* id)
	{
		auto control = new QPushButton(Ui(name), this);
		control->setObjectName(id);
		control->setAutoDefault(false);
		row->addWidget(control);
		return control;
	};
	auto header = new QHBoxLayout;
	m_provider = new QLabel(this);
	m_provider->setTextFormat(Qt::PlainText);
	m_provider->setWordWrap(true);
	header->addWidget(m_provider, 1);
	m_settings = button(header, "视频模型设置…", "montageProvider");
	root->addLayout(header);
	auto input = new QHBoxLayout;
	m_goal = new QLineEdit(this);
	m_goal->setObjectName("montageGoal");
	m_goal->setMaxLength(MaxSequenceGoalChars);
	m_goal->setAccessibleName(Ui("混剪要求"));
	m_goal->setPlaceholderText(Ui("找出每局的反击和终结，剪成约 35 秒"));
	input->addWidget(m_goal, 1);
	m_send = button(input, "生成草稿", "montageGenerate");
	m_send->setProperty("role", "primary");
	m_stop = button(input, "停止任务", "montageCancel");
	root->addLayout(input);

	auto options = new QHBoxLayout;
	const auto spin = [this, options](const char* label, const char* id, int minimum, int maximum, int initial)
	{
		auto box = new QSpinBox(this);
		box->setObjectName(id);
		box->setAccessibleName(Ui(label));
		box->setRange(minimum, maximum);
		box->setValue(initial);
		options->addWidget(new QLabel(Ui(label), this));
		options->addWidget(box);
		return box;
	};
	m_targetDuration = spin("成片目标时长", "montageTargetSeconds", -1, static_cast<int>(MaxSequenceTargetMs / 1000), -1);
	m_targetDuration->setSuffix(Ui(" 秒"));
	m_targetDuration->setSpecialValueText(Ui("按要求"));
	m_targetDuration->setToolTip(Ui("按要求读取描述中的成片时长。填 0 秒表示不限。"));
	m_candidateCount = spin("每段候选数", "montageCandidatesPerSource", 0, MaxSequenceCandidates, 0);
	m_candidateCount->setSpecialValueText(Ui("按要求"));
	m_candidateCount->setToolTip(Ui("每段素材提供的候选上限，实际数量取决于找到的动作。"));
	m_sampleRate = spin("视频分析采样率", "montageSampleFps", 0, MaxSequenceSampleFps, 0);
	m_sampleRate->setSpecialValueText(Ui("自动"));
	m_sampleRate->setSuffix(" FPS");
	m_sampleRate->setToolTip(Ui("自动以 %1 FPS 扫描，再以 %2 FPS 复核重点区域。数字档使用固定帧率扫描。")
		.arg(SequenceDiscoveryFps).arg(SequenceDetailFps));
	options->addStretch(1);
	root->addLayout(options);

	auto split = new QSplitter(Qt::Horizontal, this);
	auto left = new QWidget(split);
	auto leftLayout = new QVBoxLayout(left);
	leftLayout->setContentsMargins(0, 0, 0, 0);
	m_table = new QTableWidget(0, 4, left);
	m_table->setObjectName("montageParts");
	m_table->setHorizontalHeaderLabels({Ui("采用"), Ui("高光 / 素材"), Ui("源片区间"), Ui("依据时间")});
	const auto time = EditTime(359'999'999);
	RecorderUi::FitTableColumn(m_table, 0);
	RecorderUi::FitTableColumn(m_table, 1);
	RecorderUi::FitTableColumn(m_table, 2, time + " – " + time);
	RecorderUi::FitTableColumn(m_table, 3, time);
	auto tableHeader = m_table->horizontalHeader();
	tableHeader->resizeSection(1, qMax(m_table->columnWidth(1), RecorderUi::Px(112)));
	tableHeader->setSectionResizeMode(1, QHeaderView::Interactive);
	m_table->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
	m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
	m_table->setSelectionMode(QAbstractItemView::SingleSelection);
	m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
	m_table->verticalHeader()->setSectionsMovable(false);
	leftLayout->addWidget(m_table, 1);
	auto rows = new QHBoxLayout;
	m_up = button(rows, "上移", "montageUp");
	m_down = button(rows, "下移", "montageDown");
	rows->addStretch();
	m_evidence = button(rows, "回看依据", "montageEvidence");
	leftLayout->addLayout(rows);
	auto times = new QHBoxLayout;
	auto timeBox = [&](const char* label, const char* id)
	{
		auto box = new QDoubleSpinBox(left);
		box->setDecimals(3);
		box->setSingleStep(.1);
		box->setSuffix(Ui(" 秒"));
		box->setObjectName(id);
		box->setAccessibleName(Ui(label));
		times->addWidget(new QLabel(Ui(label), left));
		times->addWidget(box);
		return box;
	};
	m_begin = timeBox("起点", "montageBegin");
	m_end = timeBox("终点", "montageEnd");
	m_cut = button(times, "更新切点", "montageSetCut");
	leftLayout->addLayout(times);

	auto right = new QWidget(split);
	auto rightLayout = new QVBoxLayout(right);
	rightLayout->setContentsMargins(0, 0, 0, 0);
	m_canvas = new KRREditCanvas(right);
	m_canvas->setMinimumSize(330, 190);
	rightLayout->addWidget(m_canvas, 1);
	m_player = new KRREditPlayer(this);
	m_audio = new QMediaPlayer(this);
	m_audio->setNotifyInterval(100);
	auto transport = new QHBoxLayout;
	m_pause = button(transport, "暂停", "montagePause");
	m_seek = new KRRPlaybackSlider(right);
	m_seek->setAccessibleName(Ui("草稿播放位置"));
	transport->addWidget(m_seek, 1);
	m_clock = new QLabel(EditTime(0), right);
	transport->addWidget(m_clock);
	rightLayout->addLayout(transport);
	split->setSizes({650, 480});
	root->addWidget(split, 1);

	auto actions = new QHBoxLayout;
	m_usage = new QLabel(this);
	m_usage->setObjectName("montageUsage");
	m_usage->setWordWrap(true);
	actions->addWidget(m_usage, 1);
	m_selection = new QLabel(this);
	m_selection->setObjectName("montageSelection");
	actions->addWidget(m_selection);
	m_preview = button(actions, "预览混剪", "montagePreview");
	m_apply = button(actions, "应用草稿…", "montageApply");
	m_apply->setProperty("role", "primary");
	root->addLayout(actions);
	m_status = new QLabel(this);
	m_status->setObjectName("montageStatus");
	m_status->setTextFormat(Qt::PlainText);
	m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
	m_status->setWordWrap(true);
	root->addWidget(m_status);

	connect(m_send, &QPushButton::clicked, this, &KRRSequenceSmartCutDialog::analyze);
	connect(m_goal, &QLineEdit::returnPressed, this, &KRRSequenceSmartCutDialog::analyze);
	connect(m_goal, &QLineEdit::textChanged, this, &KRRSequenceSmartCutDialog::refreshControls);
	connect(m_settings, &QPushButton::clicked, this, [this] { ConfigureSpeechServices(this, "video"); showProvider(); });
	connect(m_stop, &QPushButton::clicked, this, [this] { m_worker.cancel(); });
	connect(m_up, &QPushButton::clicked, this, [this] { moveSelected(-1); });
	connect(m_down, &QPushButton::clicked, this, [this] { moveSelected(1); });
	connect(m_cut, &QPushButton::clicked, this, &KRRSequenceSmartCutDialog::editCut);
	connect(m_table, &QTableWidget::currentCellChanged, this, &KRRSequenceSmartCutDialog::refreshControls);
	connect(m_table, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item)
	{
		if (item->column() != 0 || item->row() >= m_draft.highlights.size()) return;
		m_draft.highlights[item->row()].cut.selected = item->checkState() == Qt::Checked;
		invalidatePreview();
		refreshControls();
	});
	connect(m_evidence, &QPushButton::clicked, this, &KRRSequenceSmartCutDialog::playEvidence);
	connect(m_preview, &QPushButton::clicked, this, &KRRSequenceSmartCutDialog::previewMontage);
	connect(m_apply, &QPushButton::clicked, this, &KRRSequenceSmartCutDialog::apply);
	connect(m_pause, &QPushButton::clicked, this, [this]
	{
		if (m_player->state() == QMediaPlayer::PlayingState) m_player->pause();
		else
		{
			if (m_player->position() >= m_playEnd - 50) m_player->setPosition(m_seek->minimum());
			m_player->play();
		}
	});
	connect(m_seek, &QSlider::sliderReleased, this, [this]
	{
		m_audio->pause();
		m_audioStarted = false;
		m_player->setPosition(m_seek->value());
		m_player->play();
	});
	connect(m_player, &KRREditPlayer::frameReady, m_canvas, &KRREditCanvas::setFrame);
	connect(m_player, &KRREditPlayer::positionChanged, this, [this](qint64 at)
	{
		const QSignalBlocker block(m_seek);
		if (!m_seek->isSliderDown()) m_seek->setValue(static_cast<int>(at));
		m_canvas->setPosition(at);
		m_clock->setText(EditTime(at));
		if (at >= m_playEnd - 35) m_player->pause();
		else if (!m_audioStarted && !m_audio->media().isNull())
		{
			m_audioStarted = true;
			m_audio->setPosition(at);
			m_audio->play();
		}
		else if (m_audio->state() == QMediaPlayer::PlayingState && m_audio->isSeekable() && qAbs(m_audio->position() - at) > 350)
			m_audio->setPosition(at);
	});
	connect(m_player, &KRREditPlayer::stateChanged, this, [this](QMediaPlayer::State state)
	{
		m_pause->setText(state == QMediaPlayer::PlayingState ? Ui("暂停") : Ui("播放"));
		if (state != QMediaPlayer::PlayingState)
		{
			m_audioStarted = false;
			m_audio->pause();
		}
	});
	connect(m_player, &KRREditPlayer::failed, this, [this](const auto& detail) { m_status->setText(UiDiagnostic(detail)); });
	connect(&m_worker, &KRRMediaEditor::progress, m_status, &QLabel::setText);
	connect(&m_worker, &KRRMediaEditor::sequenceDrafted, this, [this](const auto& draft)
	{
		m_draft = draft;
		showUsage();
		refreshRows(0);
	});
	connect(&m_worker, &KRRMediaEditor::completed, this, [this](const auto& output, const auto& error)
	{
		if (!error.isEmpty()) m_status->setText(UiDiagnostic(error));
		else if (!output.isEmpty()) playRendered(output);
		else
		{
			const auto missed = m_draft.withoutHighlights.size();
			m_status->setText(missed > 0 ? Ui("有 %1 段素材未找到符合要求的高光。请核对其余片段。").arg(missed) :
				Ui("草稿已生成。选中片段可回看依据。"));
			if (m_draft.failedReviews > 0)
				m_status->setText(m_status->text() + " " + Ui("有 %1 个区域未完成复核，初筛候选可继续查看。").arg(m_draft.failedReviews));
		}
		refreshControls();
	});
	showProvider();
	refreshControls();
	m_goal->setFocus();
	RecorderUi::PolishButtons(this);
	if (!goal.isEmpty())
	{
		m_goal->setText(goal);
		QTimer::singleShot(0, this, &KRRSequenceSmartCutDialog::analyze);
	}
}

void KRRSequenceSmartCutDialog::analyze()
{
	if (m_worker.isBusy() || m_goal->text().simplified().isEmpty()) return;
	if (SpeechServiceHost("video").isEmpty())
	{
		m_status->setText(Ui("请在服务设置中配置视频理解模型。"));
		return;
	}
	m_player->pause();
	invalidatePreview();
	m_draft = {};
	m_usage->clear();
	m_usage->setToolTip({});
	refreshRows(-1);
	m_status->setText(Ui("正在准备混剪分析…"));
	KRRSequenceCutOptions options;
	options.targetMs = m_targetDuration->value() < 0 ? -1 : m_targetDuration->value() * 1000LL;
	options.candidatesPerSource = m_candidateCount->value();
	options.sampleFps = m_sampleRate->value();
	m_worker.suggestSequenceCut(m_source, m_goal->text(), options);
	refreshControls();
}

void KRRSequenceSmartCutDialog::refreshRows(int selected)
{
	const QSignalBlocker block(m_table);
	m_table->setRowCount(m_draft.highlights.size());
	for (int i = 0; i < m_draft.highlights.size(); ++i)
	{
		const auto& row = m_draft.highlights[i];
		const auto& cut = row.cut;
		auto check = new QTableWidgetItem;
		check->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
		check->setCheckState(cut.selected ? Qt::Checked : Qt::Unchecked);
		m_table->setItem(i, 0, check);
		const auto label = cut.title.isEmpty() ? Ui("保留片段") : cut.title;
		const auto sourceName = QFileInfo(m_source[row.source].source()).fileName();
		auto title = new QTableWidgetItem(label + "\n" + sourceName);
		QString details = label + "\n" + sourceName;
		if (!cut.reason.isEmpty()) details += "\n" + cut.reason;
		if (cut.reviewed) details += "\n" + Ui("已用高密度画面复核");
		title->setToolTip(details);
		m_table->setItem(i, 1, title);
		m_table->setItem(i, 2, new QTableWidgetItem(EditTime(cut.range.begin) + " – " + EditTime(cut.range.end)));
		m_table->setItem(i, 3, new QTableWidgetItem(EditTime(cut.evidence)));
		m_table->setRowHeight(i, fontMetrics().height() * 2 + 8);
	}
	if (selected >= 0 && selected < m_table->rowCount()) m_table->setCurrentCell(selected, 1);
	refreshControls();
}

void KRRSequenceSmartCutDialog::refreshControls()
{
	const bool idle = !m_worker.isBusy();
	const int row = m_table->currentRow();
	const bool hasRow = row >= 0 && row < m_draft.highlights.size();
	int selected = 0;
	qint64 duration = 0;
	for (const auto& item : m_draft.highlights)
	{
		if (!item.cut.selected) continue;
		++selected;
		duration += item.cut.range.end - item.cut.range.begin;
	}
	m_selection->setText(m_draft.highlights.isEmpty() ? QString() :
		Ui("已选 %1 / %2 段 · %3").arg(selected).arg(m_draft.highlights.size()).arg(EditTime(duration)));
	m_goal->setEnabled(idle);
	for (auto option : {m_targetDuration, m_candidateCount, m_sampleRate}) option->setEnabled(idle);
	m_settings->setEnabled(idle);
	m_send->setEnabled(idle && !m_goal->text().simplified().isEmpty());
	m_stop->setVisible(!idle);
	m_table->setEnabled(idle);
	m_up->setEnabled(idle && hasRow && row > 0);
	m_down->setEnabled(idle && hasRow && row + 1 < m_draft.highlights.size());
	for (auto edit : {m_begin, m_end}) edit->setEnabled(idle && hasRow);
	m_evidence->setEnabled(idle && hasRow);
	m_cut->setEnabled(idle && hasRow);
	m_preview->setEnabled(idle && selected > 0);
	m_apply->setEnabled(idle && selected > 0);
	m_pause->setEnabled(idle && !m_view.input.isEmpty());
	m_seek->setEnabled(m_pause->isEnabled());
	if (hasRow)
	{
		const auto& pick = m_draft.highlights[row];
		const auto& source = *m_source[pick.source].edit;
		m_begin->setMaximum(source.duration / 1000.0);
		m_end->setMaximum(source.duration / 1000.0);
		m_begin->setValue(pick.cut.range.begin / 1000.0);
		m_end->setValue(pick.cut.range.end / 1000.0);
		m_evidence->setToolTip(pick.cut.reason);
	}
}

void KRRSequenceSmartCutDialog::moveSelected(int delta)
{
	const int row = m_table->currentRow();
	const int next = row + delta;
	if (row < 0 || next < 0 || next >= m_draft.highlights.size()) return;
	m_draft.highlights.move(row, next);
	invalidatePreview();
	refreshRows(next);
}

void KRRSequenceSmartCutDialog::editCut()
{
	const int row = m_table->currentRow();
	auto& highlight = m_draft.highlights[row];
	const KRREditRange range{qRound64(m_begin->value() * 1000), qRound64(m_end->value() * 1000)};
	if (range.end - range.begin < 20 || !EditRangesContain(m_source[highlight.source].edit->kept, range))
	{
		m_status->setText(Ui("选段超出原素材的保留范围，或包含已删区间。"));
		return;
	}
	highlight.cut.range = range;
	invalidatePreview();
	refreshRows(row);
	m_status->setText(Ui("切点已更新。"));
}

void KRRSequenceSmartCutDialog::playEvidence()
{
	const auto& row = m_draft.highlights[m_table->currentRow()];
	const auto& source = *m_source[row.source].edit;
	const qint64 at = qBound<qint64>(0, row.cut.evidence, source.duration - 1);
	m_status->setText(Ui("源片 %1 · %2").arg(EditTime(at), row.cut.reason));
	play(source, qMax<qint64>(0, at - 1500), qMin(source.duration, at + 2500));
}

void KRRSequenceSmartCutDialog::previewMontage()
{
	KRREditSequence compiled;
	QString error;
	if (!BuildSequenceSmartCut(m_source, m_draft, compiled, error))
	{
		m_status->setText(error);
		return;
	}
	if (!m_previewPath.isEmpty() && QFileInfo::exists(m_previewPath))
	{
		playRendered(m_previewPath);
		return;
	}
	if (!m_temporary.isValid())
	{
		m_status->setText(Ui("无法创建混剪预览目录。"));
		return;
	}
	m_renderedCaptions.clear();
	qint64 offset = 0;
	for (const auto& part : compiled)
	{
		for (auto cue : part.edit->outputCaptions(KRRSubtitleMode::Bilingual))
		{
			cue.begin += offset;
			cue.end += offset;
			m_renderedCaptions.append(std::move(cue));
		}
		offset += part.edit->outputDuration();
	}
	m_player->pause();
	const auto path = m_temporary.filePath(QString("preview-%1.mp4").arg(++m_previewNumber));
	m_status->setText(Ui("正在生成本地混剪预览…"));
	m_worker.renderSequence(compiled, path, false, true);
	refreshControls();
}

void KRRSequenceSmartCutDialog::apply()
{
	KRREditSequence compiled;
	QString error;
	if (!BuildSequenceSmartCut(m_source, m_draft, compiled, error))
	{
		m_status->setText(error);
		return;
	}
	if (QMessageBox::question(this, Ui("应用混剪草稿"),
		Ui("按草稿顺序应用 %1 个片段（%2）到当前多素材工程？").arg(compiled.size()).arg(EditTime(EditSequenceDuration(compiled))),
		QMessageBox::Apply | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Apply)
		return;
	m_result = std::move(compiled);
	m_player->clear();
	m_audio->setMedia(QMediaContent{});
	accept();
}

void KRRSequenceSmartCutDialog::invalidatePreview()
{
	if (m_previewPath.isEmpty()) return;
	if (m_view.input == m_previewPath)
	{
		m_player->clear();
		m_audio->setMedia(QMediaContent{});
		m_view = {};
		m_canvas->setProject(nullptr);
		m_canvas->setFrame({});
	}
	QFile::remove(m_previewPath);
	m_previewPath.clear();
}

void KRRSequenceSmartCutDialog::play(const KRREditProject& view, qint64 start, qint64 end)
{
	m_player->clear();
	m_view = view;
	m_playEnd = end;
	m_canvas->setProject(&m_view);
	m_canvas->setCropPreview(false);
	m_canvas->setFrame({});
	m_seek->setRange(static_cast<int>(start), static_cast<int>(end));
	m_seek->setValue(static_cast<int>(start));
	m_audioStarted = false;
	m_audio->setMedia(view.audio.isEmpty() ? QMediaContent{} : QMediaContent(QUrl::fromLocalFile(view.input)));
	m_player->setSource(view.input, view.size, view.fps, view.duration);
	m_player->setPosition(start);
	m_player->play();
	refreshControls();
}

void KRRSequenceSmartCutDialog::playRendered(const QString& path)
{
	KRREditProject rendered;
	QString error;
	if (!ProbeEditSource(path, rendered, error))
	{
		m_status->setText(error);
		return;
	}
	m_previewPath = path;
	rendered.captions = m_renderedCaptions;
	m_status->setText(Ui("正在播放混剪草稿。"));
	play(rendered, 0, rendered.duration);
}

void KRRSequenceSmartCutDialog::showUsage()
{
	if (m_draft.requests == 0) return;
	m_usage->setText(Ui("%1 次请求 · %2 秒").arg(m_draft.requests).arg(m_draft.elapsedMs / 1000.0, 0, 'f', 1));
	auto detail = Ui("分析 %1 · 样本 %2 秒 · 服务 %3 秒")
		.arg(EditTime(m_draft.analyzedMs)).arg(m_draft.preparationMs / 1000.0, 0, 'f', 1)
		.arg(m_draft.requestMs / 1000.0, 0, 'f', 1);
	if (m_draft.detailRequests > 0)
		detail += "\n" + Ui("%1 FPS 局部复核 · %2 次调用").arg(SequenceDetailFps).arg(m_draft.detailRequests);
	if (m_draft.rejectedEvidence > 0)
		detail += "\n" + Ui("有 %1 条候选的证据无效，已略过。").arg(m_draft.rejectedEvidence);
	m_usage->setToolTip(detail);
}

void KRRSequenceSmartCutDialog::showProvider()
{
	const auto service = SpeechService("video");
	m_provider->setText(service.model.isEmpty() ? Ui("请在服务设置中配置视频理解模型。") :
		Ui("%1 段素材 · %2").arg(m_source.size()).arg(service.model));
}

void KRRSequenceSmartCutDialog::interrupt()
{
	m_worker.cancel();
	m_player->clear();
	m_audio->setMedia(QMediaContent{});
	QDialog::reject();
}

void KRRSequenceSmartCutDialog::reject()
{
	interrupt();
}
