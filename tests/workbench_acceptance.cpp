#include "cameracapture.h"
#include "annotationoverlay.h"
#include "controlserver.h"
#include "clipeditor.h"
#include "displaygeometry.h"
#include "editwidgets.h"
#include "framecapture.h"
#include "mainwindow.h"
#include "playerwindow.h"
#include "previewvideo.h"
#include "recordingcheckdialog.h"
#include "recordingassistantdialog.h"
#include "studioassistant.h"
#include "recordingconfig.h"
#include "recordingmarkers.h"
#include "recordingwidgets.h"
#include "uitheme.h"
#include "uilanguage.h"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QImage>
#include <QInputDialog>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMediaPlayer>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QProgressBar>
#include <QProcess>
#include <QRegularExpression>
#include <QScreen>
#include <QSlider>
#include <QScrollArea>
#include <QScrollBar>
#include <QSplitter>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTabWidget>
#include <QTreeWidget>
#include <QPlainTextEdit>
#include <QSettings>
#include <QSpinBox>
#include <QThread>
#include <QToolBar>
#include <QWindow>
#include <algorithm>
#include <functional>
static void Mouse(QWidget *w, QEvent::Type type, const QPoint &p)
{
	QMouseEvent event(type, p, w->mapToGlobal(p), Qt::LeftButton,
					  type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
	QApplication::sendEvent(w, &event);
}
static bool WaitFor(const std::function<bool()>& condition, int timeout = 15000)
{
	QElapsedTimer watch;
	watch.start();
	while (!condition())
	{
		if (watch.elapsed() >= timeout) return false;
		QApplication::processEvents();
		QThread::msleep(15);
	}
	return true;
}

static QDialog* Modal(const QString& name)
{
	auto dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
	return dialog && dialog->objectName() == name ? dialog : nullptr;
}

static void ChooseOutput(const QString& path)
{
	auto dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget());
	if (!dialog) return;
	dialog->setDirectory(QFileInfo(path).absolutePath());
	dialog->selectFile(QFileInfo(path).fileName());
	if (auto edit = dialog->findChild<QLineEdit*>("fileNameEdit")) edit->setText(QFileInfo(path).fileName());
	QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection);
}

