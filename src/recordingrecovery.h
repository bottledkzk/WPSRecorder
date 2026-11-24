#ifndef WPSRECORDER_RECORDINGRECOVERY_H
#define WPSRECORDER_RECORDINGRECOVERY_H

#include <QDateTime>
#include <QStringList>
#include <QVector>
#include <atomic>

struct KRRRecoveryItem
{
	QString workspace;
	QString name;
	QString stage;
	QString error;
	QStringList audio;
	bool hasCamera = false;
	QDateTime modified;
};

void WriteRecordingRecovery(const QString& workspace, const QString& output, const QString& stage, const QString& error = {});
QVector<KRRRecoveryItem> RecordingRecoveryItems(const QString& directory);
QString RecoverRecording(const QString& workspace, const QString& output, const std::atomic<bool>* cancel = nullptr);

#endif
