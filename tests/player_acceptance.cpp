#include "displaygeometry.h"
#include "playbackslider.h"
#include "playerwindow.h"
#include "previewvideo.h"
#include "recordingconfig.h"
#include "uitheme.h"
#include <QApplication>
#include <QComboBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMediaPlayer>
#include <QPushButton>
#include <QScreen>
#include <QStackedWidget>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <functional>

static bool Until(const std::function<bool()>& ready, int timeout = 4800)
{
	QElapsedTimer clock;
	clock.start();
	while (clock.elapsed() < timeout)
	{
		QApplication::processEvents();
		if (ready()) return true;
		QThread::msleep(10);
	}
	return false;
}

class MeasuredPreview : public KRRPreviewVideo
{
public:
	void beginMeasurement()
	{
		m_lastFrame = frameCount();
		m_clock.start();
	}
	QVector<double> paintCosts, intervals;

protected:
	void paintEvent(QPaintEvent* event) override
	{
		if (!m_clock.isValid() || frameCount() == m_lastFrame)
		{
			KRRPreviewVideo::paintEvent(event);
			return;
		}
		const auto now = m_clock.nsecsElapsed();
		KRRPreviewVideo::paintEvent(event);
		paintCosts.append((m_clock.nsecsElapsed() - now) / 1000000.0);
		if (m_lastPaint >= 0) intervals.append((now - m_lastPaint) / 1000000.0);
		m_lastPaint = now;
		m_lastFrame = frameCount();
	}

private:
	quint64 m_lastFrame = 0;
	qint64 m_lastPaint = -1;
	QElapsedTimer m_clock;
};

static double ProcessCpuMs()
{
	FILETIME created{}, exited{}, kernel{}, user{};
	GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
	auto ticks = [](const FILETIME& time)
	{
		return (quint64(time.dwHighDateTime) << 32) | time.dwLowDateTime;
	};
	return (ticks(kernel) + ticks(user)) / 10000.0;
}

static int CheckThroughput(MeasuredPreview& video, QMediaPlayer& player, const QString& report)
{
	const bool ready = Until([&] { return video.frameCount() >= 12 && player.position() > 600; }, 6500);
	QJsonObject stats{{"ready", ready}, {"error", player.errorString()},
		{"integrity_rid", ProcessIntegrityRid()}, {"widget_width", video.width()}, {"widget_height", video.height()}};
	if (ready)
	{
		QElapsedTimer wall;
		QEventLoop loop;
		const auto startedAt = player.position();
		const auto firstFrame = video.frameCount();
		const int recoveries = video.recoveryCount();
		const double firstCpu = ProcessCpuMs();
		video.beginMeasurement();
		wall.start();
		QTimer::singleShot(6500, Qt::PreciseTimer, &loop, &QEventLoop::quit);
		loop.exec();
		const double elapsed = wall.nsecsElapsed() / 1000000.0;
		auto percentile = [](QVector<double> sample, double quantile)
		{
			if (sample.isEmpty()) return 0.0;
			std::sort(sample.begin(), sample.end());
			return sample.at(qBound(0, int(std::ceil(sample.size() * quantile)) - 1, sample.size() - 1));
		};
		stats["wall_ms"] = elapsed;
		stats["media_ms"] = player.position() - startedAt;
		stats["received_frames"] = qint64(video.frameCount() - firstFrame);
		stats["painted_frames"] = video.paintCosts.size();
		stats["painted_fps"] = video.paintCosts.size() * 1000 / elapsed;
		stats["paint_p95_ms"] = percentile(video.paintCosts, .95);
		stats["frame_gap_p95_ms"] = percentile(video.intervals, .95);
		stats["frame_gap_p99_ms"] = percentile(video.intervals, .99);
		stats["cpu_ms"] = ProcessCpuMs() - firstCpu;
		stats["recoveries"] = video.recoveryCount() - recoveries;
	}
	QFile file(report);
	if (!file.open(QIODevice::WriteOnly)) return 2;
	file.write(QJsonDocument(stats).toJson());
	return ready ? 0 : 1;
}

static QJsonObject VerifySlider(KRRPlaybackSlider& seek, const QString& path)
{
	const auto original = seek.value(), minimum = seek.minimum(), maximum = seek.maximum();
	seek.setRange(0, 100);
	seek.setValue(50);
	const auto picture = seek.grab().toImage();
	if (!path.isEmpty()) picture.save(path);
	seek.setRange(minimum, maximum);
	seek.setValue(original);
	int first = picture.height(), last = -1;
	for (int y = 0; y < picture.height(); ++y)
	{
		for (int x = picture.width() / 2 - 5; x < picture.width() / 2 + 5; ++x)
		{
			const auto color = picture.pixelColor(x, y);
			const bool blueRing = color.blue() >= 190 && color.blue() - color.red() >= 45;
			const bool whiteCenter = color.red() >= 228 && color.green() >= 228 && color.blue() >= 228;
			if (blueRing || whiteCenter)
			{
				first = qMin(first, y);
				last = y;
			}
		}
	}
	return {{"first_handle_row", first}, {"last_handle_row", last}, {"height", picture.height()},
		{"round_knob_has_padding", first >= 2 && last <= picture.height() - 3 && last - first >= 8}};
}

