#ifndef WPSRECORDER_RECORDINGASSISTANTTOOLS_H
#define WPSRECORDER_RECORDINGASSISTANTTOOLS_H

#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <atomic>

struct KRRAssistantContext
{
	QString folder;
	QStringList recent;
	QJsonObject settings;
};

QString RecorderUiSettingsPath();
KRRAssistantContext SavedAssistantContext(const QString& folder = {});
void RememberRecordingOutcome(const QJsonObject& status);
QJsonObject AssistantRecordingState();
QJsonObject FindAssistantRecordings(const KRRAssistantContext& context, const QJsonObject& arguments,
	const std::atomic<bool>* cancel = nullptr);
QJsonObject DiagnoseAssistantRecording(const KRRAssistantContext& context, const QJsonObject& arguments);
QJsonObject PreviewAssistantSettings(const QJsonObject& current, const QJsonObject& changes);
QJsonObject AssistantSettingSchema();

#endif
