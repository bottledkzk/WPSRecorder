#include "studioassistant.h"

#include "assistanthistory.h"
#include "clipeditor.h"
#include "editsequence.h"
#include "playbackslider.h"
#include "previewvideo.h"
#include "recordingassistantdialog.h"
#include "recordingconfig.h"
#include "speechsettingsdialog.h"
#include "uilanguage.h"
#include "uitheme.h"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDirIterator>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileIconProvider>
#include <QFormLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QPointer>
#include <QRunnable>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QThreadPool>
#include <QTimer>
#include <QTreeWidget>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>
#include <utility>

namespace
{
constexpr int Id = Qt::UserRole;
constexpr int Type = Qt::UserRole + 1;

QPushButton* Button(QWidget* parent, const QString& label, const QString& icon = {}, const QString& tooltip = {})
{
	auto button = new QPushButton(label, parent);
	if (!icon.isEmpty()) button->setIcon(RecorderUi::Icon(icon));
	if (!tooltip.isEmpty())
	{
		button->setToolTip(tooltip);
		button->setAccessibleName(tooltip);
	}
	button->setAutoDefault(false);
	return button;
}

QString VideoFilter()
{
	return Ui("视频 (*.mp4 *.mov *.mkv *.webm *.avi *.flv)");
}

bool IsVideo(const QString& path)
{
	return QStringList{"mp4", "mov", "mkv", "webm", "avi", "flv", "m4v"}.contains(QFileInfo(path).suffix().toLower());
}

QString DisplayProject(const QString& file)
{
	QJsonObject data;
	QString error;
	if (ReadEditProjectJson(file, data, error))
	{
		const auto original = data["draftOf"].toString();
		if (!original.isEmpty()) return QFileInfo(original).completeBaseName() + " · " + Ui("草稿");
		const auto video = data["input"].toString();
		if (!video.isEmpty()) return QFileInfo(video).completeBaseName() + " · " + Ui("草稿");
	}
	return QFileInfo(file).completeBaseName();
}

QStringList ProjectFiles(const QString& path)
{
	QJsonObject document;
	QString error;
	if (!ReadEditProjectJson(path, document, error)) return {};
	QStringList files;
	auto append = [&files](const KRREditProject& part)
	{
		files << part.input;
		for (const auto& audio : part.audio)
			if (!audio.file.isEmpty()) files << audio.file;
		for (const auto& still : part.stills)
			if (!still.image.isEmpty() && !still.image.startsWith(":")) files << still.image;
	};
	if (document["kind"] == "sequence")
	{
		KRREditSequence sequence;
		if (ReadEditSequence(document, sequence, error))
			for (const auto& part : sequence)
				if (part.edit) append(*part.edit);
	}
	else
	{
		KRREditProject project;
		if (KRREditProject::FromJson(document, project, error)) append(project);
	}
	files.removeDuplicates();
	return files;
}

void RemoveTab(QTabWidget* tabs, int index)
{
	auto page = tabs->widget(index);
	tabs->removeTab(index);
	delete page;
}

void SetPanelMargins(QLayout* layout)
{
	layout->setContentsMargins(4, 4, 4, 4);
	layout->setSpacing(4);
}

QTreeWidgetItem* FileItem(const QFileInfo& file, const QString& display = {})
{
	auto item = new QTreeWidgetItem({display.isEmpty() ? file.fileName() : display});
	item->setData(0, Id, file.absoluteFilePath());
	item->setToolTip(0, file.absoluteFilePath());
	item->setIcon(0, RecorderUi::Icon(file.isDir() ? "folder" : IsVideo(file.filePath()) ? "screen" : "file"));
	if (file.isDir()) item->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
	return item;
}
}

QString PickStudioProject(QWidget* parent, const QString& folder, const QString& current)
{
	QDialog dialog(parent);
	dialog.setWindowTitle(Ui("打开剪辑工程"));
	dialog.resize(RecorderUi::Px(480), RecorderUi::Px(360));
	auto layout = new QVBoxLayout(&dialog);
	auto list = new QListWidget(&dialog);
	list->setObjectName("studioProjectPicker");
	if (!current.isEmpty() && QFileInfo(current).isFile())
	{
		auto item = new QListWidgetItem(QFileInfo(current).completeBaseName(), list);
		item->setData(Id, current);
	}
	const auto drafts = KRRClipEditor::draftFiles();
	for (const auto& path : drafts)
	{
		auto item = new QListWidgetItem(DisplayProject(path), list);
		item->setData(Id, path);
		item->setToolTip(path);
	}
	auto browse = Button(&dialog, Ui("浏览工程文件…"), "folder");
	layout->addWidget(list, 1);
	auto footer = new QHBoxLayout;
	layout->addLayout(footer);
	footer->addWidget(browse);
	footer->addStretch(1);
	auto open = Button(&dialog, Ui("打开"));
	open->setProperty("role", "primary");
	open->setEnabled(list->count() > 0);
	footer->addWidget(open);
	QString path;
	auto choose = [&]
	{
		if (!list->currentItem()) return;
		path = list->currentItem()->data(Id).toString();
		dialog.accept();
	};
	QObject::connect(open, &QPushButton::clicked, &dialog, choose);
	QObject::connect(list, &QListWidget::itemDoubleClicked, &dialog, choose);
	QObject::connect(browse, &QPushButton::clicked, &dialog, [&]
	{
		const auto selected = QFileDialog::getOpenFileName(&dialog, Ui("打开剪辑工程"), folder, Ui("剪辑工程 (*.json)"));
		if (!selected.isEmpty()) { path = selected; dialog.accept(); }
	});
	if (list->count()) list->setCurrentRow(0);
	dialog.exec();
	return path;
}

