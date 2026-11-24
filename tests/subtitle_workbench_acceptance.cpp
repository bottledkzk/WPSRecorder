#include "clipeditor.h"
#include "displaygeometry.h"
#include "subtitledialog.h"
#include "uitheme.h"
#include "uilanguage.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFontMetrics>
#include <QHeaderView>
#include <QJsonDocument>
#include <QInputDialog>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMediaPlayer>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QThread>
#include <QToolButton>
#include <algorithm>
#include <functional>

static bool WaitFor(const std::function<bool()>& condition, int maxTime = 12000)
{
	QElapsedTimer timer;
	timer.start();
	while (!condition() && timer.elapsed() < maxTime)
	{
		QApplication::processEvents();
		QThread::msleep(20);
	}
	return condition();
}

static QJsonObject TestAsr(const QString& source, const QString& folder)
{
	KRRClipEditor editor;
	editor.show();
	if (!editor.openFile(source, folder)) return {{"error", "Cannot open MV"}};
	QTimer poll;
	QString error;
	int stage = 0, lines = 0;
	QElapsedTimer timer;
	timer.start();
	QObject::connect(&poll, &QTimer::timeout, &editor, [&]
	{
		auto dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
		if (timer.elapsed() > 150000)
		{
			error = "Timed out in ASR or returning to the subtitle workbench";
			if (dialog) dialog->reject();
			QApplication::quit();
			return;
		}
		if (!dialog) return;
		if (stage == 0 && dialog->objectName() == "subtitleWorkbench")
		{
			++stage;
			dialog->findChild<QPushButton*>("subtitleTranscribe")->click();
		}
		else if (stage == 1 && dialog->objectName() == "offlineSubtitleSettings")
		{
			++stage;
			auto language = dialog->findChild<QComboBox*>("offlineSubtitleLanguage");
			language->setCurrentIndex(language->findData("zh"));
			dialog->accept();
		}
		else if (stage == 2 && dialog->objectName() == "subtitleWorkbench")
		{
			lines = dialog->findChild<QTableWidget*>("subtitleLines")->rowCount();
			++stage;
			dialog->reject();
			QApplication::quit();
		}
	});
	poll.start(150);
	editor.showSubtitleWorkflow();
	QApplication::exec();
	return {{"passed", lines > 0 && stage == 3 && error.isEmpty()}, {"lines", lines}, {"wall_ms", timer.elapsed()},
		{"error", error}};
}

