#include "editsequencedialog.h"
#include "editplayer.h"
#include "editwidgets.h"
#include "sequencesmartcutdialog.h"
#include "uilanguage.h"
#include "uitheme.h"
#include <QApplication>
#include <QCheckBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMediaPlayer>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSlider>
#include "playbackslider.h"
#include <QSplitter>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

namespace
{
class SequenceList final : public QListWidget
{
public:
	using QListWidget::QListWidget;
	std::function<void()> reordered;
	void moveRow(int from, int to)
	{
		if (from == to) return;
		{
			const QSignalBlocker block(this);
			auto item = takeItem(from);
			insertItem(to, item);
			setCurrentItem(item);
		}
		if (reordered) reordered();
	}
protected:
	void mousePressEvent(QMouseEvent* event) override
	{
		QListWidget::mousePressEvent(event);
		m_pressed = event->button() == Qt::LeftButton ? itemAt(event->pos()) : nullptr;
		m_origin = event->pos();
	}
	void mouseMoveEvent(QMouseEvent* event) override
	{
		if (!m_pressed || currentItem() != m_pressed || !(event->buttons() & Qt::LeftButton))
		{
			QListWidget::mouseMoveEvent(event);
			return;
		}
		if (!m_dragging && (event->pos() - m_origin).manhattanLength() < QApplication::startDragDistance()) return;
		m_dragging = true;
		viewport()->setCursor(Qt::ClosedHandCursor);
		const int y = event->pos().y();
		auto bar = verticalScrollBar();
		if (y < 8) bar->setValue(bar->value() - bar->singleStep());
		else if (y > viewport()->height() - 8) bar->setValue(bar->value() + bar->singleStep());
		const auto target = itemAt(QPoint(viewport()->width() / 2, y));
		moveRow(row(m_pressed), target ? row(target) : y < 0 ? 0 : count() - 1);
		event->accept();
	}
	void mouseReleaseEvent(QMouseEvent* event) override
	{
		m_pressed = nullptr;
		m_dragging = false;
		viewport()->unsetCursor();
		QListWidget::mouseReleaseEvent(event);
	}
private:
	QListWidgetItem* m_pressed = nullptr;
	QPoint m_origin;
	bool m_dragging = false;
};
}

