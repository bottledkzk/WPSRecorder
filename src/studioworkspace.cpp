#include "mainwindow.h"

#include "annotationoverlay.h"
#include "audiocapture.h"
#include "clipeditor.h"
#include "compositor.h"
#include "controlserver.h"
#include "playbackslider.h"
#include "recordingassistantdialog.h"
#include "studioassistant.h"
#include "recordingcheck.h"
#include "recordingmarkers.h"
#include "recordingrecovery.h"
#include "recordingwidgets.h"
#include "regionselector.h"
#include "speechsettingsdialog.h"
#include "uilanguage.h"
#include "uitheme.h"

#include <windows.h>
#include <QAbstractItemModel>
#include <QApplication>
#include <QDrag>
#include <QDragEnterEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QScopedValueRollback>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTimeEdit>
#include <QDialog>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHeaderView>
#include <QIntValidator>
#include <QJsonDocument>
#include <QLineEdit>
#include <QLocalSocket>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QMenu>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStorageInfo>
#include <QTabWidget>
#include <QTableWidget>
#include <QToolButton>
#include <QUuid>
#include <algorithm>
#include <utility>

namespace
{
QPushButton* Button(QWidget* parent, const QString& text, const QString& glyph = {})
{
	auto button = new QPushButton(text, parent);
	if (!glyph.isEmpty()) button->setIcon(RecorderUi::Icon(glyph));
	return button;
}

void CopyItems(QComboBox* copy, const QComboBox* source)
{
	const QSignalBlocker block(copy);
	copy->clear();
	for (int i = 0; i < source->count(); ++i)
		copy->addItem(source->itemText(i), source->itemData(i));
	copy->setCurrentIndex(source->currentIndex());
}

QComboBox* LinkedCombo(QWidget* parent, QComboBox* source)
{
	auto copy = new QComboBox(parent);
	copy->setMinimumContentsLength(8);
	copy->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	copy->setEditable(source->isEditable());
	CopyItems(copy, source);
	if (source->isEditable()) copy->setCurrentText(source->currentText());
	QObject::connect(copy, qOverload<int>(&QComboBox::activated), source, &QComboBox::setCurrentIndex);
	QObject::connect(source, qOverload<int>(&QComboBox::currentIndexChanged), copy, &QComboBox::setCurrentIndex);
	if (source->isEditable())
	{
		QObject::connect(copy->lineEdit(), &QLineEdit::editingFinished, source,
			[source, copy] { source->setCurrentText(copy->currentText()); });
		QObject::connect(source, &QComboBox::currentTextChanged, copy, &QComboBox::setCurrentText);
	}
	return copy;
}

QCheckBox* LinkedCheck(QWidget* parent, QCheckBox* source)
{
	auto copy = new QCheckBox(source->text(), parent);
	copy->setChecked(source->isChecked());
	QObject::connect(copy, &QCheckBox::toggled, source, &QCheckBox::setChecked);
	QObject::connect(source, &QCheckBox::toggled, copy, &QCheckBox::setChecked);
	return copy;
}

QFormLayout* DockForm(QWidget* parent)
{
	auto form = new QFormLayout(parent);
	form->setContentsMargins(6, 6, 6, 6);
	form->setVerticalSpacing(5);
	form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
	return form;
}

// Dock title dragging shares the same splitter as width adjustment.
class StudioDock : public QFrame
{
public:
	StudioDock(QSplitter* owner, const QString& name, const QString& title, QWidget* contents, QWidget* action = nullptr)
		: QFrame(owner), m_owner(owner)
	{
		setObjectName(name);
		setProperty("studioDock", true);
		setAcceptDrops(true);
		setMinimumWidth(RecorderUi::Px(112));
		auto layout = new QVBoxLayout(this);
		layout->setContentsMargins(1, 1, 1, 1);
		layout->setSpacing(1);
		m_header = new QWidget(this);
		m_header->setObjectName("studioDockHeader");
		auto row = new QHBoxLayout(m_header);
		row->setContentsMargins(6, 1, 4, 1);
		row->setSpacing(3);
		auto label = new QLabel(title, m_header);
		label->setAttribute(Qt::WA_TransparentForMouseEvents);
		label->setProperty("dockTitle", true);
		row->addWidget(label, 1);
		if (action) row->addWidget(action);
		m_header->setCursor(Qt::OpenHandCursor);
		m_header->installEventFilter(this);
		layout->addWidget(m_header);
		layout->addWidget(contents, 1);
	}
protected:
	bool eventFilter(QObject* object, QEvent* event) override
	{
		if (object == m_header && event->type() == QEvent::MouseButtonPress)
			m_start = static_cast<QMouseEvent*>(event)->pos();
		else if (object == m_header && event->type() == QEvent::MouseMove)
		{
			auto mouse = static_cast<QMouseEvent*>(event);
			if ((mouse->buttons() & Qt::LeftButton) && (mouse->pos() - m_start).manhattanLength() >= QApplication::startDragDistance())
			{
				auto mime = new QMimeData;
				mime->setData("application/x-wpsrecorder-dock", objectName().toUtf8());
				auto drag = new QDrag(this);
				drag->setMimeData(mime);
				drag->exec(Qt::MoveAction);
				drag->deleteLater();
				return true;
			}
		}
		return QFrame::eventFilter(object, event);
	}
	void dragEnterEvent(QDragEnterEvent* event) override
	{
		auto dock = dynamic_cast<StudioDock*>(event->source());
		if (dock && dock != this && dock->m_owner == m_owner) event->acceptProposedAction();
	}
	void dropEvent(QDropEvent* event) override
	{
		auto dock = dynamic_cast<StudioDock*>(event->source());
		if (!dock || dock == this || dock->m_owner != m_owner) return;
		const int from = m_owner->indexOf(dock), to = m_owner->indexOf(this);
		auto sizes = m_owner->sizes();
		const int width = sizes.takeAt(from);
		sizes.insert(to, width);
		m_owner->insertWidget(to, dock);
		m_owner->setSizes(sizes);
		event->acceptProposedAction();
	}
private:
	QSplitter* m_owner;
	QWidget* m_header = nullptr;
	QPoint m_start;
};

class ChapterDelegate : public QStyledItemDelegate
{
public:
	using QStyledItemDelegate::QStyledItemDelegate;
protected:
	void initStyleOption(QStyleOptionViewItem* option, const QModelIndex& index) const override
	{
		QStyledItemDelegate::initStyleOption(option, index);
		option->text = option->text + "\n" + EditTime(index.data(Qt::UserRole).toLongLong());
	}
};

QString Clock(qint64 ms)
{
	const auto s = qMax<qint64>(0, ms) / 1000;
	return QString("%1:%2:%3").arg(s / 3600, 2, 10, QLatin1Char('0'))
		.arg(s / 60 % 60, 2, 10, QLatin1Char('0')).arg(s % 60, 2, 10, QLatin1Char('0'));
}

qint64 FileTime(const FILETIME& t)
{
	return (qint64(t.dwHighDateTime) << 32) | t.dwLowDateTime;
}
}

void KRRMainWindow::buildStudioHeader(QVBoxLayout* root)
{
	auto header = new QFrame(this);
	m_studioHeader = header;
	header->setObjectName("studioHeader");
	auto row = new QHBoxLayout(header);
	row->setContentsMargins(7, 3, 5, 3);
	row->setSpacing(4);
	auto icon = new QLabel(header);
	icon->setPixmap(windowIcon().isNull() ? RecorderUi::Icon("record").pixmap(18, 18) : windowIcon().pixmap(18, 18));
	row->addWidget(icon);
	auto title = new QLabel("WPS Recorder", header);
	title->setObjectName("studioTitle");
	row->addWidget(title);
	auto badge = new QLabel("STUDIO", header);
	badge->setObjectName("studioBadge");
	row->addWidget(badge);
	auto menu = new QMenuBar(header);
	menu->setNativeMenuBar(false);
	auto file = menu->addMenu(Ui("文件"));
	auto tools = menu->addMenu(Ui("工具"));
	auto view = menu->addMenu(Ui("视图"));
	auto settingsMenu = menu->addMenu(Ui("设置"));
	auto help = menu->addMenu(Ui("帮助"));
	row->addWidget(menu);
	row->addStretch(1);

	m_workspaceButtons = new QButtonGroup(this);
	const QStringList sections{Ui("录制工作台"), Ui("回看与管理"), Ui("剪辑工坊"), Ui("智能 AI 助手")};
	for (int i = 0; i < sections.size(); ++i)
	{
		auto button = Button(header, sections[i]);
		button->setObjectName(QString("studioWorkspace%1").arg(i));
		button->setProperty("role", "workspace");
		button->setCheckable(true);
		m_workspaceButtons->addButton(button, i);
		row->addWidget(button);
		auto workspaceAction = view->addAction(sections[i]);
		connect(button, &QPushButton::clicked, this, [this, i] { selectWorkspace(Workspace(i)); });
		connect(workspaceAction, &QAction::triggered, button, &QPushButton::click);
	}
	row->addStretch(1);
	auto schedule = Button(header, Ui("预约"), "clock");
	schedule->setObjectName("studioSchedule");
	m_floatButton = Button(header, Ui("悬浮条"), "screen");
	m_floatButton->setCheckable(true);
	m_floatButton->setChecked(true);
	m_floatButton->setToolTip(Ui("显示常驻悬浮录制控制"));
	m_assistantToggle = Button(header, {}, "assistant");
	m_assistantToggle->setToolTip(Ui("录屏助手"));
	m_assistantToggle->setObjectName("openRecordingAssistant");
	m_assistantToggle->setCheckable(true);
	for (auto button : {schedule, m_floatButton}) row->addWidget(button);
	m_otherExport = Button(header, Ui("导出"), "export");
	m_otherExport->setProperty("role", "primary");
	row->addWidget(m_otherExport);
	auto minimize = Button(header, QStringLiteral("−"));
	m_windowMaximize = Button(header, QStringLiteral("□"));
	auto close = Button(header, {}, "close");
	minimize->setObjectName("studioMinimize");
	m_windowMaximize->setObjectName("studioMaximize");
	close->setObjectName("studioClose");
	for (auto button : {minimize, m_windowMaximize, close})
	{
		button->setProperty("role", "windowControl");
		button->setFixedWidth(RecorderUi::Px(25));
		row->addWidget(button);
	}
	minimize->setAccessibleName(Ui("最小化"));
	m_windowMaximize->setAccessibleName(Ui("最大化 / 还原"));
	close->setAccessibleName(Ui("关闭"));
	connect(minimize, &QPushButton::clicked, this, &QWidget::showMinimized);
	connect(m_windowMaximize, &QPushButton::clicked, this, [this] { isMaximized() ? showNormal() : showMaximized(); });
	connect(close, &QPushButton::clicked, this, &KRRMainWindow::requestUserClose);
	m_submission = Button(this, Ui("提交档"));
	m_submission->setObjectName("openSubmission");
	m_submission->hide();
	m_transcribe = Button(this, Ui("字幕与配音"));
	m_transcribe->setObjectName("openSubtitleWorkflow");
	m_transcribe->hide();

	auto open = file->addAction(Ui("打开本地视频"));
	open->setShortcut(QKeySequence::Open);
	connect(open, &QAction::triggered, this, [this]
	{
		const auto local = QFileDialog::getOpenFileName(this, Ui("打开视频"), m_directory->text(),
			Ui("视频 (*.mp4 *.mov *.mkv *.webm *.avi *.flv)"));
		if (!local.isEmpty()) openPreview(local);
	});
	auto project = file->addAction(Ui("打开剪辑工程…"));
	project->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O));
	connect(project, &QAction::triggered, this, [this] { openEditingProject(); });
	auto edit = file->addAction(Ui("剪辑当前视频"));
	connect(edit, &QAction::triggered, this, [this] { editFile(m_preview); });
	connect(file->addAction(Ui("打开录制目录")), &QAction::triggered, this, [this]
	{
		if (validateStorage()) QDesktopServices::openUrl(QUrl::fromLocalFile(m_directory->text()));
	});
	connect(file->addAction(Ui("导出成片 / 格式转换")), &QAction::triggered, m_otherExport, &QPushButton::click);
	connect(file->addAction(Ui("提交档")), &QAction::triggered, m_submission, &QPushButton::click);
	connect(file->addAction(Ui("保存剪辑工程")), &QAction::triggered, this, [this]
	{
		if (m_clipEditor && !m_clipEditor->inputFile().isEmpty()) m_clipEditor->saveCurrentProject();
	});
	connect(m_submission, &QPushButton::clicked, this, &KRRMainWindow::showSubmission);
	connect(m_transcribe, &QPushButton::clicked, this, &KRRMainWindow::transcribeVideo);
	connect(m_otherExport, &QPushButton::clicked, this, [this]
	{
		if (m_workspace->currentIndex() == Edit && !m_clipEditor->inputFile().isEmpty())
			m_clipEditor->exportCurrentProject();
		else
		{
			m_exportDialog->show();
			m_exportDialog->raise();
		}
	});
	connect(tools->addAction(Ui("讲解工具")), &QAction::triggered, this, &KRRMainWindow::showAnnotation);
	connect(tools->addAction(Ui("恢复素材")), &QAction::triggered, this, &KRRMainWindow::showRecoveries);
	connect(tools->addAction(Ui("字幕与配音")), &QAction::triggered, m_transcribe, &QPushButton::click);
	connect(tools->addAction(Ui("导入场景预设")), &QAction::triggered, this, [this] { scenePreset(false); });
	connect(tools->addAction(Ui("导出场景预设")), &QAction::triggered, this, [this] { scenePreset(true); });
	connect(tools->addAction(Ui("清空讲解效果")), &QAction::triggered, this, [this]
	{
		m_overlay->dismiss();
		m_effects = {};
		sendScene();
	});
	connect(tools->addAction(Ui("预约录制")), &QAction::triggered, this, &KRRMainWindow::scheduleTask);
	connect(help->addAction(Ui("全局快捷键")), &QAction::triggered, this, &KRRMainWindow::configureHotkeys);
	connect(help->addAction(Ui("模型服务")), &QAction::triggered, this, [this] { ConfigureSpeechServices(this, "assistant"); });
	connect(help->addAction(Ui("软件说明")), &QAction::triggered, this, [this]
	{
		QMessageBox::information(this, Ui("软件说明"),
			Ui("录制工作台显示当前来源。开始录制后可暂停、打点和停止保存。\n回看与管理可按打点分节跳转。剪辑工坊在播放头处分割，随后拖动或处理片段。\n助手可检索录像、诊断录制与发起智能剪辑，模型可在设置中管理。"));
	});
	connect(schedule, &QPushButton::clicked, this, &KRRMainWindow::scheduleTask);
	connect(settingsMenu->addAction(Ui("设置…")), &QAction::triggered, this, [this] { showSettings(); });
	connect(settingsMenu->addAction(Ui("模型服务")), &QAction::triggered, this, [this] { ConfigureSpeechServices(this, "assistant"); });
	connect(settingsMenu->addAction(Ui("会话管理")), &QAction::triggered, this, &KRRMainWindow::showConversationSettings);
	connect(m_assistantToggle, &QPushButton::clicked, this, &KRRMainWindow::showRecordingAssistant);
	root->addWidget(header);
}

