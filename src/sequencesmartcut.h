#ifndef WPSRECORDER_SEQUENCESMARTCUT_H
#define WPSRECORDER_SEQUENCESMARTCUT_H

#include "editsequence.h"
#include "smartcut.h"

struct KRRSequenceHighlight
{
	int source = -1;
	KRRSmartCutPart cut;
};

struct KRRSequenceSmartCutDraft
{
	QString revision, goal, model;
	KRRSequenceCutOptions options;
	QVector<KRRSequenceHighlight> highlights;
	QVector<int> withoutHighlights;
	qint64 analyzedMs = 0, elapsedMs = 0, preparationMs = 0, requestMs = 0;
	qint64 inputTokens = -1, outputTokens = -1, visualTokens = 0, proxyBytes = 0;
	int requests = 0, hardwareSamples = 0, rejectedEvidence = 0;
	int detailRequests = 0, failedReviews = 0, audioHints = 0;
	bool editorialPlan = false;
	QJsonObject toJson() const;
	static bool FromJson(const QJsonObject& json, KRRSequenceSmartCutDraft& draft, QString& error);
};
Q_DECLARE_METATYPE(KRRSequenceSmartCutDraft)

// Source media stays local. The existing video provider receives only bounded proxy samples.
QString SuggestSequenceSmartCut(const KRREditSequence& sequence, const QString& goal, const KRRSequenceCutOptions& options,
	KRRSequenceSmartCutDraft& draft,
	const std::atomic<bool>* cancel = nullptr, const std::function<void(const QString&)>& progress = {});
bool BuildSequenceSmartCut(const KRREditSequence& source, const KRRSequenceSmartCutDraft& draft,
	KRREditSequence& result, QString& error);

#endif