KRREditSequenceDialog::KRREditSequenceDialog(const KRREditProject& current, const QString& folder, QWidget* parent)
	: QDialog(parent), m_current(current), m_folder(folder), m_exporter(this)
{
	setObjectName("editSequenceDialog");
	setWindowTitle(Ui("WPS Recorder · 多素材顺排"));
	setWindowFlag(Qt::WindowContextHelpButtonHint, false);
	setWindowFlag(Qt::WindowMaximizeButtonHint, true);
	resize(1070, 700);
	auto root = new QVBoxLayout(this);
	auto toolbar = new QHBoxLayout;
	auto makeButton = [this](QHBoxLayout* row, const char* label, const char* id)
	{
		auto button = new QPushButton(Ui(label), this);
		button->setObjectName(id);
		button->setAutoDefault(false);
		row->addWidget(button);
		return button;
	};
	m_add = makeButton(toolbar, "导入素材…", "sequenceImport");
	m_addCurrent = makeButton(toolbar, "加入当前剪辑", "sequenceAddCurrent");
	m_addCurrent->setVisible(!m_current.input.isEmpty());
	toolbar->addStretch();
	m_smartCut = makeButton(toolbar, "AI 混剪助手…", "sequenceSmartCut");
	m_open = makeButton(toolbar, "打开工程…", "sequenceOpen");
	m_save = makeButton(toolbar, "保存工程", "sequenceSave");
	root->addLayout(toolbar);
	auto panes = new QSplitter(Qt::Horizontal, this);
	auto left = new QWidget(panes);
	auto leftLayout = new QVBoxLayout(left);
	leftLayout->setContentsMargins(0, 0, 0, 0);
	m_summary = new QLabel(left);
	m_summary->setWordWrap(true);
	leftLayout->addWidget(m_summary);
	auto list = new SequenceList(left);
	m_list = list;
	m_list->setObjectName("sequenceList");
	m_list->setAccessibleName(Ui("素材顺序，拖动调整"));
	m_list->setSelectionMode(QAbstractItemView::SingleSelection);
	m_list->setTextElideMode(Qt::ElideMiddle);
	m_list->setMinimumWidth(300);
	leftLayout->addWidget(m_list, 1);
	auto ordering = new QHBoxLayout;
	m_up = makeButton(ordering, "上移", "sequenceUp");
	m_down = makeButton(ordering, "下移", "sequenceDown");
	m_remove = makeButton(ordering, "移除", "sequenceRemove");
	m_locate = makeButton(ordering, "定位素材…", "sequenceLocate");
	leftLayout->addLayout(ordering);
	auto right = new QWidget(panes);
	auto rightLayout = new QVBoxLayout(right);
	rightLayout->setContentsMargins(0, 0, 0, 0);
	m_canvas = new KRREditCanvas(right);
	m_canvas->setMinimumSize(340, 190);
	m_player = new KRREditPlayer(this);
	m_audio = new QMediaPlayer(this);
	m_audio->setNotifyInterval(100);
	rightLayout->addWidget(m_canvas, 1);
	auto transport = new QHBoxLayout;
	m_play = makeButton(transport, "播放", "sequencePlay");
	m_seek = new KRRPlaybackSlider(right);
	m_seek->setObjectName("sequenceSeek");
	m_seek->setAccessibleName(Ui("源视频播放位置"));
	transport->addWidget(m_seek, 1);
	m_time = new QLabel(right);
	transport->addWidget(m_time);
	rightLayout->addLayout(transport);
	auto trim = new QHBoxLayout;
	auto timeBox = [&](const char* label, const char* id)
	{
		trim->addWidget(new QLabel(Ui(label), right));
		auto spin = new QDoubleSpinBox(right);
		spin->setObjectName(id);
		spin->setAccessibleName(Ui(label));
		spin->setDecimals(3);
		spin->setSingleStep(0.1);
		spin->setSuffix(Ui(" 秒"));
		trim->addWidget(spin);
		return spin;
	};
	m_begin = timeBox("起点", "sequenceBegin");
	m_useBegin = makeButton(trim, "取播放位置", "sequenceUseBegin");
	m_end = timeBox("终点", "sequenceEnd");
	m_useEnd = makeButton(trim, "取播放位置", "sequenceUseEnd");
	trim->addStretch();
	rightLayout->addLayout(trim);
	auto cut = new QHBoxLayout;
	m_keep = makeButton(cut, "只保留选区", "sequenceKeep");
	m_cut = makeButton(cut, "删除选区", "sequenceCut");
	m_undo = makeButton(cut, "撤销上次切剪", "sequenceUndo");
	m_evidence = makeButton(cut, "回看依据", "sequenceEvidence");
	cut->addStretch();
	rightLayout->addLayout(cut);
	m_ranges = new QLabel(right);
	m_ranges->setObjectName("sequenceRanges");
	m_ranges->setWordWrap(true);
	rightLayout->addWidget(m_ranges);
	panes->setStretchFactor(1, 1);
	panes->setSizes({350, 680});
	root->addWidget(panes, 1);
	auto exportRow = new QHBoxLayout;
	m_separate = new QCheckBox(Ui("保留独立音轨"), this);
	m_separate->setToolTip(Ui("成片默认播放混音，可在播放器中切换独立音轨。"));
	m_separate->setChecked(true);
	exportRow->addWidget(m_separate);
	exportRow->addWidget(new QLabel(Ui("成片画幅和帧率跟随首段素材"), this));
	exportRow->addStretch();
	m_viewExport = makeButton(exportRow, "打开成片", "sequenceViewExport");
	m_cancel = makeButton(exportRow, "取消导出", "sequenceCancel");
	m_export = makeButton(exportRow, "导出 MP4…", "sequenceExport");
	m_export->setProperty("role", "primary");
	root->addLayout(exportRow);
	m_status = new QLabel(this);
	m_status->setObjectName("sequenceStatus");
	m_status->setWordWrap(true);
	m_status->setTextFormat(Qt::PlainText);
	m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
	root->addWidget(m_status);

	connect(m_add, &QPushButton::clicked, this, [this]
	{
		importFiles(QFileDialog::getOpenFileNames(this, Ui("导入视频或单素材剪辑工程"), m_folder,
			Ui("视频和剪辑工程 (*.mp4 *.mov *.mkv *.avi *.webm *.flv *.json)")));
	});
	connect(m_addCurrent, &QPushButton::clicked, this, [this]
	{
		KRREditSequencePart part;
		QString error;
		if (!MakeSequencePart(m_current, part, error)) { setStatus(error); return; }
		appendPart(std::move(part));
	});
	connect(m_open, &QPushButton::clicked, this, [this]
	{
		const auto path = QFileDialog::getOpenFileName(this, Ui("打开多素材工程"), m_folder, Ui("剪辑工程 (*.json)"));
		if (!path.isEmpty()) openProject(path);
	});
	connect(m_save, &QPushButton::clicked, this, [this] { saveProject(); });
	connect(m_up, &QPushButton::clicked, this, [this] { moveSelected(-1); });
	connect(m_down, &QPushButton::clicked, this, [this] { moveSelected(1); });
	connect(m_remove, &QPushButton::clicked, this, [this]
	{
		const auto id = m_list->currentItem()->data(Qt::UserRole).toInt();
		delete m_list->takeItem(m_list->currentRow());
		m_parts.remove(id);
		m_lastCut.remove(id);
		m_dirty = true;
		refreshControls();
	});
	connect(m_locate, &QPushButton::clicked, this, [this] { locateSelected(); });
	connect(list, &QListWidget::currentItemChanged, this, [this] { selectionChanged(); });
	list->reordered = [this] { m_dirty = true; refreshControls(); };
	connect(m_play, &QPushButton::clicked, this, [this]
	{
		if (m_player->state() == QMediaPlayer::PlayingState) m_player->pause();
		else
		{
			m_previewFrameOnly = false;
			m_player->setPosition(m_seek->value() >= m_seek->maximum() - 33 ? 0 : m_seek->value());
			m_player->play();
		}
	});
	connect(m_seek, &QSlider::sliderReleased, this, &KRREditSequenceDialog::seekPreview);
	connect(m_seek, &QSlider::valueChanged, this, [this](int)
	{
		if (!m_seek->isSliderDown()) seekPreview();
	});
	connect(m_player, &KRREditPlayer::frameReady, m_canvas, &KRREditCanvas::setFrame);
	connect(m_player, &KRREditPlayer::positionChanged, this, [this](qint64 position)
	{
		const QSignalBlocker block(m_seek);
		if (!m_seek->isSliderDown()) m_seek->setValue(static_cast<int>(position));
		m_canvas->setPosition(position);
		m_time->setText(EditTime(position));
		if (m_previewFrameOnly)
		{
			m_previewFrameOnly = false;
			m_player->pause();
		}
		else if (!m_audioStarted && !m_audio->media().isNull())
		{
			m_audioStarted = true;
			m_audio->setPosition(position);
			m_audio->play();
		}
		else if (m_audio->state() == QMediaPlayer::PlayingState && m_audio->isSeekable() &&
			qAbs(m_audio->position() - position) > 350)
			m_audio->setPosition(position);
	});
	connect(m_player, &KRREditPlayer::stateChanged, this, [this](QMediaPlayer::State state)
	{
		m_play->setText(state == QMediaPlayer::PlayingState ? Ui("暂停") : Ui("播放"));
		if (state != QMediaPlayer::PlayingState)
		{
			m_audioStarted = false;
			m_audio->pause();
		}
	});
	connect(m_player, &KRREditPlayer::failed, this, &KRREditSequenceDialog::setStatus);
	connect(m_useBegin, &QPushButton::clicked, this, [this] { m_begin->setValue(m_player->position() / 1000.0); });
	connect(m_useEnd, &QPushButton::clicked, this, [this] { m_end->setValue(m_player->position() / 1000.0); });
	connect(m_keep, &QPushButton::clicked, this, [this] { cutSelection(true); });
	connect(m_cut, &QPushButton::clicked, this, [this] { cutSelection(false); });
	connect(m_undo, &QPushButton::clicked, this, [this]
	{
		auto item = m_list->currentItem();
		selected()->edit->kept = m_lastCut.take(item->data(Qt::UserRole).toInt());
		m_dirty = true;
		updateRow(item);
		refreshControls();
		setStatus(Ui("已撤销该素材的上一次切剪。"));
	});
	connect(m_export, &QPushButton::clicked, this, [this] { exportSequence(); });
	connect(m_smartCut, &QPushButton::clicked, this, [this] { openSmartCut(); });
	connect(m_evidence, &QPushButton::clicked, this, [this]
	{
		const auto evidence = selected()->saved["highlight"].toObject();
		const auto at = qBound(0, evidence["evidence_ms"].toInt(), m_seek->maximum());
		m_previewFrameOnly = m_player->state() != QMediaPlayer::PlayingState;
		m_player->setPosition(at);
		if (m_previewFrameOnly) m_player->play();
		setStatus(Ui("源片 %1 · %2").arg(EditTime(at), evidence["reason"].toString()));
	});
	connect(m_cancel, &QPushButton::clicked, this, [this] { m_exporter.cancel(); });
	connect(m_viewExport, &QPushButton::clicked, this, [this]
	{
		if (!QDesktopServices::openUrl(QUrl::fromLocalFile(m_lastExport))) setStatus(m_lastExport);
	});
	connect(&m_exporter, &KRRMediaEditor::progress, this, [this](const auto& message) { setStatus(message); });
	connect(&m_exporter, &KRRMediaEditor::completed, this, [this](const auto& output, const auto& error)
	{
		if (error.isEmpty()) m_lastExport = output;
		setStatus(error.isEmpty() ? Ui("成片已导出：") + output : error);
		refreshControls();
	});
	selectionChanged();
	RecorderUi::PolishButtons(this);
}

