#ifndef WPSRECORDER_SUBMISSIONDIALOG_H
#define WPSRECORDER_SUBMISSIONDIALOG_H

#include "editproject.h"
#include <QString>

class QWidget;

struct KRRSubmission
{
	KRREditProject project;
	KRRTimelineExportOptions options;
	QString output;
	bool separateAudio = false;
	bool burnCaptions = true;
};
bool ChooseSubmission(QWidget* parent, const KRREditProject& project, const QString& directory,
					  qint64 previewAt, KRRSubmission& submission);

#endif
