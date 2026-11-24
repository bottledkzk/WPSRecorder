#ifndef WPSRECORDER_SMARTCUT_H
#define WPSRECORDER_SMARTCUT_H

#include "editproject.h"
#include <QMetaType>
#include <functional>

struct KRRSpeechService;

constexpr int MaxSmartCutQueryChars = 600;
constexpr int MaxSequenceGoalChars = 1500;
constexpr int SequenceDiscoveryFps = 4;
constexpr int SequenceDetailFps = 8;
constexpr int MaxSequenceSampleFps = 10;
constexpr int DefaultSequenceCandidates = 5;
constexpr int MaxSequenceCandidates = 24;
constexpr qint64 DefaultSequenceTargetMs = 45000;
constexpr qint64 MaxSequenceTargetMs = 86400000;

struct KRRSequenceCutOptions
{
	qint64 targetMs = -1; // Follow the goal when omitted. Zero leaves the duration open.
	int candidatesPerSource = 0; // Follow the goal when omitted.
	int sampleFps = 0; // Zero uses discovery, then a denser review of candidate regions.
};

struct KRRSmartCutOptions
{
	QString mode = "highlights", query;
	KRREditRange scope;
	qint64 maxDuration = 45000; // Zero means no duration target.
	bool useCaptions = true, useMarkers = true;
};

struct KRRSmartCutPart
{
	KRREditRange range;
	qint64 evidence = 0;
	QString title, reason;
	int score = 0;
	bool selected = false;
	bool reviewed = false;
};

struct KRRSmartCutDraft
{
	KRRSmartCutOptions options;
	QVector<KRRSmartCutPart> parts;
	QString revision, model;
	qint64 inputTokens = -1, outputTokens = -1, visualTokens = -1, proxyBytes = 0;
	qint64 preparationMs = 0, requestMs = 0;
	int requests = 0;
	int hardwareSamples = 0;
	int rejectedEvidence = 0, detailRequests = 0, failedReviews = 0, audioHints = 0;
	bool addChapters = false;
	QJsonObject toJson() const;
	static bool fromJson(const QJsonObject& object, KRRSmartCutDraft& draft, QString& error);
};
Q_DECLARE_METATYPE(KRRSmartCutDraft)

QString SmartCutRevision(const KRREditProject& project);
QVector<int> SearchEditCaptions(const QVector<KRREditCaption>& captions, const QString& query);
QString SuggestSmartCut(const KRREditProject& project, const KRRSmartCutOptions& options, KRRSmartCutDraft& draft,
						const std::atomic<bool>* cancel = nullptr, const std::function<void(int, int)>& progress = {});
QString SuggestSequenceCutSource(const KRREditProject& project, const KRRSmartCutOptions& options,
	const KRRSequenceCutOptions& sequence, const KRRSpeechService& service, KRRSmartCutDraft& draft,
	const std::atomic<bool>* cancel, const std::function<void(int, int)>& progress,
	const std::function<void(int, int)>& detailProgress);
bool ApplySmartCutDraft(const KRREditProject& project, const KRRSmartCutDraft& draft, KRREditProject& result, QString& error);

#endif