KRREditSequencePart* KRREditSequenceDialog::selected()
{
	const auto item = m_list->currentItem();
	if (!item) return nullptr;
	const auto it = m_parts.find(item->data(Qt::UserRole).toInt());
	return it == m_parts.end() ? nullptr : &it.value();
}

KRREditSequence KRREditSequenceDialog::ordered() const
{
	KRREditSequence result;
	for (int row = 0; row < m_list->count(); ++row)
		result.append(m_parts.value(m_list->item(row)->data(Qt::UserRole).toInt()));
	return result;
}

void KRREditSequenceDialog::appendPart(KRREditSequencePart part)
{
	const int id = ++m_nextId;
	m_parts.insert(id, std::move(part));
	auto item = new QListWidgetItem;
	item->setData(Qt::UserRole, id);
	m_list->addItem(item);
	updateRow(item);
	if (!m_list->currentItem()) m_list->setCurrentItem(item);
	m_dirty = true;
	refreshControls();
}

void KRREditSequenceDialog::updateRow(QListWidgetItem* item)
{
	const auto& part = m_parts[item->data(Qt::UserRole).toInt()];
	const auto detail = part.edit ? EditTime(part.edit->outputDuration()) : Ui("待定位");
	const auto highlight = part.saved["highlight"].toObject();
	const auto title = highlight["title"].toString();
	const auto label = QFileInfo(part.source()).fileName();
	item->setText((title.isEmpty() ? label : title + " / " + label) + "   ·   " + detail);
	const auto reason = highlight["reason"].toString();
	item->setToolTip(part.unavailable.isEmpty() ? part.source() + (reason.isEmpty() ? QString() : '\n' + reason) :
		part.unavailable.join('\n'));
}

