#include "clipeditor.h"
#include "displaygeometry.h"
#include "editplayer.h"
#include "editsequencedialog.h"
#include "editwidgets.h"
#include "sequencesmartcutdialog.h"
#include "uitheme.h"
#include <QAction>
#include <QApplication>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QThread>
#include <QTimer>
#include <cstdio>

namespace
{
bool Wait(const std::function<bool()>& until, int timeout = 8000)
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

QByteArray Snapshot(const QString& path)
{
	QFile project(path);
	return project.open(QIODevice::ReadOnly) ? project.readAll() : QByteArray{};
}

QPushButton* Button(QWidget* window, const char* name)
{
	return window->findChild<QPushButton*>(name);
}

bool Check(QJsonObject& qa, const char* name, bool ok)
{
	fprintf(stderr, "%s=%d\n", name, ok ? 1 : 0);
	fflush(stderr);
	qa[name] = ok;
	return ok;
}

bool Confirm(QPushButton* button, QMessageBox::StandardButton choice)
{
	bool opened = false;
	QTimer answer;
	QObject::connect(&answer, &QTimer::timeout, [&]
	{
		auto box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
		if (box && box->button(choice))
		{
			opened = true;
			box->button(choice)->click();
		}
	});
	answer.start(30);
	button->click();
	return opened;
}

bool Review(KRRSequenceSmartCutDialog* window, const QString& path, const QByteArray& original, QJsonObject& qa)
{
	const auto table = window->findChild<QTableWidget*>("montageParts");
	const auto goal = window->findChild<QLineEdit*>("montageGoal");
	const auto fps = window->findChild<QSpinBox*>("montageSampleFps");
	const auto target = window->findChild<QSpinBox*>("montageTargetSeconds");
	const auto count = window->findChild<QSpinBox*>("montageCandidatesPerSource");
	if (!Check(qa, "analysis_controls_available", fps && target && count && fps->value() == 0)) return false;
	fps->setValue(6);
	target->setValue(8);
	count->setValue(2);
	goal->setText(QStringLiteral("找出每段的反击，剪成约 6 秒，每段最多 7 个候选"));
	QMetaObject::invokeMethod(goal, "returnPressed", Qt::DirectConnection);
	const auto preview = Button(window, "montagePreview");
	const auto status = window->findChild<QLabel*>("montageStatus");
	const auto ready = Wait([&] { return table->rowCount() >= 3 && preview->isEnabled(); }, 25000);
	qa["analysis_status"] = status->text();
	if (!Check(qa, "one_sentence_generated_draft", ready && Snapshot(path) == original)) return false;
	const auto selection = window->findChild<QLabel*>("montageSelection");
	if (!Check(qa, "selected_duration_visible", selection && !selection->text().isEmpty())) return false;
	table->setCurrentCell(0, 1);
	const auto player = window->findChild<KRREditPlayer*>();
	const auto canvas = window->findChild<KRREditCanvas*>();
	Button(window, "montageEvidence")->click();
	if (!Check(qa, "evidence_jump_and_decode", status->text().contains("HUD") &&
		Wait([&] { return !canvas->currentFrame().isNull(); }))) return false;
	qa["evidence_peak_frames"] = player->peakBufferedFrames();
	const auto matches = table->findItems("MatchB-moved.mp4", Qt::MatchContains);
	if (matches.isEmpty()) return false;
	const int sourceRow = matches.first()->row();
	table->setCurrentCell(sourceRow, 1);
	const auto next = table->item(sourceRow, 1)->text();
	for (int i = 0; i < sourceRow; ++i) Button(window, "montageUp")->click();
	if (!Check(qa, "reordered_candidate", table->item(0, 1)->text() == next)) return false;
	const auto end = window->findChild<QDoubleSpinBox*>("montageEnd");
	const auto adjusted = end->value() - .2;
	end->setValue(adjusted);
	Button(window, "montageSetCut")->click();
	const auto expectedEnd = qRound64(adjusted * 1000);
	qa["expected_end_ms"] = expectedEnd;
	if (!Check(qa, "updated_cut", table->item(0, 2)->text().endsWith(EditTime(expectedEnd)))) return false;
	QElapsedTimer clock;
	clock.start();
	preview->click();
	const auto played = Wait([&] { return status->text().contains(QStringLiteral("正在播放混剪草稿")) &&
		!canvas->currentFrame().isNull() && player->position() >= 200; }, 30000);
	qa["preview_ms"] = clock.elapsed();
	qa["preview_status"] = status->text();
	const auto frame = canvas->currentFrame();
	if (!Check(qa, "combined_preview_decoded", played && frame.width() <= 960 && frame.height() <= 540 &&
		Snapshot(path) == original)) return false;
	const auto apply = Button(window, "montageApply");
	if (!Check(qa, "cancel_confirmation_kept_source", Confirm(apply, QMessageBox::Cancel) && window->isVisible() &&
		Snapshot(path) == original)) return false;
	return Check(qa, "confirmed_application", Confirm(apply, QMessageBox::Apply) && window->result().size() >= 3);
}

bool Run(KRREditSequenceDialog* sequence, const QString& path, QJsonObject& qa)
{
	const auto original = Snapshot(path);
	sequence->openProject(path);
	const auto launch = Button(sequence, "sequenceSmartCut");
	if (!Check(qa, "available_through_sequence", !original.isEmpty() && Wait([&] { return launch->isEnabled(); }))) return false;
	bool reviewed = false;
	QTimer::singleShot(25, sequence, [&]
	{
		auto dialog = dynamic_cast<KRRSequenceSmartCutDialog*>(QApplication::activeModalWidget());
		if (!dialog) return;
		reviewed = Review(dialog, path, original, qa);
		if (!reviewed) dialog->done(QDialog::Rejected);
	});
	launch->click();
	if (!reviewed) return false;
	if (!Check(qa, "only_saved_after_explicit_save", Snapshot(path) == original)) return false;
	Button(sequence, "sequenceSave")->click();
	QString error;
	QJsonObject saved;
	KRREditSequence applied;
	if (!ReadEditProjectJson(path, saved, error) || !ReadEditSequence(saved, applied, error))
	{
		qa["error"] = error;
		return false;
	}
	return Check(qa, "saved_order_cuts_and_evidence", applied.size() >= 3 &&
		QFileInfo(applied[0].source()).fileName() == "MatchB-moved.mp4" &&
		applied[0].edit->kept.front().end == qa["expected_end_ms"].toDouble() &&
		applied[0].saved["highlight"].toObject()["reason"].toString().contains("HUD"));
}
}

int main(int argc, char** argv)
{
	InitializeDisplayCoordinates();
	QApplication::setAttribute(Qt::AA_DisableHighDpiScaling);
	QApplication app(argc, argv);
	RecorderUi::Apply(app);
	const auto args = app.arguments();
	if (args.size() != 2) return 2;
	QJsonObject qa;
	KRRClipEditor editor;
	editor.show();
	const auto entry = editor.findChild<QAction*>("openEditSequence");
	if (!entry) return 3;
	bool ok = false;
	QTimer::singleShot(25, &editor, [&]
	{
		auto sequence = dynamic_cast<KRREditSequenceDialog*>(QApplication::activeModalWidget());
		if (!sequence) return;
		ok = Run(sequence, args[1], qa);
		sequence->done(0);
	});
	entry->trigger();
	qa["ok"] = ok;
	const auto bytes = QJsonDocument(qa).toJson();
	fwrite(bytes.constData(), 1, static_cast<size_t>(bytes.size()), stdout);
	return ok ? 0 : 1;
}
