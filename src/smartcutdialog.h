#ifndef WPSRECORDER_SMARTCUTDIALOG_H
#define WPSRECORDER_SMARTCUTDIALOG_H

#include "smartcut.h"
#include <functional>

class QWidget;
class KRRClipEditor;
bool ReviewSmartCut(KRRClipEditor* owner, const KRREditProject& project, KRREditRange selection,
				   KRREditProject& result, bool& requestSubtitles, const KRRSmartCutOptions* request = nullptr,
				   bool chapters = false);
void DockSmartCut(KRRClipEditor* owner, QWidget* host, const KRREditProject& project, KRREditRange selection,
	const KRRSmartCutOptions& request, bool chapters, std::function<void(const KRREditProject&, bool)> apply);
bool BatchCut(QWidget* owner, const KRREditProject& project, KRREditRange selection, KRREditProject& result);

#endif
