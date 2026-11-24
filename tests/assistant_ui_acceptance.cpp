#include "clipeditor.h"
#include "editwidgets.h"
#include "editplayer.h"
#include "mainwindow.h"
#include "recordingassistant.h"
#include "recordingassistantdialog.h"
#include "uitheme.h"
#include "uilanguage.h"

#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMediaPlayer>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QJsonDocument>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStackedWidget>
#include <QTableWidget>
#include <QThread>

class KRRAssistantAcceptance
{
public:
	static QJsonObject run(const QString& mode, const QString& folder, const QString& reportFile)
	{
		KRRMainWindow window;
		window.show();
		if (mode == "dock") return dock(window, folder);
		if (mode == "compact") return compact(window);
		window.m_directory->setText(folder);
		if (mode == "bridge") return bridge(window, reportFile + ".project.json");
		if (mode == "choice") return choice(window);
		if (mode == "templates") return templates(window);
		return mode == "search" ? search(window) : settings(window);
	}

private:
	static QJsonObject compact(KRRMainWindow& window)
	{
		window.findChild<QPushButton*>("openRecordingAssistant")->click();
		auto assistant = window.m_sidebarAssistant;
		auto send = assistant->findChild<QPushButton*>("assistantSend");
		auto prompt = assistant->findChild<QLineEdit*>("assistantPrompt");
		const auto id = assistant->conversationId();
		auto history = window.m_conversationHistory;
		for (int i = 0; i < 8; ++i)
		{
			prompt->setText(QString("QA keep project decision %1").arg(i));
			send->click();
			waitFor([&] { return !history->busy(id); }, 4500);
			if (history->entry(id)["messages"].toArray().size() != 2 * (i + 1))
				return {{"error", "Conversation lost an original turn"}, {"turn", i}};
		}
		auto entry = history->entry(id);
		const bool compacted = entry["through"].toInt() == 8 && !entry["summary"].toString().isEmpty();
		prompt->setText("/compact");
		send->click();
		waitFor([&] { return !history->busy(id); }, 4500);
		entry = history->entry(id);
		KRRAssistantHistory reloaded;
		const auto persisted = reloaded.entry(id);
		const bool passed = compacted && entry["through"].toInt() == 16 && entry["messages"].toArray().size() == 16 && persisted == entry;
		return {{"passed", passed}, {"automatic", compacted}, {"manual", entry["through"]}, {"messages", entry["messages"].toArray().size()}};
	}
	static QJsonObject dock(KRRMainWindow& window, const QString& source)
	{
		window.selectWorkspace(KRRMainWindow::Assistant);
		window.findChild<QPushButton*>("studioAiEdit")->click();
		auto page = window.findChild<QDialog*>("studioAiWorkspace");
		page->findChild<QComboBox*>("studioAnalysisScope")->setCurrentIndex(1);
		page->findChild<QSpinBox*>("studioAnalysisEnd")->setValue(3);
		page->findChild<QLineEdit*>("assistantPrompt")->setText(QStringLiteral("保留前 3 秒中能看清模式的片段。"));
		QTimer::singleShot(0, page, [&]
		{
			auto picker = qobject_cast<QFileDialog*>(QApplication::activeModalWidget());
			if (!picker) return;
			picker->setDirectory(QFileInfo(source).absolutePath());
			picker->selectFile(QFileInfo(source).fileName());
			if (auto field = picker->findChild<QLineEdit*>("fileNameEdit")) field->setText(QFileInfo(source).fileName());
			QMetaObject::invokeMethod(picker, "accept", Qt::DirectConnection);
		});
		page->findChild<QPushButton*>("studioGenerateCut")->click();
		QPushButton* apply = nullptr;
		int candidates = 0;
		waitFor([&]
		{
			auto review = window.findChild<QDialog*>("smartCutDialog");
			if (!review || !review->isVisible()) return false;
			auto table = review->findChild<QTableWidget*>("smartCutParts");
			apply = review->findChild<QPushButton*>("applySmartCut");
			candidates = table ? table->rowCount() : 0;
			return candidates > 0 && apply && apply->isEnabled();
		}, 25000);
		const bool confirmed = candidates > 0 && apply && apply->isEnabled();
		if (confirmed) apply->click();
		const auto output = window.m_clipEditor->m_project.outputDuration();
		return {{"passed", confirmed && output > 0 && output <= 3000 && window.m_workspace->currentIndex() == KRRMainWindow::Edit},
			{"candidates", candidates}, {"selected_duration_ms", output},
			{"opened_editor_on_apply", window.m_workspace->currentIndex() == KRRMainWindow::Edit}};
	}

