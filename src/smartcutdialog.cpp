#include "smartcutdialog.h"
#include "clipeditor.h"
#include "editplayer.h"
#include "editwidgets.h"
#include "mediaeditor.h"
#include "recordingconfig.h"
#include "speechservices.h"
#include "speechsettingsdialog.h"
#include "uilanguage.h"
#include "uitheme.h"

#include <QAudioOutput>
#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QPushButton>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QVBoxLayout>
#include <QtMath>

namespace
{
int Px(int n) { return RecorderUi::Px(n); }

QDoubleSpinBox* MakeTime(QWidget* owner, qint64 duration, qint64 value, const char* name)
{
	auto number = new QDoubleSpinBox(owner);
	number->setObjectName(name);
	number->setRange(0, duration / 1000.0);
	number->setDecimals(3);
	number->setSingleStep(.1);
	number->setSuffix(" s");
	number->setValue(value / 1000.0);
	return number;
}

qint64 TimeOf(const QDoubleSpinBox* number) { return qRound64(number->value() * 1000); }

QString DraftPath(const KRREditProject& project)
{
	const QDir data(DataDirectory());
	const auto key = QCryptographicHash::hash(QFileInfo(project.input).absoluteFilePath().toUtf8(), QCryptographicHash::Sha256).toHex();
	if (!data.mkpath("smartcut-drafts")) return {};
	return data.filePath("smartcut-drafts/" + QString::fromLatin1(key) + ".json");
}

class SmartCutDialog : public QDialog
{
public:
	SmartCutDialog(QWidget* owner, const KRREditProject& project, KRREditRange selection, bool docked = false)
		: QDialog(owner), m_original(project), m_cache(DataDirectory() + "/smartcut-preview-XXXXXX"),
		  m_draftPath(DraftPath(project))
	{
		setObjectName("smartCutDialog");
		setWindowTitle(Ui("智能剪辑"));
		setWindowFlag(Qt::WindowContextHelpButtonHint, false);
		if (docked) setWindowFlags(Qt::Widget);
		else resize(Px(950), Px(740));
		auto layout = new QVBoxLayout(this);
		m_options = new QWidget(this);
		auto form = new QVBoxLayout(m_options);
		form->setContentsMargins(0, 0, 0, 0);
		auto top = new QHBoxLayout;
		m_mode = new QComboBox(this);
		m_mode->setObjectName("smartCutMode");
		m_mode->addItem(Ui("找重点"), "highlights");
		m_mode->addItem(Ui("指定范围"), "range");
		m_mode->addItem(Ui("全片粗剪"), "rough");
		m_begin = MakeTime(this, project.duration, selection.begin, "analysisBegin");
		m_end = MakeTime(this, project.duration, selection.end, "analysisEnd");
		m_scope = new QWidget(this);
		auto extent = new QHBoxLayout(m_scope);
		extent->setContentsMargins(0, 0, 0, 0);
		extent->addWidget(new QLabel(Ui("分析范围"), this));
		extent->addWidget(m_begin);
		extent->addWidget(new QLabel(Ui("至"), this));
		extent->addWidget(m_end);
		m_limit = new QDoubleSpinBox(this);
		m_limit->setObjectName("smartCutLimit");
		m_limit->setRange(0, 86400);
		m_limit->setDecimals(3);
		m_limit->setSingleStep(1);
		m_limit->setSuffix(" s");
		m_limit->setSpecialValueText(Ui("不限"));
		m_limit->setValue(45);
		auto settings = new QPushButton(Ui("服务设置…"), this);
		top->addWidget(m_mode);
		top->addWidget(m_scope);
		top->addWidget(new QLabel(Ui("成片时长上限"), this));
		top->addWidget(m_limit);
		top->addStretch();
		top->addWidget(settings);
		form->addLayout(top);

		auto goal = new QHBoxLayout;
		m_query = new QLineEdit(this);
		m_query->setObjectName("smartCutQuery");
		m_query->setMaxLength(600);
		m_query->setPlaceholderText(Ui("例如：找运行成功的画面，保留前后操作"));
		m_generate = new QPushButton(Ui("生成草稿"), this);
		m_generate->setObjectName("generateSmartCut");
		m_generate->setProperty("role", "primary");
		goal->addWidget(m_query, 1);
		goal->addWidget(m_generate);
		form->addLayout(goal);
		m_addChapters = new QCheckBox(Ui("将选中片段的标题添加为章节"), this);
		m_addChapters->setObjectName("smartCutChapters");
		form->addWidget(m_addChapters);
		m_serviceInfo = new QLabel(this);
		m_serviceInfo->setTextFormat(Qt::PlainText);
		m_serviceInfo->setWordWrap(true);
		auto serviceRow = new QHBoxLayout;
		serviceRow->addWidget(m_serviceInfo, 1);
		auto asr = new QPushButton(Ui("离线识别…"), this);
		asr->setObjectName("smartCutTranscribe");
		asr->setVisible(project.captions.isEmpty() && !project.audio.isEmpty());
		serviceRow->addWidget(asr);
		form->addLayout(serviceRow);
		layout->addWidget(m_options);
		if (docked)
		{
			m_options->hide();
			layout->setContentsMargins(0, 0, 0, 0);
			layout->setSpacing(4);
		}
		m_table = new QTableWidget(0, 5, this);
		m_table->setObjectName("smartCutParts");
		m_table->setHorizontalHeaderLabels({Ui("保留"), Ui("入点"), Ui("出点"), Ui("证据位置"), Ui("建议与依据")});
		auto header = m_table->horizontalHeader();
		header->setSectionResizeMode(4, QHeaderView::Stretch);
		RecorderUi::FitTableColumn(m_table, 0);
		const auto time = EditTime(qMax<qint64>(project.duration, 359'999'999));
		for (int column : {1, 2, 3}) RecorderUi::FitTableColumn(m_table, column, time);
		m_table->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
		m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
		m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
		m_table->setSelectionMode(QAbstractItemView::SingleSelection);
		m_table->verticalHeader()->setVisible(false);
		m_table->setMinimumHeight(Px(125));
		if (docked) for (int column : {1, 2, 3}) m_table->hideColumn(column);
		layout->addWidget(m_table, 2);
		m_detail = new QLabel(this);
		m_detail->setTextFormat(Qt::PlainText);
		m_detail->setWordWrap(true);
		layout->addWidget(m_detail);
		auto row = new QHBoxLayout;
		m_partBegin = MakeTime(this, project.duration, 0, "draftBegin");
		m_partEnd = MakeTime(this, project.duration, 0, "draftEnd");
		m_change = new QPushButton(Ui("更新切点"), this);
		m_change->setObjectName("updateDraftRange");
		m_excerpt = new QPushButton(Ui("播放证据"), this);
		m_excerpt->setObjectName("playDraftEvidence");
		m_montage = new QPushButton(Ui("生成成片预览"), this);
		m_montage->setObjectName("renderSmartCutPreview");
		m_pause = new QPushButton(Ui("暂停／继续"), this);
		row->addWidget(new QLabel(Ui("片段切点"), this));
		row->addWidget(m_partBegin);
		row->addWidget(m_partEnd);
		row->addWidget(m_change);
		if (docked) layout->addLayout(row);
		else row->addStretch();
		auto previewRow = docked ? new QHBoxLayout : row;
		previewRow->addWidget(m_excerpt);
		previewRow->addWidget(m_montage);
		previewRow->addWidget(m_pause);
		layout->addLayout(previewRow);

		m_canvas = new KRREditCanvas(this);
		m_canvas->setObjectName("smartCutCanvas");
		m_canvas->setCropPreview(false);
		m_canvas->setMinimumHeight(Px(docked ? 116 : 172));
		layout->addWidget(m_canvas, 3);
		m_player = new KRREditPlayer(this);
		m_player->setObjectName("smartCutPlayer");
		m_audioData = new QBuffer(this);
		m_audioData->setObjectName("smartCutAudio");
		QAudioFormat format;
		format.setCodec("audio/pcm");
		format.setSampleRate(48000);
		format.setChannelCount(2);
		format.setSampleSize(16);
		format.setSampleType(QAudioFormat::SignedInt);
		format.setByteOrder(QAudioFormat::LittleEndian);
		auto device = QAudioDeviceInfo::defaultOutputDevice();
		if (device.isFormatSupported(format)) m_speaker = new QAudioOutput(device, format, this);
		m_summary = new QLabel(this);
		m_summary->setObjectName("smartCutSummary");
		m_summary->setTextFormat(Qt::PlainText);
		m_summary->setWordWrap(true);
		layout->addWidget(m_summary);
		m_status = new QLabel(this);
		m_status->setObjectName("smartCutStatus");
		m_status->setTextFormat(Qt::PlainText);
		m_status->setWordWrap(true);
		layout->addWidget(m_status);
		auto buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
		m_stop = buttons->addButton(Ui("取消当前任务"), QDialogButtonBox::ActionRole);
		m_apply = buttons->addButton(Ui("应用草稿"), QDialogButtonBox::ActionRole);
		m_apply->setObjectName("applySmartCut");
		m_apply->setProperty("role", "primary");
		layout->addWidget(buttons);

		connect(m_mode, qOverload<int>(&QComboBox::currentIndexChanged), this, [this]
		{
			const auto mode = m_mode->currentData().toString();
			m_scope->setVisible(mode == "range");
			m_limit->setValue(mode == "range" ? 0 : mode == "rough" ? 90 : 45);
		});
		connect(settings, &QPushButton::clicked, this, [this] { ConfigureSpeechServices(this, "video"); updateServiceLabel(); });
		connect(asr, &QPushButton::clicked, this, [this] { requestedSubtitles = true; accept(); });
		connect(m_generate, &QPushButton::clicked, this, [this] { generate(); });
		connect(m_addChapters, &QCheckBox::toggled, this, [this](bool enabled)
		{
			if (m_draft.revision.isEmpty()) return;
			m_draft.addChapters = enabled;
			reviewChanged();
		});
		connect(m_table, &QTableWidget::currentCellChanged, this, [this](int current) { select(current); });
		connect(m_table, &QTableWidget::itemChanged, this, [this](const QTableWidgetItem* item)
		{
			if (item->column() != 0 || item->row() >= m_draft.parts.size()) return;
			m_draft.parts[item->row()].selected = item->checkState() == Qt::Checked;
			reviewChanged();
		});
		connect(m_change, &QPushButton::clicked, this, [this] { changeRange(); });
		connect(m_excerpt, &QPushButton::clicked, this, [this] { playEvidence(); });
		connect(m_table, &QTableWidget::cellDoubleClicked, this, [this](int rowIndex, int)
		{
			m_table->setCurrentCell(rowIndex, 0);
			playEvidence();
		});
		connect(m_montage, &QPushButton::clicked, this, [this] { renderPreview(); });
		connect(m_pause, &QPushButton::clicked, this, [this]
		{
			if (m_player->state() == QMediaPlayer::PlayingState) m_player->pause();
			else
			{
				if (m_playFinished) m_player->setPosition(m_playFrom);
				m_playFinished = false;
				m_player->play();
			}
		});
		connect(m_apply, &QPushButton::clicked, this, [this]
		{
			QString error;
			if (!makePlan(result, error)) { status(error); return; }
			accept();
		});
		connect(buttons, &QDialogButtonBox::rejected, this, &SmartCutDialog::reject);
		connect(m_stop, &QPushButton::clicked, this, [this] { cancel(); });
		connect(&m_analysis, &KRRMediaEditor::progress, this, [this](const QString& message)
		{
			if (!m_cancelled) status(message);
		});
		connect(&m_analysis, &KRRMediaEditor::smartDrafted, this, [this](const KRRSmartCutDraft& draft)
		{
			if (m_cancelled) return;
			m_draft = draft;
			m_draft.addChapters = m_addChapters->isChecked();
			m_previewPath.clear();
			rebuildTable();
			saveDraft();
		});
		connect(&m_analysis, &KRRMediaEditor::completed, this, [this](const QString&, const QString& error)
		{
			setBusy(false);
			status(m_cancelled ? Ui("任务已取消。") : !error.isEmpty() ? error :
				m_draft.parts.isEmpty() ? Ui("未找到符合目标的片段。可以缩小范围或补充目标。") :
				Ui("可点击证据回看，调整切点和勾选后再应用。"));
		});
		connect(&m_renderer, &KRRMediaEditor::completed, this, [this](const QString& output, const QString& error)
		{
			setBusy(false);
			if (m_cancelled || !error.isEmpty()) { status(m_cancelled ? Ui("任务已取消。") : error); return; }
			QString probeError;
			if (!ProbeEditSource(output, m_previewMedia, probeError)) { status(probeError); return; }
			m_previewPath = output;
			beginPlayback(m_previewMedia, 0, m_previewMedia.duration);
			status(Ui("正在播放成片预览。"));
		});
		connect(m_player, &KRREditPlayer::frameReady, m_canvas, &KRREditCanvas::setFrame);
		connect(m_player, &KRREditPlayer::stateChanged, this, [this](QMediaPlayer::State state)
		{
			if (!m_speaker) return;
			if (state != QMediaPlayer::PlayingState || m_player->isBuffering()) { m_speaker->stop(); return; }
			if (m_audioData->isOpen())
			{
				const auto offset = qMax<qint64>(0, m_player->position() - m_playFrom) * 48 * 4;
				if (offset < m_audioData->size() && m_audioData->seek(offset)) m_speaker->start(m_audioData);
			}
		});
		connect(m_player, &KRREditPlayer::positionChanged, this, [this](qint64 time)
		{
			m_canvas->setPosition(time);
			if (time >= m_playUntil) { m_playFinished = true; m_player->pause(); }
		});
		connect(m_player, &KRREditPlayer::failed, this, [this](const QString& error) { status(error); });
		m_scope->hide();
		restoreDraft();
		updateServiceLabel();
		setBusy(false);
		RecorderUi::PolishButtons(this);
	}

