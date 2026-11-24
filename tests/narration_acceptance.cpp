#include "clipeditor.h"
#include "displaygeometry.h"
#include "uitheme.h"

#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMediaPlayer>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTabWidget>
#include <QThread>
#include <QTimer>
#include <functional>

static bool WaitFor(const std::function<bool()>& condition, int timeout = 12000)
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

class KRREditAcceptance
{
public:
	static QJsonObject run(const QString& input, const QString& output)
	{
		KRRClipEditor editor;
		editor.show();
		if (!editor.openFile(input, QFileInfo(output).absolutePath()) ||
			!WaitFor([&] { return editor.m_project.duration >= 7000; }))
			return {{"error", "Could not load a source project of at least seven seconds"}};
		QString error;
		if (!editor.m_project.editRange(0, 6500, -1, true, error)) return {{"error", error}};
		const auto sourceTracks = editor.m_project.audio.size();
		editor.seek(1800);
		const auto insert = editor.m_position;
		int samples = 0;
		QTimer deadline;
		deadline.setSingleShot(true);
		QObject::connect(&deadline, &QTimer::timeout, &editor, [&]
		{
			if (auto dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget()))
			{
				error = "Narration workspace timed out";
				dialog->done(QDialog::Rejected);
			}
		});
		deadline.start(110000);
		QTimer::singleShot(0, &editor, [&]
		{
			auto dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
			if (!dialog || dialog->objectName() != "subtitleWorkbench")
			{
				error = "Editor did not open unified workspace";
				if (dialog) dialog->reject();
				return;
			}
			const auto abort = [&](const QString& reason) { error = reason; dialog->reject(); };
			const auto button = [dialog](const char* name) { return dialog->findChild<QPushButton*>(name); };
			dialog->findChild<QTabWidget*>("speechSourceTabs")->setCurrentIndex(1);
			const auto text = dialog->findChild<QPlainTextEdit*>("freeNarrationText");
			const auto position = dialog->findChild<QDoubleSpinBox*>("freeNarrationStart");
			const auto engine = dialog->findChild<QComboBox*>("sentenceEngine");
			const auto language = dialog->findChild<QComboBox*>("freeNarrationLanguage");
			const auto voice = dialog->findChild<QComboBox*>("sentenceVoice");
			const auto player = dialog->findChild<QMediaPlayer*>("subtitleAudioPlayer");
			const auto generate = button("synthesizeNarration");
			const auto apply = button("applySubtitleDraft");
			const auto play = button("playDubLine");
			player->setMuted(true);
			if (apply->isEnabled() || generate->isEnabled() || qRound64(position->value() * 1000.) != insert)
				return abort("The empty narration pane or playhead default was incorrect");
			engine->setCurrentIndex(engine->findData("audio8"));
			language->setCurrentIndex(language->findData("zh"));
			const int female = voice->findData("mandarin-female");
			if (voice->count() != 2 || voice->findData("default") < 0 || female < 0)
				return abort("Audio8 did not offer the default and Mandarin female voices");
			voice->setCurrentIndex(female);
			button("previewSpeechVoice")->click();
			if (!WaitFor([&] { return QFileInfo(player->media().request().url().toLocalFile()).isFile(); }, 38000))
				return abort("The Mandarin female voice sample failed to load");
			++samples;
			const auto line = QStringLiteral("现在开始回放。");
			text->setPlainText(line);
			generate->click();
			button("cancelSpeechWork")->click();
			if (!WaitFor([&] { return generate->isEnabled(); }, 25000) || apply->isEnabled())
				return abort("Cancelled speech did not return to a clean draft");
			generate->click();
			if (!WaitFor([&] { return play->isEnabled() && apply->isEnabled(); }, 75000))
				return abort("Narration synthesis did not produce a take");
			play->click();
			if (!WaitFor([&] { return player->position() >= 150; }))
				return abort("Generated narration did not play");
			text->appendPlainText("Extra text.");
			if (play->isEnabled() || apply->isEnabled()) return abort("A stale take remained actionable after changing the script");
			text->setPlainText(line);
			position->setValue(position->maximum());
			apply->click();
			if (!dialog->isVisible()) return abort("Out-of-bounds narration was accepted");
			position->setValue(insert / 1000.);
			apply->click();
		});
		editor.showSubtitleWorkflow();
		deadline.stop();
		if (!error.isEmpty()) return {{"error", error}};
		if (editor.m_project.audio.size() != sourceTracks + 1 || editor.m_track != sourceTracks)
			return {{"error", "The new audio track did not reach the editor timeline"}};
		const auto track = editor.m_project.audio.last();
		if (track.offset != insert || track.sentenceDubbing || SpeechWaveDuration(track.file) != track.length ||
			editor.m_audioPlayers.last()->media().request().url() != QUrl::fromLocalFile(track.file))
			return {{"error", "Unexpected audio data or preview player mapping for the new track"}};
		editor.select(insert + 150, insert + 450);
		editor.applyRange(false);
		KRREditProject reopened;
		if (!KRREditProject::FromJson(editor.m_project.toJson(), reopened, error) ||
			EditRangesContain(reopened.audio.last().kept, {insert + 150, insert + 450}))
			return {{"error", "Audio cutting did not persist in the project"}, {"detail", error}};
		KRRMediaEditor exporter;
		bool exported = false;
		QObject::connect(&exporter, &KRRMediaEditor::completed, &editor, [&](const QString&, const QString& detail)
		{
			error = detail;
			exported = true;
		});
		if (!exporter.renderTimeline(reopened, output, "libx264", true, false) ||
			!WaitFor([&] { return exported; }, 90000) || !error.isEmpty())
			return {{"error", "Export with independent narration failed"}, {"detail", error}};
		KRREditProject result;
		if (!ProbeEditSource(output, result, error) || result.audio.size() != sourceTracks + 1)
			return {{"error", "Export lost one or more audio tracks"}, {"detail", error}};
		return {{"passed", true}, {"voice_samples", samples}, {"cancel_and_restart", true},
			{"narration_ms", track.length}, {"insertion_ms", insert}, {"export_audio_tracks", result.audio.size()},
			{"track_cut_restored", true}, {"output", output}};
	}
};

int main(int argc, char** argv)
{
	InitializeDisplayCoordinates();
	QApplication::setAttribute(Qt::AA_DisableHighDpiScaling);
	QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
	QApplication app(argc, argv);
	app.setOrganizationName("WPSCourse");
	app.setApplicationName("wpsrecorder-narration-qa");
	RecorderUi::Apply(app);
	const auto args = app.arguments();
	if (args.size() != 4) return 2;
	const auto result = KRREditAcceptance::run(args[1], args[3]);
	QFile report(args[2]);
	if (!report.open(QIODevice::WriteOnly)) return 2;
	report.write(QJsonDocument(result).toJson());
	return result["passed"].toBool() ? 0 : 1;
}