void ManageAssistantArchive(QWidget* parent, KRRAssistantHistory* history)
{
	if (!history) return;
	QDialog dialog(parent);
	dialog.setWindowTitle(Ui("已归档的对话"));
	dialog.resize(RecorderUi::Px(460), RecorderUi::Px(365));
	auto layout = new QVBoxLayout(&dialog);
	auto list = new QListWidget(&dialog);
	layout->addWidget(list, 1);
	auto actions = new QHBoxLayout;
	auto restore = Button(&dialog, Ui("还原"));
	auto remove = Button(&dialog, Ui("删除"), "trash");
	auto close = Button(&dialog, Ui("完成"));
	for (auto button : {restore, remove, close}) actions->addWidget(button);
	layout->addLayout(actions);
	auto redraw = [&]
	{
		list->clear();
		for (const auto& value : history->entries(true))
		{
			const auto item = value.toObject();
			auto row = new QListWidgetItem(KRRAssistantHistory::title(item), list);
			row->setData(Id, item["id"].toString());
		}
		list->setCurrentRow(list->count() ? 0 : -1);
		restore->setEnabled(list->currentItem());
		remove->setEnabled(list->currentItem());
	};
	QObject::connect(history, &KRRAssistantHistory::changed, &dialog, redraw);
	QObject::connect(restore, &QPushButton::clicked, &dialog, [&] { if (auto row = list->currentItem()) history->setArchived(row->data(Id).toString(), false); });
	QObject::connect(remove, &QPushButton::clicked, &dialog, [&]
	{
		if (!list->currentItem()) return;
		const auto id = list->currentItem()->data(Id).toString();
		if (QMessageBox::question(&dialog, dialog.windowTitle(), Ui("永久删除此对话？")) == QMessageBox::Yes)
			history->remove(id);
	});
	QObject::connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
	redraw();
	dialog.exec();
}

KRRStudioAssistant::KRRStudioAssistant(KRRAssistantHistory* history, KRRRecordingAssistantDialog* chat,
	KRRClipEditor* editor, std::function<QString()> folder, QWidget* parent)
	: QWidget(parent), m_history(history), m_chat(chat), m_editor(editor), m_folder(std::move(folder))
{
	QFile persisted(DataDirectory() + "/assistant-projects.json");
	if (persisted.open(QIODevice::ReadOnly)) m_projects = QJsonDocument::fromJson(persisted.readAll()).array();

	auto outer = new QVBoxLayout(this);
	outer->setContentsMargins(0, 0, 0, 0);
	auto panes = new QSplitter(Qt::Horizontal, this);
	panes->setObjectName("studioAssistantSplit");
	outer->addWidget(panes);
	auto left = new QFrame(panes);
	left->setProperty("panel", true);
	auto nav = new QVBoxLayout(left);
	SetPanelMargins(nav);
	auto header = new QHBoxLayout;
	auto title = new QLabel(Ui("AI 助手"), left);
	title->setProperty("dockTitle", true);
	header->addWidget(title, 1);
	auto collapse = Button(left, {}, "collapse", Ui("收起功能栏"));
	header->addWidget(collapse);
	nav->addLayout(header);
	auto contents = new QWidget(left);
	auto body = new QVBoxLayout(contents);
	body->setContentsMargins(0, 0, 0, 0);
	nav->addWidget(contents, 1);
	QObject::connect(collapse, &QPushButton::clicked, left, [contents] { contents->setVisible(!contents->isVisible()); });
	m_record = Button(contents, Ui("录屏助手"), "screen");
	m_edit = Button(contents, Ui("剪辑助手"), "scissors");
	m_record->setObjectName("studioAiRecord");
	m_edit->setObjectName("studioAiEdit");
	for (auto button : {m_record, m_edit})
	{
		button->setCheckable(true);
		body->addWidget(button);
	}
	QObject::connect(m_record, &QPushButton::clicked, this, [this] { selectMode(false); });
	QObject::connect(m_edit, &QPushButton::clicked, this, [this] { selectMode(true); });
	auto historyBar = new QHBoxLayout;
	historyBar->addWidget(new QLabel(Ui("历史对话"), contents), 1);
	m_add = Button(contents, "+");
	m_add->setObjectName("studioAiHistoryAdd");
	historyBar->addWidget(m_add);
	body->addLayout(historyBar);
	QObject::connect(m_add, &QPushButton::clicked, this, [this] { if (m_editing) addProject(); else newConversation(); });

	m_tree = new QTreeWidget(contents);
	m_tree->setObjectName("studioAiHistory");
	m_tree->setHeaderHidden(true);
	m_tree->setColumnCount(3);
	m_tree->setIndentation(RecorderUi::Px(11));
	m_tree->header()->setStretchLastSection(false);
	m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
	for (int i : {1, 2})
	{
		m_tree->header()->setSectionResizeMode(i, QHeaderView::Fixed);
		m_tree->header()->resizeSection(i, RecorderUi::Px(23));
	}
	m_tree->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
	body->addWidget(m_tree, 1);
	auto models = Button(contents, Ui("模型服务"), "settings");
	body->addWidget(models);
	QObject::connect(models, &QPushButton::clicked, this, [this] { ConfigureSpeechServices(this, "assistant"); });
	QObject::connect(m_tree, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item)
	{
		if (item->data(0, Type) == "project") selectProject(item->data(0, Id).toString());
		else chooseConversation(item->data(0, Id).toString());
	});
	QObject::connect(m_tree, &QWidget::customContextMenuRequested, this, [this](const QPoint& position)
	{
		auto item = m_tree->itemAt(position);
		if (!item || item->data(0, Type) == "project") return;
		const auto id = item->data(0, Id).toString();
		QMenu menu(this);
		auto archive = menu.addAction(Ui("归档对话"));
		archive->setEnabled(!m_history->busy(id));
		if (menu.exec(m_tree->viewport()->mapToGlobal(position)) == archive)
		{
			const bool selected = m_chat->conversationId() == id;
			m_history->setArchived(id, true);
			if (selected) newConversation(m_editing ? m_project : QString{});
		}
	});
	panes->addWidget(left);
	m_chat->setParent(panes);
	m_chat->setWindowFlags(Qt::Widget);
	panes->addWidget(m_chat);
	m_tabs = new QTabWidget(panes);
	m_tabs->setObjectName("studioAiRightTabs");
	m_tabs->setTabsClosable(true);
	m_tabs->setMovable(true);
	auto newTab = Button(m_tabs, "+", {}, Ui("新建空白页"));
	newTab->setObjectName("studioAiNewTab");
	m_tabs->setCornerWidget(newTab);
	QObject::connect(newTab, &QPushButton::clicked, this, &KRRStudioAssistant::addBlankPage);
	QObject::connect(m_tabs, &QTabWidget::tabCloseRequested, this, [this](int index)
	{
		if (m_candidateHost && m_tabs->widget(index)->isAncestorOf(m_candidateHost)) m_candidateHost = nullptr;
		RemoveTab(m_tabs, index);
		if (!m_tabs->count()) addBlankPage();
	});
	panes->addWidget(m_tabs);
	panes->setChildrenCollapsible(true);
	panes->setStretchFactor(1, 1);
	panes->setSizes({205, 625, 365});
	QObject::connect(m_history, &KRRAssistantHistory::changed, this, &KRRStudioAssistant::updateHistory);
	selectMode(false);
}

