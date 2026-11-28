#ifndef WPSRECORDER_WEBSTUDIO_H
#define WPSRECORDER_WEBSTUDIO_H

#include "webstudiosurface.h"
#include "editproject.h"
#include "mediaeditor.h"
#include <QHash>
#include <QPointer>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QTimer>
#include <QFutureWatcher>
#include <QtConcurrentRun>
#include <atomic>
#include <memory>
#include <optional>

class KRRMainWindow;

class KRRWebStudio final : public QObject
{
public:
	explicit KRRWebStudio(KRRMainWindow& window);
	~KRRWebStudio() override;
	void start();
	bool requestFrontendClose();
	bool requestFrontendMinimal();
	bool workInProgress() const;
	bool saveExitDraft();
	void cancelExitWork();
	void startupFile(const QString& path) { m_startupFile = path; }

private:
	template<class Work, class Complete>
	void background(Work work, Complete complete)
	{
		auto watcher = new QFutureWatcher<decltype(work())>(this);
		connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, complete = std::move(complete)]() mutable
		{
			watcher->deleteLater();
			if (m_surface) complete(watcher->result());
		});
		watcher->setFuture(QtConcurrent::run(std::move(work)));
	}
	void dispatch(const QJsonObject& message);
	bool editCommand(const QString& command, const QString& request, const QJsonObject& data);
	bool assistantCommand(const QString& command, const QString& request, const QJsonObject& data);
	void reply(const QString& request, const QJsonValue& result = {}, const QString& error = {});
	void event(const QString& type, const QJsonValue& value);
	void updateFrame();
	void updateStatus();
	void updateMixerLevels();
	QJsonObject snapshot();
	QJsonObject captureTargets() const;
	QJsonArray library();
	QJsonObject libraryEntry(const QString& file);
	QJsonObject inspectMedia(const QString& file, QString& error);
	QJsonObject editView(const KRREditProject& edit, const QString& path, const QString& id);
	QString openMediaDialog(const QString& purpose);
	QString mediaFile(const QString& id) const { return m_media.value(id); }
	QString trackFile(const QString& id) const { return m_tracks.value(id); }
	QString openFolder(const QString& title, const QString& initial = {});
	void applyPreferences(const QJsonObject& data);
	void applyScenes(const QJsonObject& data);
	void applyMixer(const QJsonObject& data);
	bool saveDraft();
	KRRMainWindow& m_window;
	KRRWebStudioSurface* m_surface;
	QTimer m_frameTimer, m_statusTimer, m_mixerTimer;
	QThreadPool m_previewWorker;
	bool m_handshake = false, m_encoding = false;
	bool m_skipExitSave = false, m_exitSyncFailed = false;
	bool m_voiceJob = false;
	std::optional<Qt::WindowStates> m_previewWindowState;
	qint64 m_lastFrame = 0;
	quint64 m_previewGeneration = 0;
	QString m_previewNotice;
	QString m_lastStatus, m_lastMessage, m_startupFile, m_editId, m_projectPath;
	QJsonObject m_mixerLevels;
	QHash<QString, QString> m_media, m_tracks, m_projects, m_workspaces;
	QHash<QString, QJsonObject> m_inspected;
	QHash<QString, std::shared_ptr<std::atomic<bool>>> m_assistantJobs;
	std::shared_ptr<std::atomic<bool>> m_backgroundCancel = std::make_shared<std::atomic<bool>>(false);
	std::shared_ptr<std::atomic<bool>> m_modelCheck;
	KRREditProject m_edit;
	KRRMediaEditor m_jobs;
	QTemporaryDir m_cache;
	QJsonObject m_pendingEdit;
};

#endif