	~SmartCutDialog() override { stopPlayback(); }

	KRREditProject result;
	bool requestedSubtitles = false;

	void startRequest(const KRRSmartCutOptions& request, bool chapters)
	{
		m_draft = {};
		rebuildTable();
		m_mode->setCurrentIndex(m_mode->findData(request.mode));
		if (request.mode == "range")
		{
			m_begin->setValue(request.scope.begin / 1000.0);
			m_end->setValue(request.scope.end / 1000.0);
		}
		m_limit->setValue(request.maxDuration / 1000.0);
		m_query->setText(request.query);
		m_addChapters->setChecked(chapters);
		// Range controls clamp to the source duration. Ask for an adjusted range before any upload.
		if (request.mode == "range" && request.scope.end > m_original.duration)
		{
			status(Ui("助手指定的范围超出原片，请调整范围再生成草稿。"));
			return;
		}
		QTimer::singleShot(0, this, [this] { if (isVisible()) generate(); });
	}

	void stopForRecording()
	{
		if (m_busy) cancel();
		QDialog::reject();
	}

	void reject() override
	{
		if (m_busy) { cancel(); return; }
		QDialog::reject();
	}

private:
	void status(const QString& text) { m_status->setText(UiDiagnostic(text)); }

	KRRSmartCutOptions options() const
	{
		KRRSmartCutOptions request;
		request.mode = m_mode->currentData().toString();
		request.query = m_query->text().trimmed();
		request.maxDuration = TimeOf(m_limit);
		request.scope = request.mode == "range" ? KRREditRange{TimeOf(m_begin), TimeOf(m_end)} :
			KRREditRange{0, m_original.duration};
		return request;
	}

