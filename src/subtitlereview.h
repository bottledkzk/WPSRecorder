#ifndef WPSRECORDER_SUBTITLEREVIEW_H
#define WPSRECORDER_SUBTITLEREVIEW_H

#include "editproject.h"
#include <QMetaType>
#include <functional>
#include <optional>

struct KRRSubtitleSuggestion
{
	int row = -1;
	std::optional<QString> translation, narration;
	QString reason;
};

struct KRRSubtitleReview
{
	QString revision, targetLanguage;
	QVector<KRRSubtitleSuggestion> suggestions;
	QJsonObject toJson(const KRREditProject& project) const;
};
Q_DECLARE_METATYPE(KRRSubtitleReview)

QString ReviewSubtitles(const KRREditProject& project, int first, int last, const QString& target,
	const QString& goal, KRRSubtitleReview& review, const std::atomic<bool>* cancel = nullptr,
	const std::function<void(int, int)>& progress = {});
bool ApplySubtitleSuggestions(const KRREditProject& base, const KRRSubtitleReview& review,
	const QVector<int>& selected, KRREditProject& result, QString& error);

#endif
