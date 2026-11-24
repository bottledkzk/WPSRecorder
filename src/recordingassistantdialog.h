#ifndef WPSRECORDER_RECORDINGASSISTANTDIALOG_H
#define WPSRECORDER_RECORDINGASSISTANTDIALOG_H

#include "recordingassistant.h"
#include <QDialog>
#include <QPointer>
#include "assistanthistory.h"
#include <functional>
#include <thread>

class QLabel;
class QComboBox;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;
class QSpinBox;

class KRRRecordingAssistantDialog : public QDialog
{
public:
	using Snapshot = std::function<KRRAssistantContext()>;
	using Apply = std::function<bool(const QJsonObject& before, const QJsonObject& changes, QString& error)>;

	enum class Presentation { Dialog, Sidebar, Workspace };
	KRRRecordingAssistantDialog(Snapshot snapshot, Apply apply, QWidget* parent,
		Presentation presentation = Presentation::Dialog);
	~KRRRecordingAssistantDialog() override;
	void setEditingContext(const QString& input, bool editing);
	void bindConversation(KRRAssistantHistory* history, const QString& id);
	void setCandidateHost(QWidget* host) { m_candidateHost = host; }
	QWidget* candidateHost() const { return m_candidateHost; }
	void setMaterials(const QStringList& files, KRREditRange range = {});
	QString conversationId() const { return m_conversationId; }
	QString selectedRecording;
	QStringList selectedMontage;
	qint64 selectedTime = -1;
	std::optional<KRRAssistantCut> selectedCut;
	bool requestedCheck = false;
	bool openAsReview = false;

protected:
	void done(int result) override;

private:
	void send();
	void runRequest(bool compactOnly);
	void displayConversation();
	void startSuggestion(int index);
	void showReply(const QString& prompt, const KRRAssistantReply& reply);
	void clearActions();
	void updateProvider();
	void openRecording();
	void applyPreview();
	void setBusy(bool busy);
	void fillTemplate(int index);
	void cutCurrentMedia();

	Snapshot m_snapshot;
	Apply m_apply;
	QJsonObject m_before, m_changes;
	QJsonArray m_conversation;
	QPointer<KRRAssistantHistory> m_history;
	QString m_conversationId, m_workingConversation;
	QStringList m_materials;
	KRREditRange m_materialRange;
	bool m_requiresProject = false;
	std::optional<KRRAssistantCut> m_pendingCut;
	Presentation m_presentation;
	QString m_editingInput;
	bool m_editing = false;
	QString m_providerId;
	std::thread m_worker;
	std::atomic<bool> m_cancel{false};
	bool m_busy = false;
	QLineEdit* m_prompt = nullptr;
	QPlainTextEdit* m_transcript = nullptr;
	QLabel *m_provider = nullptr, *m_status = nullptr;
	QListWidget* m_recordings = nullptr;
	QTableWidget* m_diff = nullptr;
	QPushButton *m_configure = nullptr, *m_send = nullptr, *m_stop = nullptr,
		*m_open = nullptr, *m_confirm = nullptr, *m_check = nullptr;
	QList<QPushButton*> m_templates;
	QWidget* m_cutParameters = nullptr;
	QPointer<QWidget> m_candidateHost;
	QPushButton* m_compact = nullptr;
	QWidget* m_composer = nullptr;
	QPushButton* m_pickProject = nullptr;
public:
	std::function<void()> chooseProject;
	std::function<void()> dismissSidebar;
private:
	QComboBox *m_scope = nullptr, *m_task = nullptr;
	QSpinBox *m_maxSeconds = nullptr, *m_scopeStart = nullptr, *m_scopeEnd = nullptr;
	QPushButton* m_cutCurrent = nullptr;
};

#endif