void KRRMainWindow::buildStudio(QVBoxLayout* root, QWidget* live, QWidget* review, QWidget* controls)
{
	m_workspace = new QStackedWidget(this);
	m_workspace->setObjectName("studioWorkspaces");
	m_studioBody = new QSplitter(Qt::Horizontal, this);
	m_studioBody->setObjectName("studioBody");
	m_studioBody->setChildrenCollapsible(false);
	m_studioBody->addWidget(m_workspace);
	m_studioBody->setStretchFactor(0, 1);
	root->addWidget(m_studioBody, 1);

	auto record = m_recordLayout = new QSplitter(Qt::Vertical, m_workspace);
	record->setObjectName("studioRecordSplitter");
	record->setChildrenCollapsible(false);
	m_workspace->addWidget(record);
	record->addWidget(live);
	auto preview = qobject_cast<QVBoxLayout*>(live->layout());
	auto monitorHeader = new QHBoxLayout;
	auto monitorTitle = new QLabel(Ui("实时监看"), live);
	monitorTitle->setProperty("heading", true);
	monitorHeader->addWidget(monitorTitle);
	monitorHeader->addWidget(m_liveStatus, 1);
	monitorHeader->addWidget(m_assistantToggle);
	monitorHeader->addWidget(m_lectureButton);
	preview->addLayout(monitorHeader);
	m_liveViewport = new QScrollArea(live);
	m_liveViewport->setObjectName("studioLiveViewport");
	m_liveViewport->setAlignment(Qt::AlignCenter);
	m_liveViewport->setWidgetResizable(true);
	m_liveViewport->setWidget(m_liveFrame);
	preview->addWidget(m_liveViewport, 1);

	auto zoom = new QHBoxLayout;
	auto out = Button(live, "−");
	auto in = Button(live, "+");
	auto fit = Button(live, Ui("适应窗口"));
	fit->setObjectName("studioZoomFit");
	in->setObjectName("studioZoomIn");
	out->setToolTip(Ui("缩小预览"));
	in->setToolTip(Ui("放大预览"));
	m_zoomLabel = new QLabel(live);
	for (auto w : QList<QWidget*>{out, m_zoomLabel, in, fit}) zoom->addWidget(w);

	auto changeZoom = [this](int delta)
	{
		if (!m_liveZoom)
		{
			const int width = m_liveFrame->frameSize().width();
			m_liveZoom = width > 0 ? qRound(100. * m_liveFrame->imageRect().width() / width) : 100;
		}
		m_liveZoom = qBound(10, m_liveZoom + delta, 400);
		refreshLiveZoom();
	};
	connect(out, &QPushButton::clicked, this, [changeZoom] { changeZoom(-10); });
	connect(in, &QPushButton::clicked, this, [changeZoom] { changeZoom(10); });
	connect(fit, &QPushButton::clicked, this, [this] { m_liveZoom = 0; refreshLiveZoom(); });
	refreshLiveZoom();

	auto sourceBar = new QHBoxLayout;
	sourceBar->addWidget(m_mode);
	m_quickTarget = new QComboBox(live);
	m_quickTarget->setObjectName("studioQuickSource");
	m_quickTarget->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	m_quickTarget->setMinimumContentsLength(12);
	sourceBar->addWidget(m_quickTarget);
	m_sourceProperties = Button(live, Ui("设置"), "settings");
	m_sourceProperties->setObjectName("studioSourceProperties");
	m_sourceProperties->setToolTip(Ui("当前画面源的属性"));
	sourceBar->addWidget(m_sourceProperties);
	sourceBar->addStretch();
	sourceBar->addLayout(zoom);
	preview->addLayout(sourceBar);
	connect(m_sourceProperties, &QPushButton::clicked, this, [this] { chooseSource(false); });
	connect(m_quickTarget, qOverload<int>(&QComboBox::activated), this, [this](int index)
	{
		const auto type = m_source->currentData().toString();
		if (type == "region" || type == "image") { chooseSource(false); return; }
		auto target = type == "window" ? m_window : type == "camera" ? m_camera : m_monitor;
		target->setCurrentIndex(index);
		sendScene();
		saveSettings();
	});
	for (auto model : {m_source, m_window, m_monitor, m_camera})
		connect(model, qOverload<int>(&QComboBox::currentIndexChanged), this, &KRRMainWindow::syncQuickSources);

	auto dock = m_recordDocks = new QSplitter(Qt::Horizontal, record);
	dock->setObjectName("studioRecordingDock");
	dock->setChildrenCollapsible(false);
	dock->setHandleWidth(4);
	record->addWidget(dock);
	record->setStretchFactor(0, 1);

	auto scenes = new QWidget(dock);
	auto scenesLayout = new QVBoxLayout(scenes);
	scenesLayout->setContentsMargins(2, 2, 2, 2);
	scenesLayout->setSpacing(2);
	m_scenes = new QListWidget(scenes);
	m_scenes->setObjectName("studioScenes");
	m_scenes->setEditTriggers(QAbstractItemView::NoEditTriggers);
	m_scenes->setMinimumHeight(50);
	scenesLayout->addWidget(m_scenes, 1);
	auto sceneActions = new QHBoxLayout;
	sceneActions->setSpacing(2);
	auto addScene = Button(scenes, "+");
	addScene->setObjectName("studioAddScene");
	addScene->setToolTip(Ui("新建场景"));
	auto renameScene = Button(scenes, {}, "pen");
	renameScene->setObjectName("studioRenameScene");
	renameScene->setToolTip(Ui("重命名场景"));
	auto deleteScene = Button(scenes, {}, "trash");
	deleteScene->setToolTip(Ui("删除选中的场景预设"));
	for (auto button : {addScene, renameScene, deleteScene}) sceneActions->addWidget(button);
	sceneActions->addStretch();
	scenesLayout->addLayout(sceneActions);
	dock->addWidget(new StudioDock(dock, "studioSceneDock", Ui("场景"), scenes));
	connect(addScene, &QPushButton::clicked, this, [this]
	{
		rememberSource();
		auto data = configuration().toJson();
		data.remove("output");
		m_scenePresets.append(QJsonObject{{"name", Ui("场景 %1").arg(m_scenePresets.size() + 1)}, {"config", data},
			{"sources", m_visualSources}, {"sourceIndex", m_sourceIndex}, {"format", m_format->currentText()},
			{"automaticResolution", m_resolution->currentIndex() == 0}, {"recommendedBitrate", m_recommendedBitrate->isChecked()}});
		m_sceneIndex = m_scenePresets.size() - 1;
		refreshSceneList();
		m_scenes->setCurrentRow(m_sceneIndex);
		m_scenes->editItem(m_scenes->currentItem());
		saveSettings();
	});
	connect(renameScene, &QPushButton::clicked, this, [this]
	{
		if (m_scenes->currentItem()) m_scenes->editItem(m_scenes->currentItem());
	});
	connect(m_scenes, &QListWidget::itemChanged, this, [this](QListWidgetItem* item)
	{
		const int row = m_scenes->row(item);
		if (row < 0 || row >= m_scenePresets.size()) return;
		auto scene = m_scenePresets[row].toObject();
		const auto label = item->text().trimmed().left(80);
		if (!label.isEmpty()) scene["name"] = label;
		const QSignalBlocker blocker(m_scenes);
		item->setText(scene["name"].toString());
		m_scenePresets.replace(row, scene);
		saveSceneList();
	});
	connect(deleteScene, &QPushButton::clicked, this, [this]
	{
		const int row = m_scenes->currentRow();
		if (row < 0 || row >= m_scenePresets.size()) return;
		m_scenePresets.removeAt(row);
		if (m_sceneIndex >= row) --m_sceneIndex;
		refreshSceneList();
		saveSettings();
	});
	connect(m_scenes, &QListWidget::itemClicked, this, [this](QListWidgetItem* item)
	{
		const int row = m_scenes->row(item);
		if (row < 0 || row == m_sceneIndex) return;
		rememberSource();
		const auto preset = m_scenePresets.at(row).toObject();
		auto scene = preset["config"].toObject();
		scene["output"] = configuration().output;
		const auto config = KRRRecordingConfig::FromJson(scene);
		QString error;
		if (!config.validate(error)) { m_message->setText(UiDiagnostic(error)); return; }
		{
			const QScopedValueRollback<bool> loading(m_updatingSources, true);
			m_sceneIndex = row;
			m_visualSources = preset["sources"].toArray();
			m_sourceIndex = preset["sourceIndex"].toInt(-1);
			applySceneConfiguration(config);
			if (!m_active)
			{
				if (preset["automaticResolution"].toBool()) m_resolution->setCurrentIndex(0);
				m_recommendedBitrate->setChecked(preset["recommendedBitrate"].toBool(m_recommendedBitrate->isChecked()));
				if (!preset["format"].toString().isEmpty()) m_format->setCurrentText(preset["format"].toString());
			}
		}
		saveSettings();
	});

	auto sources = new QWidget(dock);
	auto sourcesLayout = new QVBoxLayout(sources);
	sourcesLayout->setContentsMargins(2, 2, 2, 2);
	sourcesLayout->setSpacing(2);
	m_sources = new QListWidget(sources);
	m_sources->setObjectName("studioSources");
	m_sources->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	sourcesLayout->addWidget(m_sources, 1);
	auto sourceActions = new QHBoxLayout;
	sourceActions->setSpacing(2);
	auto addSource = Button(sources, "+");
	addSource->setObjectName("studioAddSource");
	addSource->setToolTip(Ui("添加画面来源"));
	auto deleteSource = Button(sources, {}, "trash");
	deleteSource->setObjectName("studioDeleteSource");
	deleteSource->setToolTip(Ui("删除画面来源"));
	auto refresh = Button(sources, {}, "refresh");
	refresh->setToolTip(Ui("刷新来源列表"));
	for (auto button : {addSource, deleteSource, refresh}) sourceActions->addWidget(button);
	sourceActions->addStretch();
	sourcesLayout->addLayout(sourceActions);
	dock->addWidget(new StudioDock(dock, "studioSourceDock", Ui("源"), sources));
	connect(addSource, &QPushButton::clicked, this, [this] { chooseSource(true); });
	connect(refresh, &QPushButton::clicked, this, [this] { refreshMonitors(); refreshWindows(); syncQuickSources(); });
	connect(m_sources, &QListWidget::itemClicked, this, [this](QListWidgetItem* item)
	{
		const int index = item->data(Qt::UserRole).toInt();
		if (index >= 0) activateSource(index);
	});
	connect(m_sources, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item)
	{
		if (item->data(Qt::UserRole).toInt() < 0) showSettings(3);
		else chooseSource(false);
	});
	connect(deleteSource, &QPushButton::clicked, this, [this]
	{
		if (!m_sources->currentItem()) return;
		const int index = m_sources->currentItem()->data(Qt::UserRole).toInt();
		if (index == -1) { m_cameraEnabled->setChecked(false); return; }
		if (m_visualSources.size() <= 1) { m_message->setText(Ui("请先添加一个替代画面源。")); return; }
		if (index < 0 || index >= m_visualSources.size()) return;
		m_visualSources.removeAt(index);
		if (index == m_sourceIndex)
		{
			m_sourceIndex = -1;
			activateSource(qMin(index, m_visualSources.size() - 1));
		}
		else if (index < m_sourceIndex) --m_sourceIndex;
		saveSettings();
	});

	auto mixer = new QWidget(dock);
	auto mixerLayout = new QVBoxLayout(mixer);
	mixerLayout->setContentsMargins(5, 3, 5, 3);
	mixerLayout->setSpacing(4);
	auto mixerOptions = new QWidget(mixer);
	auto mixerOptionsRow = new QHBoxLayout(mixerOptions);
	mixerOptionsRow->setContentsMargins(0, 0, 0, 0);
	mixerOptionsRow->setSpacing(2);
	auto audioMode = LinkedCheck(mixer, m_separateAudio);
	audioMode->setText(Ui("分轨"));
	mixerOptionsRow->addWidget(audioMode);
	auto addAudio = Button(mixer, "+");
	addAudio->setObjectName("studioAddAudio");
	addAudio->setToolTip(Ui("添加音频来源"));
	mixerOptionsRow->addWidget(addAudio);
	auto systemToggle = RecorderUi::Toggle("volume", Ui("系统播放声"), mixer);
	systemToggle->setObjectName("studioSystemMute");
	auto denoise = LinkedCheck(mixer, m_denoise);
	denoise->setText(Ui("降噪"));
	denoise->setObjectName("studioDenoise");
	auto gainRow = [this, mixer, mixerLayout](const QString& title, QWidget* toggle, QWidget* option, QSlider*& gain, KRRPeakMeter*& meter)
	{
		auto row = new QHBoxLayout;
		row->setSpacing(4);
		row->addWidget(toggle);
		auto label = new QLabel(title, mixer);
		label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
		row->addWidget(label, 1);
		if (option) row->addWidget(option);
		mixerLayout->addLayout(row);
		meter = new KRRPeakMeter(mixer);
		mixerLayout->addWidget(meter);
		auto volume = new QHBoxLayout;
		volume->setSpacing(4);
		gain = new QSlider(Qt::Horizontal, mixer);
		gain->setAccessibleName(title);
		gain->setRange(0, 200);
		gain->setValue(100);
		auto percent = new QLabel("100%", mixer);
		volume->addWidget(gain, 1);
		volume->addWidget(percent);
		mixerLayout->addLayout(volume);
		connect(gain, &QSlider::valueChanged, this, [this, percent](int value)
		{
			percent->setText(QString::number(value) + "%");
			m_effectTimer.start(40);
		});
	};
	gainRow(Ui("麦克风"), m_micToggle, denoise, m_micGain, m_micMeter);
	gainRow(Ui("系统播放声"), systemToggle, nullptr, m_systemGain, m_systemMeter);
	m_extraAudioLayout = new QVBoxLayout;
	m_extraAudioLayout->setSpacing(4);
	mixerLayout->addLayout(m_extraAudioLayout);
	mixerLayout->addStretch(1);
	auto updateAudio = [this, systemToggle]
	{
		const auto mode = m_audio->currentData().toString();
		systemToggle->setChecked(mode == "system" || mode == "mix");
		systemToggle->setEnabled(!m_countdownRemaining);
	};
	connect(m_audio, qOverload<int>(&QComboBox::currentIndexChanged), mixer, updateAudio);
	connect(&m_pollTimer, &QTimer::timeout, mixer, updateAudio);
	connect(systemToggle, &QPushButton::clicked, this, [this](bool on)
	{
		const auto mic = m_micToggle->isChecked();
		m_audio->setCurrentIndex(m_audio->findData(on ? (mic ? "mix" : "system") : (mic ? "mic" : "none")));
		sendScene();
		saveSettings();
	});
	connect(addAudio, &QPushButton::clicked, this, &KRRMainWindow::addAudioSource);
	auto mixerScroll = new QScrollArea(dock);
	mixerScroll->setWidgetResizable(true);
	mixerScroll->setFrameShape(QFrame::NoFrame);
	mixerScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	mixerScroll->setWidget(mixer);
	dock->addWidget(new StudioDock(dock, "studioMixerDock", Ui("混音器"), mixerScroll, mixerOptions));

	auto outputScroll = new QScrollArea(dock);
	outputScroll->setWidgetResizable(true);
	outputScroll->setFrameShape(QFrame::NoFrame);
	outputScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	auto output = new QWidget(outputScroll);
	auto form = DockForm(output);
	form->setVerticalSpacing(3);
	auto resolution = LinkedCombo(output, m_resolution);
	auto fps = LinkedCombo(output, m_fps);
	auto encoder = LinkedCombo(output, m_encoder);
	auto color = LinkedCombo(output, m_outputColor);
	auto format = LinkedCombo(output, m_format);
	auto bitrate = new QSpinBox(output);
	bitrate->setObjectName("studioBitrate");
	bitrate->setRange(m_bitrate->minimum(), m_bitrate->maximum());
	bitrate->setSingleStep(m_bitrate->singleStep());
	bitrate->setKeyboardTracking(false);
	auto defaults = Button(output, Ui("默认"));
	defaults->setObjectName("studioDefaultBitrate");
	auto bitrateRow = new QHBoxLayout;
	bitrateRow->setSpacing(4);
	bitrateRow->addWidget(bitrate, 1);
	bitrateRow->addWidget(new QLabel("kbps", output));
	bitrateRow->addWidget(defaults);
	connect(bitrate, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value)
	{
		m_recommendedBitrate->setChecked(false);
		m_bitrate->setValue(value);
	});
	connect(m_bitrate, qOverload<int>(&QSpinBox::valueChanged), bitrate, [bitrate](int value)
	{
		const QSignalBlocker guard(bitrate);
		bitrate->setValue(value);
	});
	connect(defaults, &QPushButton::clicked, this, [this]
	{
		m_recommendedBitrate->setChecked(true);
		updateBitrateRecommendation();
		saveSettings();
	});
	auto directory = new QLineEdit(m_directory->text(), output);
	directory->setObjectName("studioPath");
	auto browse = Button(output, Ui("浏览…"));
	auto pathRow = new QHBoxLayout;
	pathRow->setSpacing(3);
	pathRow->addWidget(directory, 1);
	pathRow->addWidget(browse);
	connect(browse, &QPushButton::clicked, this, &KRRMainWindow::browseRecordingFolder);
	connect(m_directory, &QLineEdit::textChanged, directory, &QLineEdit::setText);
	connect(directory, &QLineEdit::editingFinished, this, [this, directory]
	{
		if (m_directory->text() == directory->text()) return;
		m_defaultDirectory = false;
		m_directory->setText(directory->text());
		if (validateStorage()) saveSettings();
	});
	connect(&m_pollTimer, &QTimer::timeout, output, [this, controls = QList<QWidget*>{resolution, fps, encoder, color,
		format, bitrate, defaults, directory, browse}, audioMode, denoise, addAudio]
	{
		const bool editable = !m_active && !m_countdownRemaining;
		for (auto widget : controls) widget->setEnabled(editable);
		for (auto widget : QList<QWidget*>{audioMode, denoise, addAudio}) widget->setEnabled(editable);
	});
	form->addRow(Ui("分辨率"), resolution);
	form->addRow(Ui("帧率"), fps);
	form->addRow(Ui("码率"), bitrateRow);
	form->addRow(Ui("编码器"), encoder);
	form->addRow(Ui("色彩"), color);
	form->addRow(Ui("格式"), format);
	form->addRow(Ui("路径"), pathRow);
	outputScroll->setWidget(output);
	dock->addWidget(new StudioDock(dock, "studioOutputDock", Ui("录制设置"), outputScroll));
	auto scheduled = Button(controls, {}, "clock");
	scheduled->setToolTip(Ui("预约录制"));
	connect(scheduled, &QPushButton::clicked, this, &KRRMainWindow::scheduleTask);
	dock->addWidget(new StudioDock(dock, "studioControlsDock", Ui("录制控制"), controls, scheduled));
	dock->setSizes({185, 210, 180, 300, 215});
	record->setSizes({570, RecorderUi::Px(223)});

	auto reviewPage = new QWidget(m_workspace);
	auto reviewLayout = new QVBoxLayout(reviewPage);
	reviewLayout->setContentsMargins(0, 0, 0, 0);
	auto reviewHeader = new QHBoxLayout;
	auto library = Button(reviewPage, Ui("素材库"), "screen");
	library->setObjectName("studioToggleLibrary");
	library->setCheckable(true);
	library->setChecked(true);
	reviewHeader->addWidget(library);
	auto open = Button(reviewPage, Ui("打开本地"));
	reviewHeader->addWidget(open);
	reviewHeader->addStretch();
	m_reviewAssistantToggle = Button(reviewPage, {}, "assistant");
	m_reviewAssistantToggle->setCheckable(true);
	m_reviewAssistantToggle->setToolTip(Ui("录屏助手"));
	reviewHeader->addWidget(m_reviewAssistantToggle);
	connect(m_reviewAssistantToggle, &QPushButton::clicked, this, &KRRMainWindow::showRecordingAssistant);
	reviewLayout->addLayout(reviewHeader);
	m_reviewSplitter = new QSplitter(Qt::Horizontal, reviewPage);
	m_reviewSplitter->setChildrenCollapsible(false);
	reviewLayout->addWidget(m_reviewSplitter, 1);
	m_workspace->addWidget(reviewPage);
	m_libraryPane = new QWidget(m_reviewSplitter);
	auto libLayout = new QVBoxLayout(m_libraryPane);
	libLayout->setContentsMargins(0, 0, 0, 0);
	libLayout->setSpacing(4);
	m_librarySearch = new QLineEdit(m_libraryPane);
	m_librarySearch->setPlaceholderText(Ui("搜索视频名称"));
	libLayout->addWidget(m_librarySearch);
	m_libraryFilter = new QComboBox(m_libraryPane);
	m_libraryFilter->addItems({Ui("全部"), Ui("正式"), Ui("可恢复")});
	libLayout->addWidget(m_libraryFilter);
	m_recent->setIconSize(QSize(70, 42));
	m_recent->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	m_recent->setWordWrap(true);
	libLayout->addWidget(m_recent, 1);
	m_recoveryLibrary = new QListWidget(m_libraryPane);
	libLayout->addWidget(m_recoveryLibrary, 1);
	m_recoveryLibrary->hide();
	m_reviewSplitter->addWidget(m_libraryPane);
	m_reviewSplitter->addWidget(review);
	auto details = new QWidget(m_reviewSplitter);
	auto detailsLayout = new QVBoxLayout(details);
	detailsLayout->setContentsMargins(4, 4, 4, 4);
	auto chapters = new QLabel(Ui("打点分节"), details);
	chapters->setProperty("heading", true);
	auto chapterHeader = new QHBoxLayout;
	chapterHeader->addWidget(chapters, 1);
	auto addChapter = Button(details, "+");
	addChapter->setObjectName("studioAddReviewMarker");
	addChapter->setToolTip(Ui("在当前位置打点"));
	chapterHeader->addWidget(addChapter);
	detailsLayout->addLayout(chapterHeader);
	connect(addChapter, &QPushButton::clicked, this, &KRRMainWindow::addPlaybackMarker);
	m_chapters = new QListWidget(details);
	m_chapters->setObjectName("studioReviewChapters");
	m_chapters->setWordWrap(true);
	m_chapters->setItemDelegate(new ChapterDelegate(m_chapters));
	m_chapters->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	detailsLayout->addWidget(m_chapters, 1);
	m_renameMarker->hide();
	connect(m_chapters, &QListWidget::itemChanged, this, &KRRMainWindow::savePlaybackMarker);
	connect(m_chapters, &QListWidget::itemDoubleClicked, m_chapters, &QListWidget::editItem);
	m_reviewFormat = new QLabel(details);
	m_reviewFormat->setWordWrap(true);
	m_reviewFormat->setTextInteractionFlags(Qt::TextSelectableByMouse);
	detailsLayout->addWidget(m_reviewFormat);
	details->setMinimumWidth(180);
	m_reviewSplitter->addWidget(details);
	m_reviewSplitter->setStretchFactor(1, 1);
	m_reviewSplitter->setSizes({230, 620, 200});

	connect(library, &QPushButton::toggled, m_libraryPane, &QWidget::setVisible);
	connect(open, &QPushButton::clicked, this, [this]
	{
		const auto path = QFileDialog::getOpenFileName(this, Ui("打开视频"), m_directory->text(),
			Ui("视频 (*.mp4 *.mov *.mkv *.webm *.avi *.flv)"));
		if (!path.isEmpty()) openPreview(path);
	});
	connect(m_libraryFilter, qOverload<int>(&QComboBox::currentIndexChanged), this, &KRRMainWindow::updateReviewLibrary);
	connect(m_librarySearch, &QLineEdit::textChanged, this, &KRRMainWindow::updateReviewLibrary);
	connect(m_recoveryLibrary, &QListWidget::itemDoubleClicked, this, &KRRMainWindow::showRecoveries);
	connect(m_chapters, &QListWidget::itemClicked, this, [this](QListWidgetItem* item)
	{
		m_playbackMarkers->setCurrentIndex(item->data(Qt::UserRole + 1).toInt());
		if (m_player->duration() > 0)
			m_player->setPosition(qMin(m_player->duration() - 1, item->data(Qt::UserRole).toLongLong()));
	});

	m_editPane = new QWidget(m_workspace);
	auto editLayout = new QVBoxLayout(m_editPane);
	editLayout->setContentsMargins(0, 0, 0, 0);
	editLayout->setSpacing(2);
	auto editTools = new QHBoxLayout;
	editTools->addStretch();
	m_editAssistantToggle = Button(m_editPane, {}, "assistant");
	m_editAssistantToggle->setObjectName("openEditingAssistant");
	m_editAssistantToggle->setCheckable(true);
	m_editAssistantToggle->setToolTip(Ui("剪辑助手"));
	editTools->addWidget(m_editAssistantToggle);
	editLayout->addLayout(editTools);
	connect(m_editAssistantToggle, &QPushButton::clicked, this, &KRRMainWindow::showRecordingAssistant);
	m_workspace->addWidget(m_editPane);
	m_assistantPane = new QWidget(m_workspace);
	m_workspace->addWidget(m_assistantPane);
	m_workspaceButtons->button(Record)->setChecked(true);
	m_studioStatus = new QLabel(this);
	m_studioStatus->setObjectName("studioStatus");
	statusBar()->addWidget(m_message, 1);
	statusBar()->removeWidget(m_metrics);
	statusBar()->addPermanentWidget(m_studioStatus);
	statusBar()->addPermanentWidget(m_metrics);
	m_metrics->show();
	m_metrics->setObjectName("studioMetrics");
	connect(&m_meterTimer, &QTimer::timeout, this, [this]
	{
		if (m_workspace->currentIndex() != Record || !isVisible() || isMinimized()) return;
		if (!m_active) updateMeters();
		else if (m_levelSocket && m_levelSocket->state() == QLocalSocket::UnconnectedState)
		{
			m_levelDeadline.start(170);
			m_levelSocket->connectToServer(ControlServerName());
		}
	});
	m_meterTimer.start(65);
}