void KRREditSequenceDialog::selectionChanged()
{
	const auto part = selected();
	const auto edit = part && part->edit ? &*part->edit : nullptr;
	m_previewFrameOnly = false;
	m_player->clear();
	m_audio->setMedia(QMediaContent{});
	m_preview = edit ? *edit : KRREditProject{};
	m_canvas->setProject(edit ? &m_preview : nullptr);
	m_canvas->setFrame({});
	m_canvas->setPosition(0);
	if (edit)
	{
		m_player->setSource(edit->input, edit->size, edit->fps, edit->duration);
		for (const auto& track : edit->audio)
		{
			if (!track.file.isEmpty()) continue;
			m_audio->setMedia(QUrl::fromLocalFile(edit->input));
			break;
		}
	}
	const QSignalBlocker block(m_seek);
	m_seek->setRange(0, edit ? static_cast<int>(edit->duration) : 0);
	m_seek->setValue(0);
	m_time->setText(EditTime(0));
	for (auto box : {m_begin, m_end}) box->setRange(0, edit ? edit->duration / 1000.0 : 0);
	m_begin->setValue(0);
	m_end->setValue(m_end->maximum());
	refreshControls();
}

void KRREditSequenceDialog::seekPreview()
{
	if (m_preview.input.isEmpty() || !m_seek->isEnabled() || qAbs(m_player->position() - m_seek->value()) <= 3) return;
	m_previewFrameOnly = m_player->state() != QMediaPlayer::PlayingState || m_previewFrameOnly;
	m_audioStarted = false;
	m_audio->pause();
	m_player->setPosition(m_seek->value());
	if (m_previewFrameOnly) m_player->play();
}