static int CheckStability(QWidget& root, KRRPreviewVideo& video, KRRPlaybackSlider& seek,
	QMediaPlayer& player, const QString& report)
{
	auto primary = QGuiApplication::primaryScreen();
	auto screens = QGuiApplication::screens();
	QJsonArray cases;
	QString failure;
	auto image = [&] { return video.grab().toImage().scaled(48, 27, Qt::IgnoreAspectRatio, Qt::FastTransformation); };
	auto record = [&](const QString& name, bool pass, QJsonObject detail = {})
	{
		detail["case"] = name;
		detail["passed"] = pass;
		cases.append(detail);
		if (!pass && failure.isEmpty()) failure = name + ": " + player.errorString();
	};
	const bool starts = Until([&] { return player.duration() > 0 && video.frameCount() >= 6; });
	record("decodes_real_clip", starts, {{"duration_ms", player.duration()}, {"frames", qint64(video.frameCount())},
		{"position_ms", player.position()}, {"video_available", player.isVideoAvailable()}, {"recoveries", video.recoveryCount()}});
	if (starts)
	{
		player.setPosition(qMin<qint64>(1500, player.duration() / 5));
		auto slider = VerifySlider(seek, report + ".primary.png");
		slider["dpi"] = int(GetDpiForWindow(reinterpret_cast<HWND>(root.winId())));
		record("primary_slider_ring", slider["round_knob_has_padding"].toBool(), slider);
		const auto playing = [&](const QString& name, const std::function<void()>& change)
		{
			const auto oldFrame = video.frameCount();
			const auto oldImage = image();
			const auto oldPosition = player.position();
			const auto oldRecoveries = video.recoveryCount();
			QElapsedTimer elapsed;
			elapsed.start();
			change();
			const bool resumed = Until([&]
			{
				return video.frameCount() >= oldFrame + 8 && player.position() > oldPosition + 120 &&
					player.state() == QMediaPlayer::PlayingState && !image().isNull() && image() != oldImage;
			});
			record(name, resumed, {{"new_frames", qint64(video.frameCount() - oldFrame)}, {"position_ms", player.position()},
				{"recovery_actions", video.recoveryCount() - oldRecoveries}, {"wall_ms", elapsed.elapsed()}});
		};
		playing("speed_0.5_keeps_presenting", [&] { ChangePreviewRate(&player, .5); });
		playing("speed_1.5_keeps_presenting", [&] { ChangePreviewRate(&player, 1.5); });
		playing("speed_2.0_keeps_presenting", [&] { ChangePreviewRate(&player, 2.0); });
		playing("speed_1.0_keeps_presenting", [&] { ChangePreviewRate(&player, 1.0); });
		player.pause();
		const auto pausedAt = player.position();
		ChangePreviewRate(&player, .5);
		record("paused_speed_preserves_position", Until([&] { return player.state() == QMediaPlayer::PausedState &&
			qAbs(player.position() - pausedAt) <= 100; }, 1000), {{"position_ms", player.position()}});
		player.play();
		for (auto screen : screens)
		{
			if (screen == primary) continue;
			playing("live_move_to_secondary", [&] { root.move(screen->availableGeometry().topLeft() + QPoint(40, 40)); });
			auto detail = VerifySlider(seek, report + ".secondary.png");
			detail["dpi"] = int(GetDpiForWindow(reinterpret_cast<HWND>(root.winId())));
			record("secondary_slider_ring", detail["round_knob_has_padding"].toBool() &&
				QGuiApplication::screenAt(root.frameGeometry().center()) == screen, detail);
			playing("live_move_back_to_primary", [&] { root.move(primary->availableGeometry().topLeft() + QPoint(40, 40)); });
			break;
		}
	}
	QFile file(report);
	if (!file.open(QIODevice::WriteOnly)) return 2;
	file.write(QJsonDocument(QJsonObject{{"passed", failure.isEmpty()}, {"error", failure},
		{"monitor_count", screens.size()}, {"cases", cases}}).toJson());
	return failure.isEmpty() ? 0 : 1;
}
static void Mouse(QWidget *w, QEvent::Type type, int x)
{
	QPoint p(x, w->height() / 2);
	QMouseEvent e(type, p, w->mapToGlobal(p), Qt::LeftButton,
				  type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
	QApplication::sendEvent(w, &e);
}
int main(int argc, char **argv)
{
	InitializeDisplayCoordinates();
	QApplication::setAttribute(Qt::AA_DisableHighDpiScaling);
	QApplication app(argc, argv);
	RecorderUi::Apply(app);
	if (argc < 3)
		return 2;
	const auto args = app.arguments();
	const bool throughput = args.contains("--throughput");
	QWidget root;
	root.resize(throughput ? QSize(1530, 940) : QSize(960, 650));
	auto layout = new QVBoxLayout(&root);
	auto stack = new QStackedWidget(&root);
	stack->addWidget(new QWidget);
	auto video = new MeasuredPreview;
	stack->addWidget(video);
	stack->addWidget(new QWidget);
	stack->setCurrentWidget(video);
	layout->addWidget(stack, 1);
	auto inlineSeek = new KRRPlaybackSlider;
	layout->addWidget(inlineSeek);
	QMediaPlayer player;
	video->attach(&player);
	player.setMuted(true);
	QObject::connect(&player, &QMediaPlayer::durationChanged,
					 [&](qint64 n) { inlineSeek->setRange(0, static_cast<int>(n)); });
	BindPlaybackSeek(inlineSeek, &player);
	KRRPlayerWindow expanded(&player, video, &root);
	root.move(QGuiApplication::primaryScreen()->availableGeometry().topLeft() + QPoint(40, 40));
	root.show();
	KRRCompatiblePreview compatible;
	QString conversionError;
	auto play = [&player](const QString& path)
	{
		player.setMedia(QUrl::fromLocalFile(path));
		player.play();
	};
	if (NeedsCompatiblePreview(args[1]))
	{
		QObject::connect(&compatible, &KRRCompatiblePreview::ready, &player, play);
		QObject::connect(&compatible, &KRRCompatiblePreview::failed, &player,
			[&](const QString& error) { conversionError = error; });
		QObject::connect(&player, qOverload<QMediaPlayer::Error>(&QMediaPlayer::error), &player,
			[&](QMediaPlayer::Error error)
			{
				if (error != QMediaPlayer::NoError) compatible.retryWithEncoding();
			});
		compatible.prepare(args[1]);
	}
	else play(args[1]);
	if (throughput) return CheckThroughput(*video, player, args[2]);
	if (args.contains("--stability")) return CheckStability(root, *video, *inlineSeek, player, args[2]);
	QTimer timer;
	QElapsedTimer wall, stageTime;
	wall.start();
	stageTime.start();
	int stage = 0;
	QJsonArray cases;
	QString error;
	int expected = 0;
	bool done = false;
	auto finish = [&](QString e)
	{
		if (done)
			return;
		done = true;
		error = e;
		QFile f(args[2]);
		f.open(QIODevice::WriteOnly);
		f.write(QJsonDocument(QJsonObject{{"passed", e.isEmpty()}, {"error", e}, {"stage", stage}, {"cases", cases}})
					.toJson());
		app.exit(e.isEmpty() ? 0 : 1);
	};
	auto record = [&](QString name) { cases.append(QJsonObject{{"case", name}, {"passed", true}}); };
	QObject::connect(
		&timer, &QTimer::timeout,
		[&]
		{
			if (wall.elapsed() > 20000)
			{
				finish(conversionError.isEmpty() ? "Player acceptance timeout. " + player.errorString() : conversionError);
				return;
			}
			if (stage == 0 && player.duration() > 0 && inlineSeek->maximum() > 0)
			{
				player.pause();
				inlineSeek->setValue(0);
				Mouse(inlineSeek, QEvent::MouseButtonPress, inlineSeek->width() * 3 / 4);
				Mouse(inlineSeek, QEvent::MouseButtonRelease, inlineSeek->width() * 3 / 4);
				expected = inlineSeek->value();
				stage = 1;
				stageTime.restart();
			}
			else if (stage == 1 && stageTime.elapsed() > 200)
			{
				if (qAbs(player.position() - expected) > 100 && stageTime.elapsed() < 3000)
					return;
				if (qAbs(player.position() - expected) > 100 || expected < player.duration() * .70 ||
					player.state() != QMediaPlayer::PausedState)
				{
					finish(QString("Inline click: position=%1 expected=%2 duration=%3 state=%4 slider_width=%5")
							   .arg(player.position())
							   .arg(expected)
							   .arg(player.duration())
							   .arg(player.state())
							   .arg(inlineSeek->width()));
					return;
				}
				record("inline_click_seeks");
				Mouse(inlineSeek, QEvent::MouseButtonPress, inlineSeek->width() / 5);
				Mouse(inlineSeek, QEvent::MouseMove, inlineSeek->width() * 4 / 5);
				Mouse(inlineSeek, QEvent::MouseButtonRelease, inlineSeek->width() * 4 / 5);
				expected = inlineSeek->value();
				stage = 2;
				stageTime.restart();
			}
			else if (stage == 2 && stageTime.elapsed() > 200)
			{
				if (qAbs(player.position() - expected) > 100 && stageTime.elapsed() < 3000)
					return;
				if (qAbs(player.position() - expected) > 100 || expected < player.duration() * .75)
				{
					finish(QString("Inline drag: actual=%1 wanted=%2").arg(player.position()).arg(expected));
					return;
				}
				record("inline_drag_seeks");
				expanded.enter();
				stage = 3;
				stageTime.restart();
			}
			else if (stage == 3 && stageTime.elapsed() > 300)
			{
				auto screen = QGuiApplication::screenAt(expanded.frameGeometry().center());
				auto safe = screen->availableGeometry();
				auto seek = expanded.findChild<QSlider *>("fullscreenSeek");
				auto close = expanded.findChild<QPushButton *>("previewWindowClose");
				bool contained = expanded.fillsWorkArea() && safe.contains(expanded.frameGeometry());
				for (auto w : QList<QWidget *>{seek, close, expanded.findChild<QPushButton *>("fullscreenPlay")})
					contained &= w && safe.contains(QRect(w->mapToGlobal(QPoint()), w->size()));
				if (!contained)
				{
					finish("Expanded controls overlap outside available work area");
					return;
				}
				record("workarea_controls_inside_taskbar_boundary");
				Mouse(seek, QEvent::MouseButtonPress, seek->width() / 4);
				Mouse(seek, QEvent::MouseButtonRelease, seek->width() / 4);
				expected = seek->value();
				stage = 4;
				stageTime.restart();
			}
			else if (stage == 4 && stageTime.elapsed() > 200)
			{
				if (qAbs(player.position() - expected) > 100 && stageTime.elapsed() < 3000)
					return;
				if (qAbs(player.position() - expected) > 100 || expected > player.duration() * .3)
				{
					finish("Expanded click seek failed");
					return;
				}
				record("expanded_click_seeks");
				expanded.findChild<QComboBox *>("previewWindowMode")->setCurrentIndex(1);
				stage = 5;
				stageTime.restart();
			}
			else if (stage == 5 && stageTime.elapsed() > 350)
			{
				RECT bounds{};
				GetWindowRect(reinterpret_cast<HWND>(expanded.winId()), &bounds);
				auto screen = QGuiApplication::screenAt(expanded.frameGeometry().center());
				bool full = expanded.isFullScreen() &&
							QRect(bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top) ==
								screen->geometry() &&
							(GetWindowLongPtrW(reinterpret_cast<HWND>(expanded.winId()), GWL_EXSTYLE) & WS_EX_TOPMOST);
				if (!full)
				{
					finish("True fullscreen did not cover screen");
					return;
				}
				record("true_fullscreen_covers_monitor");
				expanded.findChild<QComboBox *>("previewWindowMode")->setCurrentIndex(0);
				stage = 6;
				stageTime.restart();
			}
			else if (stage == 6 && stageTime.elapsed() > 300)
			{
				qint64 position = player.position();
				expanded.findChild<QPushButton *>("previewWindowClose")->click();
				if (expanded.isVisible() || video->parentWidget() != stack || qAbs(player.position() - position) > 100)
				{
					finish("Top right close did not preserve inline playhead");
					return;
				}
				record("top_right_close_preserves_playhead");
				expanded.enter();
				QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
				QApplication::sendEvent(&expanded, &esc);
				if (expanded.isVisible() || video->parentWidget() != stack)
				{
					finish("Esc did not return preview");
					return;
				}
				record("escape_returns_preview");
				if(QGuiApplication::screens().size()>1){root.move(QGuiApplication::screens().last()->availableGeometry().topLeft()+QPoint(40,40));stage=7;stageTime.restart();}else finish({});
			}
			else if(stage==7&&stageTime.elapsed()>250){expanded.enter();stage=8;stageTime.restart();}
			else if(stage==8&&stageTime.elapsed()>250){auto safe=QGuiApplication::screens().last()->availableGeometry();if(!safe.contains(expanded.frameGeometry())){finish("Second monitor workarea failed");return;}record("second_monitor_workarea");expanded.reject();finish({});}
		});
	timer.start(50);
	return app.exec();
}
