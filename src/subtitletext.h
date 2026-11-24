#ifndef WPSRECORDER_SUBTITLETEXT_H
#define WPSRECORDER_SUBTITLETEXT_H

#include <QHash>
#include <QRegularExpression>
#include <QString>

struct KRREditCaption;
struct KRREditProject;

struct KRRPronunciations
{
	QRegularExpression pattern;
	QHash<QString, QString> readings;
	QString apply(const QString& text) const;
};

bool ParsePronunciations(const QString& text, KRRPronunciations& result, QString& error);
QString SubtitleSpokenText(const KRREditCaption& line, bool translation, const KRRPronunciations& pronunciations);
bool SubtitleTakeMatches(const KRREditCaption& line, const QString& take, const KRRPronunciations& pronunciations);
void InvalidateStoredSubtitleTake(KRREditProject& project, KRREditCaption& line, const KRRPronunciations& pronunciations);

#endif