	void generate()
	{
		stopPlayback();
		if (!m_analysis.suggestSmartCut(m_original, options())) return;
		m_cancelled = false;
		setBusy(true);
		status(Ui("正在准备智能剪辑…"));
	}

	void cancel()
	{
		m_cancelled = true;
		m_analysis.cancel();
		m_renderer.cancel();
		status(Ui("正在取消…"));
	}

	void updateServiceLabel()
	{
		const auto host = SpeechServiceHost("video");
		m_generate->setEnabled(!host.isEmpty());
		if (host.isEmpty())
		{
			m_serviceInfo->setText(Ui("请在服务设置中配置视频理解模型。"));
			return;
		}
		const bool hasCaptions = !m_original.captions.isEmpty() || !ReadAdjacentEditSubtitles(m_original.input, m_original.duration).isEmpty();
		m_serviceInfo->setText(!hasCaptions ?
			Ui("向 %1 发送低分辨率画面和标记。添加字幕后可结合原声文字分析。").arg(host) :
			Ui("向 %1 发送低分辨率画面、字幕原文和标记。").arg(host));
	}

	void setBusy(bool busy)
	{
		m_busy = busy;
		m_options->setEnabled(!busy);
		m_table->setEnabled(!busy);
		m_stop->setVisible(busy);
		m_pause->setEnabled(!busy && m_playUntil > m_playFrom && !m_audioDecode);
		updateSummary();
		select(m_table->currentRow());
	}

