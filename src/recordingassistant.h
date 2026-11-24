#ifndef WPSRECORDER_RECORDINGASSISTANT_H
#define WPSRECORDER_RECORDINGASSISTANT_H

#include "recordingassistanttools.h"
#include "smartcut.h"
#include "speechservices.h"
#include <optional>

constexpr int MaxAssistantPromptChars = 12000;

struct KRRAssistantCut
{
	KRRSmartCutOptions options;
	bool chapters = false;
	bool automatic = false;
	bool montage = false;
};

struct KRRAssistantReply
{
	QString text, error, model;
	QStringList tools;
	QJsonArray recordings;
	bool recordingResultsTruncated = false;
	QJsonObject diagnosis, preview, editPlan;
	std::optional<KRRAssistantCut> cut;
	QJsonObject toJson() const;
};

KRRAssistantReply AskRecordingAssistant(const QString& prompt, const KRRAssistantContext& context,
	const KRRSpeechService& service, const QJsonArray& conversation = {}, const std::atomic<bool>* cancel = nullptr,
	const QString& summary = {}, const QJsonObject& editing = {});

#endif