void KRRStudioAssistant::enter()
{
	if (m_history->entry(m_chat->conversationId())["unread"].toBool()) m_history->visit(m_chat->conversationId());
}

void KRRStudioAssistant::selectMode(bool editing)
{
	m_editing = editing;
	m_record->setChecked(!editing);
	m_edit->setChecked(editing);
	m_add->setToolTip(editing ? Ui("新建项目") : Ui("新建对话"));
	m_add->setAccessibleName(m_add->toolTip());
	m_tabs->setVisible(editing);
	if (editing)
	{
		if (m_project.isEmpty() && !m_projects.isEmpty()) m_project = m_projects.first().toObject()["id"].toString();
		if (!m_tabs->count()) addBlankPage();
		refreshEditing();
	}
	else
	{
		m_chat->setMaterials({});
		m_chat->setEditingContext({}, false);
	}
	QString& remembered = editing ? m_editThread : m_recordThread;
	const auto group = historyGroup();
	auto item = m_history->entry(remembered);
	if (item["group"] != group || item["archived"].toBool())
	{
		remembered.clear();
		for (const auto& value : m_history->entries())
			if (value.toObject()["group"] == group) { remembered = value.toObject()["id"].toString(); break; }
	}
	if (remembered.isEmpty()) remembered = m_history->begin(group);
	m_chat->bindConversation(m_history, remembered);
	updateHistory();
}

QString KRRStudioAssistant::historyGroup() const
{
	return m_editing ? "edit:" + m_project : "record";
}

void KRRStudioAssistant::newConversation(const QString& id)
{
	if (m_editing && !id.isEmpty() && id != m_project) selectProject(id);
	const auto thread = m_history->begin(historyGroup());
	chooseConversation(thread);
	if (auto input = m_chat->findChild<QLineEdit*>("assistantPrompt")) input->setFocus();
}

void KRRStudioAssistant::chooseConversation(const QString& id)
{
	const auto row = m_history->entry(id);
	if (row.isEmpty() || row["archived"].toBool()) return;
	const auto group = row["group"].toString();
	if (m_editing && group.startsWith("edit:") && group.mid(5) != m_project)
		selectProject(group.mid(5));
	(m_editing ? m_editThread : m_recordThread) = id;
	m_chat->bindConversation(m_history, id);
	updateHistory();
}

void KRRStudioAssistant::updateHistory()
{
	if (m_updatingHistory) return;
	const QScopedValueRollback<bool> changing(m_updatingHistory, true);
	QSet<QString> closed;
	for (int i = 0; i < m_tree->topLevelItemCount(); ++i)
	{
		auto item = m_tree->topLevelItem(i);
		if (item->data(0, Type) == "project" && !item->isExpanded()) closed.insert(item->data(0, Id).toString());
	}
	const QSignalBlocker block(m_tree);
	m_tree->clear();
	QHash<QString, QTreeWidgetItem*> groups;
	QTreeWidgetItem* selected = nullptr;
	if (m_editing)
		for (const auto& value : m_projects)
		{
			const auto item = value.toObject();
			const auto id = item["id"].toString();
			auto row = new QTreeWidgetItem(m_tree, {item["name"].toString()});
			row->setData(0, Id, id);
			row->setData(0, Type, "project");
			row->setIcon(0, RecorderUi::Icon("folder"));
			row->setToolTip(0, item["name"].toString());
			row->setExpanded(id == m_project || !closed.contains(id));
			if (id == m_project) row->setForeground(0, QColor("#90bcf1"));
			auto add = Button(m_tree, "+", {}, Ui("新建对话"));
			auto settings = Button(m_tree, QStringLiteral("…"), {}, Ui("项目设置"));
			add->setProperty("role", "quiet");
			settings->setProperty("role", "quiet");
			m_tree->setItemWidget(row, 1, add);
			m_tree->setItemWidget(row, 2, settings);
			QObject::connect(add, &QPushButton::clicked, this, [this, id] { newConversation(id); });
			QObject::connect(settings, &QPushButton::clicked, this, [this, id, settings] { projectOptions(id, settings); });
			groups[id] = row;
		}
	for (const auto& value : m_history->entries())
	{
		const auto row = value.toObject();
		QString group = row["group"].toString();
		if (group == "side:record") group = "record";
		if (group.startsWith("side:edit:"))
		{
			const auto path = group.mid(10);
			const auto match = std::find_if(m_projects.cbegin(), m_projects.cend(), [&path](const auto& entry)
			{
				const auto item = entry.toObject();
				return item["project"] == path || item["video"] == path;
			});
			if (match == m_projects.cend()) continue;
			group = "edit:" + (*match).toObject()["id"].toString();
		}
		auto parent = groups.value(group.mid(5));
		if (m_editing ? !group.startsWith("edit:") || !parent : group != "record") continue;
		const auto id = row["id"].toString();
		auto entry = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_tree);
		entry->setText(0, KRRAssistantHistory::title(row));
		entry->setData(0, Id, id);
		if (m_history->busy(id))
		{
			auto loading = new QProgressBar(m_tree);
			loading->setRange(0, 0);
			loading->setFixedHeight(5);
			loading->setTextVisible(false);
			loading->setToolTip(Ui("正在处理"));
			m_tree->setItemWidget(entry, 2, loading);
		}
		else if (row["unread"].toBool())
		{
			entry->setText(2, QStringLiteral("●"));
			entry->setForeground(2, QColor("#fa6575"));
		}
		if (id == m_chat->conversationId()) selected = entry;
	}
	if (selected) m_tree->setCurrentItem(selected);
}