	void rebuildTable()
	{
		const QSignalBlocker guard(m_table);
		m_table->clearContents();
		m_table->setRowCount(m_draft.parts.size());
		for (int row = 0; row < m_draft.parts.size(); ++row)
		{
			const auto& part = m_draft.parts[row];
			auto checked = new QTableWidgetItem;
			checked->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
			checked->setCheckState(part.selected ? Qt::Checked : Qt::Unchecked);
			m_table->setItem(row, 0, checked);
			m_table->setItem(row, 1, new QTableWidgetItem(EditTime(part.range.begin)));
			m_table->setItem(row, 2, new QTableWidgetItem(EditTime(part.range.end)));
			m_table->setItem(row, 3, new QTableWidgetItem(EditTime(part.evidence)));
			auto item = new QTableWidgetItem(part.title.isEmpty() ? part.reason : part.title);
			item->setToolTip(part.reason);
			m_table->setItem(row, 4, item);
		}
		if (m_table->rowCount()) m_table->setCurrentCell(0, 0);
		select(m_table->currentRow());
		updateSummary();
	}

	void select(int row)
	{
		const bool valid = row >= 0 && row < m_draft.parts.size();
		for (QWidget* widget : QList<QWidget*>{m_partBegin, m_partEnd, m_change, m_excerpt}) widget->setEnabled(valid && !m_busy);
		if (!valid) { m_detail->clear(); return; }
		const auto& part = m_draft.parts[row];
		m_partBegin->setValue(part.range.begin / 1000.0);
		m_partEnd->setValue(part.range.end / 1000.0);
		m_detail->setText(part.reason);
	}

