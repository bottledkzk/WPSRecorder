#include "clipeditor.h"
#include "editplayer.h"
#include "editwidgets.h"
#include "recordingconfig.h"
#include "sequencesmartcutdialog.h"
#include "smartcut.h"
#include "uitheme.h"

#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QCryptographicHash>
#include <QDialog>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMediaPlayer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <functional>

namespace
{
QJsonObject ReadJson(const QString& path)
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) return {};
	return QJsonDocument::fromJson(file.readAll()).object();
}

bool WriteJson(const QString& path, const QJsonObject& data)
{
	QFile file(path);
	const auto bytes = QJsonDocument(data).toJson();
	return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

bool WaitFor(const std::function<bool()>& condition, int timeout)
{
	QElapsedTimer timer;
	timer.start();
	while (!condition() && timer.elapsed() < timeout)
	{
		QApplication::processEvents();
		QThread::msleep(20);
	}
	return condition();
}
} // namespace

class KRRSmartCutAcceptance
{
public:
	static QJsonObject Run(const KRREditProject& project, const QString& mode, const QString& draftPath)
	{
		KRRClipEditor editor;
		editor.show();
		if (!editor.openFile(project.input, QFileInfo(project.input).absolutePath()) ||
			!WaitFor([&] { return !editor.m_project.input.isEmpty(); }, 12000))
			return {{"error", "Could not open video in editor"}};
		editor.m_project = project;
		editor.refresh();
		if (mode == "layout") return Layout(editor, draftPath);
		return mode == "manual" ? Manual(editor) : Draft(editor, draftPath);
	}

private:
	static QJsonObject Layout(KRRClipEditor& editor, const QString& folder)
	{
		auto inspect = [&](QDialog& dialog, const char* object, const QStringList& values,
			const QString& name)
		{
			auto table = dialog.findChild<QTableWidget*>(object);
			if (!table) return QJsonObject{{"passed", false}};
			dialog.resize(900, dialog.height());
			QApplication::processEvents();
			const bool empty = table->rowCount() == 0;
			QJsonArray widths;
			bool headingsFit = true;
			for (int col = 0; col < table->columnCount(); ++col)
			{
				const int width = table->columnWidth(col);
				widths.append(width);
				headingsFit = headingsFit && width >=
					table->horizontalHeader()->fontMetrics().horizontalAdvance(table->horizontalHeaderItem(col)->text()) + 8;
			}
			if (empty) dialog.grab().save(QDir(folder).filePath(name + "-empty.png"));
			const QSignalBlocker block(table);
			table->setRowCount(1);
			bool valuesFit = true;
			for (int col = 0; col < values.size(); ++col)
			{
				table->setItem(0, col, new QTableWidgetItem(values[col]));
				valuesFit = valuesFit && table->columnWidth(col) >= table->fontMetrics().horizontalAdvance(values[col]) + 8;
			}
			dialog.grab().save(QDir(folder).filePath(name + "-populated.png"));
			return QJsonObject{{"passed", empty && headingsFit && valuesFit}, {"widths_px", widths},
				{"empty_headings_fit", empty && headingsFit}, {"populated_values_fit", valuesFit}};
		};
		const auto time = EditTime(359'999'999);
		QJsonObject single;
		QTimer::singleShot(50, &editor, [&]
		{
			auto dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
			if (!dialog || dialog->objectName() != "smartCutDialog") return;
			single = inspect(*dialog, "smartCutParts",
				{"", time, time, time, QStringLiteral("动作起点")}, "smartcut");
			dialog->reject();
		});
		editor.findChild<QPushButton*>("openSmartCut")->click();
		KRREditSequencePart part;
		part.edit = editor.m_project;
		part.saved = editor.m_project.toJson();
		KRRSequenceSmartCutDialog montage(KRREditSequence{part}, &editor);
		montage.show();
		const auto multi = inspect(montage, "montageParts",
			{"", QStringLiteral("动作起点"), time + " – " + time, time}, "montage");
		return {{"passed", single["passed"].toBool() && multi["passed"].toBool()},
			{"single", single}, {"montage", multi}};
	}