void KRREditSequenceDialog::refreshControls()
{
	const bool exporting = m_exporter.isBusy();
	const bool idle = !exporting && !m_ioBusy;
	const auto part = selected();
	const bool ready = part && part->edit.has_value();
	const int count = m_list->count(), row = m_list->currentRow();
	int missing = 0;
	qint64 duration = 0;
	for (const auto& value : m_parts)
	{
		if (!value.edit) ++missing;
		else duration += value.edit->outputDuration();
	}
	auto summary = count == 0 ? Ui("导入素材后拖动调整播放顺序。") :
		Ui("%1 段素材 · %2").arg(count).arg(EditTime(duration));
	if (missing > 0) summary += Ui(" · %1 段待定位").arg(missing);
	m_summary->setText(summary);
	m_open->setEnabled(idle);
	m_add->setEnabled(idle && count < MaxSequenceParts);
	m_addCurrent->setEnabled(m_add->isEnabled());
	m_save->setEnabled(idle && count > 0);
	m_list->setEnabled(idle);
	m_up->setEnabled(idle && row > 0);
	m_down->setEnabled(idle && row >= 0 && row + 1 < count);
	m_remove->setEnabled(idle && part);
	m_locate->setEnabled(idle && part);
	for (auto button : {m_play, m_useBegin, m_useEnd, m_keep, m_cut}) button->setEnabled(idle && ready);
	m_begin->setEnabled(idle && ready);
	m_end->setEnabled(idle && ready);
	m_seek->setEnabled(idle && ready);
	m_undo->setEnabled(idle && ready && m_lastCut.contains(m_list->currentItem()->data(Qt::UserRole).toInt()));
	m_export->setEnabled(idle && count > 0 && missing == 0);
	m_smartCut->setEnabled(m_export->isEnabled());
	m_evidence->setEnabled(idle && ready && part->saved["highlight"].isObject());
	m_evidence->setVisible(part && part->saved["highlight"].isObject());
	m_separate->setEnabled(idle);
	m_viewExport->setVisible(!m_lastExport.isEmpty());
	m_cancel->setVisible(exporting);
	if (!ready)
	{
		m_ranges->setText(part ? Ui("定位缺失或已变更的文件后可继续切剪。") : QString());
		return;
	}
	const auto& edit = *part->edit;
	QStringList ranges;
	for (const auto& keep : edit.kept) ranges << EditTime(keep.begin) + " – " + EditTime(keep.end);
	m_ranges->setToolTip(ranges.join('\n'));
	m_ranges->setText(Ui("保留 %1 个区间 · %2 条音轨 · %3 条字幕 · %4 个章节\n%5")
		.arg(ranges.size()).arg(edit.audio.size()).arg(edit.outputCaptions().size()).arg(edit.outputChapters().size())
		.arg(ranges.mid(0, 3).join("   ") + (ranges.size() > 3 ? QStringLiteral(" …") : QString())));
}

void KRREditSequenceDialog::moveSelected(int delta)
{
	const int row = m_list->currentRow(), next = row + delta;
	if (row < 0 || next < 0 || next >= m_list->count()) return;
	static_cast<SequenceList*>(m_list)->moveRow(row, next);
}