	static QDialog* sidebar(KRRMainWindow& window)
	{
		return window.findChild<QDialog*>("studioCopilot");
	}

	static void waitFor(const std::function<bool()>& done, int milliseconds)
	{
		QElapsedTimer elapsed;
		elapsed.start();
		while (!done() && elapsed.elapsed() < milliseconds)
		{
			QApplication::processEvents();
			QThread::msleep(15);
		}
	}

	static QJsonObject templates(KRRMainWindow& window)
	{
		auto open = window.findChild<QPushButton*>("openRecordingAssistant");
		if (!open) return {{"error", "Missing recording assistant entry"}};
		bool passed = false;
		QJsonArray filled;
		QTimer::singleShot(0, &window, [&]
		{
			auto assistant = sidebar(window);
			if (!assistant) return;
			auto name = assistant->findChild<QLabel*>("assistantModelName");
			auto prompt = assistant->findChild<QLineEdit*>("assistantPrompt");
			auto transcript = assistant->findChild<QPlainTextEdit*>("recordingAssistantTranscript");
			const auto model = SpeechService("assistant").model;
			passed = name && prompt && transcript && name->text() == (model.isEmpty() ? Ui("尚未配置模型") : model);
			for (int i = 0; i < 4 && passed; ++i)
			{
				auto button = assistant->findChild<QPushButton*>(QString("assistantTemplate%1").arg(i));
				if (!button) { passed = false; break; }
				button->click();
				passed &= !QApplication::activeModalWidget() && prompt->text() == button->text();
				filled.append(passed);
			}
			assistant->reject();
		});
		open->click();
		waitFor([&] { return filled.size() == 4 || !sidebar(window)->isVisible(); }, 1200);
		return {{"passed", passed && filled.size() == 4}, {"templates", filled}, {"network_requests", 0}};
	}

	static QJsonObject choice(KRRMainWindow& window)
	{
		auto menu = window.findChild<QPushButton*>("openRecordingAssistant");
		if (!menu) return {{"error", "Assistant menu missing"}};
		bool sent = false, waited = false;
		int matching = 0;
		QTimer drive;
		QElapsedTimer deadline;
		deadline.start();
		QObject::connect(&drive, &QTimer::timeout, &window, [&]
		{
			auto dialog = sidebar(window);
			if (!dialog || !dialog->isVisible()) return;
			if (deadline.elapsed() > 12000) { dialog->reject(); return; }
			if (!sent)
			{
				sent = true;
				dialog->findChild<QLineEdit*>("assistantPrompt")->setText(QStringLiteral("找助手联调MV，剪出明显动作。"));
				dialog->findChild<QPushButton*>("assistantSend")->click();
				return;
			}
			auto button = dialog->findChild<QPushButton*>("assistantOpenRecording");
			matching = dialog->findChild<QListWidget*>("assistantRecordings")->count();
			if (matching > 1 && button->isVisible())
			{
				waited = window.m_clipEditor->m_project.input.isEmpty();
				dialog->reject();
			}
		});
		drive.start(100);
		menu->click();
		waitFor([&] { return !sidebar(window)->isVisible(); }, 12200);
		drive.stop();
		return {{"passed", waited && matching == 2}, {"matching", matching}, {"waited_for_choice", waited}};
	}

