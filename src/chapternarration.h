#ifndef WPSRECORDER_CHAPTERNARRATION_H
#define WPSRECORDER_CHAPTERNARRATION_H

#include "editproject.h"

constexpr int MaxChapterNarrationLines = 512;

bool ReadChapterNarration(const KRREditProject& project, const QString& script,
	QVector<KRREditCaption>& lines, QString& error);
bool PlaceChapterNarration(const KRREditProject& project, QVector<KRREditCaption>& lines,
	const QVector<qint64>& durations, QString& error);
QString ChapterNarrationScript(const KRREditProject& project);
QString ChapterNarrationTitle(const KRREditProject& project, qint64 at);

struct KRRNarrationDucking
{
	QVector<KRREditRange> speech;
	double reducedGain = 1;
	double gainAt(qint64 sourceTime) const;
	QString filter(KRREditRange segment) const;
};
KRRNarrationDucking NarrationDucking(const KRREditProject& project);

#endif