void KRREditSequenceDialog::cutSelection(bool retain)
{
	auto& edit = *selected()->edit;
	const auto previous = edit.kept;
	QString error;
	if (!edit.editRange(qRound64(m_begin->value() * 1000), qRound64(m_end->value() * 1000), -1, retain, error))
	{
		setStatus(error);
		return;
	}
	const auto item = m_list->currentItem();
	m_lastCut[item->data(Qt::UserRole).toInt()] = previous;
	m_dirty = true;
	updateRow(item);
	refreshControls();
	setStatus(Ui("已更新保留区间。"));
}

void KRREditSequenceDialog::importFiles(const QStringList& paths, const QString& montageGoal)
{
	if (paths.isEmpty() || m_ioBusy || m_exporter.isBusy()) return;
	if (paths.size() + m_list->count() > MaxSequenceParts)
	{
		setStatus(Ui("单个工程最多包含 %1 段素材。").arg(MaxSequenceParts));
		return;
	}
	auto parts = std::make_shared<KRREditSequence>();
	auto errors = std::make_shared<QStringList>();
	m_player->pause();
	setStatus(Ui("正在读取素材…"));
	runIo([paths, parts, errors]
	{
		for (const auto& path : paths)
		{
			KRREditSequencePart part;
			QString error;
			if (ImportSequencePart(path, part, error)) parts->append(std::move(part));
			else errors->append(QFileInfo(path).fileName() + " · " + error);
		}
	}, [this, parts, errors, paths, montageGoal]
	{
		for (auto& part : *parts) appendPart(std::move(part));
		m_folder = QFileInfo(paths.back()).absolutePath();
		const auto result = Ui("已导入 %1 段素材。").arg(parts->size());
		setStatus(errors->isEmpty() ? result : result + '\n' + errors->join('\n'));
		if (!montageGoal.isEmpty() && errors->isEmpty())
			QTimer::singleShot(0, this, [this, montageGoal] { openSmartCut(montageGoal); });
	});
}

void KRREditSequenceDialog::openProject(const QString& path)
{
	if (m_ioBusy || m_exporter.isBusy() || !confirmReplace()) return;
	auto parts = std::make_shared<KRREditSequence>();
	auto error = std::make_shared<QString>();
	m_player->pause();
	setStatus(Ui("正在读取工程…"));
	runIo([path, parts, error]
	{
		QJsonObject json;
		if (ReadEditProjectJson(path, json, *error)) ReadEditSequence(json, *parts, *error);
	}, [this, path, parts, error]
	{
		if (!error->isEmpty()) { setStatus(*error); return; }
		m_list->clear();
		m_parts.clear();
		m_lastCut.clear();
		for (auto& part : *parts) appendPart(std::move(part));
		m_projectPath = path;
		m_folder = QFileInfo(path).absolutePath();
		m_dirty = false;
		setStatus(Ui("已打开工程。选择待定位素材可找回原文件。"));
	});
}

void KRREditSequenceDialog::locateSelected()
{
	const auto item = m_list->currentItem();
	const auto id = item->data(Qt::UserRole).toInt();
	auto paths = UnavailableSequenceFiles(*selected());
	if (paths.isEmpty())
	{
		setStatus(Ui("该素材的所有文件均可读取。"));
		return;
	}
	QString oldPath = paths.front();
	if (paths.size() > 1)
	{
		bool ok = false;
		oldPath = QInputDialog::getItem(this, Ui("定位素材"), Ui("选择要定位的原文件路径"), paths, 0, false, &ok);
		if (!ok) return;
	}
	const auto newPath = QFileDialog::getOpenFileName(this, Ui("定位：") + QFileInfo(oldPath).fileName(), m_folder, Ui("素材文件 (*)"));
	if (newPath.isEmpty()) return;
	auto updated = std::make_shared<QHash<int, KRREditSequencePart>>(m_parts);
	auto error = std::make_shared<QString>();
	m_player->stop();
	m_audio->setMedia(QMediaContent{});
	runIo([updated, error, id, oldPath, newPath]
	{
		auto& primary = (*updated)[id];
		if (!RelocateSequenceFile(primary, oldPath, newPath, *error)) return;
		for (auto it = updated->begin(); it != updated->end(); ++it)
		{
			if (it.key() == id || !it->saved["assets"].toObject().contains(oldPath)) continue;
			QString ignored;
			RelocateSequenceFile(it.value(), oldPath, newPath, ignored);
		}
	}, [this, updated, error, newPath]
	{
		if (!error->isEmpty()) { selectionChanged(); setStatus(*error); return; }
		m_parts = std::move(*updated);
		for (int row = 0; row < m_list->count(); ++row) updateRow(m_list->item(row));
		m_folder = QFileInfo(newPath).absolutePath();
		m_dirty = true;
		selectionChanged();
		setStatus(Ui("已更新素材路径。保存工程可保留定位结果。"));
	});
}