void KRRStudioAssistant::addProject()
{
	QMenu menu(this);
	auto saved = menu.addAction(RecorderUi::Icon("folder"), Ui("选择剪辑工程…"));
	auto folder = menu.addAction(RecorderUi::Icon("folder"), Ui("选择工作区文件夹…"));
	QAction* current = nullptr;
	if (!m_editor->inputFile().isEmpty()) current = menu.addAction(Ui("使用当前剪辑工程"));
	const auto chosen = menu.exec(m_add->mapToGlobal(QPoint(0, m_add->height())));
	if (chosen == saved) registerProject(PickStudioProject(this, m_folder(), m_editor->projectPath()), false);
	else if (chosen == folder) registerProject(QFileDialog::getExistingDirectory(this, Ui("选择工作区文件夹"), m_folder()), true);
	else if (chosen && chosen == current)
		registerProject(m_editor->projectPath().isEmpty() ? m_editor->inputFile() : m_editor->projectPath(), false);
}

QJsonObject KRRStudioAssistant::project(const QString& id) const
{
	for (const auto& value : m_projects)
		if (value.toObject()["id"] == id) return value.toObject();
	return {};
}

QString KRRStudioAssistant::linkedProject() const
{
	return project(m_project)["project"].toString();
}

void KRRStudioAssistant::registerProject(QString path, bool directory)
{
	if (path.isEmpty()) return;
	const auto info = QFileInfo(path);
	path = info.absoluteFilePath();
	const bool video = !directory && IsVideo(path);
	for (const auto& value : m_projects)
	{
		const auto item = value.toObject();
		if (item[directory ? "root" : video ? "video" : "project"].toString().compare(path, Qt::CaseInsensitive) == 0)
		{
			selectProject(item["id"].toString());
			return;
		}
	}
	QStringList files = video ? QStringList{path} : directory ? QStringList{} : ProjectFiles(path);
	if (!directory && !video && files.isEmpty())
	{
		QMessageBox::warning(this, Ui("新建项目"), Ui("无法读取工程，或工程缺少可用素材。"));
		return;
	}
	const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
	QJsonArray assets;
	for (const auto& file : files) if (IsVideo(file)) assets.append(QJsonObject{{"path", file}, {"selected", true}});
	m_projects.append(QJsonObject{{"id", id}, {"name", directory ? info.fileName() : DisplayProject(path)},
		{"root", directory ? path : info.absolutePath()}, {"project", !directory && !video ? path : QString{}},
		{"video", video ? path : QString{}}, {"folders", QJsonArray{}}, {"materials", assets}});
	saveProjects();
	selectProject(id);
}

void KRRStudioAssistant::selectProject(const QString& id)
{
	if (id == m_project || project(id).isEmpty()) return;
	m_project = id;
	m_editThread.clear();
	m_candidateHost = nullptr;
	while (m_tabs->count()) RemoveTab(m_tabs, 0);
	addBlankPage();
	selectMode(true);
}

void KRRStudioAssistant::projectOptions(const QString& id, QPushButton* anchor)
{
	QMenu menu(this);
	auto change = menu.addAction(Ui("更改工作区…"));
	auto append = menu.addAction(Ui("添加工作区文件夹…"));
	auto open = menu.addAction(Ui("进入剪辑工坊"));
	auto remove = menu.addAction(RecorderUi::Icon("trash"), Ui("删除项目"));
	const auto item = project(id);
	open->setEnabled(!item["project"].toString().isEmpty() || !item["video"].toString().isEmpty());
	const auto chosen = menu.exec(anchor->mapToGlobal(QPoint(0, anchor->height())));
	if (!chosen) return;
	if (chosen == open)
	{
		if (!item["project"].toString().isEmpty()) emit projectRequested(item["project"].toString());
		else emit videoRequested(item["video"].toString());
		return;
	}
	if (chosen == remove)
	{
		if (QMessageBox::question(this, Ui("删除项目"), Ui("移除此助手项目？工程和文件会保留。")) != QMessageBox::Yes) return;
		for (int i = 0; i < m_projects.size(); ++i)
			if (m_projects[i].toObject()["id"] == id) { m_projects.removeAt(i); break; }
		for (const auto& value : m_history->entries())
			if (value.toObject()["group"] == "edit:" + id)
				m_history->setArchived(value.toObject()["id"].toString(), true);
		saveProjects();
		if (m_project == id)
		{
			m_project.clear();
			m_editThread.clear();
			m_candidateHost = nullptr;
			while (m_tabs->count()) RemoveTab(m_tabs, 0);
			selectMode(true);
		}
		updateHistory();
		return;
	}
	const auto path = QFileDialog::getExistingDirectory(this, chosen == change ? change->text() : append->text(), item["root"].toString());
	if (path.isEmpty()) return;
	auto updated = item;
	if (chosen == change) updated["root"] = path;
	else
	{
		auto folders = updated["folders"].toArray();
		if (folders.contains(path) || updated["root"] == path) return;
		folders.append(path);
		updated["folders"] = folders;
	}
	updateProject(updated);
	if (m_project == id)
		for (int i = 0; i < m_tabs->count(); ++i)
			if (m_tabs->widget(i)->property("page") == "workspace") openPage(i, "workspace");
}