	static QJsonObject Manual(KRRClipEditor& editor)
	{
		const auto before = editor.m_project.toJson();
		const int expected = 6;
		if (editor.m_project.captions.size() <= expected) return {{"error", "Missing test subtitles"}};
		const auto query = editor.m_project.captions[expected].translation;
		if (query.isEmpty()) return {{"error", "Missing test translation"}};
		editor.showSubtitles();
		editor.m_captionSearch->setText(query);
		if (editor.m_captionList->count() == 0) return {{"error", "No filtered captions"}};
		editor.m_captionList->setCurrentRow(0);
		const auto time = editor.m_project.captions[expected].begin;
		if (editor.m_caption != expected || editor.m_begin != time || qAbs(editor.m_position - time) > 1000 / editor.m_project.fps + 1)
			return {{"error", "Filtered caption selected the wrong source time"}, {"found", editor.m_caption},
				{"expected_time_ms", time}, {"position_ms", editor.m_position}};
		const auto located = editor.m_position;
		editor.select(30000, 32000);
		editor.findChild<QAction*>("protectEditRange")->trigger();
		if (!EditRangesContain(editor.m_project.protectedRanges, {30000, 32000}))
			return {{"error", "Could not protect the timeline selection"}};
		const auto withLock = editor.m_project.toJson();
		editor.select(1000, 3000);
		QTimer change;
		bool refused = false, applied = false;
		QElapsedTimer elapsed;
		elapsed.start();
		QObject::connect(&change, &QTimer::timeout, &editor, [&]
		{
			auto dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
			if (!dialog || dialog->objectName() != "batchCutDialog") return;
			if (elapsed.elapsed() > 5000) { dialog->reject(); return; }
			auto ranges = dialog->findChild<QTableWidget*>("batchCutRanges");
			if (applied || ranges->rowCount() != 1) return;
			qobject_cast<QDoubleSpinBox*>(ranges->cellWidget(0, 0))->setValue(29.0);
			qobject_cast<QDoubleSpinBox*>(ranges->cellWidget(0, 1))->setValue(33.0);
			dialog->findChild<QPushButton*>("deleteBatchRanges")->click();
			refused = dialog->isVisible();
			qobject_cast<QDoubleSpinBox*>(ranges->cellWidget(0, 0))->setValue(1.0);
			qobject_cast<QDoubleSpinBox*>(ranges->cellWidget(0, 1))->setValue(3.0);
			applied = true;
			dialog->findChild<QPushButton*>("keepBatchRanges")->click();
		});
		change.start(100);
		editor.findChild<QAction*>("openBatchCut")->trigger();
		change.stop();
		const auto after = editor.m_project;
		editor.undo();
		const bool undo = editor.m_project.toJson() == withLock;
		editor.undo();
		const bool restored = editor.m_project.toJson() == before;
		const bool passed = refused && applied && after.kept.size() == 2 && after.outputDuration() == 4000 && undo && restored;
		return {{"passed", passed}, {"search_jump_ms", located}, {"protected_delete_refused", refused},
			{"kept_duration_ms", after.outputDuration()}, {"batch_undo", undo}, {"protect_undo", restored}};
	}

