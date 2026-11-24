// Native geometry and input-routing regression. No camera media is saved.
#include "annotationoverlay.h"
#include "compositor.h"
#include "displaygeometry.h"
#include "framecapture.h"
#include "recordingwidgets.h"
#include "regionselector.h"
#include "uitheme.h"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QSaveFile>
#include <QPushButton>
#include <QScreen>
#include <QTimer>
#include <QToolBar>
#include <QtMath>
#include <algorithm>
#include <windows.h>

static QJsonArray Rect(const QRect &r)
{
	return {r.x(), r.y(), r.width(), r.height()};
}
static void Mouse(QWidget &widget, QEvent::Type type, const QPoint &local)
{
	QMouseEvent event(type, local, widget.mapToGlobal(local), Qt::LeftButton,
					  type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
	QApplication::sendEvent(&widget, &event);
}

static void LetEventsRun(int ms)
{
	QEventLoop loop;
	QTimer::singleShot(ms, &loop, &QEventLoop::quit);
	loop.exec();
}

static bool NativeGesture(QWidget& overlay, const QList<QPoint>& points)
{
	overlay.repaint();
	LetEventsRun(35);
	const auto at = overlay.mapToGlobal(points.first());
	if (GetAncestor(WindowFromPoint({at.x(), at.y()}), GA_ROOT) != reinterpret_cast<HWND>(overlay.winId())) return false;
	const auto desktop = KRRFrameCapture::VirtualDesktop();
	QVector<INPUT> events;
	auto add = [&](const QPoint& position, DWORD button)
	{
		const auto global = overlay.mapToGlobal(position) - desktop.topLeft();
		INPUT input{};
		input.type = INPUT_MOUSE;
		input.mi.dx = qBound(0, qFloor((global.x() + .5) * 65536. / desktop.width()), 65535);
		input.mi.dy = qBound(0, qFloor((global.y() + .5) * 65536. / desktop.height()), 65535);
		input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_MOVE_NOCOALESCE | MOUSEEVENTF_VIRTUALDESK |
			MOUSEEVENTF_ABSOLUTE | button;
		events.append(input);
	};
	add(points.first(), MOUSEEVENTF_LEFTDOWN);
	for (int i = 1; i < points.size(); ++i) add(points[i], 0);
	add(points.last(), MOUSEEVENTF_LEFTUP);
	const auto delivered = SendInput(static_cast<UINT>(events.size()), events.data(), sizeof(INPUT));
	LetEventsRun(70);
	return delivered == static_cast<UINT>(events.size());
}

static QImage RenderOverlay(KRRAnnotationOverlay& overlay)
{
	QImage image(overlay.size(), QImage::Format_ARGB32_Premultiplied);
	image.fill(Qt::transparent);
	overlay.render(&image, QPoint(), QRegion(), QWidget::DrawChildren);
	return image;
}

static QList<QAction*> LectureActions(const KRRAnnotationOverlay& overlay)
{
	QList<QAction*> actions;
	for (auto toolbar : overlay.findChildren<QToolBar*>()) actions.append(toolbar->actions());
	return actions;
}

static bool ChoiceValueIsVisible(QComboBox* choice, int first, int second)
{
	if (!choice || !choice->isVisible()) return false;
	const int firstIndex = choice->findData(first), secondIndex = choice->findData(second);
	if (firstIndex < 0 || secondIndex < 0) return false;
	const int initial = choice->currentIndex();
	choice->setCurrentIndex(firstIndex);
	const auto one = choice->grab().toImage();
	choice->setCurrentIndex(secondIndex);
	const auto two = choice->grab().toImage();
	choice->setCurrentIndex(initial);
	return !one.isNull() && one.size() == two.size() && one != two;
}

// The checker waits for outside input and observes the actual pointer read by the overlay.
static int CheckMouseFollow(QApplication& app, const QString& reportPath)
{
	const auto screen = QGuiApplication::primaryScreen();
	if (!screen) return 1;
	const auto area = screen->availableGeometry();
	const auto fixture = QSize(qMin(1160, area.width()), qMin(700, area.height()));
	const QRect target(area.center() - QPoint(fixture.width() / 2, fixture.height() / 2), fixture);
	QLabel card(nullptr, Qt::Window | Qt::FramelessWindowHint);
	card.setWindowTitle(QStringLiteral("WPS Recorder 跟随放大验收"));
	card.setGeometry(target);
	QPixmap pattern(fixture);
	{
		QPainter painter(&pattern);
		painter.fillRect(pattern.rect(), QColor("#163e77"));
		painter.fillRect(QRect(fixture.width() / 2, 0, fixture.width(), fixture.height()), QColor("#8c321d"));
		painter.setPen(QColor("#f3f6fb"));
		auto font = painter.font(); font.setPixelSize(24); painter.setFont(font);
		painter.drawText(pattern.rect().adjusted(0, 40, 0, 0), Qt::AlignTop | Qt::AlignHCenter,
			QStringLiteral("鼠标在桌面合成画面上移动，放大镜应显示对应区域"));
		painter.drawText(QRect(0, fixture.height() - 85, fixture.width() / 2, 65), Qt::AlignCenter, QStringLiteral("测试点 A"));
		painter.drawText(QRect(fixture.width() / 2, fixture.height() - 85, fixture.width() / 2, 65), Qt::AlignCenter, QStringLiteral("测试点 B"));
	}
	card.setPixmap(pattern);
	card.show();
	card.raise();
	KRRAnnotationOverlay overlay;
	QString error;
	const bool opened = overlay.present(target, {}, error);
	if (opened)
		for (auto action : LectureActions(overlay))
			if (action->data() == "magnifier") action->trigger();
	QJsonObject samples;
	auto finish = [&](bool passed)
	{
		QSaveFile output(reportPath);
		if (!output.open(QIODevice::WriteOnly)) { app.exit(1); return; }
		const auto report = QJsonDocument(QJsonObject{{"passed", passed}, {"overlay_presented", opened},
			{"samples", samples}, {"error", error}});
		output.write(report.toJson(QJsonDocument::Indented));
		app.exit(output.commit() && passed ? 0 : 1);
	};
	if (!opened)
	{
		QTimer::singleShot(0, &app, [&] { finish(false); });
		return app.exec();
	}
	QTimer probe;
	probe.setInterval(120);
	QObject::connect(&probe, &QTimer::timeout, &app, [&]
	{
		const QPoint at = QCursor::pos() - target.topLeft();
		const QPoint local = overlay.mapFromGlobal(QCursor::pos());
		const int safeMargin = qMin(fixture.width(), fixture.height()) / 4;
		if (at.y() < safeMargin || at.y() > fixture.height() - safeMargin ||
			at.x() < safeMargin || at.x() > fixture.width() - safeMargin ||
			qAbs(at.x() - fixture.width() / 2) < safeMargin) return;
		const bool blue = at.x() < fixture.width() / 2;
		const QString key = blue ? "blue" : "orange";
		if (samples.contains(key)) return;
		const int side = qMin(overlay.width(), overlay.height()) / 3;
		const int margin = qMin(24, overlay.width() / 10);
		const QPoint lens(local.x() < overlay.width() / 2 ? overlay.width() - side / 2 - margin : margin + side / 2,
			margin + side / 2);
		const auto color = RenderOverlay(overlay).pixelColor(lens);
		if (color.alpha() == 255 && (blue ? color.blue() > color.red() + 18 : color.red() > color.blue() + 18))
			samples[key] = QJsonObject{{"cursor", QJsonArray{QCursor::pos().x(), QCursor::pos().y()}},
				{"lens", QJsonArray{lens.x(), lens.y()}}, {"color", color.name()}};
		if (samples.size() == 2) finish(true);
	});
	probe.start();
	QTimer::singleShot(180000, &app, [&] { finish(false); });
	return app.exec();
}

static QJsonObject CheckLectureTools(KRRAnnotationOverlay& overlay, bool nativeInput)
{
	POINT previous{};
	GetCursorPos(&previous);
	QImage pattern(overlay.size(), QImage::Format_RGB32);
	pattern.fill(QColor("#328ce6"));
	{
		QPainter painter(&pattern);
		for (int x = 0; x < pattern.width(); x += 8)
			painter.fillRect(x, 0, 4, pattern.height(), QColor("#f9cf55"));
	}
	QLabel card(nullptr, Qt::Window | Qt::FramelessWindowHint);
	card.setGeometry(overlay.geometry());
	card.setPixmap(QPixmap::fromImage(pattern));
	card.show();
	overlay.raise();
	auto tool = [&overlay](const QString& name)
	{
		for (auto action : LectureActions(overlay))
			if (action->data().toString() == name) { action->trigger(); break; }
	};
	auto gesture = [&overlay, nativeInput](const QList<QPoint>& points)
	{
		if (nativeInput) return NativeGesture(overlay, points);
		Mouse(overlay, QEvent::MouseButtonPress, points.first());
		for (int i = 1; i < points.size(); ++i) Mouse(overlay, QEvent::MouseMove, points[i]);
		Mouse(overlay, QEvent::MouseButtonRelease, points.last());
		LetEventsRun(25);
		return true;
	};
	auto red = [](const QColor& pixel) { return pixel.alpha() > 120 && pixel.red() > 175 && pixel.green() < 120; };

	tool("arrow");
	const bool arrowInput = gesture({{100, 210}, {285, 210}});
	tool("rectangle");
	const bool rectangleInput = gesture({{520, 315}, {370, 220}});
	tool("highlight");
	const bool highlightInput = gesture({{630, 215}, {820, 215}, {820, 315}});
	tool("text");
	const auto textSize = overlay.findChild<QComboBox*>("lectureTextSize");
	const bool textValueVisible = ChoiceValueIsVisible(textSize, 24, 32);
	if (textSize) textSize->setCurrentIndex(textSize->findData(32));
	const bool textInput = gesture({{110, 345}});
	const auto field = overlay.findChild<QLineEdit*>("lectureTextInput");
	const bool inlineField = textInput && field && field->isVisible() && (!nativeInput || field->hasFocus()) && !QApplication::activeModalWidget();
	if (field && field->isVisible())
	{
		field->setText("Native QA");
		QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
		QApplication::sendEvent(field, &enter);
	}
	const auto layer = RenderOverlay(overlay);
	const auto shapes = overlay.effects()["shapes"].toArray();
	bool glyphs = false;
	for (int y = 345; y < 400 && !glyphs; ++y)
		for (int x = 112; x < 320 && !glyphs; ++x)
			glyphs = red(layer.pixelColor(x, y));
	const bool arrow = arrowInput && red(layer.pixelColor(180, 210));
	const bool rectangle = rectangleInput && red(layer.pixelColor(440, 220)) && layer.pixelColor(440, 258).alpha() <= 1;
	const bool highlight = highlightInput && layer.pixelColor(700, 215).alpha() >= 75 &&
		layer.pixelColor(820, 265).alpha() >= 75 && layer.pixelColor(700, 265).alpha() <= 1;
	const bool text = inlineField && !field->isVisible() && glyphs &&
		shapes.at(3).toObject()["text"] == "Native QA" && field->font().pixelSize() == 32;

	tool("solid");
	const bool solidInput = gesture({{645, 563}, {521, 465}});
	tool("pixel");
	LetEventsRun(40);
	const auto width = overlay.findChild<QComboBox*>("lectureBlurWidth");
	const auto strength = overlay.findChild<QComboBox*>("lectureBlurStrength");
	const bool blurValueVisible = ChoiceValueIsVisible(strength, 6, 36);
	const bool clickValueVisible = ChoiceValueIsVisible(overlay.findChild<QComboBox*>("lectureClickStyle"), 1, 2);
	const bool adjustable = width && strength && width->isVisible() && strength->isVisible();
	if (adjustable)
	{
		width->setCurrentIndex(width->findData(32));
		strength->setCurrentIndex(strength->findData(6));
	}
	const bool fineInput = gesture({{420, 458}, {450, 458}});
	if (adjustable)
	{
		width->setCurrentIndex(width->findData(80));
		strength->setCurrentIndex(strength->findData(36));
	}
	const bool blurInput = gesture({{420, 520}, {740, 520}, {740, 640}});
	const auto effects = overlay.effects();
	const auto masks = effects["masks"].toArray();
	const auto fine = masks.at(1).toObject(), strong = masks.at(2).toObject();
	const bool brushSettings = adjustable && fineInput && fine["strength"] == 6 && strong["strength"] == 36 &&
		qAbs(fine["width"].toDouble() * overlay.width() - 32) < .1 &&
		qAbs(strong["width"].toDouble() * overlay.width() - 80) < .1;
	QImage composition = pattern, noSource(pattern.size(), QImage::Format_ARGB32_Premultiplied);
	noSource.fill(Qt::transparent);
	ApplyEffects(composition, effects, {});
	ApplyEffects(noSource, effects, {}, true);
	const QPoint brushStart(420, 520);
	const auto blur = composition.pixelColor(brushStart);
	const bool freehand = blurInput && strong["points"].toArray().size() >= 3 &&
		blur != pattern.pixelColor(brushStart) && blur.red() > 65 && blur.green() > 100 &&
		composition.pixelColor(470, 605) == pattern.pixelColor(470, 605);
	const bool noBlackPlaceholder = noSource.pixelColor(brushStart).alpha() == 0;
	const bool solid = solidInput && composition.pixelColor(550, 520) == QColor("#20252d") &&
		composition.pixelColor(550, 480) == QColor("#20252d") &&
		composition.pixelColor(320, 440) == pattern.pixelColor(320, 440);

	tool("magnifier");
	const auto at = overlay.mapToGlobal(QPoint(275, 355));
	if (nativeInput) SetCursorPos(at.x(), at.y());
	LetEventsRun(650);
	QImage displayed = RenderOverlay(overlay);
	QImage composedZoom(pattern.size(), QImage::Format_ARGB32_Premultiplied);
	composedZoom.fill(Qt::transparent);
	ApplyEffects(composedZoom, overlay.effects(), QPointF(275.0 / overlay.width(), 355.0 / overlay.height()), true, pattern);
	const int side = qMin(overlay.width(), overlay.height()) / 3;
	const int rightLensCenter = overlay.width() - side / 2 - 24;
	const int lensCenter = overlay.mapFromGlobal(QCursor::pos()).x() < overlay.width() / 2 ? rightLensCenter : 24 + side / 2;
	int minimum = 255, maximum = 0;
	int compositionMinimum = 255, compositionMaximum = 0;
	for (int x = lensCenter - 27; x <= lensCenter + 27; ++x)
	{
		const int brightness = displayed.pixelColor(x, 24 + side / 2).red();
		minimum = qMin(minimum, brightness);
		maximum = qMax(maximum, brightness);
		const int simulated = composedZoom.pixelColor(rightLensCenter + x - lensCenter, 24 + side / 2).red();
		compositionMinimum = qMin(compositionMinimum, simulated);
		compositionMaximum = qMax(compositionMaximum, simulated);
	}
	const bool magnifier = maximum - minimum > 150;
	const auto liveBlur = displayed.pixelColor(brushStart);
	const bool desktopBlur = liveBlur.alpha() == 255 && liveBlur.red() > 65 && liveBlur.green() > 100 &&
		liveBlur != pattern.pixelColor(brushStart) && displayed.pixelColor(550, 520) == QColor("#20252d");
	tool("magnifier");
	tool("pixel");
	const QPoint start(96, 450);
	Mouse(overlay, QEvent::MouseButtonPress, start);
	for (int i = 0; i < 570; ++i)
		Mouse(overlay, QEvent::MouseMove, QPoint(99 + i, 450 + ((i % 4) == 0 ? 14 : -14)));
	Mouse(overlay, QEvent::MouseButtonRelease, QPoint(669, 450));
	const auto longEffects = overlay.effects();
	const auto longPoints = longEffects["masks"].toArray().last().toObject()["points"].toArray();
	const auto first = longPoints.isEmpty() ? QJsonArray{} : longPoints.first().toArray();
	QString validation;
	auto longComposition = pattern;
	ApplyEffects(longComposition, longEffects, {});
	const bool stableLongBrush = longPoints.size() >= 265 && first.size() == 2 &&
		qAbs(first[0].toDouble() * overlay.width() - start.x()) < 2 &&
		qAbs(first[1].toDouble() * overlay.height() - start.y()) < 2 &&
		ValidateEffects(longEffects, validation) && longComposition.pixelColor(start.x(), start.y()) != pattern.pixelColor(start.x(), start.y());
	for (auto action : LectureActions(overlay)) if (action->text() == "清空") action->trigger();
	const bool cleared = overlay.effects()["shapes"].toArray().isEmpty() && overlay.effects()["masks"].toArray().isEmpty();
	if (nativeInput) SetCursorPos(previous.x, previous.y);

	const bool passed = arrow && rectangle && highlight && text && brushSettings && freehand &&
		noBlackPlaceholder && solid && compositionMaximum - compositionMinimum > 150 &&
		(!nativeInput || (magnifier && desktopBlur)) && stableLongBrush && cleared &&
		textValueVisible && blurValueVisible && clickValueVisible;
	return {{"native_arrow", arrow}, {"native_rectangle", rectangle}, {"native_highlight", highlight},
		{"inline_text_and_glyphs", text}, {"brush_settings", brushSettings}, {"freehand_blur", freehand},
		{"brush_controls_visible", adjustable},
		{"text_value_visible", textValueVisible}, {"blur_value_visible", blurValueVisible}, {"click_value_visible", clickValueVisible},
		{"no_black_placeholder", noBlackPlaceholder}, {"solid_above_blur", solid},
		{"desktop_blur_and_mask", desktopBlur}, {"desktop_magnifier_detail", maximum - minimum},
		{"zoom_compositor_detail", compositionMaximum - compositionMinimum}, {"input_transport", nativeInput ? "Win32 SendInput" : "Qt events"},
		{"gesture_delivery", QJsonArray{arrowInput, rectangleInput, highlightInput, textInput, solidInput, fineInput, blurInput}},
		{"shapes", shapes}, {"masks_seen", masks},
		{"shape_pixels", QJsonArray{layer.pixelColor(180, 210).name(QColor::HexArgb),
			layer.pixelColor(440, 220).name(QColor::HexArgb), layer.pixelColor(700, 215).name(QColor::HexArgb)}},
		{"blur_pixel", blur.name(QColor::HexArgb)}, {"cover_pixel", composition.pixelColor(550, 520).name(QColor::HexArgb)},
		{"text_widget_visible_at_click", inlineField}, {"text_glyphs", glyphs},
		{"long_brush_start_preserved", stableLongBrush}, {"long_brush_points", longPoints.size()}, {"clear", cleared}, {"passed", passed}};
}
int main(int argc, char **argv)
{
	InitializeDisplayCoordinates();
	QApplication::setAttribute(Qt::AA_DisableHighDpiScaling);
	QApplication app(argc, argv);
	const bool nativeLecture = !app.arguments().contains("--direct-lecture-events");
	RecorderUi::Apply(app);
	if (app.arguments().contains("--follow-mouse")) return CheckMouseFollow(app, app.arguments().value(1));
	KRRRegionSelector selector;
	selector.show();
	int failures = 0;
	QTimer::singleShot(
		250, &app,
		[&]
		{
			RECT native{};
			GetWindowRect(reinterpret_cast<HWND>(selector.winId()), &native);
			auto expected = KRRFrameCapture::VirtualDesktop();
			QRect nativeRect(native.left, native.top, native.right - native.left, native.bottom - native.top);
			bool fullCoverage = nativeRect == expected;
			failures += !fullCoverage;
			QJsonObject report{
				{"virtual", Rect(expected)},
				{"qt_geometry", Rect(selector.geometry())},
				{"native_geometry", Rect(nativeRect)},
				{"full_coverage", fullCoverage},
				{"dpi", static_cast<int>(GetDpiForSystem())},
				{"monitors", KRRFrameCapture::Monitors()},
				{"awareness", static_cast<int>(GetAwarenessFromDpiAwarenessContext(GetThreadDpiAwarenessContext()))}};
			selector.hide();
			auto drag = [](KRRRegionSelector &w, const QPoint &from, const QPoint &to) {
				Mouse(w, QEvent::MouseButtonPress, w.mapFromGlobal(from));
				Mouse(w, QEvent::MouseMove, w.mapFromGlobal(to));
				Mouse(w, QEvent::MouseButtonRelease, w.mapFromGlobal(to));
			};
			auto hitWindow = [](KRRRegionSelector &w, QPoint p) {
				w.repaint();
				QApplication::processEvents();
				return GetAncestor(WindowFromPoint({p.x(), p.y()}), GA_ROOT) == reinterpret_cast<HWND>(w.winId());
			};
			QJsonArray selections;
			for (const auto &value : KRRFrameCapture::Monitors())
			{
				const auto m = value.toObject();
				failures += m["friendly_name"].toString().isEmpty();
				const QRect monitor(m["x"].toInt(), m["y"].toInt(), m["width"].toInt(), m["height"].toInt());
				KRRRegionSelector edit;
				edit.show();
				edit.raise();
				const bool bottomHit = hitWindow(edit, QPoint(monitor.center().x(), monitor.bottom() - 2));
				const QPoint begin(monitor.left() + 30, monitor.bottom() - 160), end(monitor.left() + 330, monitor.bottom() - 3);
				drag(edit, end, begin); // Reverse-direction drawing must still use the exact physical pixels.
				QRect expectedSelection(begin, end);
				const bool initial = edit.isVisible() && edit.selectedRegion() == expectedSelection;
				const bool insideHit = hitWindow(edit, expectedSelection.center());
				const QPoint topLeft = expectedSelection.topLeft(), bottomRight = expectedSelection.bottomRight();
				drag(edit, topLeft, topLeft + QPoint(16, -24));
				expectedSelection.setTopLeft(topLeft + QPoint(16, -24));
				const bool firstCorner = edit.selectedRegion() == expectedSelection && edit.isVisible();
				drag(edit, bottomRight, bottomRight - QPoint(20, 28));
				expectedSelection.setBottomRight(bottomRight - QPoint(20, 28));
				const bool secondCorner = edit.selectedRegion() == expectedSelection && edit.isVisible();
				const QPoint rightEdge(expectedSelection.right(), expectedSelection.center().y());
				drag(edit, rightEdge, rightEdge + QPoint(20, 0));
				expectedSelection.setRight(expectedSelection.right() + 20);
				const bool edge = edit.selectedRegion() == expectedSelection;
				const QPoint center = expectedSelection.center(), movement(42, -46);
				drag(edit, center, center + movement);
				expectedSelection.translate(movement);
				const bool moved = edit.selectedRegion() == expectedSelection;
				if (selections.isEmpty())
				{
					QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
					QApplication::sendEvent(&edit, &enter);
				}
				else if (auto *confirm = edit.findChild<QPushButton *>("confirmRecordingRegion"))
					confirm->click();
				const bool confirmed = edit.result() == QDialog::Accepted && !edit.isVisible() &&
					edit.selectedRegion() == expectedSelection;

				KRRRegionSelector reopened(expectedSelection);
				reopened.show();
				const bool restored = reopened.selectedRegion() == expectedSelection;
				QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
				QApplication::sendEvent(&reopened, &esc);
				const bool canceled = !reopened.isVisible() && reopened.result() == QDialog::Rejected;
				const bool passed = bottomHit && initial && insideHit && firstCorner && secondCorner && edge &&
					moved && confirmed && restored && canceled;
				failures += !passed;
				selections.append(QJsonObject{{"monitor", m["name"]}, {"bottom_hit", bottomHit}, {"frame_open_after_draw", initial},
					{"native_inside_hit", insideHit}, {"top_left_resize", firstCorner}, {"bottom_right_resize", secondCorner},
					{"edge_resize", edge}, {"move", moved}, {"confirmed", confirmed}, {"restore", restored},
					{"escape_cancel", canceled}, {"selected", Rect(expectedSelection)}, {"passed", passed}});
			}
			const auto monitors = KRRFrameCapture::Monitors();
			if (monitors.size() >= 2)
			{
				auto middle = [](const QJsonValue &value) {
					const auto m = value.toObject();
					return QPoint(m["x"].toInt() + m["width"].toInt() / 2, m["y"].toInt() + m["height"].toInt() / 2);
				};
				const QPoint from = middle(monitors[0]), to = middle(monitors[1]);
				KRRRegionSelector cross;
				cross.show();
				drag(cross, from, to);
				const bool crossExact = cross.selectedRegion() == QRect(from, to).normalized() && cross.isVisible();
				failures += !crossExact;
				cross.reject();
				report["cross_monitor"] = QJsonObject{{"passed", crossExact}, {"selected", Rect(cross.selectedRegion())}};
			}
			report["selections"] = selections;
			KRRAnnotationOverlay overlay;
			QString error;
			bool presented = overlay.present(QGuiApplication::primaryScreen()->geometry(), {}, error);
			QApplication::processEvents();
			auto toolbar = overlay.findChild<QToolBar *>();
			bool hole = false, toolbarHit = false, draw = false, undo = false, escapePointer = false,
				 escapeExit = false, compact = false;
			if (presented && toolbar)
			{
				for (auto a : toolbar->actions())
					if (a->data().toString() == "pen")
						a->trigger();
				QApplication::processEvents();
				QPoint center = toolbar->mapToGlobal(toolbar->rect().center());
				POINT p{center.x(), center.y()};
				hole = !overlay.mask().contains(overlay.mapFromGlobal(center));
				toolbarHit = GetAncestor(WindowFromPoint(p), GA_ROOT) == reinterpret_cast<HWND>(toolbar->window()->winId());
				Mouse(overlay, QEvent::MouseButtonPress, QPoint(80, 400));
				Mouse(overlay, QEvent::MouseMove, QPoint(180, 480));
				Mouse(overlay, QEvent::MouseButtonRelease, QPoint(240, 490));
				draw = overlay.effects()["shapes"].toArray().size() == 1;
				for (auto a : LectureActions(overlay))
					if (a->text() == "撤销")
						a->trigger();
				undo = overlay.effects()["shapes"].toArray().isEmpty();
				auto width = overlay.findChild<QComboBox *>("brushWidth");
				width->setCurrentIndex(width->findData(10));
				Mouse(overlay, QEvent::MouseButtonPress, QPoint(80, 400));
				Mouse(overlay, QEvent::MouseMove, QPoint(180, 480));
				Mouse(overlay, QEvent::MouseButtonRelease, QPoint(240, 490));
				const auto firstShape = overlay.effects()["shapes"].toArray().first().toObject();
				bool thick = qAbs(firstShape["width"].toDouble() * overlay.width() / 1280.0 - 10) < .01;
				for (auto a : toolbar->actions())
					if (a->data().toString() == "eraser")
						a->trigger();
				Mouse(overlay, QEvent::MouseButtonPress, QPoint(180, 480));
				Mouse(overlay, QEvent::MouseButtonRelease, QPoint(180, 480));
				bool erased = overlay.effects()["shapes"].toArray().isEmpty();
				for (auto a : LectureActions(overlay))
					if (a->text() == "撤销")
						a->trigger();
				bool eraseUndo = overlay.effects()["shapes"].toArray().size() == 1;
				for (auto a : LectureActions(overlay))
					if (a->text() == "重做")
						a->trigger();
				bool eraseRedo = overlay.effects()["shapes"].toArray().isEmpty();
				report["brush_tools"] = QJsonObject{{"width_10", thick},
													{"eraser", erased},
													{"undo_eraser", eraseUndo},
													{"redo_eraser", eraseRedo},
													{"passed", thick && erased && eraseUndo && eraseRedo}};
				failures += !(thick && erased && eraseUndo && eraseRedo);
				bool toggles = true;
				for (const auto &name : {"spotlight", "magnifier", "whiteboard"}) {
					for (auto a : LectureActions(overlay)) if (a->data().toString() == name) {
						a->trigger(); toggles &= a->isCheckable() && a->isChecked() && overlay.effects()[name].toBool();
						for (auto tool : toolbar->actions()) if (tool->data().toString() == "pen") tool->trigger();
						toggles &= a->isChecked();
						a->trigger(); toggles &= !a->isChecked() && !overlay.effects()[name].toBool();
					}
				}
				report["effect_toggle_states"] = QJsonObject{{"passed",toggles}}; failures += !toggles;
				const auto lecture = CheckLectureTools(overlay, nativeLecture);
				report["lecture_tools"] = lecture;
				failures += !lecture["passed"].toBool();
				for (auto a : LectureActions(overlay))
					if (a->objectName() == "compact")
						a->trigger();
				auto actions = toolbar->actions();
				compact = std::count_if(actions.begin(), actions.end(), [](QAction *a) { return a->isVisible(); }) == 2 &&
					!overlay.findChild<QToolBar*>("lectureSecondaryToolbar")->isVisible();
				QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
				QApplication::sendEvent(&overlay, &esc);
				for (auto a : toolbar->actions())
					if (a->data().toString() == "pointer")
						escapePointer = a->isChecked();
				QKeyEvent esc2(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
				QApplication::sendEvent(toolbar, &esc2);
				escapeExit = !overlay.isVisible() && !toolbar->isVisible();
			}
			bool passed = presented && hole && toolbarHit && draw && undo && compact && escapePointer && escapeExit;
			failures += !passed;
			report["annotation"] = QJsonObject{{"presented", presented},
											   {"error", error},
											   {"toolbar_input_hole", hole},
											   {"toolbar_hit_while_pen", toolbarHit},
											   {"draw", draw},
											   {"undo", undo},
											   {"compact", compact},
											   {"escape_pointer", escapePointer},
											   {"escape_exit", escapeExit},
											   {"passed", passed}};
			const QRect screenArea = QGuiApplication::primaryScreen()->geometry();
			const QRect inset(QPoint(220, 270) + screenArea.topLeft(), QSize(500, 300));
			bool projected = overlay.present(inset, {}, error) && overlay.geometry() == screenArea;
			if (projected)
			{
				for (auto a : LectureActions(overlay)) if (a->data() == "solid") a->trigger();
				const QPoint outside(120, 350), inside(280, 430);
				Mouse(overlay, QEvent::MouseButtonPress, outside);
				Mouse(overlay, QEvent::MouseMove, inside);
				Mouse(overlay, QEvent::MouseButtonRelease, inside);
				const auto recordingEffects = overlay.effects();
				QImage clip(inset.size(), QImage::Format_RGB32);
				clip.fill(Qt::white);
				ApplyEffects(clip, recordingEffects, {});
				const auto drawn = RenderOverlay(overlay);
				projected = ValidateEffects(recordingEffects, error) && drawn.pixelColor(190, 392) == QColor("#20252d") &&
					drawn.pixelColor(220, 460).alpha() <= 1 && clip.pixelColor(15, 122) == QColor("#20252d") &&
					clip.pixelColor(70, 122) == QColor(Qt::white);
				const auto grip = overlay.findChild<QWidget*>("lectureToolbarGrip");
				const QPoint oldPos = grip->window()->pos();
				Mouse(*grip, QEvent::MouseButtonPress, QPoint(5, 5));
				Mouse(*grip, QEvent::MouseMove, QPoint(45, 75));
				Mouse(*grip, QEvent::MouseButtonRelease, QPoint(5, 5));
				projected &= grip->window()->pos() == oldPos + QPoint(40, 70);
				overlay.dismiss();
			}
			report["annotation_screen_and_capture_projection"] = QJsonObject{{"passed", projected}, {"error", error}};
			failures += !projected;
			QImage pattern(240, 180, QImage::Format_RGB32);
			for (int y = 0; y < pattern.height(); ++y)
				for (int x = 0; x < pattern.width(); ++x)
					pattern.setPixelColor(x, y, x % 2 ? QColor("#f9cf55") : QColor("#328ce6"));
			const QJsonObject masks{{"masks", QJsonArray{
				QJsonObject{{"type", "mosaic"}, {"rect", QJsonArray{.1, .1, .4, .4}}},
				QJsonObject{{"type", "solid"}, {"rect", QJsonArray{.6, .1, .3, .4}}}}}};
			QImage composed = pattern, live(pattern.size(), QImage::Format_ARGB32_Premultiplied);
			live.fill(Qt::transparent);
			ApplyEffects(composed, masks, {});
			ApplyEffects(live, masks, {}, true, pattern);
			const auto blurred = composed.pixelColor(48, 40);
			const bool trueBlur = blurred != pattern.pixelColor(48, 40) && blurred.red() > 80 && blurred.green() > 100 &&
				qAbs(blurred.red() - composed.pixelColor(49, 40).red()) < 10 && live.pixelColor(48, 40) == blurred;
			const bool solid = composed.pixelColor(160, 40) == QColor("#20252d") && live.pixelColor(160, 40) == QColor("#20252d");
			const bool exterior = composed.pixelColor(40, 140) == pattern.pixelColor(40, 140) && live.pixelColor(40, 140).alpha() == 0;
			failures += !(trueBlur && solid && exterior);
			report["masks"] = QJsonObject{{"blur_in_preview_and_composite", trueBlur}, {"solid", solid}, {"outside_unchanged", exterior}};
			KRRLiveCanvas canvas;
			canvas.resize(900, 600);
			QImage frame(1280, 720, QImage::Format_RGB32);
			frame.fill(Qt::darkBlue);
			canvas.setFrame(frame);
			canvas.setPlacement(QRectF(.55, .3, .3, .4), true);
			canvas.show();
			QApplication::processEvents();
			QRectF placement;
			QObject::connect(&canvas, &KRRLiveCanvas::placementChanged, [&](const QRectF &r) { placement = r; });
			auto image = canvas.imageRect();
			QPoint center(image.x() + qRound(.7 * image.width()), image.y() + qRound(.5 * image.height()));
			Mouse(canvas, QEvent::MouseButtonPress, center);
			Mouse(canvas, QEvent::MouseMove, center - QPoint(100, 60));
			Mouse(canvas, QEvent::MouseButtonRelease, center - QPoint(100, 60));
			bool moved = placement.x() < .5 && placement.y() < .25 && qAbs(placement.width() - .3) < .001;
			QPoint corner(image.x() + qRound(placement.right() * image.width()) - 2,
						  image.y() + qRound(placement.bottom() * image.height()) - 2);
			Mouse(canvas, QEvent::MouseButtonPress, corner);
			Mouse(canvas, QEvent::MouseMove, corner + QPoint(45, 30));
			Mouse(canvas, QEvent::MouseButtonRelease, corner + QPoint(45, 30));
			bool resized = placement.width() > .34 && placement.height() > .44 && placement.right() <= 1 &&
						   placement.bottom() <= 1;
			report["pip_drag_resize"] = QJsonObject{
				{"letterbox_top", image.y()}, {"drag", moved}, {"resize", resized}, {"passed", moved && resized}};
			failures += !(moved && resized);
			canvas.hide();
			KRRRecordingToolbar bar(nullptr);
			int startCount = 0, pauseCount = 0, shotCount = 0, stopCount = 0;
			bool camera = false, mic = false;
			QObject::connect(&bar, &KRRRecordingToolbar::startRequested, [&] { ++startCount; });
			QObject::connect(&bar, &KRRRecordingToolbar::pauseRequested, [&] { ++pauseCount; });
			QObject::connect(&bar, &KRRRecordingToolbar::screenshotRequested, [&] { ++shotCount; });
			QObject::connect(&bar, &KRRRecordingToolbar::stopRequested, [&] { ++stopCount; });
			QObject::connect(&bar, &KRRRecordingToolbar::cameraRequested, [&](bool selected) { camera = selected; });
			QObject::connect(&bar, &KRRRecordingToolbar::micRequested, [&](bool selected) { mic = selected; });
			QString barError;
			bool visible = bar.present(barError);
			bar.findChild<QPushButton*>("floatStop")->click();
			bar.setCaptureButtons(false, false, true, true);
			bar.findChild<QPushButton*>("floatCameraToggle")->click();
			bar.findChild<QPushButton*>("floatMicToggle")->click();
			const auto strip = bar.findChild<QWidget*>("floatControlStrip");
			const auto emblem = bar.findChild<QPushButton*>("floatExpandEmblem");
			bar.findChild<QPushButton*>("floatCollapse")->click();
			const bool collapsed = !strip->isVisible() && emblem->isVisible();
			const auto position = bar.captureAnchor();
			Mouse(*emblem, QEvent::MouseButtonPress, QPoint(5, 5));
			Mouse(*emblem, QEvent::MouseMove, QPoint(45, -45));
			Mouse(*emblem, QEvent::MouseButtonRelease, QPoint(5, 5));
			const bool draggable = bar.captureAnchor() - position == QPoint(40, -50) && emblem->isVisible();
			Mouse(*emblem, QEvent::MouseButtonPress, QPoint(5, 5));
			Mouse(*emblem, QEvent::MouseButtonRelease, QPoint(5, 5));
			const bool expanded = strip->isVisible() && !emblem->isVisible();
			bar.setStatus(true, false, false, "00:00:04");
			for (auto name : {"floatPause", "floatScreenshot", "floatStop"})
				bar.findChild<QPushButton *>(name)->click();
			bar.setStatus(true, true, false, "00:00:05");
			bool continued = bar.findChild<QPushButton *>("floatPause")->toolTip() == "继续";
			bar.setStatus(true, false, true, "00:00:05");
			bool saving = !bar.findChild<QPushButton *>("floatStop")->isEnabled();
			bool barPassed = visible && startCount == 1 && pauseCount == 1 && shotCount == 1 && stopCount == 1 &&
				continued && saving && camera && mic && collapsed && draggable && expanded;
			report["floating_controls"] = QJsonObject{
				{"capture_excluded", visible}, {"pause_signal", pauseCount}, {"screenshot_signal", shotCount},
				{"start_signal", startCount}, {"stop_signal", stopCount}, {"paused_tooltip", continued}, {"saving_disabled", saving},
				{"camera_signal", camera}, {"mic_signal", mic}, {"compact_draggable", draggable},
				{"collapsed", collapsed}, {"expanded", expanded},
				{"error", barError},		   {"passed", barPassed}};
			failures += !barPassed;
			const auto themedLecture = overlay.findChild<QWidget*>("lectureToolbarWindow");
			const auto sampleBackground = [](QWidget* widget)
			{
				const auto image = widget->grab().toImage();
				return image.pixelColor(image.width() / 2, 2);
			};
			const auto hasRoundedCorners = [strip]
			{
				const auto image = strip->grab().toImage();
				const int edge = qRound(image.devicePixelRatio());
				return image.pixelColor(edge, edge).alpha() < 128 &&
					image.pixelColor(image.width() - 1 - edge, edge).alpha() < 128 &&
					image.pixelColor(image.width() / 2, edge).alpha() == 255;
			};
			const auto savedTheme = RecorderUi::ReadToolbarTheme();
			const bool loadedPalette = sampleBackground(strip) ==
				(savedTheme.isLight() ? savedTheme.background : QColor("#212630")) &&
				sampleBackground(themedLecture) ==
				(savedTheme.isLight() ? savedTheme.background : QColor("#151a23"));
			RecorderUi::ToolbarTheme light{
				QColor("#dfd9e9"), QColor("#b6a9c9"), QColor("#332746"), QColor("#f7f2fb"),
				QColor("#e9deef"), QColor("#ede2f7"), QColor("#e4d6f3"), QColor("#b8a5cd"),
				QColor("#766886"), QColor("#a43a59")};
			bar.setTheme(light);
			overlay.setTheme(light);
			const bool lightColors = sampleBackground(strip) == light.background &&
				sampleBackground(themedLecture) == light.background;
			const bool roundedLight = hasRoundedCorners();
			bar.setTheme({});
			overlay.setTheme({});
			const bool restoredDark = sampleBackground(strip) == QColor("#212630") &&
				sampleBackground(themedLecture) == QColor("#151a23");
			const bool roundedDark = hasRoundedCorners();
			const auto shot = bar.findChild<QPushButton*>("floatScreenshot");
			const bool sizes = shot->width() == RecorderUi::Px(29) &&
				shot->iconSize() == QSize(RecorderUi::Px(18), RecorderUi::Px(18)) &&
				emblem->width() == RecorderUi::Px(34) &&
				emblem->iconSize() == QSize(RecorderUi::Px(29), RecorderUi::Px(29));
			const bool themePassed = loadedPalette && lightColors && restoredDark && sizes && roundedLight && roundedDark;
			report["floating_and_lecture_theme"] = QJsonObject{{"light_palette_rendered", lightColors},
				{"dark_palette_restored", restoredDark}, {"sizes", sizes},
				{"rounded_light", roundedLight}, {"rounded_dark", roundedDark},
				{"startup_palette_rendered", loadedPalette},
				{"strip_height_px", strip->height()},
				{"button_px", QJsonArray{shot->width(), shot->height()}},
				{"emblem_px", QJsonArray{emblem->width(), emblem->height(), emblem->iconSize().width()}},
				{"shadow_vertical_margin_px", RecorderUi::Px(5) + RecorderUi::Px(10)},
				{"passed", themePassed}};
			failures += !themePassed;
			bar.hide();
			report["passed"] = failures == 0;
			QFile file(argc > 1 ? argv[1] : "ui-qa.json");
			file.open(QIODevice::WriteOnly);
			file.write(QJsonDocument(report).toJson());
			file.close();
			app.exit(failures ? 1 : 0);
		});
	return app.exec();
}