void KRRStudioAssistant::updateProject(const QJsonObject& item)
{
	for (int i = 0; i < m_projects.size(); ++i)
		if (m_projects[i].toObject()["id"] == item["id"])
		{
			m_projects.replace(i, item);
			break;
		}
	saveProjects();
}

void KRRStudioAssistant::saveProjects()
{
	QSaveFile file(DataDirectory() + "/assistant-projects.json");
	const auto content = QJsonDocument(m_projects).toJson(QJsonDocument::Compact);
	if (!file.open(QIODevice::WriteOnly) || file.write(content) != content.size() || !file.commit())
		QMessageBox::warning(this, Ui("剪辑助手"), Ui("无法保存助手项目。"));
}

QJsonArray KRRStudioAssistant::materials() const
{
	return project(m_project)["materials"].toArray();
}

void KRRStudioAssistant::setMaterials(QJsonArray assets)
{
	auto item = project(m_project);
	if (item.isEmpty()) return;
	item["materials"] = std::move(assets);
	updateProject(item);
	refreshEditing();
}

void KRRStudioAssistant::refreshEditing()
{
	QStringList selected;
	KRREditRange range;
	for (const auto& value : materials())
	{
		const auto item = value.toObject();
		if (!item["selected"].toBool() || !QFileInfo(item["path"].toString()).isFile()) continue;
		selected << item["path"].toString();
		range = {item["begin"].toVariant().toLongLong(), item["end"].toVariant().toLongLong()};
	}
	m_chat->setEditingContext(selected.size() == 1 ? selected.front() : QString{}, true);
	m_chat->setMaterials(selected, selected.size() == 1 ? range : KRREditRange{});
}

void KRRStudioAssistant::addBlankPage()
{
	const int index = m_tabs->addTab(new QWidget(m_tabs), QString{});
	openPage(index, {});
}

void KRRStudioAssistant::openPage(int index, const QString& page, const QString& argument)
{
	QWidget* panel = nullptr;
	QString title;
	if (page == "workspace") { panel = workspacePage(); title = Ui("工作区"); }
	else if (page == "materials") { panel = materialsPage(); title = Ui("素材"); }
	else if (page == "preview") { panel = videoPage(argument); title = Ui("视频预览"); }
	else if (page == "terminal") { panel = terminalPage(); title = Ui("CLI 终端"); }
	else
	{
		panel = new QWidget(m_tabs);
		title = Ui("空白页");
		auto layout = new QVBoxLayout(panel);
		SetPanelMargins(layout);
		for (const auto& choice : QList<QPair<QString, QString>>{{"workspace", Ui("工作区")}, {"materials", Ui("素材")},
			{"preview", Ui("视频预览")}, {"terminal", Ui("CLI 终端")}})
		{
			auto button = Button(panel, choice.second);
			button->setObjectName("studioAiPage-" + choice.first);
			layout->addWidget(button);
			QObject::connect(button, &QPushButton::clicked, panel, [this, panel, key = choice.first] { openPage(m_tabs->indexOf(panel), key); });
		}
		layout->addStretch();
	}
	const auto old = m_tabs->widget(index);
	panel->setProperty("page", page);
	if (old)
	{
		if (m_candidateHost && old->isAncestorOf(m_candidateHost) && page != "materials") m_candidateHost = nullptr;
		m_tabs->removeTab(index);
		old->deleteLater();
	}
	index = m_tabs->insertTab(index, panel, title);
	m_tabs->setCurrentIndex(index);
}