void KRRMainWindow::selectWorkspace(Workspace workspace)
{
	if (!m_studioReady) return;
	if (workspace != Review && workspace != m_workspace->currentIndex()) m_player->pause();
	if (workspace != Edit && m_workspace->currentIndex() == Edit) m_clipEditor->pausePlayback();
	m_workspace->setCurrentIndex(workspace);
	m_workspaceButtons->button(workspace)->setChecked(true);
	if (workspace == Record)
	{
		syncQuickSources();
		if (!m_active) m_idleConfigClock.invalidate();
	}
	else releaseIdlePreview();
	if (workspace == Review) updateReviewLibrary();
	selectStudioSidebar();
	if (workspace == Assistant && m_studioAssistant) m_studioAssistant->enter();
	m_otherExport->setEnabled(workspace == Edit ? !m_clipEditor->inputFile().isEmpty() : QFileInfo(m_preview).isFile());
}

void KRRMainWindow::openEditingProject(const QString& requested)
{
	const auto path = requested.isEmpty() ? PickStudioProject(this, m_directory->text(), m_clipEditor->projectPath()) : requested;
	if (!path.isEmpty() && m_clipEditor->openProjectFile(path, m_directory->text())) selectWorkspace(Edit);
}

void KRRMainWindow::showConversationSettings()
{
	ManageAssistantArchive(this, m_conversationHistory);
}