static QJsonObject TestDubbing(const QString& projectFile, const QString& outputFolder)
{
	KRREditProject project;
	QFile file(projectFile);
	QString error;
	if (!file.open(QIODevice::ReadOnly) || !KRREditProject::FromJson(QJsonDocument::fromJson(file.readAll()).object(), project, error))
		return {{"error", error}};
	KRRSubtitleDialog dialog(project, outputFolder, nullptr);
	dialog.show();
	auto button = [&dialog](const char* name) { return dialog.findChild<QPushButton*>(name); };
	auto status = dialog.findChild<QLabel*>("subtitleStatus");
	auto table = dialog.findChild<QTableWidget*>("subtitleLines");
	auto engine = dialog.findChild<QComboBox*>("sentenceEngine");
	auto text = dialog.findChild<QLineEdit*>("sentenceNarration");
	auto player = dialog.findChild<QMediaPlayer*>("subtitleAudioPlayer");
	auto read = dialog.findChild<QComboBox*>("sentenceLanguage");
	auto failure = [status](const QString& cause) { return QJsonObject{{"error", cause}, {"status", status->text()}}; };
	if (table->rowCount() < 3) return failure("Expected translated MV lines");
	if (read->currentData() != "original") return failure("Translation was selected automatically for new takes");
	read->setCurrentIndex(read->findData("translation"));
	table->setCurrentCell(0, 0);
	button("playSourceLine")->click();
	if (!WaitFor([&] { return player->position() >= 200; })) return failure("Source line did not play");
	engine->setCurrentIndex(engine->findData("audio8"));
	button("synthesizeLine")->click();
	if (!WaitFor([&] { return button("playDubLine")->isEnabled(); }, 70000)) return failure("First dub did not complete");
	const auto first = table->item(0, 3)->text();
	if (!first.contains(QStringLiteral("超时"))) return failure("Expected long translation to be flagged");
	button("playDubLine")->click();
	if (!WaitFor([&] { return player->position() >= 200 && player->media().request().url().toLocalFile().endsWith(".wav"); }))
		return failure("Long take could not be auditioned");
	const auto firstFile = player->media().request().url().toLocalFile();
	text->setText("The door opens.");
	button("synthesizeLine")->click();
	if (!WaitFor([&] { return button("addDubTrack")->isEnabled(); }, 55000)) return failure("Shorter replacement did not fit");
	button("playDubLine")->click();
	if (!WaitFor([&] { return player->position() >= 200 && player->media().request().url().toLocalFile() != firstFile; }))
		return failure("Replacement wave did not play");
	QTimer files;
	QObject::connect(&files, &QTimer::timeout, &dialog, [&]
	{
		auto modal = QApplication::activeModalWidget();
		if (auto prompt = qobject_cast<QMessageBox*>(modal))
		{
			if (auto yes = prompt->button(QMessageBox::Yes)) yes->click();
		}
		else if (auto save = qobject_cast<QFileDialog*>(modal))
		{
			if (auto name = save->findChild<QLineEdit*>("fileNameEdit")) name->setText("aligned.wav");
			QMetaObject::invokeMethod(save, "accept", Qt::DirectConnection);
		}
	});
	files.start(100);
	button("addDubTrack")->click();
	files.stop();
	if (!WaitFor([&]{ return dialog.draft().audio.size() > project.audio.size(); })) return failure("No aligned track");
	button("applySubtitleDraft")->click();
	const auto& copy = dialog.draft();
	if (dialog.result() != QDialog::Accepted || !QFileInfo::exists(copy.captions.first().dubbingFile) ||
		copy.captions.first().translation != project.captions.first().translation ||
		!copy.audio.last().sentenceDubbing || SpeechWaveDuration(copy.audio.last().file) != project.duration)
	{
		auto result = failure("Applied track or saved take missing");
		result["accepted"] = dialog.result() == QDialog::Accepted;
		result["take"] = QFileInfo::exists(copy.captions.first().dubbingFile);
		result["track_ms"] = SpeechWaveDuration(copy.audio.last().file);
		return result;
	}
	KRRSubtitleDialog reopened(copy, outputFolder, nullptr);
	reopened.show();
	if (!reopened.findChild<QPushButton*>("playDubLine")->isEnabled()) return failure("Saved take did not reopen");
	const auto draftFile = QDir(outputFolder).filePath("with-dubbing.json");
	QFile draft(draftFile);
	if (!draft.open(QIODevice::WriteOnly) || draft.write(QJsonDocument(copy.toJson()).toJson()) < 0)
		return failure("Could not write verification project");
	return {{"passed", true}, {"first_take", first}, {"fitted_ms", SpeechWaveDuration(copy.captions.first().dubbingFile)},
		{"track_ms", SpeechWaveDuration(copy.audio.last().file)}, {"saved_take", QFileInfo::exists(copy.captions.first().dubbingFile)},
		{"project", draftFile}};
}