QWidget* KRRStudioAssistant::workspacePage()
{
	auto page = new QWidget(m_tabs);
	auto layout = new QVBoxLayout(page);
	SetPanelMargins(layout);
	auto filter = new QLineEdit(page);
	filter->setPlaceholderText(Ui("筛选文件…"));
	filter->setObjectName("studioWorkspaceFilter");
	auto tree = new QTreeWidget(page);
	tree->setObjectName("studioWorkspaceFiles");
	tree->setHeaderHidden(true);
	tree->setIndentation(RecorderUi::Px(14));
	tree->setUniformRowHeights(true);
	tree->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	layout->addWidget(filter);
	layout->addWidget(tree, 1);
	const auto item = project(m_project);
	QStringList roots;
	if (!item["root"].toString().isEmpty()) roots << item["root"].toString();
	for (const auto& folder : item["folders"].toArray()) roots << folder.toString();
	QStringList links;
	const auto json = item["project"].toString();
	if (!json.isEmpty()) links << json << ProjectFiles(json);
	for (const auto& entry : materials()) links << entry.toObject()["path"].toString();
	roots.removeDuplicates();
	links.removeDuplicates();
	auto populate = [tree, roots, links]
	{
		tree->clear();
		for (const auto& path : roots)
		{
			const QFileInfo file(path);
			tree->addTopLevelItem(FileItem(file, file.fileName().isEmpty() ? QDir::toNativeSeparators(path) : file.fileName()));
		}
		QStringList outside;
		for (const auto& path : links)
			if (!std::any_of(roots.cbegin(), roots.cend(), [&path](const auto& root)
			{
				return path.startsWith(QDir(root).absolutePath() + "/", Qt::CaseInsensitive);
			})) outside << path;
		if (!outside.isEmpty())
		{
			auto group = new QTreeWidgetItem(tree, {Ui("工程文件")});
			group->setIcon(0, RecorderUi::Icon("folder"));
			for (const auto& path : outside) group->addChild(FileItem(QFileInfo(path)));
			group->setExpanded(true);
		}
	};
	QObject::connect(tree, &QTreeWidget::itemExpanded, page, [filter](QTreeWidgetItem* item)
	{
		if (item->childCount() || !filter->text().trimmed().isEmpty()) return;
		const auto path = item->data(0, Id).toString();
		if (path.isEmpty() || !QFileInfo(path).isDir()) return;
		const auto list = QDir(path).entryInfoList(QDir::AllDirs | QDir::Files | QDir::NoDotAndDotDot | QDir::NoSymLinks,
			QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);
		for (const auto& file : list) item->addChild(FileItem(file));
		if (list.isEmpty()) item->setChildIndicatorPolicy(QTreeWidgetItem::DontShowIndicatorWhenChildless);
	});
	QObject::connect(tree, &QTreeWidget::itemDoubleClicked, page, [this, json](QTreeWidgetItem* item)
	{
		const auto path = item->data(0, Id).toString();
		const QFileInfo file(path);
		if (path.isEmpty() || !file.isFile()) return;
		if (IsVideo(path))
			openPage(m_tabs->addTab(new QWidget(m_tabs), QString{}), "preview", path);
		else if (path == json) emit projectRequested(path);
		else QDesktopServices::openUrl(QUrl::fromLocalFile(path));
	});
	auto debounce = new QTimer(page);
	debounce->setInterval(180);
	debounce->setSingleShot(true);
	QObject::connect(filter, &QLineEdit::textChanged, debounce, qOverload<>(&QTimer::start));
	QObject::connect(debounce, &QTimer::timeout, page, [this, tree, filter, roots, links, populate]
	{
		const auto query = filter->text().trimmed();
		if (query.isEmpty()) { populate(); return; }
		const auto generation = tree->property("searchId").toInt() + 1;
		tree->setProperty("searchId", generation);
		tree->clear();
		QPointer<QTreeWidget> live(tree);
		QPointer<QLineEdit> search(filter);
		QThreadPool::globalInstance()->start(QRunnable::create([live, search, roots, links, query, generation]
		{
			QStringList results;
			for (const auto& link : links)
				if (QFileInfo(link).fileName().contains(query, Qt::CaseInsensitive)) results << link;
			for (const auto& root : roots)
			{
				QDirIterator files(root, QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDirIterator::Subdirectories);
				while (results.size() < 500 && files.hasNext())
				{
					const auto path = files.next();
					if (files.fileName().contains(query, Qt::CaseInsensitive)) results << path;
				}
			}
			results.removeDuplicates();
			QMetaObject::invokeMethod(QCoreApplication::instance(), [live, search, query, generation, results]
			{
				if (!live || !search || search->text().trimmed() != query || live->property("searchId").toInt() != generation) return;
				live->clear();
				if (results.isEmpty()) new QTreeWidgetItem(live, {Ui("没有匹配的文件")});
				for (const auto& path : results)
				{
					const QFileInfo file(path);
					live->addTopLevelItem(FileItem(file, file.fileName() + "  ·  " + QDir::toNativeSeparators(file.absolutePath())));
				}
			}, Qt::QueuedConnection);
		}));
	});
	populate();
	if (tree->topLevelItemCount()) tree->topLevelItem(0)->setExpanded(true);
	return page;
}