	void changeRange()
	{
		const int row = m_table->currentRow();
		if (row < 0) return;
		const KRREditRange range{TimeOf(m_partBegin), TimeOf(m_partEnd)};
		if (range.end - range.begin < 20 || range.begin < m_draft.options.scope.begin ||
			range.end > m_draft.options.scope.end || !EditRangesContain(m_original.kept, range))
		{
			status(Ui("草稿片段超出分析范围或含有已删内容。"));
			return;
		}
		const QSignalBlocker guard(m_table);
		m_draft.parts[row].range = range;
		m_table->item(row, 1)->setText(EditTime(range.begin));
		m_table->item(row, 2)->setText(EditTime(range.end));
		reviewChanged();
	}

	void reviewChanged()
	{
		stopPlayback();
		m_previewPath.clear();
		saveDraft();
		updateSummary();
	}

	bool makePlan(KRREditProject& plan, QString& error) const
	{
		return !m_draft.revision.isEmpty() && ApplySmartCutDraft(m_original, m_draft, plan, error);
	}

	void updateSummary()
	{
		KRREditProject plan;
		QString error;
		const bool valid = makePlan(plan, error);
		m_apply->setEnabled(valid && !m_busy);
		m_montage->setEnabled(valid && !m_busy && m_cache.isValid());
		if (m_draft.revision.isEmpty())
		{
			m_summary->setText(Ui("当前有 %1 个必保区间。").arg(m_original.protectedRanges.size()));
			return;
		}
		const auto& scope = m_draft.options.scope;
		auto text = Ui("草稿范围 %1 至 %2 · %3 个必保区间").arg(EditTime(scope.begin), EditTime(scope.end))
			.arg(m_original.protectedRanges.size());
		if (m_draft.options.maxDuration > 0) text += " · " + Ui("上限 %1").arg(EditTime(m_draft.options.maxDuration));
		if (valid)
		{
			text += " · " + Ui("成片 %1").arg(EditTime(plan.outputDuration()));
			if (m_draft.addChapters) text += " · " + Ui("%1 个成片章节").arg(plan.outputChapters().size());
		}
		else if (!error.isEmpty()) text += "\n" + error;
		if (m_draft.inputTokens >= 0) text += "\n" + Ui("输入 %1 / 输出 %2 Token")
			.arg(m_draft.inputTokens).arg(m_draft.outputTokens);
		if (m_draft.proxyBytes > 0) text += " · " + Ui("画面样本 %1 MiB").arg(m_draft.proxyBytes / 1048576.0, 0, 'f', 2);
		m_summary->setText(text);
	}