void KRRMainWindow::buildStudioAssistants()
{
	auto snapshot = [this] { return assistantContext(); };
	auto apply = [this](const QJsonObject& before, const QJsonObject& changes, QString& error)
	{ return applyAssistantSettings(before, changes, error); };
	using Presentation = KRRRecordingAssistantDialog::Presentation;
	m_conversationHistory = new KRRAssistantHistory(this);
	connect(m_conversationHistory, &KRRAssistantHistory::saveFailed, m_message, &QLabel::setText);
	auto sidebar = [&](const QString& name)
	{
		auto chat = new KRRRecordingAssistantDialog(snapshot, apply, m_studioBody, Presentation::Sidebar);
		chat->setObjectName(name);
		chat->setMinimumWidth(RecorderUi::Px(230));
		m_studioBody->addWidget(chat);
		chat->hide();
		return chat;
	};
	m_sidebarAssistant = sidebar("studioCopilot");
	m_editSidebarAssistant = sidebar("studioEditingCopilot");
	m_sidebarAssistant->dismissSidebar = [this] { m_sidebarVisible = false; selectStudioSidebar(); };
	m_editSidebarAssistant->dismissSidebar = [this] { m_editSidebarVisible = false; selectStudioSidebar(); };
	m_editSidebarAssistant->chooseProject = [this] { openEditingProject(); };
	m_studioBody->setSizes({900, 300, 300});
	m_studioBody->setStretchFactor(1, 0);
	m_studioBody->setStretchFactor(2, 0);
	connect(m_studioBody, &QSplitter::splitterMoved, this, [this]
	{
		const auto sizes = m_studioBody->sizes();
		if (m_sidebarAssistant->isVisible()) m_recordSideWidth = sizes.value(1, 300);
		if (m_editSidebarAssistant->isVisible()) m_editSideWidth = sizes.value(2, 300);
	});
	connect(m_clipEditor, &KRRClipEditor::sourceChanged, this, [this](const QString& file)
	{
		if (m_workspace->currentIndex() != Edit) return;
		selectStudioSidebar();
		m_otherExport->setEnabled(!file.isEmpty());
	});
	connect(m_clipEditor, &KRRClipEditor::studioCutApplied, this, [this] { selectWorkspace(Edit); });
	m_workspaceAssistant = new KRRRecordingAssistantDialog(snapshot, apply, m_assistantPane, Presentation::Workspace);
	m_workspaceAssistant->setObjectName("studioAiWorkspace");
	m_studioAssistant = new KRRStudioAssistant(m_conversationHistory, m_workspaceAssistant, m_clipEditor,
		[this] { return m_directory->text(); }, m_assistantPane);
	auto ai = new QVBoxLayout(m_assistantPane);
	ai->setContentsMargins(0, 0, 0, 0);
	ai->addWidget(m_studioAssistant);
	connect(m_studioAssistant, &KRRStudioAssistant::projectRequested, this, [this](const QString& path) { openEditingProject(path); });
	connect(m_studioAssistant, &KRRStudioAssistant::videoRequested, this, &KRRMainWindow::editFile);
	for (auto assistant : {m_sidebarAssistant, m_editSidebarAssistant, m_workspaceAssistant})
		connect(assistant, &QDialog::finished, this, [this, assistant](int result)
		{
			if (result == QDialog::Accepted) handleAssistantResult(assistant);
		});
}

