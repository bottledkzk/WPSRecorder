// main.cpp | 3226535826@qq.com | 2026-10-31 | GUI entry point.
#include "studioapp.h"
#include "uitheme.h"
#include "displaygeometry.h"
#include "windowidentity.h"
#include <windows.h>
#include <shobjidl.h>
#include <QApplication>
#include <QFont>

int main(int argc, char* argv[])
{
	InitializeDisplayCoordinates();
	SetCurrentProcessExplicitAppUserModelID(RecorderAppId);
	// Media Foundation avoids DirectShow video filter stalls on supported Windows builds.
	if (!qEnvironmentVariableIsSet("QT_MULTIMEDIA_PREFERRED_PLUGINS"))
		qputenv("QT_MULTIMEDIA_PREFERRED_PLUGINS", "windowsmediafoundation");
	QApplication::setAttribute(Qt::AA_DisableHighDpiScaling);
	QApplication app(argc, argv);
	app.setOrganizationName("WPSCourse");
	app.setApplicationName("wpsrecorder");
	app.setApplicationVersion(WPSRECORDER_VERSION);
	app.setWindowIcon(QIcon(":/ui/recorder.ico"));
	app.setFont(QFont("Microsoft YaHei UI", 10));
	RecorderUi::Apply(app);
	return RunRecordingStudio(app);
}