QWidget* KRRStudioAssistant::materialsPage()
{
	auto page = new QWidget(m_tabs);
	auto layout = new QVBoxLayout(page);
	SetPanelMargins(layout);
	auto tools = new QHBoxLayout;
	auto add = Button(page, Ui("添加素材…"), "folder");
	auto mark = Button(page, {}, "flag", Ui("标注素材与片段"));
	auto remove = Button(page, {}, "trash", Ui("从项目移除素材"));
	auto edit = Button(page, {}, "scissors", Ui("进入剪辑工坊"));
	add->setEnabled(!m_project.isEmpty());
	for (auto button : {add, mark, remove, edit}) tools->addWidget(button);
	tools->addStretch(1);
	layout->addLayout(tools);
	auto list = new QListWidget(page);
	list->setObjectName("studioAiMaterials");
	list->setSelectionMode(QAbstractItemView::SingleSelection);
	layout->addWidget(list, 1);
	const auto projectId = m_project;
	auto update = [this, list]
	{
		const QSignalBlocker block(list);
		const int current = list->currentRow();
		list->clear();
		for (const auto& value : materials())
		{
			const auto item = value.toObject();
			const auto path = item["path"].toString();
			const auto label = item["label"].toString();
			const auto begin = item["begin"].toVariant().toLongLong(), end = item["end"].toVariant().toLongLong();
			auto row = new QListWidgetItem(RecorderUi::Icon(label.isEmpty() ? "screen" : "flag"),
				(label.isEmpty() ? QFileInfo(path).fileName() : label) + (end > begin ? " · " + EditTime(begin) + "–" + EditTime(end) : QString{}), list);
			row->setData(Id, path);
			row->setToolTip(path);
			row->setFlags(row->flags() | Qt::ItemIsUserCheckable);
			row->setCheckState(item["selected"].toBool() ? Qt::Checked : Qt::Unchecked);
		}
		if (list->count()) list->setCurrentRow(qBound(0, current, list->count() - 1));
	};
	QObject::connect(add, &QPushButton::clicked, page, [this, projectId, update]
	{
		const auto files = QFileDialog::getOpenFileNames(this, Ui("添加素材"), project(projectId)["root"].toString(), VideoFilter());
		if (files.isEmpty()) return;
		auto rows = materials();
		for (const auto& path : files)
			if (std::none_of(rows.cbegin(), rows.cend(), [&path](const auto& v) { return v.toObject()["path"] == path; }))
				rows.append(QJsonObject{{"path", path}, {"selected", true}});
		setMaterials(rows);
		update();
	});
	QObject::connect(list, &QListWidget::itemChanged, page, [this, list](QListWidgetItem* item)
	{
		auto rows = materials();
		const int index = list->row(item);
		if (index < 0 || index >= rows.size()) return;
		auto value = rows[index].toObject();
		value["selected"] = item->checkState() == Qt::Checked;
		rows.replace(index, value);
		setMaterials(rows);
	});
	QObject::connect(list, &QListWidget::itemDoubleClicked, page, [this](QListWidgetItem* item)
	{
		openPage(m_tabs->addTab(new QWidget(m_tabs), {}), "preview", item->data(Id).toString());
	});
	QObject::connect(mark, &QPushButton::clicked, page, [this, list, update]
	{
		const int index = list->currentRow();
		auto rows = materials();
		if (index < 0 || index >= rows.size()) return;
		auto asset = rows[index].toObject();
		QDialog dialog(this);
		dialog.setWindowTitle(Ui("标注片段"));
		auto form = new QFormLayout(&dialog);
		auto name = new QLineEdit(asset["label"].toString(), &dialog);
		name->setMaxLength(120);
		form->addRow(Ui("名称"), name);
		auto time = [&](const QString& title, const QString& key)
		{
			auto value = new QDoubleSpinBox(&dialog);
			value->setDecimals(3);
			value->setRange(0, 86400);
			value->setSuffix(" s");
			value->setValue(asset[key].toDouble() / 1000.);
			form->addRow(title, value);
			return value;
		};
		auto begin = time(Ui("开始"), "begin");
		auto end = time(Ui("结束"), "end");
		auto buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
		form->addRow(buttons);
		QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&]
		{
			if (end->value() != 0 && end->value() <= begin->value())
			{
				end->setFocus();
				return;
			}
			dialog.accept();
		});
		QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
		if (dialog.exec() != QDialog::Accepted) return;
		asset["label"] = name->text().trimmed();
		asset["begin"] = static_cast<double>(qRound64(begin->value() * 1000));
		asset["end"] = static_cast<double>(qRound64(end->value() * 1000));
		rows.replace(index, asset);
		setMaterials(rows);
		update();
	});
	QObject::connect(remove, &QPushButton::clicked, page, [this, list, update]
	{
		const int index = list->currentRow();
		auto rows = materials();
		if (index < 0 || index >= rows.size()) return;
		rows.removeAt(index);
		setMaterials(rows);
		update();
	});
	QObject::connect(edit, &QPushButton::clicked, page, [this, list]
	{
		const auto associated = linkedProject();
		if (!associated.isEmpty()) emit projectRequested(associated);
		else if (list->currentItem()) emit videoRequested(list->currentItem()->data(Id).toString());
	});
	update();
	m_candidateHost = new QWidget(page);
	m_candidateHost->setObjectName("studioAiCandidates");
	auto candidates = new QVBoxLayout(m_candidateHost);
	candidates->setContentsMargins(0, 0, 0, 0);
	layout->addWidget(m_candidateHost, 4);
	m_candidateHost->hide();
	return page;
}

QWidget* KRRStudioAssistant::candidateHost()
{
	for (int i = 0; i < m_tabs->count(); ++i)
		if (m_tabs->widget(i)->property("page") == "materials")
		{
			m_tabs->setCurrentIndex(i);
			m_candidateHost = m_tabs->widget(i)->findChild<QWidget*>("studioAiCandidates");
			return m_candidateHost;
		}
	openPage(m_tabs->currentIndex(), "materials");
	return m_candidateHost;
}