void KRRMainWindow::selectStudioSidebar()
{
	if (!m_sidebarAssistant) return;
	const auto page = m_workspace->currentIndex();
	const bool recordVisible = m_sidebarVisible && (page == Record || page == Review);
	const bool editVisible = m_editSidebarVisible && page == Edit;
	if (recordVisible && m_sideRecordThread.isEmpty())
	{
		m_sideRecordThread = m_conversationHistory->begin("side:record");
		m_sidebarAssistant->bindConversation(m_conversationHistory, m_sideRecordThread);
	}
	if (page == Edit)
	{
		const auto input = m_clipEditor->inputFile();
		const auto project = input.isEmpty() ? QString{} : m_clipEditor->projectPath().isEmpty() ? input : m_clipEditor->projectPath();
		if (project != m_sideEditProject)
		{
			m_sideEditProject = project;
			m_sideEditThread.clear();
			m_editSidebarAssistant->bindConversation(m_conversationHistory, {});
		}
		m_editSidebarAssistant->setEditingContext(input, true);
		if (editVisible && !project.isEmpty() && m_sideEditThread.isEmpty())
		{
			m_sideEditThread = m_conversationHistory->begin("side:edit:" + project);
			m_editSidebarAssistant->bindConversation(m_conversationHistory, m_sideEditThread);
		}
	}
	const bool visibilityChanged = !m_sidebarAssistant->isHidden() != recordVisible || !m_editSidebarAssistant->isHidden() != editVisible;
	m_sidebarAssistant->setVisible(recordVisible);
	m_editSidebarAssistant->setVisible(editVisible);
	if (visibilityChanged)
	{
		const auto width = m_studioBody->width();
		const auto side = recordVisible ? m_recordSideWidth : editVisible ? m_editSideWidth : 0;
		m_studioBody->setSizes({qMax(1, width - side), recordVisible ? side : 0, editVisible ? side : 0});
	}
	m_assistantToggle->setChecked(m_sidebarVisible);
	m_reviewAssistantToggle->setChecked(m_sidebarVisible);
	m_editAssistantToggle->setChecked(m_editSidebarVisible);
}

void KRRMainWindow::showRecordingAssistant()
{
	const int page = m_workspace->currentIndex();
	if (page == Edit) m_editSidebarVisible = m_editAssistantToggle->isChecked();
	else if (page == Review) m_sidebarVisible = m_reviewAssistantToggle->isChecked();
	else if (page == Record) m_sidebarVisible = m_assistantToggle->isChecked();
	selectStudioSidebar();
}

void KRRMainWindow::handleAssistantResult(KRRRecordingAssistantDialog* assistant)
{
	const auto recording = std::exchange(assistant->selectedRecording, {});
	const auto montage = std::exchange(assistant->selectedMontage, {});
	const auto cut = std::exchange(assistant->selectedCut, std::nullopt);
	const auto time = std::exchange(assistant->selectedTime, -1);
	const bool live = std::exchange(assistant->requestedCheck, false);
	const bool review = std::exchange(assistant->openAsReview, false);
	if (live) { selectWorkspace(Record); return; }
	if (review)
	{
		openPreview(recording);
		if (time >= 0 && m_preview == recording)
		{
			if (m_player->duration() > 0) m_player->setPosition(qMin(m_player->duration() - 1, time));
			else m_reviewPendingSeek = time;
		}
		return;
	}
	if (cut && (m_active || m_countdownRemaining))
	{
		m_message->setText(Ui("录制进行中，结束后可生成剪辑草稿。"));
		return;
	}
	if (!montage.isEmpty() && cut)
	{
		selectWorkspace(Edit);
		m_clipEditor->openSequenceMontage(montage, m_directory->text(), cut->options.query);
		return;
	}
	if (recording.isEmpty()) return;
	if (cut && assistant == m_workspaceAssistant && m_studioAssistant->isEditing())
	{
		if (!m_clipEditor->openSmartCutProjectRequest(m_studioAssistant->linkedProject(), recording, m_directory->text(), time,
			cut->options, cut->chapters, m_studioAssistant->candidateHost()))
			m_message->setText(Ui("无法打开剪辑素材。"));
		return;
	}
	const bool loaded = cut ? m_clipEditor->openSmartCutRequest(recording, m_directory->text(), time,
		cut->options, cut->chapters) : m_clipEditor->openFile(recording, m_directory->text(), time);
	if (loaded) selectWorkspace(Edit);
}

void KRRMainWindow::syncQuickSources()
{
	if (!m_quickTarget) return;
	const auto type = m_source->currentData().toString();
	m_quickTarget->setEnabled(!m_active || type != "camera");
	if (type == "screen" || type == "window" || type == "camera")
	{
		CopyItems(m_quickTarget, type == "window" ? m_window : type == "camera" ? m_camera : m_monitor);
	}
	else
	{
		const QSignalBlocker block(m_quickTarget);
		m_quickTarget->clear();
		m_quickTarget->addItem(type == "image"
			? (m_image->text().isEmpty() ? Ui("选择本地图片") : QFileInfo(m_image->text()).fileName())
			: (m_region.isValid() ? m_regionLabel->text() : Ui("框选录制区域")));
	}
	m_quickTarget->setToolTip(m_quickTarget->currentText());
	if (auto label = findChild<QLabel*>("studioSourceSummary"))
		label->setText(m_mode->currentText() + "\n" + m_quickTarget->currentText());
}

bool KRRMainWindow::chooseSource(bool changeType)
{
	refreshMonitors();
	refreshWindows();
	QDialog dialog(this);
	dialog.setWindowFlag(Qt::WindowContextHelpButtonHint, false);
	dialog.setObjectName("studioSourceDialog");
	dialog.setWindowTitle(changeType ? Ui("添加画面来源") : Ui("当前画面源的属性"));
	auto root = new QVBoxLayout(&dialog);
	auto form = new QFormLayout;
	auto type = new QComboBox(&dialog);
	CopyItems(type, m_source);
	if (changeType) type->setCurrentIndex(m_mode->currentIndex());
	if (changeType) form->addRow(Ui("来源"), type);
	else
	{
		type->hide();
		form->addRow(Ui("来源"), new QLabel(m_mode->currentText(), &dialog));
	}
	auto monitor = new QComboBox(&dialog);
	auto window = new QComboBox(&dialog);
	auto camera = new QComboBox(&dialog);
	CopyItems(monitor, m_monitor);
	CopyItems(window, m_window);
	CopyItems(camera, m_camera);
	camera->setEnabled(!m_active);
	window->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	window->setMinimumContentsLength(22);
	auto region = m_region;
	auto select = Button(&dialog, Ui("框选录制区域"));
	auto area = new QLabel(m_region.isValid() ? m_regionLabel->text() : Ui("尚未框选"), &dialog);
	auto path = new QLineEdit(m_image->text(), &dialog);
	auto browse = Button(&dialog, Ui("选择本地图片"));
	auto cursor = new QCheckBox(Ui("显示鼠标指针"), &dialog);
	cursor->setChecked(m_cursor->isChecked());
	auto color = new QComboBox(&dialog);
	CopyItems(color, m_screenColor);
	form->addRow(Ui("显示器"), monitor);
	form->addRow(Ui("窗口"), window);
	form->addRow(Ui("设备"), camera);
	auto asPip = new QCheckBox(Ui("作为画中画添加"), &dialog);
	asPip->setChecked(changeType && m_source->currentData() != "camera");
	form->addRow(asPip);
	form->addRow(Ui("区域"), select);
	form->addRow(area);
	form->addRow(Ui("图片"), path);
	form->addRow(browse);
	form->addRow(Ui("色彩"), color);
	form->addRow(cursor);
	root->addLayout(form);
	auto update = [=]
	{
		const auto source = type->currentData().toString();
		for (const auto& row : QList<QPair<QWidget*, bool>>{{monitor, source == "screen"}, {window, source == "window"},
			{camera, source == "camera"}, {asPip, changeType && !m_active && source == "camera" && m_source->currentData() != "camera"}, {select, source == "region"}, {area, source == "region"},
			{path, source == "image"}, {browse, source == "image"}, {color, source == "screen" || source == "region"},
			{cursor, source == "screen" || source == "region" || source == "window"}})
		{
			row.first->setVisible(row.second);
			if (auto label = form->labelForField(row.first)) label->setVisible(row.second);
		}
	};
	connect(type, qOverload<int>(&QComboBox::currentIndexChanged), &dialog, update);
	update();
	connect(select, &QPushButton::clicked, &dialog, [this, &region, area, &dialog]
	{
		KRRRegionSelector selector(region, &dialog);
		if (selector.exec() == QDialog::Accepted)
		{
			region = selector.selectedRegion();
			area->setText(QString("%1, %2 · %3 × %4").arg(region.x()).arg(region.y()).arg(region.width()).arg(region.height()));
		}
	});
	connect(browse, &QPushButton::clicked, &dialog, [this, &dialog, path]
	{
		auto file = QFileDialog::getOpenFileName(&dialog, Ui("选择本地图片"), m_directory->text(),
			Ui("图片 (*.png *.jpg *.jpeg *.bmp *.webp)"));
		if (!file.isEmpty()) path->setText(file);
	});
	auto actions = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Cancel, &dialog);
	actions->button(QDialogButtonBox::Apply)->setText(Ui("应用到当前场景"));
	actions->button(QDialogButtonBox::Cancel)->setText(Ui("取消"));
	root->addWidget(actions);
	connect(actions, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	connect(actions->button(QDialogButtonBox::Apply), &QPushButton::clicked, &dialog, [=, &dialog, &region]
	{
		const auto source = type->currentData().toString();
		QString error;
		if (source == "screen" && monitor->currentIndex() < 0) error = Ui("请选择可用的显示器。");
		if (source == "window" && window->currentIndex() < 0) error = Ui("请选择可用的窗口。");
		if (source == "camera" && camera->currentData().toInt() < 0) error = Ui("请选择可用的摄像头。");
		if (source == "region" && !region.isValid()) error = Ui("请先框选录制区域。");
		if (source == "image" && !QFileInfo(path->text()).isFile()) error = Ui("请选择可用的图片文件。");
		if (!error.isEmpty()) { QMessageBox::information(&dialog, dialog.windowTitle(), error); return; }
		const QScopedValueRollback<bool> updating(m_updatingSources, true);
		if (source == "screen") m_monitor->setCurrentIndex(monitor->currentIndex());
		if (source == "window") m_window->setCurrentIndex(window->currentIndex());
		if (source == "camera" && !m_active) m_camera->setCurrentIndex(camera->currentIndex());
		if (source == "region") { m_region = region; m_regionLabel->setText(area->text()); }
		if (source == "image") m_image->setText(path->text());
		m_cursor->setChecked(cursor->isChecked());
		m_screenColor->setCurrentIndex(color->currentIndex());
		if (source == "camera" && asPip->isVisible() && asPip->isChecked())
		{
			m_cameraEnabled->setChecked(true);
			dialog.setProperty("pipOnly", true);
		}
		else m_source->setCurrentIndex(type->currentIndex());
		updateBitrateRecommendation();
		syncQuickSources();
		dialog.accept();
	});
	dialog.setMinimumWidth(410);
	if (dialog.exec() != QDialog::Accepted) return false;
	rememberSource(changeType && !dialog.property("pipOnly").toBool());
	sendScene();
	saveSettings();
	return true;
}