	void stopPlayback()
	{
		if (m_audioDecode)
		{
			m_audioDecode->disconnect(this);
			m_audioDecode->kill();
			m_audioDecode->deleteLater();
			m_audioDecode = nullptr;
		}
		m_player->stop();
		if (m_speaker) m_speaker->stop();
		m_audioData->close();
		m_audioData->setData({});
		m_playUntil = 0;
		m_pause->setEnabled(false);
	}

	void beginPlayback(const KRREditProject& media, qint64 begin, qint64 until)
	{
		stopPlayback();
		m_canvas->setProject(&media);
		m_canvas->setFrame({});
		m_player->setSource(media.input, media.size.scaled(QSize(640, 480), Qt::KeepAspectRatio), media.fps, media.duration);
		m_playFrom = begin;
		m_playUntil = until;
		m_playFinished = false;
		m_player->setPosition(begin);
		// Buffer a few seconds of PCM, so evidence also plays on Windows without the source media codec.
		if (media.audio.isEmpty())
		{
			m_player->play();
			m_pause->setEnabled(true);
			return;
		}
		auto decode = new QProcess(this);
		m_audioDecode = decode;
		const auto startVideo = [this, decode](bool ok)
		{
			if (m_audioDecode != decode) return;
			m_audioDecode = nullptr;
			if (ok)
			{
				m_audioData->setData(decode->readAllStandardOutput());
				m_audioData->open(QIODevice::ReadOnly);
			}
			if (decode->state() != QProcess::NotRunning) decode->kill();
			decode->deleteLater();
			m_player->play();
			m_pause->setEnabled(true);
		};
		connect(decode, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
			[startVideo](int code, QProcess::ExitStatus exit) { startVideo(exit == QProcess::NormalExit && code == 0); });
		connect(decode, &QProcess::errorOccurred, this, [startVideo](QProcess::ProcessError error)
			{ if (error == QProcess::FailedToStart) startVideo(false); });
		QTimer::singleShot(4000, decode, [startVideo] { startVideo(false); });
		HideConsoleProcess(*decode, true);
		const auto seconds = [](qint64 ms) { return QString::number(ms / 1000.0, 'f', 3); };
		decode->start(FindFFmpeg(), {"-v", "error", "-nostdin", "-threads", "2", "-ss", seconds(begin),
			"-i", media.input, "-t", seconds(until - begin), "-map", "0:a:0", "-ac", "2", "-ar", "48000", "-f", "s16le", "pipe:1"});
	}

	void playEvidence()
	{
		const int row = m_table->currentRow();
		if (row < 0 || m_busy) return;
		const auto& part = m_draft.parts[row];
		const auto at = qBound(part.range.begin, part.evidence, part.range.end - 1);
		beginPlayback(m_original, qMax(part.range.begin, at - 800), qMin(part.range.end, at + 5500));
		status(Ui("证据位置 %1").arg(EditTime(at)));
	}