static bool SaveProject(const QString& path, const KRREditProject& project)
{
	QFile file(path);
	const auto bytes = QJsonDocument(project.toJson()).toJson();
	return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

static QJsonObject TestSubtitleEdits(const QString& mode, const QString& projectFile, const QString& folder)
{
	QFile file(projectFile);
	KRREditProject project;
	QString error;
	if (!file.open(QIODevice::ReadOnly) || !KRREditProject::FromJson(QJsonDocument::fromJson(file.readAll()).object(), project, error) ||
		project.captions.size() < 2 || !project.audio.last().sentenceDubbing) return {{"error", "Missing MV test project"}, {"detail", error}};
	KRRSubtitleDialog workbench(project, folder, nullptr);
	workbench.show();
	const auto button = [&](const char* name) { return workbench.findChild<QPushButton*>(name); };
	auto table = workbench.findChild<QTableWidget*>("subtitleLines");
	table->setCurrentCell(0, 0);
	QTimer poll;
	poll.setInterval(50);
	bool sawComparison = false;
	if (mode == "review")
	{
		for (const bool adopt : {false, true})
		{
			QElapsedTimer timer;
			timer.start();
			bool requested = false;
			auto reviewConnection = QObject::connect(&poll, &QTimer::timeout, &workbench, [&]
			{
				auto review = qobject_cast<QDialog*>(QApplication::activeModalWidget());
				if (!review || review->objectName() != "subtitleReview") return;
				if (timer.elapsed() > 20000) { review->reject(); return; }
				if (!requested)
				{
					requested = true;
					review->findChild<QSpinBox*>("reviewLastLine")->setValue(2);
					review->findChild<QLineEdit*>("reviewGoal")->setText("Use an active subject.");
					review->findChild<QPushButton*>("runSubtitleReview")->click();
					return;
				}
				auto suggestions = review->findChild<QTableWidget*>("reviewSuggestions");
				if (suggestions->rowCount() != 2 || suggestions->currentRow() < 0) return;
				const auto detail = review->findChild<QPlainTextEdit*>("reviewBeforeAfter")->toPlainText();
				auto apply = review->findChild<QPushButton*>("applySubtitleSuggestions");
				sawComparison = detail.contains("The door opens.") && detail.contains("You open the door.") &&
					suggestions->item(0, 2)->text() == "The source addresses the viewer." && !apply->isEnabled();
				if (!adopt || !sawComparison) { review->reject(); return; }
				suggestions->item(0, 0)->setCheckState(Qt::Checked);
				apply->click();
			});
			poll.start();
			button("reviewSubtitles")->click();
			poll.stop();
			QObject::disconnect(reviewConnection);
			if (!sawComparison || (!adopt && workbench.draft().toJson() != project.toJson()))
				return {{"error", "Review preview or cancel changed the project"}};
		}
	}
	else
	{
		KRRPronunciations rules;
		if (!ParsePronunciations("Door opens => Door\nDoor => Threshold\n", rules, error) ||
			rules.apply("door opens. outdoor Door.") != "Door. outdoor Threshold." ||
			ParsePronunciations("Door => one\ndoor => two", rules, error))
			return {{"error", "Glossary boundaries, precedence, or duplicate detection failed"}};
		auto inputConnection = QObject::connect(&poll, &QTimer::timeout, &workbench, [&]
		{
			auto words = qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
			if (!words) return;
			words->setTextValue("door => gate");
			words->accept();
		});
		poll.start();
		button("subtitlePronunciations")->click();
		poll.stop();
		QObject::disconnect(inputConnection);
	}
	const auto& selected = workbench.draft();
	const auto& changed = selected.captions[0];
	const auto& untouched = selected.captions[1];
	if (changed.text != project.captions[0].text || changed.begin != project.captions[0].begin ||
		changed.end != project.captions[0].end || !changed.dubbingNeedsRefresh || !changed.dubbingFile.isEmpty() ||
		untouched.translation != project.captions[1].translation || untouched.dubbingFile != project.captions[1].dubbingFile ||
		!QFileInfo::exists(project.captions[0].dubbingFile) || selected.audio.last().audible() || !selected.audio.last().enabled ||
		button("playDubLine")->isEnabled() || !SaveProject(QDir(folder).filePath("stale.json"), selected))
		return {{"error", "Selective invalidation or stale-track persistence failed"}};
	if (mode == "review" && (changed.translation != "You open the door." || changed.narrationText != changed.translation))
		return {{"error", "The selected suggestion was not applied"}};
	if (mode == "pronunciations" && (changed.translation != project.captions[0].translation || selected.pronunciationGuide != "door => gate"))
		return {{"error", "Pronunciation edits changed the displayed translation"}};

	auto engine = workbench.findChild<QComboBox*>("sentenceEngine");
	engine->setCurrentIndex(engine->findData("audio8"));
	button("synthesizeLine")->click();
	auto status = workbench.findChild<QLabel*>("subtitleStatus");
	if (!WaitFor([&] { return button("playDubLine")->isEnabled(); }, 40000))
		return {{"error", "Could not regenerate the affected line"}, {"detail", status->text()}};
	auto player = workbench.findChild<QMediaPlayer*>("subtitleAudioPlayer");
	button("playDubLine")->click();
	if (!WaitFor([&] { return player->position() >= 150; }))
		return {{"error", "Regenerated line could not be auditioned"}, {"detail", status->text()}};
	auto saveConnection = QObject::connect(&poll, &QTimer::timeout, &workbench, [&]
	{
		if (auto question = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
		{
			if (auto yes = question->button(QMessageBox::Yes)) yes->click();
		}
	});
	poll.start();
	button("addDubTrack")->click();
	poll.stop();
	QObject::disconnect(saveConnection);
	if (!WaitFor([&] { return selected.audio.last().audible(); }, 25000))
		return {{"error", "Could not rebuild the dubbing track"}, {"detail", status->text()}};
	button("applySubtitleDraft")->click();
	const auto& finished = workbench.draft();
	const auto& take = finished.captions[0];
	const auto expected = mode == "review" ? "You open the door." : "The gate opens.";
	const auto mixed = finished.audio.last().file;
	if (workbench.result() != QDialog::Accepted || take.dubbingNeedsRefresh || take.dubbingText != expected ||
		take.dubbingFile == project.captions[0].dubbingFile || finished.captions[1].dubbingFile != project.captions[1].dubbingFile ||
		finished.audio.last().gain != project.audio.last().gain || finished.audio.last().file == project.audio.last().file ||
		SpeechWaveDuration(mixed) != project.duration || !SaveProject(QDir(folder).filePath("applied.json"), finished))
		return {{"error", "Saving the rebuilt track lost the existing take or its settings"}, {"detail", status->text()}};
	return {{"passed", true}, {"mode", mode}, {"compared", sawComparison}, {"speech", take.dubbingText},
		{"wave_ms", SpeechWaveDuration(take.dubbingFile)}, {"mix_ms", SpeechWaveDuration(mixed)},
		{"auditioned", true}, {"unchanged_take", finished.captions[1].dubbingFile}, {"source_ms", take.begin}};
}

static QJsonObject TestNarrationLayout(const QString& media)
{
	KRREditProject project;
	QString error;
	if (!ProbeEditSource(media, project, error)) return {{"error", error}};
	project.chapters = {{1000, "Intro"}, {6500, "Fight"}};
	if (project.duration < 15000) return {{"error", "Narration layout fixture must be at least 15 seconds"}};
	project.kept = {{0, 1500}, {2800, 6000}, {8000, 15000}};
	QVector<KRREditCaption> cues;
	if (!ReadChapterNarration(project, "## Intro\nReady. Guard.\n\n## 2. Fight\nMove.", cues, error) || cues.size() != 3)
		return {{"error", "Could not parse chapter headings and split sentences"}, {"detail", error}};
	if (!PlaceChapterNarration(project, cues, {900, 1700, 1000}, error) ||
		cues[0].begin != 2800 || cues[0].end != 3700 || cues[1].begin != 3850 || cues[2].begin != 8000)
		return {{"error", "Could not place real timings inside kept sections"}, {"detail", error}};
	const auto saved = cues[0].begin;
	if (PlaceChapterNarration(project, cues, {3500, 150, 1000}, error) || cues[0].begin != saved)
		return {{"error", "Overlong placement changed the draft"}};
	if (ReadChapterNarration(project, "## Missing\nReady.", cues, error) || cues.size() != 3 ||
		ReadChapterNarration(project, "## Intro\nReady.\n## Intro\nAgain.", cues, error))
		return {{"error", "Invalid chapter headings were accepted"}};
	project.kept = {{0, project.duration}};
	QStringList blocks;
	project.chapters.clear();
	const auto chapterLength = project.duration / 4;
	for (int chapter = 0; chapter < 4; ++chapter)
	{
		const auto title = QString("Section %1").arg(chapter + 1);
		project.chapters.append({chapter * chapterLength, title});
		blocks.append("## " + title);
		for (int row = 0; row < 36; ++row) blocks.append(QString("Section %1 line %2.").arg(chapter + 1).arg(row + 1));
	}
	if (!ReadChapterNarration(project, blocks.join('\n'), project.chapterNarration, error) ||
		project.chapterNarration.size() != 144 || !PlaceChapterNarration(project, project.chapterNarration, QVector<qint64>(144, 100), error) ||
		!project.validate(error) || project.chapterNarration[108].begin != 3 * chapterLength)
		return {{"error", "Long script placement failed"}, {"detail", error}};
	return {{"passed", true}, {"fragmented_placement_ms", QJsonArray{2800, 3850, 8000}},
		{"rejected_overflow", true}, {"long_script_rows", project.chapterNarration.size()}, {"chapter_length_ms", chapterLength}};
}

class KRREditAcceptance
{
public:
	static QJsonObject ChapterVolumes(const KRREditProject& project, const QString& folder)
	{
		const auto original = std::find_if(project.audio.cbegin(), project.audio.cend(), [](const KRREditTrack& track)
		{
			return track.file.isEmpty() && track.audible() && track.gain > 0;
		});
		if (original == project.audio.cend()) return {{"error", "No enabled original audio track to test"}};
		const int originalIndex = static_cast<int>(original - project.audio.cbegin());

		KRRClipEditor editor;
		if (!editor.openFile(project.input, folder)) return {{"error", "Failed to reopen source media"}};
		editor.m_pendingProject = std::make_unique<KRREditProject>(project);
		if (!WaitFor([&] { return editor.m_audioPlayers.size() == project.audio.size(); }, 16000))
			return {{"error", "Reopened project did not load its audio tracks"}};
		bool opened = false;
		QTimer inspect;
		QObject::connect(&inspect, &QTimer::timeout, &editor, [&]
		{
			auto modal = qobject_cast<QDialog*>(QApplication::activeModalWidget());
			if (!modal || modal->objectName() != "chapterNarrationWorkbench") return;
			const auto rows = modal->findChild<QTableWidget*>("subtitleLines");
			opened = rows && rows->rowCount() == project.chapterNarration.size();
			modal->reject();
		});
		inspect.start(100);
		editor.findChild<QPushButton*>("openChapterNarration")->click();
		inspect.stop();
		if (!opened) return {{"error", "Reopened narration text was unavailable"}};

		const auto& voice = project.chapterNarration.first();
		QJsonArray levels;
		int disabledTracks = 0;
		bool disabledTracksSilent = true;
		for (const auto& track : project.audio)
			if (track.file.isEmpty() && !track.enabled) ++disabledTracks;
		for (qint64 source : {qint64(200), voice.begin - 40, voice.begin + 200, voice.end + 50, qint64(13000)})
		{
			editor.m_position = source;
			editor.syncAudio(false);
			levels.append(editor.m_audioPlayers[originalIndex]->volume());
			for (int i = 0; i < project.audio.size(); ++i)
			{
				const auto& track = project.audio[i];
				if (track.file.isEmpty() && !track.enabled && editor.m_audioPlayers[i]->volume() != 0)
					disabledTracksSilent = false;
			}
		}
		editor.m_project.audio.last().enabled = false;
		editor.refresh();
		editor.m_position = voice.begin + 200;
		editor.syncAudio(false);
		levels.append(editor.m_audioPlayers[originalIndex]->volume());

		const auto volume = [gain = original->gain](double factor) { return qMin(100, qRound(100 * gain * factor)); };
		const QJsonArray expected{volume(1), volume(.675), volume(.35), volume(.35 + .65 * 50. / 120.),
			volume(1), volume(1)};
		return {{"passed", levels == expected && disabledTracksSilent}, {"preview_percent", levels},
			{"original_audio_stream", original->stream}, {"disabled_original_tracks", disabledTracks},
			{"disabled_original_tracks_stayed_muted", disabledTracksSilent}};
	}
};

static QJsonObject TestChapterWorkbench(const QString& media, const QString& folder)
{
	KRREditProject project;
	QString error;
	if (!ProbeEditSource(media, project, error)) return {{"error", error}};
	project.chapters = {{1000, "Intro"}, {6500, "Fight"}};
	project.kept = {{0, 4500}, {6000, 15500}};
	project.captions.append({250, 850, "Round one", QStringLiteral("第一回合")});
	project.subtitleLanguage = "en";
	project.translationLanguage = "zh";
	KRRSubtitleDialog dialog(project, folder, nullptr, true);
	dialog.show();
	auto button = [&dialog](const char* name) { return dialog.findChild<QPushButton*>(name); };
	auto status = dialog.findChild<QLabel*>("subtitleStatus");
	auto failure = [status](const QString& reason) { return QJsonObject{{"error", reason}, {"status", status->text()}}; };
	QTimer fill;
	auto import = QObject::connect(&fill, &QTimer::timeout, &dialog, []
	{
		const auto modal = QApplication::activeModalWidget();
		if (!modal) return;
		auto text = modal->findChild<QPlainTextEdit*>("chapterNarrationScript");
		if (!text) return;
		text->setPlainText("## 1. Intro\nFight begins.\nWatch the space.\n\n## Fight\nKeep your guard.\nWait before you attack.");
		modal->findChild<QPushButton*>("applyChapterScript")->click();
	});
	fill.start(100);
	button("importNarrationScript")->click();
	fill.stop();
	QObject::disconnect(import);
	auto table = dialog.findChild<QTableWidget*>("subtitleLines");
	if (table->rowCount() != 4 || table->item(2, 2)->text() != "Fight" || dialog.draft().captions.size() != 1)
		return failure("Script was not split into chapter rows");
	const auto provisional = dialog.draft().chapterNarration[1].begin;
	auto language = dialog.findChild<QComboBox*>("subtitleDirection");
	language->setCurrentIndex(language->findData("en"));
	auto engine = dialog.findChild<QComboBox*>("sentenceEngine");
	engine->setCurrentIndex(engine->findData("audio8"));
	table->setCurrentCell(0, 0);
	button("synthesizeBatch")->click();
	if (!WaitFor([&] { return button("addDubTrack")->isEnabled(); }, 160000)) return failure("Could not synthesize all four sentences");
	auto player = dialog.findChild<QMediaPlayer*>("subtitleAudioPlayer");
	player->setVolume(0);
	button("playDubLine")->click();
	if (!WaitFor([&] { return player->position() >= 150 && player->media().request().url().toLocalFile().endsWith(".wav"); }))
		return failure("Sentence take did not play");
	auto save = QObject::connect(&fill, &QTimer::timeout, &dialog, []
	{
		if (auto file = qobject_cast<QFileDialog*>(QApplication::activeModalWidget()))
		{
			if (auto name = file->findChild<QLineEdit*>("fileNameEdit")) name->setText("chapter-voice.wav");
			QMetaObject::invokeMethod(file, "accept", Qt::DirectConnection);
		}
	});
	fill.start(100);
	button("addDubTrack")->click();
	fill.stop();
	QObject::disconnect(save);
	if (!WaitFor([&] { return dialog.draft().audio.size() > project.audio.size(); }, 25000))
		return failure("Could not add an aligned narration track");
	button("applySubtitleDraft")->click();
	const auto& draft = dialog.draft();
	const auto output = QDir(folder).filePath("game-with-narration.json");
	KRREditProject restored;
	if (dialog.result() != QDialog::Accepted || !draft.audio.last().chapterDubbing || draft.audio.last().speech.size() != 4 ||
		draft.chapterNarration[1].begin == provisional || draft.captions.first().translation != project.captions.first().translation ||
		!KRREditProject::FromJson(draft.toJson(), restored, error) || !SaveProject(output, restored))
		return failure("Applied draft lost its chapter positions, audio coverage or subtitle");
	for (const auto& cue : restored.chapterNarration)
		if (SpeechWaveDuration(cue.dubbingFile) != cue.end - cue.begin || !EditRangesContain(restored.kept, {cue.begin, cue.end}))
			return failure("Saved take did not fit the assigned source range");
	return {{"passed", true}, {"sentences", 4}, {"provisional_second_ms", provisional},
		{"aligned_second_ms", restored.chapterNarration[1].begin}, {"project", output}};
}

static QJsonObject TestOriginalDubbing(const QString& media, const QString& folder)
{
	KRREditProject project;
	QString error;
	if (!ProbeEditSource(media, project, error)) return {{"error", error}};
	project.subtitleLanguage = "en";
	project.captions = {{1000, 5600, "First round."}, {6000, 10000, "Watch the corner."}};
	QJsonObject draft;
	{
		KRRSubtitleDialog dialog(project, folder, nullptr);
		dialog.show();
		auto button = [&dialog](const char* name) { return dialog.findChild<QPushButton*>(name); };
		const auto status = dialog.findChild<QLabel*>("subtitleStatus");
		auto fail = [status](const QString& reason) { return QJsonObject{{"error", reason}, {"status", status->text()}}; };
		auto table = dialog.findChild<QTableWidget*>("subtitleLines");
		auto translate = dialog.findChild<QToolButton*>("showTranslationOptions");
		auto read = dialog.findChild<QComboBox*>("sentenceLanguage");
		if (translate->isChecked() || !table->isColumnHidden(2) || read->currentData() != "original")
			return fail("Translation occupied the original-only workflow");
		translate->click();
		if (table->isColumnHidden(2) || !button("translateAll")->isVisible()) return fail("Couldn't expand translation");
		translate->click();
		auto engine = dialog.findChild<QComboBox*>("sentenceEngine");
		engine->setCurrentIndex(engine->findData("audio8"));
		button("synthesizeBatch")->click();
		if (!WaitFor([&] { return button("addDubTrack")->isEnabled() && button("playDubLine")->isEnabled(); }, 90000))
			return fail("Could not synthesize original captions");
		button("addDubTrack")->click();
		if (!WaitFor([&] { return dialog.draft().audio.size() == project.audio.size() + 1; }))
			return fail("Could not generate the aligned track");
		button("applySubtitleDraft")->click();
		if (dialog.result() != QDialog::Accepted) return fail("Could not apply the track");
		draft = dialog.draft().toJson();
	}
	KRREditProject restored;
	if (!KRREditProject::FromJson(draft, restored, error) || !restored.translationLanguage.isEmpty() ||
		!restored.captions[0].translation.isEmpty() || restored.captions[0].dubbingText != project.captions[0].text)
		return {{"error", "Reopened project did not preserve the original-only dubbing"}};
	const auto track = restored.audio.last();
	if (!track.sentenceDubbing || SpeechWaveDuration(track.file) != project.duration ||
		!restored.editRange(1000, 1500, restored.audio.size() - 1, false, error) ||
		EditRangesContain(restored.audio.last().kept, {1000, 1500}))
		return {{"error", "Aligned track was not saved or editable"}, {"detail", error}};
	const auto output = QDir(folder).filePath("original-dubbing-cut.json");
	if (!SaveProject(output, restored)) return {{"error", "Could not save cut project"}};
	return {{"passed", true}, {"generated_without_translation", true}, {"audio_track_ms", SpeechWaveDuration(track.file)},
		{"edit_applied", true}, {"project", output}};
}

static QJsonObject TestChapterPreview(const QString& fileName, const QString& folder)
{
	QFile file(fileName);
	KRREditProject project;
	QString error;
	if (!file.open(QIODevice::ReadOnly) || !KRREditProject::FromJson(QJsonDocument::fromJson(file.readAll()).object(), project, error) ||
		project.chapterNarration.isEmpty()) return {{"error", error}};
	return KRREditAcceptance::ChapterVolumes(project, folder);
}

static QJsonObject TestSourceAudition(const QString& media, const QString& folder)
{
	KRREditProject project;
	QString error;
	if (!ProbeEditSource(media, project, error)) return {{"error", error}};
	project.captions = {{200, qMin<qint64>(1600, project.duration), "Source preview"}};
	KRRSubtitleDialog dialog(project, folder, nullptr);
	dialog.show();
	auto player = dialog.findChild<QMediaPlayer*>("subtitleAudioPlayer");
	player->setMuted(true);
	dialog.findChild<QPushButton*>("playSourceLine")->click();
	const bool played = WaitFor([&] { return player->position() > 500; }, 12000);
	const auto preview = player->media().request().url().toLocalFile();
	return {{"passed", played && QFileInfo(preview).suffix() == "m4a" && dialog.draft().input == project.input},
		{"position_ms", player->position()}, {"preview_format", QFileInfo(preview).suffix()}, {"error", player->errorString()}};
}

static QJsonObject TestColumnLayout(const QString& media, const QString& folder)
{
	KRREditProject project;
	QString error;
	if (!ProbeEditSource(media, project, error)) return {{"error", error}};
	project.captions.clear();
	const auto verify = [&](const QString& screenshot) {
		KRRSubtitleDialog dialog(project, folder, nullptr);
		dialog.show();
		QApplication::processEvents();
		const auto table = dialog.findChild<QTableWidget*>("subtitleLines");
		const auto body = table->fontMetrics();
		const auto header = table->horizontalHeader()->fontMetrics();
		const auto timestamp = EditTime(project.duration);
		const auto width = [&](int column, const QString& sample) {
			return table->columnWidth(column) >
				qMax(body.horizontalAdvance(sample), header.horizontalAdvance(table->horizontalHeaderItem(column)->text()));
		};
		const bool fits = width(0, timestamp + "–" + timestamp) && width(3, Ui("当前读法待生成")) &&
			table->columnViewportPosition(3) + table->columnWidth(3) <= table->viewport()->width();
		dialog.grab().save(QDir(folder).filePath(screenshot));
		return QJsonObject{{"fits", fits}, {"time_width", table->columnWidth(0)}, {"dubbing_width", table->columnWidth(3)}};
	};
	const auto empty = verify("subtitle-empty.png");
	project.captions = {{10750, 12455, Ui("原文"), Ui("译文")}, {13425, 15562, Ui("旁白稿")}};
	project.captions[0].dubbingNeedsRefresh = true;
	const auto populated = verify("subtitle-with-lines.png");
	return {{"passed", empty["fits"].toBool() && populated["fits"].toBool()}, {"empty", empty}, {"populated", populated}};
}

int main(int argc, char** argv)
{
	InitializeDisplayCoordinates();
	QApplication::setAttribute(Qt::AA_DisableHighDpiScaling);
	QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
	QApplication app(argc, argv);
	app.setApplicationName("wpsrecorder-speech-qa");
	app.setOrganizationName("WPSCourse");
	RecorderUi::Apply(app);
	const auto args = app.arguments();
	if (args.size() != 4 || !QDir().mkpath(args[3])) return 2;
	QJsonObject result;
	if (args[1] == "asr") result = TestAsr(args[2], args[3]);
	else if (args[1] == "dubbing") result = TestDubbing(args[2], args[3]);
	else if (args[1] == "original-dubbing") result = TestOriginalDubbing(args[2], args[3]);
	else if (args[1] == "review" || args[1] == "pronunciations") result = TestSubtitleEdits(args[1], args[2], args[3]);
	else if (args[1] == "chapter-layout") result = TestNarrationLayout(args[2]);
	else if (args[1] == "chapter-workbench") result = TestChapterWorkbench(args[2], args[3]);
	else if (args[1] == "chapter-preview") result = TestChapterPreview(args[2], args[3]);
	else if (args[1] == "source-audition") result = TestSourceAudition(args[2], args[3]);
	else if (args[1] == "column-layout") result = TestColumnLayout(args[2], args[3]);
	else return 2;
	QFile report(QDir(args[3]).filePath("result.json"));
	if (!report.open(QIODevice::WriteOnly)) return 2;
	report.write(QJsonDocument(result).toJson());
	return result["passed"].toBool() ? 0 : 1;
}
