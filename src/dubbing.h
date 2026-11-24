#ifndef WPSRECORDER_DUBBING_H
#define WPSRECORDER_DUBBING_H

#include "editproject.h"

struct KRRDubLine
{
	qint64 begin = 0, end = 0;
	QString audio;
};

qint64 SpeechWaveDuration(const QString& path);
QString MixDubbingTrack(qint64 duration, const QVector<KRRDubLine>& lines, const QString& output,
						const std::atomic<bool>* cancel = nullptr);
QString ExportSubtitleFiles(const KRREditProject& project, const QString& folder);

#endif