	void renderPreview()
	{
		if (QFileInfo::exists(m_previewPath))
		{
			beginPlayback(m_previewMedia, 0, m_previewMedia.duration);
			status(Ui("正在播放成片预览。"));
			return;
		}
		KRREditProject plan;
		QString error;
		if (!makePlan(plan, error)) { status(error); return; }
		stopPlayback();
		auto options = SubmissionPreset("small");
		options.previewLength = 8000;
		const auto segments = plan.segments();
		if (segments.size() > 1) options.previewStart = qMax<qint64>(0, segments.first().end - segments.first().begin - 3000);
		const auto file = m_cache.filePath(QString("draft-%1.mp4").arg(++m_renderCount));
		if (!m_renderer.renderTimeline(plan, file, "auto", false, true, options)) return;
		m_cancelled = false;
		setBusy(true);
		status(Ui("正在生成草稿成片预览…"));
	}

	void saveDraft()
	{
		if (m_draftPath.isEmpty()) return;
		QSaveFile file(m_draftPath);
		const auto bytes = QJsonDocument(m_draft.toJson()).toJson(QJsonDocument::Compact);
		if (file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size()) file.commit();
	}

	void restoreDraft()
	{
		if (m_draftPath.isEmpty()) return;
		QFile saved(m_draftPath);
		QString error;
		KRRSmartCutDraft draft;
		if (!saved.open(QIODevice::ReadOnly) || saved.size() > 500000 ||
			!KRRSmartCutDraft::fromJson(QJsonDocument::fromJson(saved.readAll()).object(), draft, error) ||
			draft.revision != SmartCutRevision(m_original)) return;
		m_draft = draft;
		m_mode->setCurrentIndex(m_mode->findData(draft.options.mode));
		m_begin->setValue(draft.options.scope.begin / 1000.0);
		m_end->setValue(draft.options.scope.end / 1000.0);
		m_limit->setValue(draft.options.maxDuration / 1000.0);
		m_query->setText(draft.options.query);
		const QSignalBlocker chaptersGuard(m_addChapters);
		m_addChapters->setChecked(draft.addChapters);
		rebuildTable();
		status(Ui("已恢复此工程的剪辑草稿。"));
	}

	KRREditProject m_original, m_previewMedia;
	KRRSmartCutDraft m_draft;
	QTemporaryDir m_cache;
	KRRMediaEditor m_analysis, m_renderer;
	QString m_draftPath, m_previewPath;
	QWidget *m_options = nullptr, *m_scope = nullptr;
	QComboBox* m_mode = nullptr;
	QDoubleSpinBox *m_begin = nullptr, *m_end = nullptr, *m_partBegin = nullptr, *m_partEnd = nullptr, *m_limit = nullptr;
	QCheckBox* m_addChapters = nullptr;
	QLineEdit* m_query = nullptr;
	QLabel *m_serviceInfo = nullptr, *m_status = nullptr, *m_summary = nullptr, *m_detail = nullptr;
	QPushButton *m_generate = nullptr, *m_stop = nullptr, *m_apply = nullptr, *m_change = nullptr, *m_excerpt = nullptr, *m_montage = nullptr;
	QPushButton* m_pause = nullptr;
	QTableWidget* m_table = nullptr;
	KRREditCanvas* m_canvas = nullptr;
	KRREditPlayer* m_player = nullptr;
	QAudioOutput* m_speaker = nullptr;
	QBuffer* m_audioData = nullptr;
	QProcess* m_audioDecode = nullptr;
	qint64 m_playFrom = 0, m_playUntil = 0;
	int m_renderCount = 0;
	bool m_busy = false, m_cancelled = false, m_playFinished = false;
};
} // namespace

bool ReviewSmartCut(KRRClipEditor* owner, const KRREditProject& project, KRREditRange selection,
				   KRREditProject& result, bool& requestSubtitles, const KRRSmartCutOptions* request, bool chapters)
{
	SmartCutDialog dialog(owner, project, selection);
	QObject::connect(owner, &KRRClipEditor::smartCutInterrupted, &dialog, [&dialog] { dialog.stopForRecording(); });
	if (request) dialog.startRequest(*request, chapters);
	if (dialog.exec() != QDialog::Accepted) return false;
	requestSubtitles = dialog.requestedSubtitles;
	if (!requestSubtitles) result = dialog.result;
	return true;
}

