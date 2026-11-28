#ifndef WPSRECORDER_SPEECH_H
#define WPSRECORDER_SPEECH_H
#include <QString>
#include <QVector>
#include <atomic>
struct KRRVoiceOptions
{
	QString engine = "audio8";
	QString voice = "default";
	double speed = 1.0;
	double volume = 1.0;
	QString language = "zh";
};
struct KRRVoiceLine
{
	QString text, output;
};
QString SynthesizeSpeech(const QString &text, const QString &output, const KRRVoiceOptions &options,
						 const std::atomic<bool> *cancel = nullptr);
QString SynthesizeAudio8Speech(const QVector<KRRVoiceLine>& lines, const KRRVoiceOptions& options,
	const std::atomic<bool>* cancel = nullptr);
QString MixNarration(const QString &input, const QString &speech, const QString &output, double begin,
					 double originalVolume = .3, const std::atomic<bool> *cancel = nullptr);
#endif
