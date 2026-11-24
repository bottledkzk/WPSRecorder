#ifndef WPSRECORDER_STUDIOASSISTANT_H
#define WPSRECORDER_STUDIOASSISTANT_H

#include "editproject.h"
#include <QJsonArray>
#include <QWidget>
#include <functional>

class KRRAssistantHistory;
class KRRClipEditor;
class KRRRecordingAssistantDialog;
class QStackedWidget;
class QTabWidget;
class QTreeWidget;
class QTreeWidgetItem;
class QPushButton;

QString PickStudioProject(QWidget* parent, const QString& folder, const QString& current = {});
void ManageAssistantArchive(QWidget* parent, KRRAssistantHistory* history);

class KRRStudioAssistant : public QWidget
{
	Q_OBJECT
public:
	KRRStudioAssistant(KRRAssistantHistory* history, KRRRecordingAssistantDialog* chat,
		KRRClipEditor* editor, std::function<QString()> folder, QWidget* parent = nullptr);
	QWidget* candidateHost();
	QString linkedProject() const;
	void enter();
	bool isEditing() const { return m_editing; }

signals:
	void projectRequested(const QString& path);
	void videoRequested(const QString& path);

private:
	void selectMode(bool editing);
	void updateHistory();
	void chooseConversation(const QString& id);
	QString historyGroup() const;
	void newConversation(const QString& project = {});
	void addProject();
	void projectOptions(const QString& id, QPushButton* anchor);
	void registerProject(QString path, bool directory);
	QJsonObject project(const QString& id) const;
	void updateProject(const QJsonObject& item);
	void selectProject(const QString& id);
	void saveProjects();
	void refreshEditing();
	void addBlankPage();
	void openPage(int index, const QString& page, const QString& argument = {});
	QWidget* workspacePage();
	QWidget* materialsPage();
	QWidget* videoPage(const QString& file);
	QWidget* terminalPage();
	void setMaterials(QJsonArray materials);
	QJsonArray materials() const;

	KRRAssistantHistory* m_history;
	KRRRecordingAssistantDialog* m_chat;
	KRRClipEditor* m_editor;
	std::function<QString()> m_folder;
	QTreeWidget* m_tree = nullptr;
	QTabWidget* m_tabs = nullptr;
	QWidget* m_candidateHost = nullptr;
	QPushButton* m_add = nullptr;
	QPushButton* m_record = nullptr;
	QPushButton* m_edit = nullptr;
	QJsonArray m_projects;
	QString m_project, m_recordThread, m_editThread;
	bool m_editing = false, m_updatingHistory = false;
};
#endif
