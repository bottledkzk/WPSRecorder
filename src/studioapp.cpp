#include "studioapp.h"
#include "mainwindow.h"
#include "minimalrecorder.h"
#include "uilanguage.h"
#include "windowidentity.h"
#include <QApplication>
#include <QMessageBox>
#include <QMenu>
#include <QSystemTrayIcon>
#include <QTimer>
#ifdef WPS_WEB_STUDIO
#include "webstudio.h"
#endif

static int RunWorkbench(QApplication &app, KRRUiLanguage &language, bool &initial)
{
	constexpr int ReloadLanguage = 77;
	int result;
	do
	{
		if (!language.install(ReadUiLanguage()))
			qWarning("WPS Recorder could not load the interface translation.");
		KRRMainWindow window;
		KRRWindowIdentity identity(reinterpret_cast<HWND>(window.winId()));
		QObject::connect(&window, &KRRMainWindow::languageChangeRequested, &window, [&](const QString &next) {
			QTimer::singleShot(0, &window, [&, next] {
				if (!window.close()) return;
				if (!WriteUiLanguage(next))
				{
					window.show();
					QMessageBox::warning(&window, Ui("切换界面语言"), Ui("无法保存界面语言。"));
					return;
				}
				app.exit(ReloadLanguage);
			});
		});
#ifdef WPS_WEB_STUDIO
		KRRWebStudio studio(window);
#endif
		QTimer backgroundCheck;
		backgroundCheck.setInterval(250);
		QObject::connect(&backgroundCheck, &QTimer::timeout, &window, [&]
		{
			if (window.isVisible())
			{
				backgroundCheck.stop();
				return;
			}
			if (window.canEnterMinimalMode())
			{
				backgroundCheck.stop();
				if (window.close()) app.exit(SwitchToMinimalRecorder);
				else window.restoreWorkbench();
			}
		});
		auto openMinimal = [&]
		{
			QTimer::singleShot(0, &window, [&]
			{
				if (window.canEnterMinimalMode())
				{
					if (window.close()) app.exit(SwitchToMinimalRecorder);
					return;
				}
				window.showBackgroundBar();
				backgroundCheck.start();
			});
		};
		QMenu trayMenu;
		trayMenu.addAction(Ui("极简录制"), &window, &KRRMainWindow::switchRecordingMode);
		trayMenu.addAction(Ui("录制工作台"), &window, &KRRMainWindow::restoreWorkbench);
		trayMenu.addAction(Ui("退出"), &window, [&window]
		{
			window.restoreWorkbench();
			window.requestUserClose();
		});
		QSystemTrayIcon tray(app.windowIcon());
		tray.setToolTip(Ui("WPS Recorder"));
		tray.setContextMenu(&trayMenu);
		if (QSystemTrayIcon::isSystemTrayAvailable()) tray.show();
		QObject::connect(&window, &KRRMainWindow::minimalRecorderRequested, &window, openMinimal);
		QObject::connect(&window, &KRRMainWindow::backgroundRequested, &window, openMinimal);
		QObject::connect(&tray, &QSystemTrayIcon::activated, &window, [&window](auto reason)
		{
			if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick)
				window.restoreWorkbench();
		});
		window.show();
		const int edit = app.arguments().indexOf("--edit");
		if (initial && edit >= 0 && edit + 1 < app.arguments().size())
#ifdef WPS_WEB_STUDIO
			studio.startupFile(app.arguments()[edit + 1]);
		studio.start();
#else
			window.editFile(app.arguments()[edit + 1]);
#endif
		initial = false;
		result = app.exec();
	} while (result == ReloadLanguage);
	return result;
}

int RunRecordingStudio(QApplication &app)
{
	KRRUiLanguage language;
	bool initial = true;
	int next = app.arguments().contains("--minimal") ? SwitchToMinimalRecorder : SwitchToRecordingStudio;
	while (next == SwitchToMinimalRecorder || next == SwitchToRecordingStudio)
	{
		if (next == SwitchToMinimalRecorder)
		{
			language.install(ReadUiLanguage());
			next = RunMinimalRecorder(app);
			initial = false;
		}
		else next = RunWorkbench(app, language, initial);
	}
	return next;
}