static bool CheckDesktopPreview(const QString& source, const QString& root, QJsonObject& result)
{
	const auto monitors = KRRFrameCapture::Monitors();
	if (monitors.isEmpty() || (source == "gdi" && monitors.size() < 2)) return false;
	const auto first = monitors.first().toObject();
	QWidget swatch(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
	swatch.setAttribute(Qt::WA_ShowWithoutActivating);
	swatch.setStyleSheet(source == "black" ? "background-color:#000000;" : "background-color:#19a5d9;");
	swatch.setGeometry(first["x"].toInt() + 32, first["y"].toInt() + 60, 400, 240);
	swatch.show();
	if (!WaitFor([&] { return swatch.windowHandle() && swatch.windowHandle()->isExposed(); }, 3000)) return false;
	QApplication::processEvents();
	QThread::msleep(140);
	RECT native{};
	if (!GetWindowRect(reinterpret_cast<HWND>(swatch.winId()), &native)) return false;
	KRRRecordingConfig config;
	config.source = source == "screen" ? "screen" : "region";
	config.region = source == "gdi" ? KRRFrameCapture::VirtualDesktop() :
		QRect(native.left + 8, native.top + 8, native.right - native.left - 16, native.bottom - native.top - 16);
	config.output = QDir(root).absoluteFilePath("desktop-check.mp4");
	config.encoder = "libx264";
	QProcess probe;
	HideConsoleProcess(probe);
	probe.start(QCoreApplication::applicationDirPath() + "/wpsrecorder_cli.exe", {"--capture-check"});
	if (!probe.waitForStarted(5000)) return false;
	probe.write(QJsonDocument(config.toJson()).toJson(QJsonDocument::Compact));
	probe.closeWriteChannel();
	if (!probe.waitForFinished(15000)) return false;
	const auto report = QJsonDocument::fromJson(probe.readAllStandardOutput()).object();
	const auto capture = report["capture"].toObject();
	const auto image = QImage::fromData(QByteArray::fromBase64(report["image"].toString().toLatin1()), "JPEG");
	int swatchPixels = 0;
	for (int y = 0; y < image.height(); y += 2)
		for (int x = 0; x < image.width(); x += 2)
		{
			const auto rgb = image.pixel(x, y);
			if (qRed(rgb) < 75 && qGreen(rgb) > 120 && qBlue(rgb) > 165) ++swatchPixels;
		}
	result["source"] = source;
	result["capture"] = capture;
	result["preview_width"] = image.width();
	result["preview_height"] = image.height();
	result["colored_pixels"] = swatchPixels;
	result["capture_error"] = report["capture_error"];
	if (!report["ok"].toBool() || image.isNull()) return false;
	if (source == "black") return capture["desktop_black"].toBool();
	return swatchPixels >= (source == "region" ? 2000 : 150) &&
		(source != "gdi" || capture["api"].toString() == "GDI fallback");
}

class KRRWorkbenchAcceptance : public QObject
{
  public:
	static bool focused(const QString& name, const QString& root, const QString& argument, QJsonObject& result)
	{
		const auto input = QFileInfo(argument).isFile() ? QFileInfo(argument).absoluteFilePath() : argument;
		if (name == "preflight-desktop") return CheckDesktopPreview(input, root, result);
		KRRMainWindow w;
		w.show();
		w.m_directory->setText(root);
		w.m_resolution->setCurrentText("640x360");
		w.m_fps->setCurrentIndex(w.m_fps->findData(30));
		w.m_encoder->setCurrentIndex(w.m_encoder->findData("libx264"));
		w.m_cameraEnabled->setChecked(false);
		w.m_cameraTrack->setChecked(false);
		w.m_floatButton->setChecked(false);
		auto fail = [&](const QString& reason)
		{
			result["error"] = reason;
			result["message"] = w.m_message->text();
			if ((name == "screen-stop-paths" || name == "lecture-recording") && w.m_active)
			{
				QJsonObject response;
				QString error;
				SendControl({{"command", "stop"}}, response, error, 1500);
			}
			return false;
		};
		if (name == "lecture-recording")
		{
			QImage pattern(640, 360, QImage::Format_RGB32);
			for (int y = 0; y < pattern.height(); ++y)
			{
				auto line = reinterpret_cast<QRgb*>(pattern.scanLine(y));
				for (int x = 0; x < pattern.width(); ++x) line[x] = (x / 4) % 2 ? qRgb(248, 248, 248) : qRgb(8, 8, 8);
			}
			const auto image = QDir(root).filePath("lecture-source.png");
			if (!pattern.save(image)) return fail("Could not save the lecture test image");
			w.m_source->setCurrentIndex(w.m_source->findData("image"));
			w.m_image->setText(image);
			w.m_audio->setCurrentIndex(w.m_audio->findData("none"));
			w.m_encoder->setCurrentIndex(w.m_encoder->findData("auto"));
			w.m_cursor->setChecked(false);
			w.startRecording();
			if (!WaitFor([&] { return w.m_active && w.m_lastRecordingStatus["frames_written"].toInt() >= 12; }, 11000))
				return fail("The lecture test recording did not start");
			w.showAnnotation();
			const auto toolbar = w.m_overlay->findChild<QToolBar*>();
			if (!toolbar || !w.m_overlay->isVisible()) return fail("The recording lecture tools did not open");
			auto tool = [toolbar](const QString& name)
			{
				for (auto action : toolbar->actions()) if (action->data() == name) { action->trigger(); break; }
			};
			auto point = [&w](double x, double y)
			{
				return QPoint(qRound(x * w.m_overlay->width()), qRound(y * w.m_overlay->height()));
			};
			auto drag = [&w](const QList<QPoint>& points)
			{
				Mouse(w.m_overlay, QEvent::MouseButtonPress, points.first());
				for (int i = 1; i < points.size(); ++i) Mouse(w.m_overlay, QEvent::MouseMove, points[i]);
				Mouse(w.m_overlay, QEvent::MouseButtonRelease, points.last());
			};
			tool("solid");
			drag({point(.60, .67), point(.46, .43)});
			tool("pixel");
			drag({point(.16, .55), point(.70, .55), point(.70, .81)});
			tool("arrow");
			drag({point(.15, .24), point(.45, .24)});
			tool("rectangle");
			drag({point(.58, .16), point(.83, .33)});
			tool("highlight");
			drag({point(.13, .72), point(.34, .83)});
			tool("text");
			Mouse(w.m_overlay, QEvent::MouseButtonPress, point(.17, .30));
			const auto field = w.m_overlay->findChild<QLineEdit*>("lectureTextInput");
			if (!field || !field->isVisible()) return fail("The inline text field did not open during recording");
			field->setText("QA text");
			QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
			QApplication::sendEvent(field, &enter);
			const int first = w.m_lastRecordingStatus["frames_written"].toInt();
			if (!WaitFor([&] { return w.m_lastRecordingStatus["frames_written"].toInt() >= first + 36; }, 10000))
				return fail("Recording did not continue after drawing");
			w.m_stop->click();
			if (!WaitFor([&] { return !w.m_active && QFileInfo(w.m_lastOutput).isFile(); }, 10000))
				return fail("The lecture test recording did not finish");
			result["output"] = w.m_lastOutput;
			return true;
		}
		if (name == "screen-stop-paths")
		{
			QJsonArray frameRates;
			for (int i = 0; i < w.m_fps->count(); ++i) frameRates.append(w.m_fps->itemData(i).toInt());
			if (frameRates != QJsonArray{30, 60, 90, 144} || w.m_fps->isEditable())
				return fail("The recording frame rate selector differs from the four presets");
			w.m_resolution->setCurrentText("1920x1080");
			w.m_fps->setCurrentIndex(w.m_fps->findData(60));
			w.m_recommendedBitrate->setChecked(true);
			if (w.configuration().bitrate != 6000) return fail("1080p60 recommended bitrate is not 6000 kbps");
			w.m_resolution->setCurrentText("1280x720");
			if (w.configuration().bitrate != 2500) return fail("720p60 recommended bitrate is not 2500 kbps");
			w.m_resolution->setCurrentText("1920x1080");
			w.m_fps->setCurrentIndex(w.m_fps->findData(144));
			if (w.configuration().bitrate != 14500) return fail("1080p144 recommended bitrate is not 14500 kbps");
			w.m_fps->setCurrentIndex(w.m_fps->findData(60));
			w.m_recommendedBitrate->setChecked(false);
			w.m_bitrate->setValue(6200);
			w.m_fps->setCurrentIndex(w.m_fps->findData(144));
			if (w.configuration().fps != 144 || w.configuration().bitrate != 6200)
				return fail("Changing the frame-rate preset overwrote a manual bitrate");
			w.m_fps->setCurrentIndex(w.m_fps->findData(60));
			w.m_bitrate->setValue(6000);
			w.m_source->setCurrentIndex(w.m_source->findData("screen"));
			w.m_audio->setCurrentIndex(w.m_audio->findData("none"));
			w.m_encoder->setCurrentIndex(w.m_encoder->findData("auto"));
			w.m_floatButton->setChecked(true);
			if (!w.m_floatBar->isVisible()) return fail("Floating toolbar was not visible at idle");
			w.m_hotkeys[2] = "Ctrl+Shift+F11";
			w.registerHotkeys();

			QWidget card(nullptr, Qt::Window | Qt::FramelessWindowHint);
			card.setWindowTitle("Recorder QA moving screen fixture");
			card.setGeometry(KRRFrameCapture::CaptureArea(w.configuration()));
			card.setStyleSheet("background:#284e87;");
			QWidget tile(&card);
			tile.resize(320, 200);
			tile.setStyleSheet("background:#238763;");
			card.show();
			w.raise();
			QTimer motion, dismissWarnings;
			connect(&motion, &QTimer::timeout, &card, [&]
			{
				tile.move((tile.x() + 13) % qMax(1, card.width() - tile.width()), card.height() / 2);
			});
			connect(&dismissWarnings, &QTimer::timeout, &w, []
			{
				if (auto warning = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) warning->reject();
			});
			motion.start(100);
			dismissWarnings.start(100);
			if (!WaitFor([&] { return w.m_idleSequence > 6 && w.m_liveFrame->frameSize() == QSize(1920, 1080); }, 6500))
				return fail("Idle monitoring did not update at 1080p");
			result["idle_frame"] = QJsonArray{1920, 1080};
			const auto firstSequence = w.m_idleSequence;
			const auto firstImage = w.m_liveFrame->grab().toImage();
			if (!WaitFor([&] { return w.m_idleSequence >= firstSequence + 5 && w.m_liveFrame->grab().toImage() != firstImage; }, 3000))
				return fail("Idle monitoring did not show the changed source pixels");

			QJsonArray recordings;
			for (const QString& entry : {"main", "floating", "hotkey"})
			{
				qInfo("Testing Stop from %s", qPrintable(entry));
				w.selectWorkspace(KRRMainWindow::Record);
				w.startRecording();
				qint64 lastPreview = 0;
				int previewUpdates = 0;
				if (!WaitFor([&]
				{
					if (w.m_liveSequence > 0 && w.m_liveSequence != lastPreview)
					{
						lastPreview = w.m_liveSequence;
						++previewUpdates;
					}
					return w.m_active && w.m_lastRecordingStatus["phase"] == "recording" &&
					w.m_lastRecordingStatus["frames_written"].toInt() >= 100 && previewUpdates >= 20;
				}, 17000))
					return fail("1080p60 screen recording or live preview failed at " + entry);
				const auto liveSize = w.m_liveFrame->frameSize();
				if (liveSize != QSize(1920, 1080)) return fail("Live preview did not retain 1080p detail at " + entry);
				result["live_frame"] = QJsonArray{liveSize.width(), liveSize.height()};
				QElapsedTimer stop;
				stop.start();
				if (entry == "main") w.m_stop->click();
				else if (entry == "floating")
				{
					auto button = w.m_floatBar->findChild<QPushButton*>("floatStop");
					if (!w.m_floatBar->isVisible() || !button || !button->isEnabled())
						return fail("Floating Stop was unavailable during recording");
					button->click();
				}
				else
				{
					w.raise();
					w.activateWindow();
					const WORD keys[]{VK_LCONTROL, VK_LSHIFT, VK_F11, VK_F11, VK_LSHIFT, VK_LCONTROL};
					INPUT keyEvents[6]{};
					for (UINT i = 0; i < 6; ++i)
					{
						keyEvents[i].type = INPUT_KEYBOARD;
						keyEvents[i].ki.wVk = keys[i];
						if (i >= 3) keyEvents[i].ki.dwFlags = KEYEVENTF_KEYUP;
					}
					const auto pressed = SendInput(3, keyEvents, sizeof(INPUT));
					QThread::msleep(35);
					const auto released = SendInput(3, keyEvents + 3, sizeof(INPUT));
					if (pressed != 3 || released != 3) return fail("Windows could not inject the Stop test hotkey");
					if (!WaitFor([&] { return !w.m_stop->isEnabled(); }, 1400))
					{
						result["hotkey_os_delivery"] = false;
						PostMessageW(reinterpret_cast<HWND>(w.winId()), WM_HOTKEY, 402, MAKELPARAM(MOD_CONTROL | MOD_SHIFT, VK_F11));
					}
					else result["hotkey_os_delivery"] = true;
				}
				if (!WaitFor([&] { return !w.m_active && !w.m_preview.isEmpty() &&
					w.m_preview == w.m_lastOutput && QFileInfo(w.m_lastOutput).isFile(); }, 12000))
					return fail("Stop did not publish a playable file from " + entry);
				qInfo("Saved from %s", qPrintable(entry));
				const auto stopMs = stop.elapsed();
				KRREditProject video;
				QString error;
				QJsonObject container;
				if (!ProbeEditSource(w.m_lastOutput, video, error, &container) || video.size != QSize(1920, 1080) ||
					qAbs(video.fps - 60.) > .5 || video.duration < 900 ||
					container["streams"].toArray().first().toObject()["r_frame_rate"].toString() != "60/1")
					return fail("Unexpected video metadata after " + entry + ": " + error);
				if (!WaitFor([&] { return w.m_player->position() > 100; }, 12000))
					return fail("Recorded 60 fps preview did not play after " + entry);
				if (!w.m_floatBar->isVisible()) return fail("Floating toolbar did not remain available after Stop");
				recordings.append(QJsonObject{{"entry", entry}, {"stop_ms", stopMs},
					{"duration_ms", video.duration}, {"container_average_fps", video.fps},
					{"observed_preview_updates", previewUpdates},
					{"encoder", w.m_lastRecordingStatus["encoder"]},
					{"frames", w.m_lastRecordingStatus["frames_written"]},
					{"duplicate_frames", w.m_lastRecordingStatus["duplicate_frames"]}});
				result["recordings"] = recordings;
			}
			result["dynamic_preflight"] = true;
			qInfo("Testing expanded playback and early Stop");
			w.m_player->pause();
			w.m_fullscreen->setRecordings(QJsonArray{QJsonObject{{"at_ms", 500}, {"label", "QA chapter"}}},
				QStringList{w.m_preview}, w.m_preview);
			const auto expand = w.findChild<QPushButton*>("expandPreviewButton");
			if (!expand) return fail("The expanded preview entry was unavailable");
			expand->click();
			auto mode = w.m_fullscreen->findChild<QComboBox*>("previewWindowMode");
			auto chapter = w.m_fullscreen->findChild<QListWidget*>("previewRecordingChapters");
			auto chaptersToggle = w.m_fullscreen->findChild<QPushButton*>("previewChaptersToggle");
			auto header = w.m_fullscreen->findChild<QWidget*>("previewHeader");
			auto controls = w.m_fullscreen->findChild<QWidget*>("previewControls");
			if (!mode || !chapter || chapter->count() != 2 || !header || !controls || !chaptersToggle)
				return fail("Recording chapters were missing from expanded preview");
			if (!WaitFor([&] { return w.m_fullscreen->isFullScreen() && w.m_fullscreen->windowHandle() &&
				QRect(w.m_video->mapToGlobal({0, 0}), w.m_video->size()) == w.m_fullscreen->windowHandle()->screen()->geometry(); }, 3000) ||
				!w.m_fullscreen->windowFlags().testFlag(Qt::FramelessWindowHint))
				return fail("The fullscreen video surface did not reach all four screen edges");
			const auto screen = w.m_fullscreen->windowHandle()->screen()->geometry();
			RECT native{};
			GetWindowRect(reinterpret_cast<HWND>(w.m_fullscreen->winId()), &native);
			if (QRect(native.left, native.top, native.right - native.left, native.bottom - native.top) != screen ||
				header->isVisible() || controls->isVisible() || chapter->isVisible())
				return fail("The fullscreen native window or hidden controls left a border");
			Mouse(w.m_video, QEvent::MouseMove, QPoint(w.m_video->width() / 2, 3));
			if (!header->isVisible() || !controls->isVisible()) return fail("The pointer did not reveal the fullscreen controls");
			chaptersToggle->click();
			if (!chapter->isVisible() || w.m_video->size() != screen.size())
				return fail("The chapter panel was unavailable or resized the fullscreen video");
			const auto cue = chapter->visualItemRect(chapter->item(1)).center();
			Mouse(chapter->viewport(), QEvent::MouseButtonPress, cue);
			Mouse(chapter->viewport(), QEvent::MouseButtonRelease, cue);
			if (!WaitFor([&] { return qAbs(w.m_player->position() - 500) < 140; }, 3500))
				return fail("Clicking a recording chapter did not seek the preview");
			result["fullscreen_surface"] = QJsonArray{screen.x(), screen.y(), screen.width(), screen.height()};
			for (int i = 0; i < 2; ++i)
			{
				QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
				QApplication::sendEvent(w.m_fullscreen, &escape);
				if (w.m_fullscreen->isVisible() != (i == 0) || w.m_fullscreen->isFullScreen())
					return fail("Escape did not return to expanded preview and then the workbench");
			}
			if (qAbs(w.m_player->position() - 500) >= 140) return fail("Fullscreen exit changed the playhead");
			result["borderless_preview_chapters_and_escape"] = true;
			w.showAnnotation();
			if (!w.m_lectureButton->isChecked()) return fail("Opening lecture mode did not update the workbench");
			w.m_countdown->setCurrentIndex(w.m_countdown->findData(3));
			w.beginCountdown();
			if (!WaitFor([&] { return w.m_active; }, 8000) || !w.m_stop->isEnabled())
				return fail("Stop was unavailable immediately after the countdown");
			w.m_stop->click();
			if (!WaitFor([&] { return !w.m_active && w.m_stopProcess.state() == QProcess::NotRunning; }, 10000))
				return fail("Stop did not finish immediately after the countdown");
			result["stopped_after_countdown"] = true;
			return true;
		}
		if (name == "camera-preview")
		{
			auto button = w.findChild<QPushButton*>("testCamera");
			if (!button || w.m_camera->currentData().toInt() < 0) return fail("Camera is unavailable");
			QTimer watch;
			QElapsedTimer wall;
			wall.start();
			int frames = 0;
			QString error;
			connect(&watch, &QTimer::timeout, &w, [&]
			{
				if (auto warning = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
				{
					error = warning->text();
					warning->reject();
					return;
				}
				auto dialog = Modal("cameraTestPreview");
				if (!dialog) return;
				const auto status = dialog->findChild<QLabel*>("cameraTestStatus");
				const auto content = status ? status->text() : QString();
				frames = QRegularExpression(QStringLiteral("已接收 (\\d+) 帧")).match(content).captured(1).toInt();
				if (frames >= 12 || wall.elapsed() > 6000)
				{
					result["camera_preview_status"] = content;
					dialog->reject();
				}
			});
			watch.start(40);
			button->click();
			result["camera_preview_frames"] = frames;
			return frames >= 12 || fail(error.isEmpty() ? "Camera preview did not update" : error);
		}
		if (name == "lecture-screenshot-mask")
		{
			QImage source(640, 360, QImage::Format_RGB32);
			for (int y = 0; y < source.height(); ++y)
				for (int x = 0; x < source.width(); ++x)
					source.setPixelColor(x, y, x % 2 == 0 ? QColor("#328ce6") : QColor("#f9cf55"));
			const auto path = QDir(root).filePath("color-stripes.png");
			if (!source.save(path)) return fail("Could not save the screenshot source");
			w.m_source->setCurrentIndex(w.m_source->findData("image"));
			w.m_image->setText(path);
			w.m_clicks->setChecked(false);
			w.m_cursor->setChecked(false);
			w.m_effects = {{"masks", QJsonArray{
				QJsonObject{{"type", "mosaic"}, {"rect", QJsonArray{.1, .1, .4, .4}}},
				QJsonObject{{"type", "solid"}, {"rect", QJsonArray{.6, .1, .3, .4}}}}}};
			w.takeScreenshot();
			const auto files = QDir(root).entryList({"screenshot-*.png"}, QDir::Files);
			if (files.size() != 1) return fail("The recording screenshot was not saved");
			const QImage screenshot(QDir(root).filePath(files.front()));
			if (screenshot.size() != source.size()) return fail("Unexpected screenshot size");
			const auto blurred = screenshot.pixelColor(100, 100);
			result["blur"] = blurred != source.pixelColor(100, 100) &&
				blurred.red() > 80 && qAbs(blurred.red() - screenshot.pixelColor(101, 100).red()) < 10;
			result["solid"] = screenshot.pixelColor(480, 100) == QColor("#20252d");
			result["outside_unchanged"] = screenshot.pixelColor(100, 240) == source.pixelColor(100, 240);
			return result["blur"].toBool() && result["solid"].toBool() && result["outside_unchanged"].toBool() ||
				fail("The saved screenshot did not preserve the configured masks");
		}
		if (name == "scheduled-editor" || name == "delete-editor-draft")
		{
			const auto path = QDir(root).filePath("editor-copy.mp4");
			if (!QFile::copy(input, path)) return fail("Could not make an isolated recording copy");
			w.m_lastOutput = path;
			w.openPreview(path);
			auto editor = w.m_clipEditor;
			auto speech = editor->findChild<QAction*>("editorSpeech");
			if (!speech || !editor->openFile(path, root) ||
				!WaitFor([&] { return w.m_player->duration() > 0 && speech->isEnabled(); }, 9000))
				return fail("Could not prepare preview and editing");
			editor->show();
			auto timeline = editor->findChild<KRREditTimeline*>("editTimeline");
			if (!timeline) return fail("Timeline is unavailable");
			if (name == "scheduled-editor")
			{
				QImage fixture(640, 360, QImage::Format_RGB32);
				fixture.fill(QColor("#335577"));
				const auto image = QDir(root).filePath("schedule-source.png");
				if (!fixture.save(image)) return fail("Could not save scheduled source");
				auto config = w.configuration();
				config.source = "image";
				config.image = image;
				config.audio = "none";
				config.maxDuration = 12;
				config.output = QDir(root).filePath("scheduled.mp4");
				w.m_floatButton->setChecked(true);
				QJsonObject status;
				QString error;
				if (!EnsureControlService(error) ||
					!SendControl({{"command","schedule"}, {"config",config.toJson()},
						{"startAt",QDateTime::currentDateTimeUtc().addSecs(4).toString(Qt::ISODate)}}, status, error, 4000))
					return fail("Schedule could not start: " + error);
				if (!WaitFor([&] { return w.m_active && w.m_lastRecordingStatus["phase"] == "recording" &&
					w.m_lastRecordingStatus["frames_written"].toInt() >= 30; }, 14000))
					return fail("Scheduled recording did not reach the active state");
				const auto audioRow = QPoint(RecorderUi::Px(80), RecorderUi::Px(166));
				Mouse(timeline, QEvent::MouseButtonPress, audioRow);
				Mouse(timeline, QEvent::MouseButtonRelease, audioRow);
				if (!w.m_floatBar->isVisible() || !speech->isEnabled() || timeline->track() != 0)
					return fail("Scheduled recording disabled editor track selection or the floating toolbar");
				auto stop = w.m_floatBar->findChild<QPushButton*>("floatStop");
				if (!stop || !stop->isEnabled()) return fail("Scheduled recording cannot stop from the floating toolbar");
				stop->click();
				if (!WaitFor([&] { return !w.m_active && w.m_preview == config.output &&
					QFileInfo(config.output).size() > 0; }, 11000))
					return fail("Scheduled recording did not publish its output after Stop");
				result["editor_track_selection"] = true;
				result["floating_stop"] = true;
				return true;
			}
			const auto at = timeline->videoY();
			const QPoint from(timeline->xForTime(1100), at), to(timeline->xForTime(2200), at);
			Mouse(timeline, QEvent::MouseButtonPress, from);
			Mouse(timeline, QEvent::MouseMove, to);
			Mouse(timeline, QEvent::MouseButtonRelease, to);
			auto protect = editor->findChild<QAction*>("protectEditRange");
			if (!protect) return fail("The draft editing action is unavailable");
			protect->trigger();
			bool dirty = false;
			for (auto label : editor->findChildren<QLabel*>())
				dirty |= label->text() == QFileInfo(path).fileName() + " *";
			if (!dirty || !editor->close()) return fail("Could not create and close a test draft");
			const auto confirm = [&](QMessageBox::StandardButton answer)
			{
				QTimer watch;
				QObject::connect(&watch, &QTimer::timeout, &w, [answer]
				{
					if (auto modal = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
					{
						if (auto button = modal->button(answer)) button->click();
						else modal->reject();
					}
				});
				watch.start(65);
				w.m_deletePreview->click();
			};
			confirm(QMessageBox::No);
			if (!QFileInfo::exists(path) || !speech->isEnabled()) return fail("Cancelling deletion removed the draft or recording");
			confirm(QMessageBox::Yes);
			if (QFileInfo::exists(path) || !w.m_preview.isEmpty() || speech->isEnabled())
				return fail("Confirmed deletion did not release the recording and its draft");
			result["confirmation_cancel_preserved_draft"] = true;
			result["moved_to_trash_and_discarded_draft"] = true;
			return true;
		}
		if (name == "preview-slow-status" || name == "preview-blocking-status")
		{
			w.m_pollTimer.stop();
			w.m_player->setMuted(true);
			w.openPreview(input);
			if (!WaitFor([&] { return w.m_player->position() > 600 && w.m_video->frameCount() > 10; }, 6500))
				return fail("Preview did not start");
			QLocalServer slowServer;
			slowServer.setSocketOptions(QLocalServer::UserAccessOption);
			if (!slowServer.listen(ControlServerName())) return fail(slowServer.errorString());
			int requests = 0, replies = 0;
			connect(&slowServer, &QLocalServer::newConnection, &w, [&]
			{
				auto client = slowServer.nextPendingConnection();
				connect(client, &QLocalSocket::disconnected, client, &QObject::deleteLater);
				connect(client, &QLocalSocket::readyRead, client, [&, client]
				{
					if (!client->canReadLine()) return;
					client->readLine();
					++requests;
					QTimer::singleShot(180, client, [&, client]
					{
						client->write("{\"ok\":true,\"state\":\"idle\"}\n");
						++replies;
					});
				});
			});
			const bool blocking = name == "preview-blocking-status";
			if (blocking)
			{
				disconnect(&w.m_pollTimer, nullptr, &w, nullptr);
				connect(&w.m_pollTimer, &QTimer::timeout, &w, &KRRMainWindow::updateStatus);
			}
			QEventLoop loop;
			QTimer clock;
			clock.setTimerType(Qt::PreciseTimer);
			QElapsedTimer wall;
			wall.start();
			qint64 last = 0, longest = 0;
			connect(&clock, &QTimer::timeout, &loop, [&]
			{
				const auto now = wall.elapsed();
				longest = qMax(longest, now - last);
				last = now;
			});
			const auto first = w.m_video->frameCount();
			clock.start(16);
			w.m_pollTimer.start(500);
			QTimer::singleShot(4200, Qt::PreciseTimer, &loop, &QEventLoop::quit);
			loop.exec();
			w.m_pollTimer.stop();
			w.cancelStatusPoll();
			const double frames = (w.m_video->frameCount() - first) * 1000.0 / wall.elapsed();
			result["received_fps"] = frames;
			result["gui_longest_interval_ms"] = longest;
			result["status_requests"] = requests;
			result["status_replies"] = replies;
			return blocking ? longest > 200 : longest < 130 && frames >= 45.0 && replies >= 4;
		}
		if (name == "toolbar-polish")
		{
			auto models = w.findChild<QPushButton*>("openModelServices");
			auto assistant = w.findChild<QPushButton*>("openRecordingAssistant");
			auto speech = w.m_clipEditor->findChild<QAction*>("editorSpeech");
			auto editorSubmission = w.m_clipEditor->findChild<QPushButton*>("editorSubmission");
			auto disabledMain = [&]
			{ return !w.m_submission->isEnabled() && !w.m_transcribe->isEnabled(); };
			const auto menus = w.findChild<QMenuBar*>();
			if (!models || !assistant || !speech || !editorSubmission || !menus || menus->actions().size() != 5 ||
				!assistant->isVisible() || !disabledMain() || speech->isEnabled() ||
				editorSubmission->isEnabled() || w.m_checkButton->isVisible())
				return fail("Workbench menu or unavailable media controls are incorrect");
			assistant->click();
			auto sidebar = w.findChild<QWidget*>("studioCopilot");
			if (!sidebar || !sidebar->isVisible()) return fail("Copilot did not open");
			assistant->click();
			if (sidebar->isVisible()) return fail("Copilot did not close");
			w.showSettings(7);
			if (!models->isVisible()) return fail("Model service entry is missing from settings");
			w.m_settingsDialog->hide();
			QApplication::processEvents();
			w.grab().save(QDir(root).filePath("record-workspace.png"));
			w.openPreview(input);
			if (!WaitFor([&] { return w.m_player->duration() > 0 && w.m_submission->isEnabled() &&
				w.m_transcribe->isEnabled(); }))
				return fail("Selecting video did not enable workbench media controls");
			if (NeedsCompatiblePreview(input))
			{
				if (!WaitFor([&] { return w.m_player->position() > 200 && w.m_video->frameCount() >= 6; }))
					return fail("Legacy-format preview did not play");
				const auto playing = w.m_player->media().request().url().toLocalFile();
				if (QFileInfo(playing).suffix() != "mp4" || w.m_preview != input || playing == input)
					return fail("Compatible preview replaced the source file reference");
				w.openPreview(input);
				if (!WaitFor([&] { return w.m_player->position() > 200 &&
					w.m_player->media().request().url().toLocalFile() != playing; }) ||
					!WaitFor([&] { return !QFileInfo::exists(playing); }, 3000))
					return fail("Reopening a legacy video did not release the old preview");
				result["compatible_preview_played"] = true;
				result["kept_original_source"] = true;
				result["released_previous_preview"] = true;
			}
			w.m_player->pause();
			if (!w.m_clipEditor->openFile(input, root) ||
				!WaitFor([&] { return speech->isEnabled() && editorSubmission->isEnabled(); }))
				return fail("Editor did not enable controls when the media became ready");
			w.m_active = true;
			w.refreshMediaActions();
			w.m_clipEditor->setRecording(true);
			if (!disabledMain() || !speech->isEnabled() || !editorSubmission->isEnabled())
				return fail("An existing clip could not be processed independently during recording");
			w.m_active = false;
			w.refreshMediaActions();
			w.m_clipEditor->setRecording(false);
			if (!w.m_submission->isEnabled() || !speech->isEnabled() || !editorSubmission->isEnabled())
				return fail("Media controls did not recover after stop");
			w.m_trialPath = w.m_preview;
			w.refreshMediaActions();
			if (!disabledMain()) return fail("Trial media was available for delivery");
			result["verified"] = QJsonArray{"initial_disabled", "five_menus", "copilot_toggle", "model_services", "valid_source",
				"unified_speech", "editor_available_during_recording", "stop_restored", "trial_excluded"};
			return true;
		}
		if (name == "studio-rebuild")
		{
			w.resize(1360, 850);
			const QStringList dockOrder{"studioSceneDock", "studioSourceDock", "studioMixerDock", "studioOutputDock", "studioControlsDock"};
			QStringList actualDockOrder;
			for (int i = 0; i < w.m_recordDocks->count(); ++i) actualDockOrder << w.m_recordDocks->widget(i)->objectName();
			if (!w.windowFlags().testFlag(Qt::FramelessWindowHint) || !w.findChild<QLabel*>("studioBadge") ||
				w.m_recordLayout->orientation() != Qt::Vertical || actualDockOrder != dockOrder)
				return fail("Studio title or five recording docks are incorrect");
			auto rate = w.findChild<QSpinBox*>("studioBitrate");
			auto defaults = w.findChild<QPushButton*>("studioDefaultBitrate");
			if (!rate || !defaults) return fail("The bitrate editor or default button is missing");
			w.m_resolution->setCurrentText("1920x1080");
			w.m_fps->setCurrentIndex(w.m_fps->findData(60));
			rate->setValue(6200);
			w.m_fps->setCurrentIndex(w.m_fps->findData(90));
			if (rate->value() != 6200) return fail("The user's bitrate did not survive the frame rate change");
			defaults->click();
			if (rate->value() != 9000) return fail("The default button did not match the frame rate and resolution");
			w.m_assistantToggle->click();
			const auto recordingThread = w.m_sidebarAssistant->conversationId();
			w.selectWorkspace(KRRMainWindow::Review);
			if (!w.m_sidebarAssistant->isVisible() || w.m_sidebarAssistant->conversationId() != recordingThread)
				return fail("Recording and review did not share the sidebar");
			w.selectWorkspace(KRRMainWindow::Edit);
			if (w.m_sidebarAssistant->isVisible() || w.m_editSidebarAssistant->isVisible())
				return fail("The editing sidebar was opened by the recorder sidebar toggle");
			w.m_editAssistantToggle->click();
			if (!w.m_editSidebarAssistant->isVisible() || !w.m_editSidebarAssistant->conversationId().isEmpty())
				return fail("The editing sidebar started a conversation before a project was selected");
			w.m_editAssistantToggle->click();
			w.selectWorkspace(KRRMainWindow::Record);
			w.m_assistantToggle->click();
			w.m_resolution->setCurrentText("1920x1080");
			QImage fixture(1920, 1080, QImage::Format_RGB32);
			for (int y = 0; y < fixture.height(); ++y)
				for (int x = 0; x < fixture.width(); ++x)
					fixture.setPixel(x, y, qRgb(15 + x / 9, 22 + y / 5, 40 + (x + y) / 20));
			const auto imagePath = QDir(root).filePath("preview-source.png");
			if (!fixture.save(imagePath)) return fail("Could not prepare the preview image");
			w.m_source->setCurrentIndex(w.m_source->findData("image"));
			w.m_image->setText(imagePath);
			if (!WaitFor([&] { return w.m_idleSequence > 3 && w.m_liveFrame->frameSize() == fixture.size(); }, 6000))
				return fail("The recording workspace did not monitor its source before recording");
			if (w.m_active || w.m_checkButton->isVisible()) return fail("Live monitoring required an old diagnostic or started recording");
			bool sawSourceProperties = false;
			QTimer::singleShot(0, &w, [&]
			{
				auto dialog = Modal("studioSourceDialog");
				if (!dialog) return;
				sawSourceProperties = dialog->windowTitle() == Ui("当前画面源的属性") &&
					dialog->findChild<QLineEdit*>()->text() == imagePath;
				dialog->grab().save(QDir(root).filePath("source-properties.png"));
				dialog->reject();
			});
			w.m_sourceProperties->click();
			if (!sawSourceProperties) return fail("The source control did not open current-source properties");
			auto zoom = w.findChild<QPushButton*>("studioZoomIn");
			for (int i = 0; i < 8; ++i) zoom->click();
			QApplication::processEvents();
			if (w.m_liveViewport->widgetResizable() || !w.m_liveViewport->horizontalScrollBar()->isVisible() ||
				!w.m_liveViewport->verticalScrollBar()->isVisible())
				return fail("Preview zoom did not expose both pan scrollbars");
			w.findChild<QPushButton*>("studioZoomFit")->click();
			if (!WaitFor([&]
			{
				return !w.m_liveViewport->horizontalScrollBar()->isVisible() && !w.m_liveViewport->verticalScrollBar()->isVisible();
			}, 2000)) return fail("Fit mode did not close the preview scrollbars");
			w.grab().save(QDir(root).filePath("studio-record.png"));

			w.openPreview(input);
			if (!WaitFor([&] { return w.m_workspace->currentIndex() == KRRMainWindow::Review &&
				w.m_player->duration() > 0 && w.m_video->frameCount() >= 2 && w.m_chapters->count() >= 1 &&
				!w.m_reviewFormat->text().isEmpty(); }, 12000))
				return fail("Review did not show chapters and measured encoding information");
			w.m_player->pause();
			const auto marker = w.m_chapters->item(0);
			const auto at = marker->data(Qt::UserRole).toLongLong();
			Mouse(w.m_chapters->viewport(), QEvent::MouseButtonPress, w.m_chapters->visualItemRect(marker).center());
			Mouse(w.m_chapters->viewport(), QEvent::MouseButtonRelease, w.m_chapters->visualItemRect(marker).center());
			if (!WaitFor([&] { return qAbs(w.m_player->position() - at) < 180; }, 4000))
				return fail("Chapter navigation did not seek to the associated marker");
			auto library = w.findChild<QPushButton*>("studioToggleLibrary");
			library->click();
			if (w.m_libraryPane->isVisible()) return fail("The review library did not collapse");
			library->click();
			w.grab().save(QDir(root).filePath("studio-review.png"));

			w.editFile(input);
			auto seek = w.m_clipEditor->findChild<QSlider*>("editPreviewSeek");
			auto timeline = w.m_clipEditor->findChild<KRREditTimeline*>("editTimeline");
			if (!WaitFor([&] { return w.m_workspace->currentIndex() == KRRMainWindow::Edit &&
				seek && seek->maximum() > 0 && timeline && w.m_clipEditor->inputFile() == input; }, 10000))
				return fail("The edit workspace did not embed the native editor");
			Mouse(timeline, QEvent::MouseButtonPress, QPoint(timeline->xForTime(4500), 13));
			Mouse(timeline, QEvent::MouseButtonRelease, QPoint(timeline->xForTime(4500), 13));
			if (qAbs(seek->value() - 4500) > 180) return fail("Editor preview seek did not follow the timeline");
			w.m_clipEditor->findChild<QPushButton*>("editSplitClip")->click();
			const auto cut = timeline->xForTime(2300);
			Mouse(timeline, QEvent::MouseButtonPress, QPoint(cut, timeline->videoY()));
			Mouse(timeline, QEvent::MouseButtonRelease, QPoint(cut, timeline->videoY()));
			auto remove = w.m_clipEditor->findChild<QPushButton*>("editDeleteClip");
			auto lock = w.m_clipEditor->findChild<QPushButton*>("editLockTrack");
			if (!remove || !remove->isEnabled() || !lock) return fail("Separated clip was not directly selectable");
			lock->click();
			if (remove->isEnabled()) return fail("A locked video track still offered deletion");
			lock->click();
			auto before = seek->maximum();
			remove->click();
			if (seek->maximum() > before || seek->maximum() < 200) return fail("Ripple deletion produced invalid edit time");
			const auto canvas = w.m_clipEditor->findChild<KRREditCanvas*>();
			if (!WaitFor([&] { return canvas && !canvas->currentFrame().isNull(); }, 4000))
				return fail("The adjusted timeline did not show a decoded preview");
			QApplication::processEvents();
			if (canvas->mapTo(&w, QPoint(0, canvas->height())).y() > seek->mapTo(&w, QPoint()).y())
				return fail("The preview seek bar overlapped the video canvas");
			w.grab().save(QDir(root).filePath("studio-editor.png"));

			w.m_editAssistantToggle->click();
			if (!w.m_editSidebarAssistant->isVisible() || w.m_editSidebarAssistant->conversationId().isEmpty() ||
				!w.m_editSidebarAssistant->findChild<QPushButton*>("studioGenerateCut")->isVisible())
				return fail("The editing sidebar did not bind to the opened project");
			w.findChild<QPushButton*>("studioWorkspace3")->click();
			w.findChild<QPushButton*>("studioAiEdit")->click();
			auto assistant = w.findChild<QSplitter*>("studioAssistantSplit");
			if (!assistant || assistant->count() != 3 || w.m_editSidebarAssistant->isVisible() ||
				w.m_sidebarAssistant->isVisible() || !assistant->isVisible())
				return fail("The independent assistant was missing or showed a sidebar");
			auto tabs = w.findChild<QTabWidget*>("studioAiRightTabs");
			if (!tabs || tabs->count() != 1 || tabs->tabText(0) != Ui("空白页")) return fail("Editing assistant did not start with a blank page");
			tabs->findChild<QPushButton*>("studioAiPage-terminal")->click();
			auto command = tabs->findChild<QLineEdit*>("studioCliInput");
			if (!command) return fail("Terminal input missing");
			command->setText("--version");
			emit command->returnPressed();
			if (!WaitFor([&] { return tabs->findChild<QPlainTextEdit*>("studioCliOutput")->toPlainText().contains("2.3."); }, 6000))
				return fail("The terminal could not call the real recorder CLI");
			tabs->findChild<QPushButton*>("studioAiNewTab")->click();
			if (tabs->count() != 2 || tabs->tabText(1) != Ui("空白页")) return fail("Could not open another blank page");
			assistant->setSizes({155, 0, 640});
			QApplication::processEvents();
			if (assistant->sizes()[1] != 0) return fail("The middle assistant pane did not collapse");
			assistant->setSizes({155, 610, 330});
			QApplication::processEvents();
			if (assistant->sizes()[1] <= 0) return fail("The middle assistant pane did not reopen");
			w.grab().save(QDir(root).filePath("studio-assistant.png"));
			result["verified"] = QJsonArray{"automatic_1080_preview", "source_properties", "zoom_both_scrollbars", "review_metadata",
				"chapter_seeking", "embedded_editor", "preview_timeline_sync", "split_clip_selection", "track_lock",
				"contextual_copilot", "assistant_pane_reopen"};
			return true;
		}
		if (name == "studio-schedules")
		{
			QImage still(640, 360, QImage::Format_RGB32);
			still.fill(QColor("#356985"));
			const auto path = QDir(root).filePath("scheduled-source.png");
			if (!still.save(path)) return fail("Cannot prepare scheduled input");
			w.m_source->setCurrentIndex(w.m_source->findData("image"));
			w.m_image->setText(path);
			auto config = w.configuration();
			config.maxDuration = 2;
			config.output = QDir(root).filePath("expired.mp4");
			const auto serviceData = QFileInfo(DataDirectory()).dir().filePath("wpsrecorder");
			if (!QDir().mkpath(serviceData)) return fail("Cannot prepare isolated service directory");
			QFile fixture(QDir(serviceData).filePath(ControlServerName() + "-schedule.json"));
			if (!fixture.open(QIODevice::WriteOnly)) return fail("Cannot write isolated restart seed");
			fixture.write(QJsonDocument(QJsonArray{QJsonObject{{"id", "expired-test"}, {"name", "Expired"},
				{"state", "waiting"}, {"startAt", QDateTime::currentDateTimeUtc().addSecs(-60).toString(Qt::ISODateWithMs)},
				{"config", config.toJson()}}}).toJson());
			fixture.close();
			QString error;
			QJsonObject response;
			if (!EnsureControlService(error)) return fail(error);
			const auto now = QDateTime::currentDateTimeUtc();
			auto task = [&](const QString& title, int after, double seconds)
			{
				config.maxDuration = seconds;
				config.output = QDir(root).filePath(title + ".mp4");
				if (!SendControl({{"command", "schedule"}, {"name", title}, {"config", config.toJson()},
					{"startAt", now.addSecs(after).toString(Qt::ISODateWithMs)}}, response, error, 1000)) return QString{};
				return response["id"].toString();
			};
			const auto indefinite = task("manual-stop", 22, 0);
			const auto behind = task("after-manual", 27, 1);
			if (indefinite.isEmpty() || behind.isEmpty()) return fail("A reservation without a stop time was rejected: " + error);
			if (!SendControl({{"command", "schedules"}}, response, error, 1000)) return fail(error);
			const auto planned = response["schedules"].toArray();
			if (planned.size() != 3 || planned[2].toObject()["state"] != "conflict") return fail("An open-ended schedule did not reserve following time");
			for (const auto& id : {indefinite, behind})
				if (!SendControl({{"command", "cancel-schedule"}, {"id", id}}, response, error, 1000)) return fail(error);
			const auto first = task("publish", 6, 2);
			const auto second = task("overlap", 7, 2);
			if (first.isEmpty() || second.isEmpty()) return fail(error);
			bool queueShown = false;
			QTimer::singleShot(150, &w, [&]
			{
				const auto dialog = Modal("studioScheduleDialog");
				if (!dialog) return;
				const auto table = dialog->findChild<QTableWidget*>("studioScheduleQueue");
				queueShown = dialog->findChild<QLineEdit*>("scheduleHours") && dialog->findChild<QLineEdit*>("scheduleMinutes") &&
					dialog->findChild<QLineEdit*>("scheduleSeconds") && table && table->rowCount() == 3 && table->item(0, 3)->text() == Ui("已过期") &&
					table->item(2, 3)->text() == Ui("时段冲突");
				dialog->grab().save(QDir(root).filePath("studio-schedule.png"));
				dialog->reject();
			});
			w.findChild<QPushButton*>("studioSchedule")->click();
			if (!queueShown || !w.m_start->isEnabled()) return fail("Schedule conflict/expiry or manual recording availability is incorrect");
			if (!SendControl({{"command", "cancel-schedule"}, {"id", second}}, response, error, 1000)) return fail(error);
			auto published = QDir(root).filePath("publish.mp4");
			if (!WaitFor([&] { return QFileInfo(published).isFile() && QFileInfo(published).size() > 1000; }, 20000))
				return fail("The queued recording did not publish a video");
			QJsonArray rows;
			if (!WaitFor([&]
			{
				if (!SendControl({{"command", "schedules"}}, response, error, 500)) return false;
				rows = response["schedules"].toArray();
				return rows.size() == 2 && rows[1].toObject()["state"] == "completed";
			}, 3500))
			{
				result["queue"] = rows;
				return fail("The published recording did not reach the completed queue state");
			}
			result["schedule_count"] = rows.size();
			result["verified"] = QJsonArray{"multiple_jobs", "overlap_status", "expired_on_restart", "delete_one",
				"manual_start_enabled", "recorded_at_due_time", "auto_stop"};
			return true;
		}
		if (name == "storage-fallback")
		{
			QFile obstruction(QDir(root).filePath("not-a-directory"));
			if (!obstruction.open(QIODevice::WriteOnly) || obstruction.write("test", 4) != 4) return fail("Cannot set up obstruction");
			obstruction.close();
			const auto blocked = obstruction.fileName() + "/recordings";
			w.m_directory->setText(blocked);
			w.m_defaultDirectory = false;
			QTimer detect;
			QString customError;
			connect(&detect, &QTimer::timeout, &w, [&]
			{
				auto box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
				if (!box) return;
				customError = box->text();
				box->accept();
			});
			detect.start(25);
			if (w.validateStorage() || w.m_directory->text() != blocked || !customError.contains(blocked))
				return fail("An explicit user path was replaced or did not show the failing path");
			w.m_defaultDirectory = true;
			const auto picture = QDir(root).filePath("recording-source.png");
			QImage image(640, 360, QImage::Format_RGB32);
			image.fill(QColor("#355d9b"));
			if (!image.save(picture)) return fail("Could not make the synthetic recording input");
			w.m_source->setCurrentIndex(w.m_source->findData("image"));
			w.m_image->setText(picture);
			w.m_audio->setCurrentIndex(w.m_audio->findData("none"));
			w.m_countdown->setCurrentIndex(w.m_countdown->findData(0));
			w.m_floatButton->setChecked(true);
			customError.clear();
			w.m_start->click();
			if (!WaitFor([&] { return w.m_active && w.m_addMarker->isEnabled(); }, 8000))
				return fail("The record button did not start capture using the fallback: " + customError);
			auto marker = w.m_floatBar->findChild<QPushButton*>("floatAddMarker");
			if (!marker || !marker->isVisible() || !marker->isEnabled() || w.m_submission->isEnabled())
				return fail("Recording actions and floating marker not in the expected state");
			marker->click();
			if (!WaitFor([&] { return w.m_lastRecordingStatus["frames_written"].toInt() >= 24; }, 6000))
				return fail("Recording did not produce frames");
			w.m_stop->click();
			if (!WaitFor([&] { return !w.m_active && w.m_submission->isEnabled() && w.m_player->duration() > 0; }))
				return fail("Recording did not finalize and become available");
			if (w.m_directory->text() == blocked || QFileInfo(w.m_preview).absolutePath() != w.m_directory->text() ||
				ReadRecordingMarkers(w.m_preview).size() != 1)
				return fail("Fallback recording or floating marker was not saved");
			QFile::copy(w.m_preview, QDir(root).filePath("fallback-recording.mp4"));
			result["fallback_recorded"] = true;
			result["duration_ms"] = w.m_player->duration();
			result["floating_marker_saved"] = true;
			result["explicit_path_unchanged"] = true;
			return true;
		}
		if (name == "preflight-live-camera")
		{
			if (KRRCameraCapture::Devices().isEmpty()) return fail("No Windows camera was enumerated");
			KRRRecordingConfig config;
			config.source = input == "pip" || input == "track" ? "window" : "camera";
			config.window = static_cast<quint64>(w.winId());
			config.cameraEnabled = input == "pip";
			config.cameraTrack = input == "track";
			config.encoder = "libx264";
			config.output = QDir(root).filePath("camera-probe.mp4");
			KRRRecordingCheckDialog dialog(config, Ui("摄像头"), {}, &w);
			dialog.show();
			const auto status = dialog.findChild<QLabel*>("preflightCameraStatus");
			const auto image = dialog.findChild<QLabel*>("recordingCheckFrame");
			const auto picture = input == "track" ? dialog.findChild<QLabel*>("recordingCheckCameraTrack") : image;
			const auto trial = dialog.findChild<QPushButton*>("preflightTrial");
			const auto sequence = [&] { return image->property("cameraFrameSequence").toULongLong(); };
			const bool ready = WaitFor([&] { return sequence() > 0 && !picture->pixmap(Qt::ReturnByValue).isNull() &&
				trial->isEnabled(); });
			result["camera_status"] = status->text();
			if (!ready) return fail("Camera preview and encoder probe did not become ready");
			const auto first = sequence();
			const auto pixmap = picture->pixmap(Qt::ReturnByValue);
			const bool moving = WaitFor([&] { return sequence() >= first + 10 &&
				picture->pixmap(Qt::ReturnByValue).cacheKey() != pixmap.cacheKey(); }, 3000);
			result["frame_sequence_delta"] = static_cast<qint64>(sequence() - first);
			result["preview_size"] = QJsonArray{pixmap.width(), pixmap.height()};
			result["preview_updates"] = moving;
			dialog.reject();
			KRRCameraCapture next;
			QString error;
			const bool released = next.start(config.camera, error);
			result["released_for_trial"] = released;
			return (moving && released) || fail("Camera preview stopped updating or failed to release: " + error);
		}
		if (name == "preflight" || name == "trial" || name == "trial-mix")
		{
			bool validHandle = false;
			const auto handle = input.toULongLong(&validHandle);
			if (!validHandle) return fail("Expected the test window handle");
			w.m_source->setCurrentIndex(w.m_source->findData("window"));
			w.refreshWindows();
			const int index = w.m_window->findData(QVariant::fromValue<qulonglong>(handle));
			if (index < 0) return fail("QA window was not listed");
			w.m_window->setCurrentIndex(index);
			const bool mixed = name == "trial-mix";
			w.m_audio->setCurrentIndex(w.m_audio->findData(mixed ? "mix" : "system"));
			const int recent = w.m_recent->count();
			QTimer watch;
			int attempts = 0;
			bool preflightOk = false;
			connect(&watch, &QTimer::timeout, &w, [&]
			{
				auto dialog = Modal("recordingCheckDialog");
				if (!dialog) return;
				auto button = dialog->findChild<QPushButton*>("preflightTrial");
				auto image = dialog->findChild<QLabel*>("recordingCheckFrame");
				auto microphone = dialog->findChild<QProgressBar*>("preflightMicLevel");
				auto system = dialog->findChild<QProgressBar*>("preflightSystemLevel");
				auto diagnostics = dialog->findChild<QLabel*>("recordingDiagnostics");
				const auto screenshot = image->pixmap(Qt::ReturnByValue);
				const auto micInfo = dialog->findChild<QLabel*>("preflightMicDevice")->text();
				const auto systemInfo = dialog->findChild<QLabel*>("preflightSystemDevice")->text();
				result["frame_width"] = screenshot.width();
				result["mic_level"] = microphone->value();
				result["system_level"] = system->value();
				result["mic_device"] = micInfo;
				result["system_device"] = systemInfo;
				result["diagnostics"] = diagnostics->text();
				preflightOk = button->isEnabled() && !screenshot.isNull() && system->value() > 0 &&
					!micInfo.isEmpty() && (!mixed || microphone->value() > 0) && diagnostics->text().contains("libx264");
				if (!preflightOk && ++attempts < 110) return;
				watch.stop();
				if (name != "preflight" && preflightOk) button->click();
				else dialog->reject();
			});
			watch.start(100);
			w.m_checkButton->click();
			if (!preflightOk) return fail("Window, diagnostics or system meter did not become ready");
			if (name == "preflight") return true;
			if (!WaitFor([&] { return w.m_preview == w.m_trialPath && !w.m_trialPath.isEmpty() &&
					w.m_player->position() > 200; }, 22000))
				return fail("Trial did not open and play");
			KRREditProject clip;
			QString error;
			if (!ProbeEditSource(w.m_trialPath, clip, error) || clip.audio.size() != (mixed ? 2 : 1) ||
				qAbs(clip.duration - 6000) > 220 || w.m_recent->count() != recent || !w.m_trialTracks->isVisible())
				return fail("Trial duration, track, playback controls or recents are incorrect: " + error);
			QStringList waveNames;
			if (mixed) waveNames << "trial-mic.wav";
			waveNames << "trial-system.wav";
			for (int i = 0; i < waveNames.size(); ++i)
			{
				w.m_trialTracks->setCurrentIndex(i + 1);
				w.m_trialReplay->click();
				const auto wave = w.m_trialWorkspace->filePath(QString("listen-%1.wav").arg(i));
				if (!WaitFor([&]{ return QFileInfo(wave).size() > 100000 &&
						w.m_player->media().request().url().toLocalFile() == wave && w.m_player->position() > 150; }))
					return fail("Trial track did not play independently: " + waveNames[i]);
				QFile::copy(wave, QDir(root).filePath(waveNames[i]));
				result[waveNames[i]] = QFileInfo(wave).size();
			}
			result["trial_ms"] = clip.duration;
			result["trial_audio_tracks"] = clip.audio.size();
			return true;
		}
		if (name == "jump" || name == "rename")
		{
			w.openPreview(input);
			if (!WaitFor([&]{ return w.m_player->duration() > 0; })) return fail("Media did not load");
			if (!w.m_chapters->isVisible() || w.m_chapters->count() == 0)
				return fail("Recorded markers missing from playback");
			w.m_player->pause();
			const auto target = w.m_playbackMarkers->itemData(1).toLongLong();
			w.m_playbackMarkers->setCurrentIndex(1);
			w.m_playbackMarkers->activated(1);
			if (!WaitFor([&]{ return qAbs(w.m_player->position() - target) <= 120; }))
				return fail("Playback did not seek to the selected marker");
			result["target_ms"] = target;
			result["position_ms"] = w.m_player->position();
			if (name == "rename")
			{
				w.m_chapters->item(0)->setText(QStringLiteral("运行成功"));
				const auto marker = ReadRecordingMarkers(input).at(0).toObject();
				if (marker["label"].toString() != QStringLiteral("运行成功") || marker["at_ms"].toDouble() != target)
					return fail("Renaming changed the timestamp or did not persist");
				result["label"] = marker["label"];
				w.m_player->setPosition(1000);
				w.findChild<QPushButton*>("studioAddReviewMarker")->click();
				if (QApplication::activeModalWidget() || ReadRecordingMarkers(input).size() != 2 || w.m_chapters->count() != 2)
					return fail("Adding a marker did not create an editable inline row");
				result["inline_add"] = true;
			}
			return true;
		}
		if (name == "submission" || name == "recovery")
		{
			const auto output = QDir(root).filePath(name + ".mp4");
			if (name == "submission")
			{
				w.openPreview(input);
				if (!WaitFor([&]{ return w.m_player->duration() > 20000; })) return fail("MV did not load");
				w.m_player->pause();
				w.m_player->setPosition(234500);
				if (!WaitFor([&]{ return qAbs(w.m_player->position() - 234500) < 120; }))
					return fail("MV did not seek before submission preview");
			}
			QString dialogError;
			int state = 0, attempts = 0;
			QTimer watch;
			QTimer watchdog;
			watchdog.setSingleShot(true);
			connect(&watchdog, &QTimer::timeout, &w, [&]
			{
				const auto modal = QApplication::activeModalWidget();
				dialogError = "Dialog timeout: " + (modal ? modal->windowTitle() : QString("no active modal"));
				for (auto widget : QApplication::topLevelWidgets())
					if (auto dialog = qobject_cast<QDialog*>(widget); dialog && dialog->isModal() && dialog->isVisible())
						dialog->reject();
			});
			connect(&watch, &QTimer::timeout, &w, [&]
			{
				auto dialog = Modal(name == "submission" ? "submissionPreviewDialog" : "recordingRecovery");
				if (!dialog) return;
				if (++attempts > 260)
				{
					dialogError = "Focused dialog timed out";
					if (name == "submission") dialogError += ": " + dialog->findChild<QLabel*>("submissionPreviewStatus")->text();
					dialog->reject();
					return;
				}
				if (state == 0 && name == "submission")
				{
					auto quality = dialog->findChild<QComboBox*>("submissionQuality");
					quality->setCurrentIndex(quality->findData("small"));
					result["summary"] = dialog->findChild<QLabel*>("submissionSummary")->text();
					state = 1;
					dialog->findChild<QPushButton*>("renderSubmissionPreview")->click();
				}
				else if (state == 1 && name == "submission")
				{
					auto player = dialog->findChild<QMediaPlayer*>("submissionPlayer");
					const auto previewPath = player->media().request().url().toLocalFile();
					if (player->position() < 100 || !QFileInfo(previewPath).isFile()) return;
					KRREditProject sample;
					if (!ProbeEditSource(previewPath, sample, dialogError) ||
						sample.size != QSize(640, 360) || sample.audio.size() != 1 || qAbs(sample.duration - 8000) > 120)
					{
						dialogError = "Sample did not match submission settings: " + dialogError;
						dialog->reject();
						return;
					}
					result["sample_ms"] = sample.duration;
					result["sample_width"] = sample.size.width();
					result["sample_status"] = dialog->findChild<QLabel*>("submissionPreviewStatus")->text();
					QFile::copy(sample.input, QDir(root).filePath("sample.mp4"));
					state = 2;
				}
				else if (state == 2 && name == "submission")
				{
					auto quality = dialog->findChild<QComboBox*>("submissionQuality");
					quality->setCurrentIndex(quality->findData("small"));
					auto audio = dialog->findChild<QComboBox*>("submissionAudioMode");
					audio->setCurrentIndex(audio->findData("mute"));
					state = 3;
					QTimer::singleShot(300, &w, [&] { ChooseOutput(output); });
					dialog->findChild<QPushButton*>("exportSubmission")->click();
				}
				else if (name == "recovery" && state == 0)
				{
					auto list = dialog->findChild<QListWidget*>("recordingRecoveryItems");
					if (list->count() < 1) { dialogError = "Recovery list is empty"; dialog->reject(); return; }
					state = 1;
					QTimer::singleShot(300, &w, [&] { ChooseOutput(output); });
					dialog->findChild<QPushButton*>("saveRecovery")->click();
				}
			});
			watch.start(100);
			watchdog.start(45000);
			if (name == "submission")
			{
				w.m_trimBegin->setValue(232);
				w.m_trimEnd->setValue(246);
				w.showSubmission();
			}
			else
			{
				w.m_directory->setText(input);
				w.showRecoveries();
			}
			watch.stop();
			watchdog.stop();
			if (!dialogError.isEmpty()) return fail(dialogError);
			if (name == "submission")
			{
				if (state != 3 || !WaitFor([&]{ return QDir::cleanPath(w.m_preview) == QDir::cleanPath(output) &&
						w.m_player->position() > 150; }))
					return fail("Submission export did not open and play");
				KRREditProject movie;
				QString error;
				if (!ProbeEditSource(output, movie, error) || movie.size != QSize(640, 360) ||
					!movie.audio.isEmpty() || qAbs(movie.duration - 14000) > 120)
					return fail("Submission export settings did not match: " + error);
				result["export_ms"] = movie.duration;
				result["export_size_bytes"] = movie.sourceBytes;
				return true;
			}
			if (!WaitFor([&]{ return QDir::cleanPath(w.m_preview) == QDir::cleanPath(output) && w.m_player->position() > 150; }))
				return fail("Recovered copy did not open and play");
			result["output"] = output;
			result["markers"] = ReadRecordingMarkers(output);
			return true;
		}
		return fail("Unknown focused test");
	}

	KRRWorkbenchAcceptance(const QString &root, const QString &report, bool hd = false) : m_root(root), m_report(report)
	{
		QImage fixture(640, 360, QImage::Format_RGB32);
		fixture.fill(QColor("#25374f"));
		fixture.save(root + "/fixture.png");
		m_window.show();
		m_window.m_directory->setText(root);
		m_window.m_floatButton->setChecked(true);
		m_window.m_image->setText(root + "/fixture.png");
		m_window.m_source->setCurrentIndex(4);
		m_window.m_resolution->setCurrentText(hd ? "1920x1080" : "640x360");
		m_window.m_fps->setCurrentIndex(m_window.m_fps->findData(hd ? 60 : 30));
		m_window.m_countdown->setCurrentIndex(0);
		m_window.m_cameraEnabled->setChecked(true);
		m_window.m_cameraTrack->setChecked(true);
		m_window.m_separateAudio->setChecked(true);
		m_window.m_audio->setCurrentIndex(m_window.m_audio->findData("mix"));
		m_window.m_cameraX->setValue(70);
		m_window.m_cameraY->setValue(60);
		m_window.m_cameraW->setValue(25);
		m_window.m_cameraH->setValue(35);
		m_window.m_cameraMirror->setChecked(false);
		m_window.m_cameraShape->setCurrentIndex(m_window.m_cameraShape->findData("rectangle"));
		m_time.start();
		m_tick.start(100);
		m_meter.start(10);
		connect(&m_meter, &QTimer::timeout, this,
				[this]
				{
					if (m_stage != 1 || m_window.m_liveSequence == m_seen)
						return;
					if (m_seen >= 0)
						m_intervals.append(m_sampling.elapsed() - m_seenTime);
					m_seen = m_window.m_liveSequence;
					m_seenTime = m_sampling.elapsed();
					++m_samples;
				});
		connect(&m_tick, &QTimer::timeout, this, [this] { tick(); });
		QTimer::singleShot(300, this, [this] { m_window.m_start->click(); });
	}

  private:
	void tick()
	{
		if (m_time.elapsed() > 90000)
		{
			finish("Workbench acceptance timeout: " + m_window.m_message->text());
			return;
		}
		auto &w = m_window;
		if (m_stage == 0 && w.m_active && w.m_liveSequence > 5 && w.m_floatBar->isVisible())
		{
			m_cases.append(QJsonObject{
				{"case", "live_camera_and_floating_bar"}, {"passed", true}, {"preview_sequence", w.m_liveSequence}});
			w.m_floatButton->click();
			if (w.m_floatBar->isVisible() || w.m_floatButton->isChecked())
			{
				finish("External floating toggle did not hide bar");
				return;
			}
			w.m_floatButton->click();
			if (!w.m_floatBar->isVisible() || !w.m_floatButton->isChecked())
			{
				finish("External floating toggle did not show bar");
				return;
			}
			m_cases.append(QJsonObject{
				{"case", "external_floating_toggle"}, {"passed", true}, {"checked_text", w.m_floatButton->text()}});
			m_sampling.start();
			auto r = w.m_liveFrame->imageRect();
			QPoint center(r.x() + r.width() * .825, r.y() + r.height() * .775);
			Mouse(w.m_liveFrame, QEvent::MouseButtonPress, center);
			Mouse(w.m_liveFrame, QEvent::MouseMove, center - QPoint(r.width() * .35, r.height() * .25));
			Mouse(w.m_liveFrame, QEvent::MouseButtonRelease, center - QPoint(r.width() * .35, r.height() * .25));
			m_changeTime.restart();
			m_stage = 1;
		}
		else if (m_stage == 1 && m_changeTime.elapsed() > 4200)
		{
			if (w.m_cameraX->value() >= 50 || w.m_cameraY->value() >= 45)
			{
				finish("Live canvas did not update camera placement controls");
				return;
			}
			m_cases.append(QJsonObject{{"case", "drag_updates_recording_scene"},
									   {"passed", true},
									   {"x", w.m_cameraX->value()},
									   {"y", w.m_cameraY->value()},
									   {"width", w.m_cameraW->value()},
									   {"height", w.m_cameraH->value()}});
			double fps = m_samples * 1000.0 / m_sampling.elapsed();
			std::sort(m_intervals.begin(), m_intervals.end());
			if (fps < 20)
			{
				finish("Preview FPS below 20: " + QString::number(fps));
				return;
			}
			m_cases.append(QJsonObject{
				{"case", "shared_memory_preview"},
				{"passed", true},
				{"sample_ms", m_sampling.elapsed()},
				{"unique_frames", m_samples},
				{"fps", fps},
				{"interval_p95_ms", m_intervals[qMin(m_intervals.size() - 1, qRound(m_intervals.size() * .95))]}});
			w.showMinimized();
			w.m_floatBar->findChild<QPushButton *>("floatPause")->click();
			m_stage = 2;
		}
		else if (m_stage == 2 && w.m_paused)
		{
			m_frozenTime = w.m_time->text();
			m_changeTime.restart();
			m_stage = 3;
		}
		else if (m_stage == 3 && m_changeTime.elapsed() > 700)
		{
			if (!IsWindowVisible(reinterpret_cast<HWND>(w.m_floatBar->winId())))
			{
				finish("Floating toolbar hidden with minimized main window");
				return;
			}
			if (w.m_time->text() != m_frozenTime)
			{
				finish("Floating pause did not freeze active time");
				return;
			}
			w.m_floatBar->findChild<QPushButton *>("floatScreenshot")->click();
			bool screenshot = !QDir(m_root).entryList({"screenshot-*.png"}, QDir::Files).isEmpty();
			if (!screenshot)
			{
				finish("Floating screenshot did not save");
				return;
			}
			w.m_floatBar->findChild<QPushButton *>("floatPause")->click();
			m_changeTime.restart();
			m_stage = 4;
		}
		else if (m_stage == 4 && !w.m_paused && m_changeTime.elapsed() > 1400)
		{
			m_cases.append(QJsonObject{{"case", "floating_pause_screenshot_resume"},
									   {"passed", true},
									   {"visible_while_main_minimized", true}});
			w.m_floatBar->findChild<QPushButton *>("floatStop")->click();
			m_stage = 5;
		}
		else if (m_stage == 5 && !w.m_active && QFileInfo::exists(w.m_lastOutput))
		{
			KRREditProject p;
			QString error;
			bool valid = ProbeEditSource(w.m_lastOutput, p, error);
			if (!valid || p.audio.size() != 2 || !QFileInfo::exists(w.m_lastOutput + ".camera.mkv") ||
				!w.m_floatBar->isVisible() || w.m_preview != w.m_lastOutput)
			{
				finish("Stopped recording/automatic preview validation failed: " + error);
				return;
			}
			m_cases.append(QJsonObject{{"case", "floating_stop_auto_preview"},
									   {"passed", true},
									   {"duration_ms", p.duration},
									   {"output", w.m_lastOutput},
									   {"audio_tracks", p.audio.size()}});
			w.showNormal();
			w.m_fullscreen->enter();
			m_stage = 6;
			m_changeTime.restart();
		}
		else if (m_stage == 6 && w.m_player->duration() > 0)
		{
			auto seek = w.m_fullscreen->findChild<QSlider *>("fullscreenSeek");
			if (!w.m_fullscreen->fillsWorkArea() || !seek || seek->maximum() <= 0)
			{
				if (m_changeTime.elapsed() < 3000) return;
				finish(QString("Fullscreen transport missing: fullscreen=%1 max=%2").arg(w.m_fullscreen->fillsWorkArea()).arg(seek ? seek->maximum() : -1));
				return;
			}
			w.m_player->pause();
			m_seekTarget = seek->maximum() / 2;
			const QPoint midpoint = seek->rect().center();
			Mouse(seek, QEvent::MouseButtonPress, QPoint(seek->width() / 4, midpoint.y()));
			Mouse(seek, QEvent::MouseMove, midpoint);
			Mouse(seek, QEvent::MouseButtonRelease, midpoint);
			m_changeTime.restart();
			m_stage = 7;
		}
		else if (m_stage == 7)
		{
			if (qAbs(w.m_player->position() - m_seekTarget) > 180)
			{
				if (m_changeTime.elapsed() > 3000)
					finish(QString("Fullscreen seek landed at %1 ms instead of %2 ms")
						.arg(w.m_player->position()).arg(m_seekTarget));
				return;
			}
			w.m_fullscreen->findChild<QPushButton *>("fullscreenPlay")->click();
			m_changeTime.restart();
			m_stage = 8;
		}
		else if (m_stage == 8 && m_changeTime.elapsed() > 650)
		{
			if (w.m_player->state() != QMediaPlayer::PlayingState || w.m_player->position() < m_seekTarget + 150)
			{
				finish("Fullscreen seek/play did not advance");
				return;
			}
			w.m_fullscreen->findChild<QPushButton *>("fullscreenPlay")->click();
			m_changeTime.restart();
			m_stage = 9;
		}
		else if (m_stage == 9 && m_changeTime.elapsed() > 150)
		{
			if (w.m_player->state() != QMediaPlayer::PausedState)
			{
				finish("Fullscreen pause failed");
				return;
			}
			auto position = w.m_player->position();
			w.m_fullscreen->reject();
			if (w.m_fullscreen->isVisible() || qAbs(w.m_player->position() - position) > 100)
			{
				finish("Fullscreen return lost playhead");
				return;
			}
			m_cases.append(QJsonObject{{"case", "fullscreen_transport"},
									   {"passed", true},
									   {"seek_ms", m_seekTarget},
									   {"pause_position_ms", position},
									   {"return_preserves_position", true}});
			finish({});
		}
	}
	void finish(const QString &error)
	{
		m_tick.stop();
		QFile f(m_report);
		if (f.open(QIODevice::WriteOnly))
			f.write(QJsonDocument(QJsonObject{{"complete", true},
											  {"passed", error.isEmpty()},
											  {"error", error},
											  {"cases", m_cases},
											  {"wall_ms", m_time.elapsed()},
											  {"integrity_rid", ProcessIntegrityRid()}})
						.toJson());
		if (m_window.m_active)
		{
			QJsonObject response;
			QString ignored;
			SendControl({{"command", "stop"}}, response, ignored, 60000);
		}
		QJsonObject response;
		QString ignored;
		SendControl({{"command", "service-exit"}}, response, ignored, 3000);
		QCoreApplication::exit(error.isEmpty() ? 0 : 1);
	}
	KRRMainWindow m_window;
	QString m_root, m_report, m_frozenTime;
	QJsonArray m_cases;
	QTimer m_tick, m_meter;
	QElapsedTimer m_sampling;
	QVector<qint64> m_intervals;
	qint64 m_seen = -1, m_seenTime = 0;
	int m_samples = 0, m_seekTarget = 0;
	QElapsedTimer m_time, m_changeTime;
	int m_stage = 0;
};
int main(int argc, char **argv)
{
	InitializeDisplayCoordinates();
	if (!qEnvironmentVariableIsSet("QT_MULTIMEDIA_PREFERRED_PLUGINS"))
		qputenv("QT_MULTIMEDIA_PREFERRED_PLUGINS", "windowsmediafoundation");
	QApplication::setAttribute(Qt::AA_DisableHighDpiScaling);
	QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
	QApplication app(argc, argv);
	app.setFont(QFont("Microsoft YaHei UI", 10));
	app.setOrganizationName("WPSCourse");
	app.setApplicationName("wpsrecorder-workbench-qa");
	RecorderUi::Apply(app);
	const auto args = app.arguments();
	if (args.size() == 6 && args[1] == "--focused")
	{
		if (QDir(args[3]).exists() || !QDir().mkpath(args[3])) return 2;
		qputenv("WPSRECORDER_INSTANCE", "focused-qa-" + QByteArray::number(QCoreApplication::applicationPid()));
		QSettings settings(DataDirectory() + "/ui-settings.ini", QSettings::IniFormat);
		settings.setValue("hotkeys", QStringList{QString(), QString(), QString(), QString(), QString(), QString(), QString(), QString()});
		settings.sync();
		QJsonObject result{{"case", args[2]}};
		const bool passed = KRRWorkbenchAcceptance::focused(args[2], args[3], args[5], result);
		result["passed"] = passed;
		QJsonObject status;
		QString error;
		if (!passed)
		{
			SendControl({{"command", "stop"}}, status, error, 1000);
			WaitFor([&]
			{
				return !SendControl({{"command", "status"}}, status, error, 350) || status["state"] != "recording";
			}, 6000);
		}
		SendControl({{"command", "service-exit"}}, status, error, 800);
		QFile report(args[4]);
		if (!report.open(QIODevice::WriteOnly)) return 2;
		report.write(QJsonDocument(result).toJson());
		return passed ? 0 : 1;
	}
	if (argc < 3 || QDir(app.arguments()[1]).exists())
		return 2;
	QDir().mkpath(app.arguments()[1]);
	qputenv("WPSRECORDER_INSTANCE", ("workbench-qa-" + QByteArray::number(QCoreApplication::applicationPid())));
	KRRWorkbenchAcceptance qa(app.arguments()[1], app.arguments()[2], app.arguments().contains("--hd"));
	return app.exec();
}