	static QJsonObject Draft(KRRClipEditor& editor, const QString& path)
	{
		auto input = ReadJson(path)["draft"].toObject();
		KRRSmartCutDraft draft;
		QString error;
		if (!KRRSmartCutDraft::fromJson(input, draft, error) || draft.parts.isEmpty()) return {{"error", error}};
		QDir dir(DataDirectory());
		dir.mkpath("smartcut-drafts");
		const auto key = QCryptographicHash::hash(QFileInfo(editor.m_project.input).absoluteFilePath().toUtf8(),
			QCryptographicHash::Sha256).toHex();
		const auto cacheFile = dir.filePath("smartcut-drafts/" + QString::fromLatin1(key) + ".json");
		if (!WriteJson(cacheFile, input))
			return {{"error", "Could not stage a saved draft"}};
		const auto before = editor.m_project.toJson();
		QTimer tick;
		QElapsedTimer elapsed;
		elapsed.start();
		int step = 0;
		qint64 evidence = -1, preview = 0;
		qint64 evidenceAudioBytes = 0;
		QJsonObject probe;
		const int target = qMin(1, draft.parts.size() - 1);
		const auto& choice = draft.parts[target];
		QObject::connect(&tick, &QTimer::timeout, &editor, [&]
		{
			auto dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
			if (!dialog || dialog->objectName() != "smartCutDialog") return;
			auto table = dialog->findChild<QTableWidget*>("smartCutParts");
			auto player = dialog->findChild<KRREditPlayer*>("smartCutPlayer");
			auto canvas = dialog->findChild<KRREditCanvas*>("smartCutCanvas");
			auto audio = dialog->findChild<QBuffer*>("smartCutAudio");
			auto excerpt = dialog->findChild<QPushButton*>("playDraftEvidence");
			if (elapsed.elapsed() > 23000)
			{
				error = dialog->findChild<QLabel*>("smartCutStatus")->text();
				probe = {{"row", table->currentRow()}, {"can_play", excerpt->isEnabled()},
					{"state", int(player->state())}, {"audio_bytes", audio->size()},
					{"position_ms", player->position()}, {"error", player->errorString()}};
				dialog->reject();
				return;
			}
			if (step == 0 && table->rowCount() == draft.parts.size())
			{
				table->setCurrentCell(target, 0);
				step = 1;
				excerpt->click();
			}
			else if (step == 1 && player->state() == QMediaPlayer::PlayingState &&
				!canvas->currentFrame().isNull() && player->position() >= qMax(choice.range.begin, choice.evidence - 800))
			{
				evidence = player->position();
				evidenceAudioBytes = audio->size();
				dialog->findChild<QDoubleSpinBox*>("draftEnd")->setValue((choice.range.end - 200) / 1000.0);
				dialog->findChild<QPushButton*>("updateDraftRange")->click();
				step = 2;
				dialog->findChild<QPushButton*>("renderSmartCutPreview")->click();
			}
			else if (step == 2 && player->state() == QMediaPlayer::PlayingState && !canvas->currentFrame().isNull() &&
				player->position() > 50 && player->position() < 3000)
			{
				preview = audio->size() / (48 * 4);
				step = 3;
				dialog->findChild<QPushButton*>("applySmartCut")->click();
			}
		});
		tick.start(100);
		editor.findChild<QPushButton*>("openSmartCut")->click();
		tick.stop();
		const auto applied = editor.m_project.outputDuration();
		const auto changedCut = std::any_of(editor.m_project.kept.cbegin(), editor.m_project.kept.cend(),
			[&](KRREditRange kept) { return kept.end == choice.range.end - 200; });
		KRRSmartCutDraft saved;
		QString savedError;
		const bool savedCut = KRRSmartCutDraft::fromJson(ReadJson(cacheFile), saved, savedError) &&
			saved.parts.size() == draft.parts.size() && saved.parts[target].range.end == choice.range.end - 200;
		editor.undo();
		const bool undo = editor.m_project.toJson() == before;
		return {{"passed", step == 3 && preview >= 7800 && evidenceAudioBytes > 0 && changedCut && savedCut &&
			applied < editor.m_project.duration && undo},
			{"stage", step}, {"evidence_position_ms", evidence}, {"preview_duration_ms", preview},
			{"source_audio_bytes", evidenceAudioBytes}, {"applied_duration_ms", applied},
			{"edited_cut_applied", changedCut}, {"edited_cut_saved", savedCut},
			{"undo_ok", undo}, {"error", error}, {"probe", probe}};
	}
};

int main(int argc, char** argv)
{
	QApplication::setAttribute(Qt::AA_DisableHighDpiScaling);
	QApplication app(argc, argv);
	app.setOrganizationName("WPSCourse");
	app.setApplicationName("wpsrecorder-smartcut-qa-" + QString::number(app.applicationPid()));
	RecorderUi::Apply(app);
	const auto args = app.arguments();
	if (args.size() != 5) return 2;
	KRREditProject project;
	QString error;
	if (args[1] == "layout")
	{
		qputenv("LOCALAPPDATA", QDir(args[3]).filePath("profile").toUtf8());
		if (!ProbeEditSource(args[2], project, error)) return 2;
	}
	else if (!KRREditProject::FromJson(ReadJson(args[2]), project, error)) return 2;
	const auto result = KRRSmartCutAcceptance::Run(project, args[1], args[3]);
	if (!WriteJson(args[4], result)) return 2;
	return result["passed"].toBool() ? 0 : 1;
}
