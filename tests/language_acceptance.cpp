#include "displaygeometry.h"
#include "mainwindow.h"
#include "recordingassistanttools.h"
#include "studioapp.h"
#include "uilanguage.h"
#include "uitheme.h"
#include <QAction>
#include <QApplication>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QTimer>
#include <windows.h>

static bool Chinese(const QString &text)
{
	static const QRegularExpression pattern("[\\x{4e00}-\\x{9fff}]");
	return pattern.match(text).hasMatch();
}
static QString Plain(QString label)
{
	return label.remove('&');
}
static QString CheckOptionalHotkeys()
{
	QSettings settings(DataDirectory() + "/ui-settings.ini", QSettings::IniFormat);
	settings.setIniCodec("UTF-8");
	const QStringList initial{"Ctrl+Alt+Shift+F10", "Ctrl+Alt+Shift+F11", "", "", "", "", "", "Ctrl+Alt+Shift+F12"};
	const QStringList firstCleared{"", initial[1], "", "", "", "", "", initial[7], "Ctrl+Alt+I"};
	const QStringList allCleared{"", "", "", "", "", "", "", "", ""};
	settings.setValue("hotkeys", initial);
	settings.sync();
	const auto available = [](UINT key) {
		if (!RegisterHotKey(nullptr, 890, MOD_CONTROL | MOD_ALT | MOD_SHIFT, key)) return false;
		UnregisterHotKey(nullptr, 890);
		return true;
	};
	const auto dialog = [](KRRMainWindow &window, const QString &button, bool accept, bool expectAllBlank) {
		QAction *entry = nullptr;
		for (auto *action : window.findChildren<QAction *>())
			if (action->text() == Ui("全局快捷键")) { entry = action; break; }
		if (!entry) return false;
		bool checked = false;
		QTimer timeout;
		timeout.setSingleShot(true);
		QObject::connect(&timeout, &QTimer::timeout, &window, [&] {
			if (auto *popup = window.findChild<QDialog *>("globalHotkeysDialog")) popup->reject();
		});
		QTimer::singleShot(0, &window, [&] {
			auto *popup = qobject_cast<QDialog *>(QApplication::activeModalWidget());
			if (!popup || popup->objectName() != "globalHotkeysDialog") return;
			const auto edits = popup->findChildren<QKeySequenceEdit *>();
			if (edits.size() == 9)
			{
				if (expectAllBlank)
				{
					checked = true;
					for (auto *edit : edits) checked &= edit->keySequence().isEmpty();
				}
				else if (auto *clear = popup->findChild<QPushButton *>(button))
				{
					const auto *first = popup->findChild<QKeySequenceEdit *>("globalHotkey0");
					const auto *second = popup->findChild<QKeySequenceEdit *>("globalHotkey1");
					const auto before = second->keySequence();
					clear->click();
					checked = first->keySequence().isEmpty() &&
						(second->keySequence() == (button == "clearAllGlobalHotkeys" ? QKeySequence() : before));
				}
			}
			if (accept && checked) popup->accept();
			else popup->reject();
		});
		timeout.start(1500);
		entry->trigger();
		return checked;
	};
	if (!available(VK_F10) || !available(VK_F11) || !available(VK_F12)) return "QA hotkey chords are already in use";
	{
		KRRMainWindow window;
		if (available(VK_F10) || available(VK_F11) || available(VK_F12)) return "The initial Windows hotkeys were not registered";
		if (!dialog(window, "clearGlobalHotkey0", true, false)) return "Single clear did not update the dialog";
		settings.sync();
		if (settings.value("hotkeys").toStringList() != firstCleared || !available(VK_F10) || available(VK_F11) || available(VK_F12))
			return "Single clear did not release just the selected Windows shortcut";
		if (!dialog(window, "clearAllGlobalHotkeys", false, false) || settings.value("hotkeys").toStringList() != firstCleared ||
			available(VK_F11)) return "Cancel did not preserve the previous shortcut";
		if (!dialog(window, "clearAllGlobalHotkeys", true, false) || settings.value("hotkeys").toStringList() != allCleared ||
			!available(VK_F11) || !available(VK_F12)) return "Clear all did not persist all empty slots or release the shortcuts";
	}
	KRRMainWindow reopened;
	if (!dialog(reopened, {}, false, true) || !available(VK_F10) || !available(VK_F11) || !available(VK_F12))
		return "Empty shortcuts did not survive reopening";
	return {};
}
int main(int argc, char **argv)
{
	InitializeDisplayCoordinates();
	QApplication::setAttribute(Qt::AA_DisableHighDpiScaling);
	QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
	QApplication app(argc, argv);
	app.setOrganizationName("WPSCourse");
	app.setApplicationName("wpsrecorder-language-qa-" + QString::number(app.applicationPid()));
	app.setApplicationVersion("2.2.6");
	const auto arguments = app.arguments();
	if (arguments.size() < 2 || arguments.size() > 3) return 2;
	if (arguments.size() == 3 && !QFile::copy(arguments[2], RecorderUiSettingsPath()))
	{
		qCritical("Could not seed the isolated UI settings.");
		return 2;
	}
	qputenv("WPSRECORDER_INSTANCE", ("language-qa-" + QByteArray::number(app.applicationPid())));
	RecorderUi::Apply(app);
	KRRUiLanguage translations;
	QJsonArray results;
	QString error;
	for (const auto &language : {QString("zh-CN"), QString("en"), QString("zh-CN")})
	{
		const bool zh = language == "zh-CN";
		if (!WriteUiLanguage(language) || ReadUiLanguage() != language || !translations.install(ReadUiLanguage()))
		{
			error = "Could not save or load selected language";
			break;
		}
		KRRMainWindow window;
		const auto nav = window.findChild<QListWidget *>("settingsNav");
		const auto choice = window.findChild<QComboBox *>("interfaceLanguage");
		const auto apply = window.findChild<QPushButton *>("applyInterfaceLanguage");
		const QListWidgetItem* languagePage = nullptr;
		for (int row = 0; nav && row < nav->count(); ++row)
			if (nav->item(row)->data(Qt::UserRole) == "interface-language") languagePage = nav->item(row);
		if (!languagePage || !choice || !apply || choice->currentData() != language || apply->isEnabled() ||
			languagePage->text() != (zh ? QString::fromUtf8("界面语言") : "Interface language") ||
			Chinese(window.windowTitle()) != zh)
		{
			error = "Main window or language page did not use the selected language";
			break;
		}
		QString requested;
		QObject::connect(&window, &KRRMainWindow::languageChangeRequested, &window,
			[&requested](const QString &next) { requested = next; });
		choice->setCurrentIndex(zh ? 1 : 0);
		QTimer::singleShot(25, &window, [] {
			if (auto *message = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
				message->button(QMessageBox::Yes)->click();
		});
		QTimer::singleShot(1500, &window, [] {
			if (auto *message = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) message->reject();
		});
		apply->click();
		if (requested != (zh ? "en" : "zh-CN"))
		{
			error = "Language apply did not request the correct locale";
			break;
		}
		QMessageBox quit(QMessageBox::Question, Ui("录制仍在进行"), Ui("关闭界面后，后台录制会继续。仍要关闭吗？"),
			QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
		QColorDialog color;
		color.setOption(QColorDialog::DontUseNativeDialog);
		QFileDialog picker;
		picker.setOption(QFileDialog::DontUseNativeDialog);
		const auto yes = Plain(quit.button(QMessageBox::Yes)->text());
		const auto no = Plain(quit.button(QMessageBox::No)->text());
		const auto cancel = Plain(quit.button(QMessageBox::Cancel)->text());
		const auto fileName = Plain(picker.labelText(QFileDialog::FileName));
		QStringList colorFields;
		for (auto *label : color.findChildren<QLabel *>())
			if (!label->text().isEmpty()) colorFields.append(Plain(label->text()));
		bool colorTranslated = false;
		for (const auto &field : colorFields) colorTranslated |= Chinese(field);
		if (Chinese(yes) != zh || Chinese(no) != zh || Chinese(cancel) != zh ||
			Chinese(color.windowTitle()) != zh || colorTranslated != zh || Chinese(fileName) != zh ||
			Chinese(Ui("字幕样式")) != zh || Chinese(Ui("生成试听")) != zh ||
			Chinese(UiDiagnostic("Selected window is unavailable or minimized")) != zh)
		{
			error = "Qt widgets or application dialogs were not localized: " + yes + " / " + no + " / " + cancel +
				" / " + color.windowTitle() + " / " + fileName;
			break;
		}
		results.append(QJsonObject{{"language", language}, {"requested_switch", requested},
			{"main", window.windowTitle()}, {"yes", yes}, {"no", no}, {"cancel", cancel},
			{"color_title", color.windowTitle()}, {"color_fields", QJsonArray::fromStringList(colorFields)},
			{"file_name_label", fileName}, {"subtitle_style", Ui("字幕样式")}, {"voice_preview", Ui("生成试听")}});
	}
	if (error.isEmpty())
	{
		int stage = 0;
		QTimer poll;
		QObject::connect(&poll, &QTimer::timeout, &app, [&] {
			const auto expected = stage == 1 ? "en" : "zh-CN";
			if (CurrentUiLanguage() != expected) return;
			for (auto *widget : app.topLevelWidgets())
			{
				auto *window = qobject_cast<KRRMainWindow *>(widget);
				if (!window || !window->isVisible()) continue;
				const auto *nav = window->findChild<QListWidget *>("settingsNav");
				auto *choice = window->findChild<QComboBox *>("interfaceLanguage");
				auto *apply = window->findChild<QPushButton *>("applyInterfaceLanguage");
				if (!nav || !choice || !apply || choice->currentData().toString() != expected ||
					ReadUiLanguage() != expected || Chinese(window->windowTitle()) != (stage != 1))
					return;
				if (stage == 2)
				{
					stage = 3;
					window->close();
					return;
				}
				choice->setCurrentIndex(stage == 0 ? 1 : 0);
				++stage;
				QTimer::singleShot(60, &app, [] {
					if (auto *msg = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
						msg->button(QMessageBox::Yes)->click();
				});
				apply->click();
				return;
			}
		});
		QTimer watchdog;
		watchdog.setSingleShot(true);
		QObject::connect(&watchdog, &QTimer::timeout, &app, [&] {
			if (auto *msg = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) msg->reject();
			app.exit(2);
		});
		watchdog.start(8000);
		poll.start(100);
		const auto exitCode = RunRecordingStudio(app);
		if (exitCode != 0 || stage != 3 || ReadUiLanguage() != "zh-CN")
			error = "Production interface reload did not complete (stage " + QString::number(stage) + ")";
	}
	const bool reloadPassed = error.isEmpty();
	const auto hotkeyError = reloadPassed ? CheckOptionalHotkeys() : QString();
	if (error.isEmpty()) error = hotkeyError;
	QFile report(app.arguments()[1]);
	if (!report.open(QIODevice::WriteOnly)) return 2;
	report.write(QJsonDocument(QJsonObject{{"passed", error.isEmpty()}, {"error", error}, {"cases", results},
		{"production_reload", reloadPassed}, {"optional_hotkeys", error.isEmpty()}}).toJson());
	return error.isEmpty() ? 0 : 1;
}