void KRRMainWindow::refreshLiveZoom()
{
	const auto size = m_liveFrame->frameSize();
	m_liveViewport->setWidgetResizable(m_liveZoom == 0);
	m_liveViewport->setHorizontalScrollBarPolicy(m_liveZoom ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);
	m_liveViewport->setVerticalScrollBarPolicy(m_liveZoom ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);
	if (m_liveZoom != 0 && size.isValid())
		m_liveFrame->resize(size * (m_liveZoom / 100.));
	const int fit = size.width() > 0 ? qRound(100. * m_liveFrame->imageRect().width() / size.width()) : 100;
	m_zoomLabel->setText(QString::number(m_liveZoom == 0 ? fit : m_liveZoom) + "%");
}

void KRRMainWindow::releaseIdlePreview()
{
	m_idlePreview.reset();
	m_idleSequence = -1;
	m_idleCameraReady = false;
	m_idleCameraError.clear();
	m_idleConfigClock.invalidate();
	m_idleFrameClock.invalidate();
	m_idleFps = 0;
	m_micMonitor.reset();
	m_systemMonitor.reset();
	m_extraMonitors.clear();
}

void KRRMainWindow::updateIdlePreview()
{
	if (!m_studioReady || m_workspace->currentIndex() != Record || !isVisible() || isMinimized())
	{
		if (m_idlePreview || m_micMonitor || m_systemMonitor) releaseIdlePreview();
		return;
	}
	if (!m_idleConfigClock.isValid() || m_idleConfigClock.elapsed() >= 700)
	{
		m_idleConfigClock.restart();
		auto config = configuration();
		config.output.clear();
		if (config.outputColor == "hdr-to-sdr") config.screenColor = "highlights";
		config.outputColor = "sdr";
		config.cameraTrack = false;
		if (m_overlay->isVisible()) config.effects = {};
		const auto source = config.toJson();
		QJsonObject identity{{"source", config.source}, {"width", config.width}, {"height", config.height}, {"fps", config.fps}};
		if (config.source == "image") identity["image"] = config.image;
		else if (config.source == "camera") identity["camera"] = config.camera;
		else
		{
			if (config.source == "screen")
			{
				if (config.monitorDevice.isEmpty()) identity["monitor"] = config.monitor;
				else identity["monitorDevice"] = config.monitorDevice;
			}
			else if (config.source == "window") identity["window"] = source["window"];
			else
				for (const auto& key : {"x", "y", "rw", "rh"}) identity[key] = source[key];
			for (const auto& key : {"screenColor", "screenWhiteNits", "screenExposureEV", "cursor", "clicks", "clickColor", "clickRadius"})
				identity[key] = source[key];
		}
		if (!m_idlePreview || identity != m_idleConfiguration)
		{
			m_idlePreview.reset();
			m_idleConfiguration = identity;
			m_idleSequence = -1;
			m_idleCameraReady = false;
			m_idleCameraError.clear();
			m_idleFrameClock.invalidate();
			m_liveFrame->setFrame({});
			m_liveFrame->setText(Ui("正在连接画面源…"));
			m_liveStatus->clear();
			m_idlePreview = std::make_unique<KRRLiveCapturePreview>(config, qMax(33, 1000 / qMax(1, config.fps)),
				QSize(2560, 1600), true);
		}
		else m_idlePreview->updateComposition(config);
		m_liveFrame->setPlacement(config.cameraRect, config.cameraEnabled && config.source != "camera");
	}
	const auto next = m_idlePreview->latest();
	if (!next.sequence || next.sequence == m_idleSequence) return;
	m_idleSequence = next.sequence;
	m_idleCameraReady = !next.camera.isNull();
	m_idleCameraError = next.cameraError;
	if (next.image.isNull())
	{
		m_liveFrame->setFrame({});
		if (!next.error.isEmpty()) m_liveFrame->setText(UiDiagnostic(next.error));
		m_idleFps = 0;
		return;
	}
	const auto oldSize = m_liveFrame->frameSize();
	m_liveFrame->setFrame(next.image);
	if (oldSize != next.image.size() || m_liveZoom == 0) refreshLiveZoom();
	if (!m_idleFrameClock.isValid() || m_idleFrameClock.elapsed() >= 1000)
	{
		if (m_idleFrameClock.isValid())
			m_idleFps = 1000. * (next.sequence - m_idleFpsSequence) / m_idleFrameClock.elapsed();
		m_idleFpsSequence = next.sequence;
		m_idleFrameClock.restart();
		m_liveStatus->setText(next.error.isEmpty() ? Ui("● 监看中") : UiDiagnostic(next.error));
	}
}

void KRRMainWindow::updateMeters(const QJsonObject& status)
{
	if (!m_studioReady) return;
	if (status["state"] == "recording")
	{
		m_micMeter->setLevel(status["mic_level"].toDouble());
		m_systemMeter->setLevel(status["system_level"].toDouble());
		const auto levels = status["audio_levels"].toObject();
		for (auto it = m_extraMeters.cbegin(); it != m_extraMeters.cend(); ++it)
			it.value()->setLevel(levels[it.key()].toDouble());
		return;
	}
	if (m_workspace->currentIndex() != Record || !isVisible() || isMinimized()) return;
	const auto mode = m_audio->currentData().toString();
	auto sample = [](std::unique_ptr<KRRAudioCapture>& device, bool enabled, bool loopback, int gain)
	{
		if (!enabled) { device.reset(); return 0.f; }
		if (!device) { device = std::make_unique<KRRAudioCapture>(loopback); device->startMeter(); }
		device->setGain(gain / 100.f);
		return device->takePeak();
	};
	m_micMeter->setLevel(sample(m_micMonitor, mode == "mic" || mode == "mix", false, m_micGain->value()));
	m_systemMeter->setLevel(sample(m_systemMonitor, mode == "system" || mode == "mix", true, m_systemGain->value()));
	for (const auto& value : m_additionalAudio)
	{
		const auto item = value.toObject();
		const auto id = item["id"].toString();
		auto& monitor = m_extraMonitors[id];
		if (item["muted"].toBool()) monitor.reset();
		else if (!monitor)
		{
			monitor = std::make_unique<KRRAudioCapture>(item["loopback"].toBool(), id);
			monitor->startMeter();
		}
		if (monitor) monitor->setGain(float(item["gain"].toDouble(1.)));
		if (auto meter = m_extraMeters.value(id)) meter->setLevel(monitor ? monitor->takePeak() : 0.f);
	}
}