QWidget* KRRStudioAssistant::videoPage(const QString& original)
{
	auto page = new QWidget(m_tabs);
	auto layout = new QVBoxLayout(page);
	SetPanelMargins(layout);
	auto row = new QHBoxLayout;
	auto sources = new QComboBox(page);
	sources->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
	for (const auto& value : materials())
	{
		const auto file = value.toObject()["path"].toString();
		if (QFileInfo(file).isFile()) sources->addItem(QFileInfo(file).fileName(), file);
	}
	if (!original.isEmpty())
	{
		if (sources->findData(original) < 0) sources->addItem(QFileInfo(original).fileName(), original);
		sources->setCurrentIndex(sources->findData(original));
	}
	auto browse = Button(page, {}, "folder", Ui("打开视频"));
	row->addWidget(sources, 1);
	row->addWidget(browse);
	layout->addLayout(row);
	auto video = new KRRPreviewVideo(page);
	auto player = new QMediaPlayer(video);
	video->attach(player);
	layout->addWidget(video, 1);
	auto compatibility = new KRRCompatiblePreview;
	compatibility->setParent(player);
	auto play = Button(page, {}, "play", Ui("播放 / 暂停"));
	auto seek = new KRRPlaybackSlider(page);
	auto status = new QLabel(page);
	status->setWordWrap(true);
	auto transport = new QHBoxLayout;
	transport->addWidget(play);
	transport->addWidget(seek, 1);
	layout->addLayout(transport);
	layout->addWidget(status);
	BindPlaybackSeek(seek, player);
	QObject::connect(player, &QMediaPlayer::durationChanged, seek, [seek](qint64 duration) { seek->setRange(0, static_cast<int>(qMin<qint64>(INT_MAX, duration))); });
	QObject::connect(player, &QMediaPlayer::positionChanged, seek, [seek](qint64 at) { if (!seek->isSliderDown()) seek->setValue(static_cast<int>(at)); });
	QObject::connect(play, &QPushButton::clicked, player, [player] { player->state() == QMediaPlayer::PlayingState ? player->pause() : player->play(); });
	QObject::connect(player, &QMediaPlayer::stateChanged, play, [play](auto state) { play->setIcon(RecorderUi::Icon(state == QMediaPlayer::PlayingState ? "pause" : "play")); });
	QObject::connect(compatibility, &KRRCompatiblePreview::ready, player, [player, status](const QString& file)
	{
		status->clear();
		player->setMedia(QUrl::fromLocalFile(file));
	});
	QObject::connect(compatibility, &KRRCompatiblePreview::failed, status, &QLabel::setText);
	auto load = [sources, player, status, compatibility, video]
	{
		player->stop();
		player->setMedia({});
		compatibility->clear();
		video->clear();
		const auto file = sources->currentData().toString();
		status->setText(file.isEmpty() ? Ui("选择视频") : QString{});
		if (file.isEmpty()) return;
		if (NeedsCompatiblePreview(file))
		{
			status->setText(Ui("正在准备兼容预览…"));
			compatibility->prepare(file);
		}
		else player->setMedia(QUrl::fromLocalFile(file));
	};
	QObject::connect(sources, qOverload<int>(&QComboBox::currentIndexChanged), page, load);
	QObject::connect(player, qOverload<QMediaPlayer::Error>(&QMediaPlayer::error), page, [player, status, sources, compatibility](auto error)
	{
		if (error == QMediaPlayer::NoError) return;
		if (compatibility->retryWithEncoding()) return;
		if (compatibility->file().isEmpty() && !compatibility->isPreparing() && !sources->currentData().toString().isEmpty())
		{
			compatibility->prepare(sources->currentData().toString());
			return;
		}
		status->setText(player->errorString());
	});
	QObject::connect(browse, &QPushButton::clicked, page, [this, sources]
	{
		const auto file = QFileDialog::getOpenFileName(this, Ui("打开视频"), m_folder(), VideoFilter());
		if (file.isEmpty()) return;
		sources->addItem(QFileInfo(file).fileName(), file);
		sources->setCurrentIndex(sources->count() - 1);
	});
	QObject::connect(m_tabs, &QTabWidget::currentChanged, player, [this, page, player](int index) { if (m_tabs->widget(index) != page) player->pause(); });
	load();
	return page;
}

QWidget* KRRStudioAssistant::terminalPage()
{
	auto page = new QWidget(m_tabs);
	auto layout = new QVBoxLayout(page);
	SetPanelMargins(layout);
	auto header = new QHBoxLayout;
	auto label = new QLabel("wpsrecorder_cli", page);
	header->addWidget(label, 1);
	auto clear = Button(page, Ui("清屏"));
	auto cancel = Button(page, Ui("取消"));
	cancel->setEnabled(false);
	header->addWidget(cancel);
	header->addWidget(clear);
	layout->addLayout(header);
	auto output = new QPlainTextEdit(page);
	output->setObjectName("studioCliOutput");
	output->setReadOnly(true);
	output->setMaximumBlockCount(2500);
	output->setFont(QFont("Consolas"));
	layout->addWidget(output, 1);
	auto input = new QLineEdit(page);
	input->setObjectName("studioCliInput");
	input->setPlaceholderText("--help");
	layout->addWidget(input);
	auto process = new QProcess(page);
	HideConsoleProcess(*process);
	const auto root = project(m_project)["root"].toString();
	if (QFileInfo(root).isDir()) process->setWorkingDirectory(root);
	QObject::connect(clear, &QPushButton::clicked, output, &QPlainTextEdit::clear);
	QObject::connect(cancel, &QPushButton::clicked, process, &QProcess::kill);
	QObject::connect(process, &QProcess::readyReadStandardOutput, output, [process, output] { output->appendPlainText(QString::fromUtf8(process->readAllStandardOutput()).trimmed()); });
	QObject::connect(process, &QProcess::readyReadStandardError, output, [process, output] { output->appendPlainText(QString::fromUtf8(process->readAllStandardError()).trimmed()); });
	auto idle = [input, cancel] { input->setEnabled(true); input->setFocus(); cancel->setEnabled(false); };
	QObject::connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), page, [output, idle](int code, QProcess::ExitStatus state)
	{
		output->appendPlainText(state == QProcess::CrashExit ? Ui("进程已终止。") : Ui("退出码 %1").arg(code));
		idle();
	});
	QObject::connect(process, &QProcess::errorOccurred, page, [output, process, idle](QProcess::ProcessError error)
	{
		if (error == QProcess::FailedToStart) { output->appendPlainText(process->errorString()); idle(); }
	});
	QObject::connect(input, &QLineEdit::returnPressed, page, [input, output, cancel, process]
	{
		const auto text = input->text().trimmed();
		if (text.isEmpty() || process->state() != QProcess::NotRunning) return;
		auto arguments = QProcess::splitCommand(text);
		if (!arguments.isEmpty() && (arguments.front() == "wpsrecorder_cli" || arguments.front() == "wpsrecorder_cli.exe"))
			arguments.removeFirst();
		output->appendPlainText("> wpsrecorder_cli " + arguments.join(' '));
		input->clear();
		input->setEnabled(false);
		cancel->setEnabled(true);
		process->start(QCoreApplication::applicationDirPath() + "/wpsrecorder_cli.exe", arguments);
		process->closeWriteChannel();
	});
	return page;
}