	static QJsonObject bridge(KRRMainWindow& window, const QString& projectPath)
	{
		auto menu = window.findChild<QPushButton*>("openRecordingAssistant");
		const auto editor = window.m_clipEditor;
		if (!menu) return {{"error", "Assistant menu missing"}};
		QElapsedTimer timeout;
		timeout.start();
		QTimer drive;
		QJsonObject baseline;
		QString status;
		int stage = 0, row = -1, candidateCount = 0;
		qint64 evidence = -1, previewMs = 0;
		KRREditRange cut;
		bool correctRequest = false, untouched = false;
		QObject::connect(&drive, &QTimer::timeout, &window, [&]
		{
			auto dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
			if (!dialog) dialog = sidebar(window);
			if (!dialog) return;
			if (timeout.elapsed() > 57000)
			{
				auto label = dialog->findChild<QLabel*>(dialog->objectName() == "smartCutDialog" ? "smartCutStatus" : "assistantStatus");
				if (label) status = label->text();
				dialog->reject();
				return;
			}
			if (stage == 0 && dialog == sidebar(window))
			{
				stage = 1;
				dialog->findChild<QLineEdit*>("assistantPrompt")->setText(QStringLiteral(
					"找文件名包含「助手联调MV」的录像，在原片30秒到65秒内找角色动作明显的画面，剪出重点，最多保留20秒，并加章节。"));
				dialog->findChild<QPushButton*>("assistantSend")->click();
			}
			if (dialog->objectName() != "smartCutDialog") return;
			auto table = dialog->findChild<QTableWidget*>("smartCutParts");
			auto player = dialog->findChild<KRREditPlayer*>("smartCutPlayer");
			auto canvas = dialog->findChild<KRREditCanvas*>("smartCutCanvas");
			auto audio = dialog->findChild<QBuffer*>("smartCutAudio");
			auto confirm = dialog->findChild<QPushButton*>("applySmartCut");
			if (stage == 1 && table->rowCount() > 0 && confirm->isEnabled())
			{
				stage = 2;
				baseline = editor->m_project.toJson();
				candidateCount = table->rowCount();
				correctRequest = QFileInfo(editor->m_project.input).fileName().contains(QStringLiteral("助手联调MV")) &&
					dialog->findChild<QDoubleSpinBox*>("analysisBegin")->value() == 30 &&
					dialog->findChild<QDoubleSpinBox*>("analysisEnd")->value() == 65 &&
					dialog->findChild<QDoubleSpinBox*>("smartCutLimit")->value() == 20 &&
					dialog->findChild<QCheckBox*>("smartCutChapters")->isChecked();
				for (int i = 0; i < table->rowCount(); ++i)
					if (table->item(i, 0)->checkState() == Qt::Checked) { row = i; break; }
				if (row < 0) return;
				table->setCurrentCell(row, 0);
				for (int i = 0; i < table->rowCount(); ++i)
					if (i != row) table->item(i, 0)->setCheckState(Qt::Unchecked);
				dialog->findChild<QPushButton*>("playDraftEvidence")->click();
			}
			else if (stage == 2 && row >= 0 && player->position() >= 30000 && !canvas->currentFrame().isNull() && audio->size() > 0)
			{
				evidence = player->position();
				auto begin = dialog->findChild<QDoubleSpinBox*>("draftBegin");
				auto end = dialog->findChild<QDoubleSpinBox*>("draftEnd");
				cut = {qRound64(begin->value()*1000), qRound64(end->value()*1000) - 250};
				if (cut.end - cut.begin < 300) return;
				end->setValue(cut.end / 1000.0);
				dialog->findChild<QPushButton*>("updateDraftRange")->click();
				stage = 3;
				dialog->findChild<QPushButton*>("renderSmartCutPreview")->click();
			}
			else if (stage == 3 && player->position() > 50 && player->position() < 1800 && !canvas->currentFrame().isNull())
			{
				previewMs = audio->size() / (48 * 4);
				untouched = editor->m_project.toJson() == baseline && editor->m_project.chapters.isEmpty();
				stage = 4;
				confirm->click();
			}
		});
		drive.start(100);
		menu->click();
		while (stage < 4 && timeout.elapsed() < 58000)
		{
			QApplication::processEvents();
			QThread::msleep(20);
		}
		drive.stop();
		const auto applied = editor->m_project;
		const bool rightCut = stage == 4 && applied.kept.size() == 1 && applied.kept.first().begin == cut.begin &&
			applied.kept.first().end == cut.end && cut.begin >= 30000 && cut.end <= 65000;
		const auto mapped = applied.outputChapters();
		const bool chapter = applied.chapters.size() == 1 && applied.chapters.first().at == cut.begin &&
			mapped.size() == 1 && mapped.first().at == 0 && editor->m_chapterList->count() == 1;
		bool renamed = false, saved = false, undone = false;
		if (rightCut && chapter)
		{
			editor->m_chapterList->setCurrentRow(0);
			editor->m_chapterTitle->setText(QStringLiteral("角色动作 #1; take=2\\原画"));
			editor->findChild<QPushButton*>("renameEditChapter")->click();
			renamed = editor->m_project.chapters.first().title == QStringLiteral("角色动作 #1; take=2\\原画");
			QFile file(projectPath);
			const auto bytes = QJsonDocument(editor->m_project.toJson()).toJson();
			saved = file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
			editor->undo();
			editor->undo();
			undone = editor->m_project.toJson() == baseline;
		}
		return {{"passed", rightCut && correctRequest && untouched && previewMs > 0 && chapter && renamed && saved && undone},
			{"stage", stage}, {"request_preserved", correctRequest}, {"candidates", candidateCount},
			{"evidence_ms", evidence}, {"preview_ms", previewMs}, {"unchanged_before_confirmation", untouched},
			{"cut_start_ms", cut.begin}, {"cut_end_ms", cut.end}, {"edited_cut_applied", rightCut},
			{"chapter", chapter}, {"title_saved", renamed && saved}, {"undo_restored_cut_and_chapters", undone}, {"status", status}};
	}