void DockSmartCut(KRRClipEditor* owner, QWidget* host, const KRREditProject& project, KRREditRange selection,
	const KRRSmartCutOptions& request, bool chapters, std::function<void(const KRREditProject&, bool)> apply)
{
	if (auto previous = host->findChild<QDialog*>("smartCutDialog", Qt::FindDirectChildrenOnly))
	{
		previous->hide();
		previous->deleteLater();
	}
	auto dialog = new SmartCutDialog(host, project, selection, true);
	host->layout()->addWidget(dialog);
	QObject::connect(owner, &KRRClipEditor::smartCutInterrupted, dialog, [dialog] { dialog->stopForRecording(); });
	QObject::connect(dialog, &QDialog::finished, host, [dialog, host, apply = std::move(apply)](int result)
	{
		host->hide();
		if (result == QDialog::Accepted) apply(dialog->result, dialog->requestedSubtitles);
		dialog->deleteLater();
	});
	host->show();
	dialog->show();
	dialog->startRequest(request, chapters);
}

bool BatchCut(QWidget* owner, const KRREditProject& project, KRREditRange selection, KRREditProject& result)
{
	QDialog dialog(owner);
	dialog.setObjectName("batchCutDialog");
	dialog.setWindowTitle(Ui("批量剪切"));
	dialog.setWindowFlag(Qt::WindowContextHelpButtonHint, false);
	dialog.resize(Px(520), Px(430));
	auto layout = new QVBoxLayout(&dialog);
	auto table = new QTableWidget(0, 2, &dialog);
	table->setObjectName("batchCutRanges");
	table->setHorizontalHeaderLabels({Ui("入点"), Ui("出点")});
	table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
	table->setSelectionBehavior(QAbstractItemView::SelectRows);
	layout->addWidget(table, 1);
	auto status = new QLabel(&dialog);
	status->setTextFormat(Qt::PlainText);
	status->setWordWrap(true);
	layout->addWidget(status);
	auto actions = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
	auto add = actions->addButton(Ui("添加区间"), QDialogButtonBox::ActionRole);
	auto remove = actions->addButton(Ui("移除所选"), QDialogButtonBox::ActionRole);
	auto keep = actions->addButton(Ui("批量保留"), QDialogButtonBox::ActionRole);
	keep->setObjectName("keepBatchRanges");
	auto discard = actions->addButton(Ui("批量删除"), QDialogButtonBox::ActionRole);
	discard->setObjectName("deleteBatchRanges");
	layout->addWidget(actions);
	auto insert = [&](KRREditRange r)
	{
		const int index = table->rowCount();
		if (index >= 128) return;
		table->insertRow(index);
		table->setCellWidget(index, 0, MakeTime(table, project.duration, r.begin, "batchBegin"));
		table->setCellWidget(index, 1, MakeTime(table, project.duration, r.end, "batchEnd"));
		table->setCurrentCell(index, 0);
	};
	insert(selection.end - selection.begin >= 20 ? selection : KRREditRange{0, qMin<qint64>(5000, project.duration)});
	QObject::connect(add, &QPushButton::clicked, &dialog, [&] { insert(selection); });
	QObject::connect(remove, &QPushButton::clicked, &dialog, [&] { if (table->currentRow() >= 0) table->removeRow(table->currentRow()); });
	auto apply = [&](bool retain)
	{
		QVector<KRREditRange> ranges;
		for (int row = 0; row < table->rowCount(); ++row)
		{
			auto begin = qobject_cast<QDoubleSpinBox*>(table->cellWidget(row, 0));
			auto end = qobject_cast<QDoubleSpinBox*>(table->cellWidget(row, 1));
			ranges.append({TimeOf(begin), TimeOf(end)});
		}
		QString error;
		auto draft = project;
		if (!draft.editVideoRanges(ranges, retain, error) || !draft.validate(error)) { status->setText(error); return; }
		result = draft;
		dialog.accept();
	};
	QObject::connect(keep, &QPushButton::clicked, &dialog, [&] { apply(true); });
	QObject::connect(discard, &QPushButton::clicked, &dialog, [&] { apply(false); });
	QObject::connect(actions, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	RecorderUi::PolishButtons(&dialog);
	return dialog.exec() == QDialog::Accepted;
}
