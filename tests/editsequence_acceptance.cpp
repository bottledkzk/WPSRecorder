#include "clipeditor.h"
#include "displaygeometry.h"
#include "editplayer.h"
#include "editsequencedialog.h"
#include "editwidgets.h"
#include "uitheme.h"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMediaPlayer>
#include <QMouseEvent>
#include <QPushButton>
#include <QThread>
#include <QTimer>
#include <cstdio>

namespace
{
bool Wait(const std::function<bool()>& until, int timeout = 5000)
{
	QElapsedTimer clock;
	clock.start();
	while (!until())
	{
		if (clock.elapsed() >= timeout) return false;
		QApplication::processEvents(QEventLoop::AllEvents, 30);
		QThread::msleep(10);
	}
	return true;
}

QPushButton* Button(QWidget* window, const char* name)
{
	return window->findChild<QPushButton*>(name);
}

bool ChooseFiles(QPushButton* button, const QStringList& files, const QString& oldPath = {})
{
	if (!button || !button->isEnabled() || files.isEmpty()) return false;
	QTimer answer;
	int ticks = 0;
	bool chosen = false;
	QObject::connect(&answer, &QTimer::timeout, [&]
	{
		const auto modal = QApplication::activeModalWidget();
		if (++ticks > 100)
		{
			if (auto dialog = qobject_cast<QDialog*>(modal)) dialog->reject();
			return;
		}
		if (auto picker = qobject_cast<QInputDialog*>(modal))
		{
			const auto combo = picker->findChild<QComboBox*>();
			const int at = combo ? combo->findText(oldPath) : -1;
			if (at >= 0)
			{
				combo->setCurrentIndex(at);
				picker->accept();
			}
			return;
		}
		const auto chooser = qobject_cast<QFileDialog*>(modal);
		if (!chooser) return;
		chooser->setDirectory(QFileInfo(files.front()).absolutePath());
		QStringList names;
		for (const auto& file : files) names << '"' + QFileInfo(file).fileName() + '"';
		if (files.size() == 1) chooser->selectFile(QFileInfo(files.front()).fileName());
		if (auto edit = chooser->findChild<QLineEdit*>("fileNameEdit"))
			edit->setText(files.size() == 1 ? QFileInfo(files.front()).fileName() : names.join(' '));
		chosen = true;
		QMetaObject::invokeMethod(chooser, "accept", Qt::DirectConnection);
	});
	answer.start(50);
	button->click();
	return chosen;
}

void Mouse(QWidget* viewport, QEvent::Type type, QPoint point, Qt::MouseButton button, Qt::MouseButtons buttons)
{
	QMouseEvent event(type, point, viewport->mapToGlobal(point), button, buttons, Qt::NoModifier);
	QApplication::sendEvent(viewport, &event);
}

bool DragLastToFront(QListWidget* list)
{
	const auto from = list->visualItemRect(list->item(2)).center();
	const auto to = list->visualItemRect(list->item(0)).center();
	auto viewport = list->viewport();
	const auto expected = list->item(2)->data(Qt::UserRole);
	Mouse(viewport, QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
	Mouse(viewport, QEvent::MouseMove, (from + to) / 2, Qt::NoButton, Qt::LeftButton);
	Mouse(viewport, QEvent::MouseMove, to, Qt::NoButton, Qt::LeftButton);
	Mouse(viewport, QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
	return list->item(0)->data(Qt::UserRole) == expected;
}

bool Check(QJsonObject& result, const char* name, bool passed)
{
	result[name] = passed;
	return passed;
}

bool Edit(KRREditSequenceDialog* window, const QString& fixtures, const QString& output, QJsonObject& result)
{
	const auto list = window->findChild<QListWidget*>("sequenceList");
	const auto a = fixtures + "/A-match-60.mp4";
	const auto b = fixtures + "/B-edit.json";
	const auto c = fixtures + "/C-match-chapters-60.mp4";
	const auto status = window->findChild<QLabel*>("sequenceStatus");
	if (!Check(result, "import_three", ChooseFiles(Button(window, "sequenceImport"), {b, c, a}) &&
		Wait([&] { return list->count() == 3; }, 20000) && list->item(0)->text().startsWith("B-match"))) return false;
	auto player = window->findChild<KRREditPlayer*>();
	auto canvas = window->findChild<KRREditCanvas*>();
	Button(window, "sequencePlay")->click();
	const bool played = Wait([&] { return !canvas->currentFrame().isNull() && player->position() >= 200; }, 7000);
	result["preview_error"] = player->errorString();
	result["preview_peak_frames"] = player->peakBufferedFrames();
	player->pause();
	if (!Check(result, "real_media_preview", played)) return false;
	if (!Check(result, "mouse_drag_events", DragLastToFront(list))) return false;
	auto cut = [&](int row, const char* button, double begin, double end)
	{
		list->setCurrentRow(row);
		window->findChild<QDoubleSpinBox*>("sequenceBegin")->setValue(begin);
		window->findChild<QDoubleSpinBox*>("sequenceEnd")->setValue(end);
		Button(window, button)->click();
	};
	cut(0, "sequenceKeep", 1, 5);
	cut(0, "sequenceCut", 2.5, 3.5);
	cut(1, "sequenceKeep", 15, 19);
	cut(2, "sequenceKeep", 0.2, 5.2);
	Button(window, "sequenceUndo")->click();
	if (!Check(result, "undo_cut", list->item(2)->text().endsWith("00:00:14.000"))) return false;
	cut(2, "sequenceKeep", 0.2, 5.2);
	if (!Check(result, "save_from_ui", ChooseFiles(Button(window, "sequenceSave"), {output + "/before-move.json"}))) return false;
	QJsonObject json;
	QString error;
	if (!ReadEditProjectJson(output + "/before-move.json", json, error))
	{
		result["save_error"] = error + ' ' + status->text();
		return false;
	}
	const auto parts = json["parts"].toArray();
	return Check(result, "saved_order_and_cuts", parts.size() == 3 &&
		parts[0].toObject()["input"].toString() == a &&
		parts[0].toObject()["kept"].toArray() == QJsonArray{QJsonArray{1000, 2500}, QJsonArray{3500, 5000}} &&
		parts[1].toObject()["input"].toString().endsWith("/B-match-30.mp4") &&
		parts[1].toObject()["kept"].toArray() == QJsonArray{QJsonValue(QJsonArray{15000, 19000})} &&
		parts[2].toObject()["input"].toString() == c &&
		parts[2].toObject()["kept"].toArray() == QJsonArray{QJsonValue(QJsonArray{200, 5200})});
}

bool Restore(KRREditSequenceDialog* window, const QString& fixtures, const QString& output, QJsonObject& result)
{
	const auto list = window->findChild<QListWidget*>("sequenceList");
	const auto status = window->findChild<QLabel*>("sequenceStatus");
	const auto exportButton = Button(window, "sequenceExport");
	const auto a = output + "/restored.json";
	if (!Check(result, "open_with_missing_assets", ChooseFiles(Button(window, "sequenceOpen"), {a}) &&
		Wait([&] { return list->count() == 3 && list->item(1)->text().contains(QStringLiteral("待定位")); }, 15000) &&
		!exportButton->isEnabled())) return false;
	list->setCurrentRow(1);
	const auto locate = Button(window, "sequenceLocate");
	const auto oldVideo = fixtures + "/B-match-30.mp4";
	if (!Check(result, "reject_different_file_with_same_size", ChooseFiles(locate, {output + "/relocated/wrong.mp4"}, oldVideo) &&
		Wait([&] { return status->text().contains(QStringLiteral("不匹配")); }) && !exportButton->isEnabled())) return false;
	if (!Check(result, "locate_renamed_video", ChooseFiles(locate, {output + "/relocated/MatchB-moved.mp4"}, oldVideo) &&
		Wait([&] { return status->text().contains(QStringLiteral("已更新素材路径")); }) && !exportButton->isEnabled())) return false;
	if (!Check(result, "locate_external_audio", ChooseFiles(locate, {output + "/relocated/Voice-moved.wav"}) &&
		Wait([&] { return exportButton->isEnabled(); }, 15000))) return false;
	Button(window, "sequenceSave")->click();
	QString error;
	QJsonObject json;
	KRREditSequence loaded;
	const auto reopened = ReadEditProjectJson(a, json, error) && ReadEditSequence(json, loaded, error);
	result["reopen_error"] = error;
	if (!Check(result, "reopen_and_restore_all_edits", reopened && EditSequenceDuration(loaded) == 12000 &&
		loaded[1].edit && loaded[1].edit->input.endsWith("/MatchB-moved.mp4") &&
		loaded[1].edit->audio.back().file.endsWith("/Voice-moved.wav"))) return false;
	QElapsedTimer clock;
	clock.start();
	const auto out = output + "/SF6-sequence.mp4";
	const auto rendered = ChooseFiles(exportButton, {out}) &&
		Wait([&] { return exportButton->isEnabled(); }, 120000) && QFileInfo::exists(out);
	result["render_ms"] = clock.elapsed();
	result["export_status"] = status->text();
	return Check(result, "export_from_ui", rendered);
}
}

int main(int argc, char** argv)
{
	InitializeDisplayCoordinates();
	QApplication::setAttribute(Qt::AA_DisableHighDpiScaling);
	QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
	QApplication app(argc, argv);
	RecorderUi::Apply(app);
	const auto args = app.arguments();
	if (args.size() != 4 || (args[3] != "edit" && args[3] != "restore")) return 2;
	const auto fixtures = QDir::fromNativeSeparators(args[1]);
	const auto output = QDir::fromNativeSeparators(args[2]);
	QJsonObject result;
	KRRClipEditor editor;
	auto entry = editor.findChild<QAction*>("openEditSequence");
	bool passed = false;
	if (entry)
	{
		editor.show();
		QTimer::singleShot(20, &editor, [&]
		{
			auto dialog = dynamic_cast<KRREditSequenceDialog*>(QApplication::activeModalWidget());
			if (!Check(result, "open_from_editor_menu", dialog != nullptr)) return;
			passed = args[3] == "edit" ? Edit(dialog, fixtures, output, result) : Restore(dialog, fixtures, output, result);
			dialog->done(0);
		});
		entry->trigger();
	}
	result["ok"] = passed;
	const auto bytes = QJsonDocument(result).toJson();
	fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size()), stdout);
	return passed ? 0 : 1;
}