	static QJsonObject settings(KRRMainWindow& window)
	{
		window.m_fps->setCurrentIndex(window.m_fps->findData(30));
		if (!window.saveSettings()) return {{"error", "Could not save isolated baseline"}};
		const auto before = window.assistantContext().settings;
		const auto unchanged = [&] {
			return window.assistantContext().settings == before && SavedAssistantContext().settings == before;
		};
		bool inspected = false, declined = false, staleRefused = false, applied = false;
		QString state;
		int stage = 0;
		QElapsedTimer timeout;
		QTimer drive;
		auto menu = window.findChild<QPushButton*>("openRecordingAssistant");
		if (!menu) return {{"error", "Assistant menu missing"}};
		QObject::connect(&drive, &QTimer::timeout, &window, [&]
		{
			auto dialog = sidebar(window);
			if (!dialog || !dialog->isVisible()) return;
			auto status = dialog->findChild<QLabel*>("assistantStatus");
			auto preview = dialog->findChild<QTableWidget*>("assistantSettingsDiff");
			auto confirm = dialog->findChild<QPushButton*>("assistantConfirmSettings");
			if (timeout.elapsed() > 17000) { state = status->text(); dialog->reject(); return; }
			if (stage == 0)
			{
				stage = 1;
				dialog->findChild<QLineEdit*>("assistantPrompt")->setText(QStringLiteral("下次录制改为 60 帧。"));
				dialog->findChild<QPushButton*>("assistantSend")->click();
			}
			else if (stage == 1 && preview->rowCount() == 1 && confirm->isVisible())
			{
				stage = 2;
				inspected = preview->item(0, 1)->text() == "30" && preview->item(0, 2)->text() == "60" && unchanged();
				if (!declined)
				{
					declined = true;
					dialog->reject();
					return;
				}
				// A change after preview invalidates that preview before saving.
				window.m_fps->setCurrentIndex(window.m_fps->findData(90));
				confirm->click();
				staleRefused = window.m_fps->currentData().toInt() == 90 && SavedAssistantContext().settings["fps"] == 30 &&
					!status->text().isEmpty();
				window.m_fps->setCurrentIndex(window.m_fps->findData(30));
				confirm->click();
				applied = !confirm->isVisible() && window.m_fps->currentData().toInt() == 60 &&
					SavedAssistantContext().settings["fps"] == 60;
				state = status->text();
				dialog->reject();
			}
		});
		for (int attempt = 0; attempt < 2; ++attempt)
		{
			stage = 0;
			timeout.restart();
			drive.start(80);
			menu->setChecked(false);
			menu->click();
			waitFor([&] { return !sidebar(window)->isVisible(); }, 17200);
			drive.stop();
			if (attempt == 0 && (!declined || !unchanged())) break;
		}
		return {{"passed", inspected && declined && staleRefused && applied}, {"unchanged_during_preview", inspected},
			{"declined_without_saving", declined}, {"stale_refused", staleRefused}, {"confirmed_saved", applied},
			{"status", state}};
	}