bool KRREditSequenceDialog::saveProject()
{
	auto path = m_projectPath;
	if (path.isEmpty())
		path = QFileDialog::getSaveFileName(this, Ui("保存多素材工程"), m_folder + "/sequence.json", Ui("剪辑工程 (*.json)"));
	if (path.isEmpty()) return false;
	QString error;
	if (!SaveEditSequence(path, ordered(), error))
	{
		setStatus(error);
		return false;
	}
	m_projectPath = path;
	m_dirty = false;
	setStatus(Ui("工程已保存：") + path);
	return true;
}

bool KRREditSequenceDialog::confirmReplace()
{
	if (!m_dirty) return true;
	auto buttons = QMessageBox::Discard | QMessageBox::Cancel;
	if (m_list->count() > 0) buttons |= QMessageBox::Save;
	const auto answer = QMessageBox::question(this, Ui("保存多素材工程"), Ui("当前素材编排有改动。是否保存？"),
		buttons, m_list->count() > 0 ? QMessageBox::Save : QMessageBox::Cancel);
	return answer == QMessageBox::Discard || (answer == QMessageBox::Save && saveProject());
}

void KRREditSequenceDialog::exportSequence()
{
	const auto label = "sequence-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss") + ".mp4";
	const auto path = QFileDialog::getSaveFileName(this, Ui("导出多素材 MP4"), m_folder + '/' + label, "MP4 (*.mp4)");
	if (path.isEmpty()) return;
	m_player->stop();
	m_audio->setMedia(QMediaContent{});
	m_exporter.renderSequence(ordered(), path, m_separate->isChecked());
	refreshControls();
}

void KRREditSequenceDialog::openSmartCut(const QString& goal)
{
	m_player->pause();
	KRRSequenceSmartCutDialog review(ordered(), this, goal);
	m_activeSmartCut = &review;
	const auto accepted = review.exec() == QDialog::Accepted;
	m_activeSmartCut = nullptr;
	if (!accepted) return;
	m_player->clear();
	m_list->clear();
	m_parts.clear();
	m_lastCut.clear();
	for (auto part : review.result()) appendPart(std::move(part));
	m_dirty = true;
	setStatus(Ui("混剪草稿已应用。可继续调整或保存工程。"));
}

void KRREditSequenceDialog::interruptSmartCut()
{
	if (m_activeSmartCut) m_activeSmartCut->interrupt();
}

void KRREditSequenceDialog::runIo(std::function<void()> work, std::function<void()> finished)
{
	m_ioBusy = true;
	refreshControls();
	auto thread = QThread::create(std::move(work));
	connect(thread, &QThread::finished, this, [this, finished]
	{
		m_ioBusy = false;
		finished();
		refreshControls();
	});
	connect(thread, &QThread::finished, thread, &QObject::deleteLater);
	thread->start(QThread::LowPriority);
}

void KRREditSequenceDialog::setStatus(const QString& message)
{
	m_status->setText(UiDiagnostic(message));
}

void KRREditSequenceDialog::reject()
{
	if (m_ioBusy) return;
	if (m_exporter.isBusy())
	{
		setStatus(Ui("请等待导出完成，或点击取消导出。"));
		return;
	}
	if (confirmReplace()) QDialog::reject();
}
