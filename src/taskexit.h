#ifndef WPSRECORDER_TASKEXIT_H
#define WPSRECORDER_TASKEXIT_H

#include "uilanguage.h"
#include <QMessageBox>
#include <QPushButton>

enum class KRRTaskExit { Return, Save, Discard, Background };

inline KRRTaskExit ChooseTaskExit(QWidget* parent, bool saveFailed = false, bool offerBackground = false,
	bool busy = true)
{
	QMessageBox dialog(busy || saveFailed ? QMessageBox::Warning : QMessageBox::Question, Ui("退出 WPS Recorder"),
		saveFailed ? Ui("工程无法保存。仍要退出吗？") :
		busy ? Ui("仍有任务正在运行。退出会取消未完成的任务，已经保存的文件会保留。") :
			Ui("关闭工作台后是否转入后台？"),
		QMessageBox::NoButton, parent);
	dialog.setObjectName("exitTasksDialog");
	auto back = dialog.addButton(Ui("返回"), QMessageBox::RejectRole);
	QPushButton* discard = nullptr;
	QPushButton* background = nullptr;
	if (offerBackground)
	{
		background = dialog.addButton(Ui("后台运行"), QMessageBox::ActionRole);
		background->setObjectName("runInBackground");
	}
	if (busy || saveFailed)
	{
		discard = dialog.addButton(Ui("放弃任务并退出"), QMessageBox::DestructiveRole);
		discard->setObjectName("discardTasksAndExit");
	}
	QPushButton* save = nullptr;
	if (!saveFailed)
	{
		save = dialog.addButton(busy ? Ui("保存工程并退出") : Ui("退出"), QMessageBox::AcceptRole);
		save->setObjectName("saveProjectAndExit");
	}
	dialog.setDefaultButton(back);
	dialog.setEscapeButton(back);
	dialog.exec();
	if (background && dialog.clickedButton() == background) return KRRTaskExit::Background;
	if (discard && dialog.clickedButton() == discard) return KRRTaskExit::Discard;
	if (save && dialog.clickedButton() == save) return KRRTaskExit::Save;
	return KRRTaskExit::Return;
}

#endif