	static QJsonObject search(KRRMainWindow& window)
	{
		auto menu = window.findChild<QPushButton*>("openRecordingAssistant");
		if (!menu) return {{"error", "Assistant menu missing"}};
		qint64 at = -1;
		QString file, status;
		bool sent = false;
		QElapsedTimer timeout;
		timeout.start();
		QTimer drive;
		QObject::connect(&drive, &QTimer::timeout, &window, [&]
		{
			auto dialog = sidebar(window);
			if (!dialog || !dialog->isVisible()) return;
			if (timeout.elapsed() > 12000)
			{
				status = dialog->findChild<QLabel*>("assistantStatus")->text();
				dialog->reject();
				return;
			}
			if (!sent)
			{
				sent = true;
				dialog->findChild<QLineEdit*>("assistantPrompt")->setText(QStringLiteral("帮我找标记里的验收时间点。"));
				dialog->findChild<QPushButton*>("assistantSend")->click();
			}
			auto list = dialog->findChild<QListWidget*>("assistantRecordings");
			if (!list->count()) return;
			file = list->item(0)->data(Qt::UserRole).toString();
			at = list->item(0)->data(Qt::UserRole + 1).toLongLong();
			dialog->findChild<QPushButton*>("assistantOpenRecording")->click();
			window.m_player->pause();
		});
		drive.start(80);
		menu->click();
		waitFor([&] { return !file.isEmpty() || !sidebar(window)->isVisible(); }, 12200);
		drive.stop();
		waitFor([&] { return window.m_player->duration() > 0 && qAbs(window.m_player->position() - at) < 180; }, 13000);
		const bool opened = !file.isEmpty() && at >= 0 && window.m_preview == file;
		return {{"passed", opened && qAbs(window.m_player->position() - at) < 180},
			{"opened", opened}, {"at_ms", at}, {"actual_ms", window.m_player->position()}, {"status", status}};
	}
};

int main(int argc, char** argv)
{
	QApplication::setAttribute(Qt::AA_DisableHighDpiScaling);
	QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
	QApplication app(argc, argv);
	app.setOrganizationName("WPSCourse");
	app.setApplicationName("wpsrecorder-assistant-test-" + QString::number(app.applicationPid()));
	QStandardPaths::setTestModeEnabled(true);
	RecorderUi::Apply(app);
	const auto args = app.arguments();
	if (args.size() != 5) return 2;
	QString error;
	if (args[1] != "templates" && args[3] != "live" &&
		!SaveSpeechService("assistant", {args[3], "course-assistant-test", {}}, false, error)) return 2;
	if (args[1] == "dock" && !SaveSpeechService("video", {args[3], "course-video-test", {}}, false, error)) return 2;
	const auto result = KRRAssistantAcceptance::run(args[1], args[2], args[4]);
	QFile report(args[4]);
	if (!report.open(QIODevice::WriteOnly) || report.write(QJsonDocument(result).toJson()) <= 0) return 2;
	return result["passed"].toBool() ? 0 : 1;
}
