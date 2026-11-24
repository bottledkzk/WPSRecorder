#ifndef WPSRECORDER_SUBTITLEREVIEWDIALOG_H
#define WPSRECORDER_SUBTITLEREVIEWDIALOG_H

#include "editproject.h"

class KRRSubtitleDialog;

bool ShowSubtitleReview(KRRSubtitleDialog* parent, const KRREditProject& project, int currentRow,
	const QString& target, KRREditProject& result, int& firstChanged);

#endif
