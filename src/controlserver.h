// controlserver.h | 3226535826@qq.com | 2026-10-31 | Per-user local recording control.
#ifndef WPSRECORDER_CONTROLSERVER_H
#define WPSRECORDER_CONTROLSERVER_H
#include "recordingengine.h"
#include <QLocalServer>
#include <QLockFile>
#include <QPointer>
#include <QDateTime>
#include <QJsonArray>
#include <QTimer>
#include <memory>

class KRRControlServer : public QObject
{
	Q_OBJECT
public:
	explicit KRRControlServer(KRRRecordingEngine& engine, QObject* parent = nullptr);
	bool listen(QString& error);
	bool hasScheduledTask() const;
private:
	struct Schedule
	{
		QString id, name, state, error;
		QDateTime at;
		KRRRecordingConfig config;
	};
	void loadSchedules();
	bool saveSchedules(QString& error) const;
	void refreshScheduleTimer();
	void startDueSchedules();
	QJsonArray listSchedules() const;
	void dispatch(class QLocalSocket* socket, const QJsonObject& request);
	KRRRecordingEngine& m_engine;
	QLocalServer m_server;
	std::unique_ptr<QLockFile> m_lock;
	QList<QPointer<class QLocalSocket>> m_stopClients;
	QTimer m_scheduleTimer;
	QList<Schedule> m_schedules;
	QString m_scheduleError;
	QString m_requestedEncoder;
};

bool SendControl(const QJsonObject& request, QJsonObject& response, QString& error, int timeout);
bool EnsureControlService(QString& error, bool requireScheduleQueue = false);
#endif