void KRRMainWindow::addAudioSource()
{
	if (m_active || m_countdownRemaining) return;
	if (m_additionalAudio.size() >= MaxAdditionalAudioSources)
	{
		m_message->setText(Ui("已达到额外音源上限。"));
		return;
	}
	const auto endpoints = RecordingAudioEndpoints();
	QDialog dialog(this);
	dialog.setWindowTitle(Ui("添加音频来源"));
	dialog.setWindowFlag(Qt::WindowContextHelpButtonHint, false);
	auto layout = new QFormLayout(&dialog);
	auto kind = new QComboBox(&dialog);
	kind->addItems({Ui("麦克风 / 输入"), Ui("系统播放设备")});
	auto devices = new QComboBox(&dialog);
	devices->setMinimumContentsLength(16);
	devices->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	layout->addRow(Ui("类型"), kind);
	layout->addRow(Ui("设备"), devices);
	auto actions = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
	layout->addRow(actions);
	const auto refresh = [&]
	{
		devices->clear();
		for (int i = 0; i < endpoints.size(); ++i)
		{
			const auto& endpoint = endpoints[i];
			if (endpoint.loopback != (kind->currentIndex() == 1)) continue;
			if (std::any_of(m_additionalAudio.begin(), m_additionalAudio.end(), [&endpoint](const auto& item)
			{ return item.toObject()["id"] == endpoint.id; })) continue;
			devices->addItem(endpoint.name, i);
		}
		actions->button(QDialogButtonBox::Ok)->setEnabled(devices->count() > 0);
	};
	connect(kind, qOverload<int>(&QComboBox::currentIndexChanged), &dialog, refresh);
	connect(actions, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	connect(actions, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	refresh();
	if (dialog.exec() != QDialog::Accepted || devices->currentIndex() < 0) return;
	const auto endpoint = endpoints[devices->currentData().toInt()];
	m_additionalAudio.append(QJsonObject{{"id", endpoint.id}, {"name", endpoint.name.left(200)},
		{"loopback", endpoint.loopback}, {"muted", false}, {"gain", 1.}, {"denoise", false}});
	rebuildAudioSources();
	saveSettings();
}

void KRRMainWindow::rebuildAudioSources()
{
	m_extraMeters.clear();
	m_extraMonitors.clear();
	while (auto item = m_extraAudioLayout->takeAt(0))
	{
		if (item->widget()) item->widget()->deleteLater();
		delete item;
	}
	for (int i = 0; i < m_additionalAudio.size(); ++i)
	{
		const auto source = m_additionalAudio[i].toObject();
		const bool system = source["loopback"].toBool();
		auto widget = new QWidget(this);
		auto layout = new QVBoxLayout(widget);
		layout->setContentsMargins(0, 0, 0, 1);
		layout->setSpacing(3);
		auto update = [this, i](const QString& key, const QJsonValue& value)
		{
			if (i >= m_additionalAudio.size()) return;
			auto changed = m_additionalAudio[i].toObject();
			changed[key] = value;
			m_additionalAudio.replace(i, changed);
			m_effectTimer.start(40);
		};
		auto row = new QHBoxLayout;
		row->setSpacing(3);
		auto toggle = RecorderUi::Toggle(system ? "volume" : "mic", source["name"].toString(), widget);
		toggle->setChecked(!source["muted"].toBool());
		row->addWidget(toggle);
		connect(toggle, &QPushButton::clicked, this, [update](bool on) { update("muted", !on); });
		auto name = new QLabel(source["name"].toString(), widget);
		name->setToolTip(name->text());
		name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
		row->addWidget(name, 1);
		auto noise = new QCheckBox(Ui("降噪"), widget);
		noise->setObjectName("studioDenoise");
		noise->setChecked(source["denoise"].toBool());
		if (!system) row->addWidget(noise);
		else noise->hide();
		connect(noise, &QCheckBox::toggled, this, [update](bool on) { update("denoise", on); });
		auto drop = Button(widget, {}, "trash");
		drop->setToolTip(Ui("移除音源"));
		row->addWidget(drop);
		connect(drop, &QPushButton::clicked, widget, [this, i]
		{
			if (m_active || m_countdownRemaining || i >= m_additionalAudio.size()) return;
			m_additionalAudio.removeAt(i);
			rebuildAudioSources();
			saveSettings();
		});
		connect(&m_pollTimer, &QTimer::timeout, widget, [this, drop, noise]
		{
			drop->setEnabled(!m_active && !m_countdownRemaining);
			noise->setEnabled(!m_active && !m_countdownRemaining);
		});
		layout->addLayout(row);
		auto meter = new KRRPeakMeter(widget);
		m_extraMeters[source["id"].toString()] = meter;
		layout->addWidget(meter);
		auto gain = new QSlider(Qt::Horizontal, widget);
		gain->setAccessibleName(name->text());
		gain->setRange(0, 200);
		gain->setValue(qRound(source["gain"].toDouble(1.) * 100));
		layout->addWidget(gain);
		connect(gain, &QSlider::valueChanged, widget, [update](int value) { update("gain", value / 100.); });
		m_extraAudioLayout->addWidget(widget);
	}
}

QString KRRMainWindow::studioMetrics(const QJsonObject& status, bool active)
{
	FILETIME create, exit, kernel, user, now;
	GetSystemTimeAsFileTime(&now);
	if (GetProcessTimes(GetCurrentProcess(), &create, &exit, &kernel, &user))
	{
		const auto at = FileTime(now), ticks = FileTime(kernel) + FileTime(user);
		if (m_cpuAt && at > m_cpuAt)
			m_guiCpu = 100. * (ticks - m_cpuTicks) / (at - m_cpuAt)
				/ qMax<DWORD>(1, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
		m_cpuAt = at;
		m_cpuTicks = ticks;
	}
	const auto available = QStorageInfo(m_directory->text()).bytesAvailable();
	const auto size = m_liveFrame->frameSize();
	const auto target = active ? status["target_fps"].toInt() : m_fps->currentData().toInt();
	auto codec = active ? status["encoder"].toString() : m_encoder->currentText();
	if (codec.isEmpty()) codec = "—";
	const auto resolution = active ? QSize(status["width"].toInt(), status["height"].toInt()) : size;
	const auto performance = Ui("%1 / %2 fps    CPU %3%")
		.arg(active ? status["effective_fps"].toDouble() : m_idleFps, 0, 'f', 1).arg(target)
		.arg(m_guiCpu + (active ? status["cpu_pct"].toDouble() : 0.), 0, 'f', 1);
	const auto frameCounts = active ? Ui("    帧 %1    补帧 %2").arg(status["frames_written"].toVariant().toLongLong())
		.arg(status["duplicate_frames"].toVariant().toLongLong()) : QString{};
	const auto dimensions = !resolution.isEmpty() ? QString("    %1×%2").arg(resolution.width()).arg(resolution.height()) : QString{};
	const auto storage = available < 0 ? QString{} : QString("    %1 GB").arg(available / 1.e9, 0, 'f', 1);
	return codec + dimensions + "    " + performance + frameCounts
		+ QString("    %1 kbps").arg(active ? status["bitrate"].toInt() : m_bitrate->value()) + storage;
}

void KRRMainWindow::updateReviewLibrary()
{
	if (!m_studioReady) return;
	const auto query = m_librarySearch->text().trimmed();
	const auto filter = m_libraryFilter->currentIndex();
	m_recent->setVisible(filter != 2);
	if (m_recent->count() < 50 && !m_directory->text().isEmpty())
	{
		QStringList known;
		for (int i = 0; i < m_recent->count(); ++i)
			known << m_recent->item(i)->data(Qt::UserRole).toString();
		const auto output = QDir(m_directory->text()).entryInfoList(
			{"recording-*", "scheduled-*"}, QDir::Files, QDir::Time);
		for (const auto& file : output)
		{
			const auto path = file.absoluteFilePath();
			if (!QStringList{"mp4", "mov", "mkv", "avi", "flv"}.contains(file.suffix().toLower()) ||
				known.contains(path, Qt::CaseInsensitive)) continue;
			auto row = new QListWidgetItem(file.fileName(), m_recent);
			row->setData(Qt::UserRole, path);
			row->setToolTip(path);
			if (m_recent->count() >= 50) break;
		}
	}
	m_recoveryLibrary->clear();
	const auto restored = filter != 1 ? RecordingRecoveryItems(m_directory->text()) : QVector<KRRRecoveryItem>();
	for (const auto& item : restored)
		if (item.name.contains(query, Qt::CaseInsensitive))
		{
			auto row = new QListWidgetItem(item.name + "\n" + Ui("待恢复") + "  " + item.modified.toString("MM-dd HH:mm"), m_recoveryLibrary);
			row->setToolTip(item.workspace);
		}
	m_recoveryLibrary->setVisible(filter == 2 || m_recoveryLibrary->count() > 0);
	if (filter != 2) m_recoveryLibrary->setMaximumHeight(m_recoveryLibrary->count() ? 110 : 0);
	else m_recoveryLibrary->setMaximumHeight(QWIDGETSIZE_MAX);
	for (int i = 0; i < m_recent->count(); ++i)
	{
		auto item = m_recent->item(i);
		const QFileInfo file(item->data(Qt::UserRole).toString());
		if (!item->data(Qt::UserRole + 1).isValid())
		{
			QStringList names;
			const auto markers = ReadRecordingMarkers(file.absoluteFilePath());
			for (const auto& marker : markers) names << marker.toObject()["label"].toString();
			item->setData(Qt::UserRole + 1, names.join(' '));
			item->setData(Qt::UserRole + 2, markers.size());
		}
		const int count = item->data(Qt::UserRole + 2).toInt();
		const auto length = item->data(Qt::UserRole + 3).toLongLong();
		item->setText(file.fileName() + "\n" + file.lastModified().toString("MM-dd HH:mm")
			+ (length > 0 ? "  " + Clock(length) : QString{}) + Ui("  打点 %1").arg(count));
		item->setHidden(!file.fileName().contains(query, Qt::CaseInsensitive));
		if (file.absoluteFilePath() == m_preview) item->setSelected(true);
	}
	if (m_reviewThumbnail.state() != QProcess::NotRunning || FindFFmpeg().isEmpty() || filter == 2) return;
	for (int i = 0; i < m_recent->count(); ++i)
	{
		auto item = m_recent->item(i);
		if (item->isHidden() || item->data(Qt::UserRole + 4).toBool()) continue;
		item->setData(Qt::UserRole + 4, true);
		m_pendingThumbnail = item->data(Qt::UserRole).toString();
		m_reviewThumbnail.start(FindFFmpeg(), {"-hide_banner", "-loglevel", "info", "-nostdin", "-i", m_pendingThumbnail,
			"-map", "0:v:0", "-frames:v", "1", "-vf", "scale=140:84:force_original_aspect_ratio=decrease",
			"-f", "image2pipe", "-c:v", "mjpeg", "-threads", "1", "pipe:1"});
		break;
	}
}

void KRRMainWindow::describePreview(const QString& file)
{
	if (!m_reviewFormat) return;
	m_reviewName->setText(file.isEmpty() ? Ui("视频回看") : QFileInfo(file).completeBaseName());
	m_reviewName->setToolTip(file);
	m_reviewFormat->clear();
	m_reviewProbe.setProperty("nextFile", file);
	if (m_reviewProbe.state() != QProcess::NotRunning) { m_reviewProbe.kill(); return; }
	if (file.isEmpty() || FindFFprobe().isEmpty()) return;
	m_reviewProbe.setProperty("nextFile", QString{});
	m_reviewProbe.setProperty("probeFile", file);
	m_reviewProbe.start(FindFFprobe(), {"-v", "error", "-show_entries",
		"format=duration,size,bit_rate:stream=codec_name,codec_type,width,height,avg_frame_rate,nb_frames",
		"-of", "json", file});
}

void KRRMainWindow::initializeReviewProbe()
{
	HideConsoleProcess(m_reviewProbe, true);
	HideConsoleProcess(m_reviewThumbnail, true);
	connect(&m_reviewProbe, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this](int code, QProcess::ExitStatus)
	{
		const auto data = QJsonDocument::fromJson(m_reviewProbe.readAllStandardOutput()).object();
		const auto path = m_reviewProbe.property("probeFile").toString();
		const auto next = m_reviewProbe.property("nextFile").toString();
		if (!next.isEmpty()) { describePreview(next); return; }
		if (code || path != m_preview || data.isEmpty()) return;
		const auto format = data["format"].toObject();
		const auto duration = qRound64(format["duration"].toVariant().toDouble() * 1000);
		QJsonObject video;
		QStringList audio;
		for (const auto& stream : data["streams"].toArray())
		{
			const auto object = stream.toObject();
			if (object["codec_type"] == "video" && video.isEmpty()) video = object;
			if (object["codec_type"] == "audio") audio << object["codec_name"].toString();
		}
		const auto fraction = video["avg_frame_rate"].toString().split('/');
		const auto fps = fraction.value(1).toDouble() > 0 ? fraction.value(0).toDouble() / fraction[1].toDouble() : 0.;
		QString encoder = video["codec_name"].toString().toUpper();
		if (path == m_lastRecordingStatus["output"].toString() && !m_lastRecordingStatus["encoder"].toString().isEmpty())
			encoder = m_lastRecordingStatus["encoder"].toString();
		m_reviewFormat->setText(Ui("成片编码") + "\n" + encoder + QString("   %1×%2   %3 fps")
			.arg(video["width"].toInt()).arg(video["height"].toInt()).arg(fps, 0, 'f', 2)
			+ "\n" + QString("%1 kbps   %2 MB").arg(format["bit_rate"].toVariant().toDouble() / 1000., 0, 'f', 0)
				.arg(format["size"].toVariant().toDouble() / 1.e6, 0, 'f', 1)
			+ (video["nb_frames"].toVariant().toLongLong() ? Ui("\n帧数 %1").arg(video["nb_frames"].toString()) : QString{})
			+ (audio.isEmpty() ? QString{} : Ui("\n音轨 %1").arg(audio.join(" / "))));
		for (int i = 0; i < m_recent->count(); ++i)
			if (m_recent->item(i)->data(Qt::UserRole).toString() == path)
				m_recent->item(i)->setData(Qt::UserRole + 3, duration);
		updateReviewLibrary();
	});
	connect(&m_reviewThumbnail, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this](int code, QProcess::ExitStatus)
	{
		const auto file = std::exchange(m_pendingThumbnail, QString{});
		if (file.isEmpty()) return;
		const auto jpeg = m_reviewThumbnail.readAllStandardOutput();
		const auto image = QImage::fromData(jpeg);
		static const QRegularExpression durationPattern(R"(Duration: (\d+):(\d+):(\d+\.\d+))");
		const auto duration = durationPattern.match(QString::fromUtf8(m_reviewThumbnail.readAllStandardError()));
		for (int i = 0; i < m_recent->count(); ++i)
		{
			auto row = m_recent->item(i);
			if (row->data(Qt::UserRole).toString() != file) continue;
			const auto validImage = !code && !image.isNull();
			if (validImage)
			{
				row->setIcon(QPixmap::fromImage(image));
				row->setData(Qt::UserRole + 5, jpeg);
			}
			if (duration.hasMatch() && !row->data(Qt::UserRole + 3).isValid())
				row->setData(Qt::UserRole + 3, qRound64(1000. * (duration.captured(1).toDouble() * 3600
					+ duration.captured(2).toDouble() * 60 + duration.captured(3).toDouble())));
			emit reviewThumbnailReady(file, validImage ? jpeg : QByteArray{}, row->data(Qt::UserRole + 3).toLongLong());
			break;
		}
		if (m_workspace->currentIndex() == Review)
			QTimer::singleShot(0, this, &KRRMainWindow::updateReviewLibrary);
	});
}

void KRRMainWindow::refreshSceneList()
{
	const QSignalBlocker guard(m_scenes);
	m_scenes->clear();
	for (const auto& scene : m_scenePresets)
	{
		const auto row = scene.toObject();
		auto item = new QListWidgetItem(row["name"].toString(), m_scenes);
		item->setFlags(item->flags() | Qt::ItemIsEditable);
	}
}

void KRRMainWindow::saveSceneList()
{
	QSettings settings(RecorderUiSettingsPath(), QSettings::IniFormat);
	settings.setIniCodec("UTF-8");
	settings.setValue("studioScenes", QJsonDocument(m_scenePresets).toJson(QJsonDocument::Compact));
	settings.sync();
	if (settings.status() != QSettings::NoError) m_message->setText(Ui("无法保存场景预设"));
}

void KRRMainWindow::rememberSource(bool append)
{
	if (!m_studioReady || m_updatingSources) return;
	syncQuickSources();
	if (!append && m_sceneIndex >= 0 && m_sceneIndex < m_scenePresets.size() &&
		m_scenePresets[m_sceneIndex].toObject()["layeredSources"].toBool())
	{
		refreshSources();
		return;
	}
	const auto all = configuration().toJson();
	QJsonObject source;
	for (const auto& key : QStringList{"source", "monitor", "monitorDevice", "window", "x", "y", "rw", "rh", "camera", "image", "cursor"})
		source[key] = all[key];
	source["label"] = m_quickTarget->currentText().isEmpty() ? m_mode->currentText() : m_quickTarget->currentText();
	if (append || m_sourceIndex < 0 || m_sourceIndex >= m_visualSources.size())
	{
		m_visualSources.append(source);
		m_sourceIndex = m_visualSources.size() - 1;
	}
	else m_visualSources.replace(m_sourceIndex, source);
	refreshSources();
	if (m_sceneIndex < 0 || m_sceneIndex >= m_scenePresets.size()) return;
	auto scene = m_scenePresets[m_sceneIndex].toObject();
	auto config = all;
	config.remove("output");
	scene["config"] = config;
	scene["sources"] = m_visualSources;
	scene["sourceIndex"] = m_sourceIndex;
	scene["automaticResolution"] = m_resolution->currentIndex() == 0;
	scene["recommendedBitrate"] = m_recommendedBitrate->isChecked();
	scene["format"] = m_format->currentText();
	m_scenePresets.replace(m_sceneIndex, scene);
	saveSceneList();
}

void KRRMainWindow::refreshSources()
{
	const int selected = m_sources->currentItem() ? m_sources->currentItem()->data(Qt::UserRole).toInt() : m_sourceIndex;
	m_sources->clear();
	for (int i = 0; i < m_visualSources.size(); ++i)
	{
		const auto source = m_visualSources[i].toObject();
		const auto type = source["source"].toString();
		auto item = new QListWidgetItem(RecorderUi::Icon(type == "camera" ? "camera" : type == "image" ? "image" : "screen"),
			source["label"].toString(), m_sources);
		item->setToolTip(item->text());
		item->setData(Qt::UserRole, i);
	}
	if (m_cameraEnabled->isChecked() && m_source->currentData() != "camera")
	{
		auto item = new QListWidgetItem(RecorderUi::Icon("camera"), m_camera->currentText() + " · " + Ui("画中画"), m_sources);
		item->setData(Qt::UserRole, -1);
	}
	m_sources->setCurrentRow(selected < 0 && m_sources->count() > m_visualSources.size() ? m_sources->count() - 1 : m_sourceIndex);
}

void KRRMainWindow::activateSource(int index)
{
	if (index < 0 || index >= m_visualSources.size() || index == m_sourceIndex) return;
	const auto selected = m_visualSources[index].toObject();
	auto merged = configuration().toJson();
	merged.remove("monitorDevice");
	for (auto it = selected.begin(); it != selected.end(); ++it) merged[it.key()] = it.value();
	const auto config = KRRRecordingConfig::FromJson(merged);
	if (config.source == "camera" && m_active) { m_sources->setCurrentRow(m_sourceIndex); return; }
	QString error;
	if (!config.validate(error))
	{
		m_message->setText(UiDiagnostic(error));
		m_sources->setCurrentRow(m_sourceIndex);
		return;
	}
	{
		const QScopedValueRollback<bool> loading(m_updatingSources, true);
		m_sourceIndex = index;
		m_source->setCurrentIndex(m_source->findData(config.source));
		const auto monitor = config.monitorDevice.isEmpty() ? config.monitor : m_monitor->findData(config.monitorDevice, Qt::UserRole + 1);
		m_monitor->setCurrentIndex(monitor);
		const auto window = m_window->findData(QVariant::fromValue<qulonglong>(config.window));
		if (window >= 0) m_window->setCurrentIndex(window);
		const auto camera = m_camera->findData(config.camera);
		if (camera >= 0 && !m_active) m_camera->setCurrentIndex(camera);
		m_image->setText(config.image);
		m_region = config.region;
		if (m_region.isValid()) m_regionLabel->setText(QString("%1, %2 · %3 × %4").arg(m_region.x()).arg(m_region.y())
			.arg(m_region.width()).arg(m_region.height()));
		m_cursor->setChecked(config.cursor);
		updateBitrateRecommendation();
		syncQuickSources();
		sendScene();
	}
	saveSettings();
}

void KRRMainWindow::scheduleTask()
{
	QDialog dialog(this);
	dialog.setWindowTitle(Ui("预约录制"));
	dialog.setObjectName("studioScheduleDialog");
	dialog.setWindowFlag(Qt::WindowContextHelpButtonHint, false);
	dialog.resize(700, 550);
	auto layout = new QVBoxLayout(&dialog);
	auto form = new QFormLayout;
	auto name = new QLineEdit(&dialog);
	name->setMaxLength(80);
	auto when = new QDateTimeEdit(QDateTime::currentDateTime().addSecs(60), &dialog);
	when->setDisplayFormat("yyyy-MM-dd HH:mm:ss");
	when->setCalendarPopup(true);
	auto duration = new QHBoxLayout;
	auto makeTime = [&dialog](const QString& id, const QString& placeholder, int limit)
	{
		auto field = new QLineEdit(&dialog);
		field->setObjectName(id);
		field->setPlaceholderText(placeholder);
		field->setAccessibleName(placeholder);
		field->setValidator(new QIntValidator(0, limit, field));
		return field;
	};
	auto hours = makeTime("scheduleHours", "hh", 168);
	auto minutes = makeTime("scheduleMinutes", "mm", 59);
	auto seconds = makeTime("scheduleSeconds", "ss", 59);
	for (auto field : {hours, minutes, seconds})
	{
		if (field != hours) duration->addWidget(new QLabel(":", &dialog));
		duration->addWidget(field);
	}
	auto scene = new QComboBox(&dialog);
	scene->addItem(Ui("当前场景"));
	for (const auto& value : m_scenePresets) scene->addItem(value.toObject()["name"].toString());
	form->addRow(Ui("任务名称"), name);
	form->addRow(Ui("开始时间"), when);
	form->addRow(Ui("自动停录"), duration);
	form->addRow(Ui("录制场景"), scene);
	layout->addLayout(form);
	auto add = Button(&dialog, Ui("添加到预约队列"));
	add->setProperty("role", "primary");
	layout->addWidget(add, 0, Qt::AlignRight);
	auto table = new QTableWidget(0, 4, &dialog);
	table->setObjectName("studioScheduleQueue");
	table->setHorizontalHeaderLabels({Ui("任务"), Ui("开始时间"), Ui("时长"), Ui("状态")});
	table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
	table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
	table->verticalHeader()->hide();
	table->verticalHeader()->setDefaultSectionSize(RecorderUi::Px(27));
	table->setMinimumHeight(RecorderUi::Px(112));
	table->setEditTriggers(QAbstractItemView::NoEditTriggers);
	table->setSelectionBehavior(QAbstractItemView::SelectRows);
	table->setSelectionMode(QAbstractItemView::SingleSelection);
	layout->addWidget(table, 1);
	auto remove = Button(&dialog, Ui("删除选中预约"), "trash");
	auto feedback = new QLabel(&dialog);
	feedback->setWordWrap(true);
	layout->addWidget(feedback);
	layout->addWidget(remove, 0, Qt::AlignRight);
	auto refresh = [table]
	{
		QString error;
		QJsonObject response;
		if (!SendControl({{"command", "schedules"}}, response, error, 180)) return;
		const auto rows = response["schedules"].toArray();
		const auto selected = table->currentRow() >= 0 && table->item(table->currentRow(), 0)
			? table->item(table->currentRow(), 0)->data(Qt::UserRole).toString() : QString{};
		if (table->property("queue").toJsonArray() == rows) return;
		table->setProperty("queue", rows);
		table->setRowCount(rows.size());
		const QHash<QString, QString> states{{"waiting", Ui("等待触发")}, {"conflict", Ui("时段冲突")},
			{"expired", Ui("已过期")}, {"recording", Ui("录制中")}, {"completed", Ui("已完成")},
			{"missed", Ui("错过 · 录制忙")}, {"failed", Ui("失败")}};
		for (int i = 0; i < rows.size(); ++i)
		{
			const auto task = rows[i].toObject();
			const auto state = task["state"].toString();
			const QStringList columns{task["name"].toString(), QDateTime::fromString(task["startAt"].toString(), Qt::ISODateWithMs)
				.toLocalTime().toString("MM-dd HH:mm:ss"), task["duration"].toDouble() > 0 ? Clock(qRound64(task["duration"].toDouble() * 1000)) : Ui("手动停止"), states.value(state, state)};
			for (int j = 0; j < columns.size(); ++j) table->setItem(i, j, new QTableWidgetItem(columns[j]));
			table->item(i, 0)->setData(Qt::UserRole, task["id"].toString());
			table->item(i, 0)->setToolTip(task["output"].toString());
			table->item(i, 3)->setToolTip(task["conflict"].toString().isEmpty() ? UiDiagnostic(task["error"].toString())
				: Ui("与 %1 的时段重叠").arg(task["conflict"].toString()));
			if (task["id"] == selected) table->selectRow(i);
		}
	};
	connect(add, &QPushButton::clicked, &dialog, [this, name, when, hours, minutes, seconds, scene, feedback, refresh]
	{
		if (!validateStorage()) return;
		auto config = configuration();
		const auto output = config.output;
		auto extension = QFileInfo(output).suffix();
		if (scene->currentIndex() > 0)
		{
			const auto preset = m_scenePresets.at(scene->currentIndex() - 1).toObject();
			config = KRRRecordingConfig::FromJson(preset["config"].toObject());
			extension = preset["format"].toString(extension);
		}
		config.output = QDir(QFileInfo(output).absolutePath()).filePath("scheduled-"
			+ when->dateTime().toString("yyyyMMdd-HHmmss") + "-" + QUuid::createUuid().toString(QUuid::Id128).left(8)
			+ "." + extension);
		for (auto field : {hours, minutes, seconds})
			if (!field->text().isEmpty() && !field->hasAcceptableInput())
			{
				feedback->setText(Ui("请输入有效的时、分、秒。"));
				field->setFocus();
				return;
			}
		config.maxDuration = hours->text().toInt() * 3600 + minutes->text().toInt() * 60 + seconds->text().toInt();
		QString error;
		QJsonObject response;
		if (!config.validate(error) || !EnsureControlService(error, true) || !SendControl({
			{"command", "schedule"}, {"config", config.toJson()}, {"name", name->text().trimmed()},
			{"startAt", when->dateTime().toUTC().toString(Qt::ISODateWithMs)}}, response, error, 3000))
		{
			feedback->setText(UiDiagnostic(error));
			return;
		}
		feedback->clear();
		name->clear();
		saveSettings();
		refresh();
		updateStatus();
	});
	connect(remove, &QPushButton::clicked, &dialog, [this, table, feedback, refresh]
	{
		auto item = table->item(table->currentRow(), 0);
		if (!item) return;
		QString error;
		QJsonObject response;
		if (!SendControl({{"command", "cancel-schedule"}, {"id", item->data(Qt::UserRole).toString()}}, response, error, 500))
			feedback->setText(UiDiagnostic(error));
		else { feedback->clear(); refresh(); updateStatus(); }
	});
	QTimer timer(&dialog);
	connect(&timer, &QTimer::timeout, &dialog, refresh);
	timer.start(1500);
	refresh();
	dialog.exec();
}
