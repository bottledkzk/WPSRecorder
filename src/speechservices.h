#ifndef WPSRECORDER_SPEECHSERVICES_H
#define WPSRECORDER_SPEECHSERVICES_H

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <atomic>
#include <functional>

struct KRRVoiceOptions;

struct KRRSpeechService
{
	QString endpoint, model;
	QByteArray key;
	QString protocol = "openai";
	QString voice, requestTemplate, responsePath;
	QString audioEncoding = "binary";
	QString credentialHeader = "Authorization", credentialPrefix = "Bearer ";
	QString label;
};

KRRSpeechService SpeechService(const QString& id);
bool SaveSpeechService(const QString& id, const KRRSpeechService& service, bool clearCredential, QString& error);
bool RemoveTtsService(const QString& id, QString& error);
bool IsTtsService(const QString& id);
QStringList TtsServiceIds();
QString SpeechServiceFingerprint(const KRRSpeechService& service);
QString ValidateSpeechService(const QString& id, const KRRSpeechService& service);
QString CheckSpeechService(const QString& id, KRRSpeechService service, const std::atomic<bool>* cancel);
QString Audio8Directory();
void SetAudio8Directory(const QString& directory);
bool Audio8Available(const QString& directory = {});
QStringList Audio8Voices();
QString SpeechServiceHost(const QString& id);
QString DefaultVoice(const QString& engine, const QString& language);
QString RequestVideoReview(const KRRSpeechService& service, QJsonObject payload,
						   QJsonObject& result, const std::atomic<bool>* cancel);
QString RequestAssistantReply(const KRRSpeechService& service, QJsonObject payload,
							   QJsonObject& result, const std::atomic<bool>* cancel);
QString SynthesizeWithService(const QString& text, const QString& output, const KRRVoiceOptions& options,
							  const std::atomic<bool>* cancel, const KRRSpeechService* overrideService = nullptr, int timeout = 90000);
QString TranslateSubtitleLines(const QVector<QString>& source, const QString& targetLanguage, const QString& glossary,
							   QVector<QString>& translations, const std::atomic<bool>* cancel,
							   const std::function<void(int, int)>& progress = {}, int only = -1,
							   const KRRSpeechService* overrideService = nullptr, int timeout = 90000);

#endif
