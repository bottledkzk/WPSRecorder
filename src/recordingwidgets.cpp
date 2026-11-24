#include "uilanguage.h"
#include "recordingwidgets.h"
#include "uitheme.h"
#include <QApplication>
#include <QFontDatabase>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QStyle>
#include <windows.h>
#include <cmath>
static int P(int value)
{
	return RecorderUi::Px(value);
}
KRRRecordingToolbar::KRRRecordingToolbar(QWidget *owner)
	: QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint), m_owner(owner)
{
	setWindowTitle(Ui("WPS Recorder · 悬浮控制条"));
	setObjectName("recordingFloatBar");
	setAttribute(Qt::WA_TranslucentBackground);
	auto base = new QHBoxLayout(this);
	base->setContentsMargins(P(8), P(5), P(8), P(10));
	m_strip = new QWidget(this);
	m_strip->setObjectName("floatControlStrip");
	m_strip->setAttribute(Qt::WA_StyledBackground);
	base->addWidget(m_strip);
	auto shadow = new QGraphicsDropShadowEffect(m_strip);
	shadow->setColor(QColor(0, 0, 0, 120));
	shadow->setOffset(0, P(4));
	shadow->setBlurRadius(P(12));
	m_strip->setGraphicsEffect(shadow);
	m_emblem = new QPushButton(this);
	m_emblem->setObjectName("floatExpandEmblem");
	m_emblem->setIcon(QIcon(":/ui/floating-app.svg"));
	m_emblem->setFixedSize(P(34), P(34));
	m_emblem->setStyleSheet("QPushButton{background:transparent;border:0;padding:0;}");
	m_emblem->setToolTip(Ui("展开或拖动悬浮条"));
	m_emblem->installEventFilter(this);
	m_emblem->hide();
	base->addWidget(m_emblem);
	auto layout = new QHBoxLayout(m_strip);
	layout->setContentsMargins(P(8), P(5), P(8), P(5));
	layout->setSpacing(P(7));
	auto grip = new QLabel(this);
	grip->setToolTip(Ui("拖动控制条"));
	grip->setObjectName("floatDragGrip");
	grip->setCursor(Qt::OpenHandCursor);
	grip->setFixedWidth(P(20));
	grip->setAlignment(Qt::AlignCenter);
	grip->installEventFilter(this);
	layout->addWidget(grip);
	m_statusDot = new QLabel(m_strip);
	m_statusDot->setObjectName("floatStatusDot");
	m_statusDot->setFixedSize(P(8), P(8));
	m_statusDot->setCursor(Qt::OpenHandCursor);
	m_statusDot->installEventFilter(this);
	layout->addWidget(m_statusDot);
	m_state = new QLabel(this);
	m_state->setObjectName("floatTime");
	m_state->setCursor(Qt::OpenHandCursor);
	m_state->installEventFilter(this);
	QFont clock(QFontDatabase().families().contains("Cascadia Mono") ? "Cascadia Mono" : "Consolas");
	clock.setPixelSize(P(16));
	m_state->setFont(clock);
	m_state->setMinimumWidth(m_state->fontMetrics().horizontalAdvance("00:00:00"));
	layout->addWidget(m_state);
	m_pause = new QPushButton(RecorderUi::Icon("pause"), {}, this);
	m_pause->setObjectName("floatPause");
	m_shot = new QPushButton(RecorderUi::Icon("screenshot"), {}, this);
	m_shot->setToolTip(Ui("截图"));
	m_shot->setObjectName("floatScreenshot");
	m_stop = new QPushButton(RecorderUi::Icon("floating-play"), {}, this);
	m_stop->setObjectName("floatStop");
	m_mark = new QPushButton(RecorderUi::Icon("flag"), {}, this);
	m_mark->setToolTip(Ui("打点"));
	m_mark->setObjectName("floatAddMarker");

	m_mark->setEnabled(false);
	m_markerFeedback.setSingleShot(true);
	connect(&m_markerFeedback, &QTimer::timeout, m_mark, [this] { m_mark->setIcon(m_theme.icon("flag")); });
	auto back = m_modeSwitch = new QPushButton(this);
	setSwitchToMinimal(false);
	auto hide = new QPushButton(this), close = new QPushButton(this);
	m_lecture = new QPushButton(RecorderUi::Icon("pen"), {}, this);
	m_lecture->setObjectName("floatAnnotation");
	m_lecture->setToolTip(Ui("讲解工具"));
	m_lecture->setCheckable(true);
	connect(m_lecture, &QPushButton::clicked, this, &KRRRecordingToolbar::annotationRequested);
	hide->setIcon(RecorderUi::Icon("chevron-right"));
	hide->setObjectName("floatCollapse");
	hide->setAccessibleName(Ui("收起悬浮控制条"));
	hide->setToolTip(hide->accessibleName());
	close->setIcon(RecorderUi::Icon("close"));
	close->setObjectName("floatClose");
	close->setAccessibleName(Ui("关闭悬浮控制条"));
	close->setToolTip(close->accessibleName());
	m_camera = RecorderUi::Toggle("camera", Ui("摄像头画中画"), this);
	m_mic = RecorderUi::Toggle("mic", Ui("麦克风"), this);
	m_camera->setObjectName("floatCameraToggle");
	m_mic->setObjectName("floatMicToggle");
	connect(m_camera, &QPushButton::clicked, this, &KRRRecordingToolbar::cameraRequested);
	connect(m_mic, &QPushButton::clicked, this, &KRRRecordingToolbar::micRequested);
	for (auto b : {m_stop, m_pause, m_shot, m_mark, m_camera, m_mic, back, m_lecture, hide, close})
	{
		if (b == back || b == hide)
		{
			auto divider = new QFrame(this);
			divider->setFixedSize(P(1), P(19));
			divider->setObjectName("floatDivider");
			layout->addWidget(divider);
		}
		b->setAccessibleName(b->toolTip());
		b->setFixedSize(P(b == m_stop ? 31 : 29), P(34));
		layout->addWidget(b);
	}
	connect(m_pause, &QPushButton::clicked, this, &KRRRecordingToolbar::pauseRequested);
	connect(m_shot, &QPushButton::clicked, this, &KRRRecordingToolbar::screenshotRequested);
	connect(m_mark, &QPushButton::clicked, this, &KRRRecordingToolbar::markerRequested);
	connect(m_stop, &QPushButton::clicked, this, [this] { if (m_active) emit stopRequested(); else emit startRequested(); });
	connect(back, &QPushButton::clicked, this, &KRRRecordingToolbar::returnRequested);
	connect(hide, &QPushButton::clicked, this, [this] { setCollapsed(true); });
	connect(m_emblem, &QPushButton::clicked, this, [this] { setCollapsed(false); });
	connect(close, &QPushButton::clicked, this, [this] { this->hide(); emit hideRequested(); });
	for (auto button : m_strip->findChildren<QPushButton*>())
		button->setIconSize(QSize(P(18), P(18)));
	m_emblem->setIconSize(QSize(P(29), P(29)));
	setTheme(RecorderUi::ReadToolbarTheme());
	setStatus(false, false, false, "00:00:00");
}

void KRRRecordingToolbar::setTheme(const RecorderUi::ToolbarTheme& theme)
{
	m_theme = theme;
	// Qt drops both corner radii if their combined height exceeds the strip height.
	QString style = QString(R"(
		#floatControlStrip{background:#212630;border:1px solid #4c5668;border-radius:%1px %2px;}
		#floatControlStrip QPushButton{background:#303640;border:1px solid #363c46;padding:0;border-radius:4px;}
		#floatControlStrip QPushButton:hover{background:#3c4350;border-color:#535c6b;}
		#floatControlStrip QPushButton:checked{background:#283b5a;border-color:#4d6b9b;}
		#floatControlStrip QPushButton:disabled{background:#292e37;border-color:#333944;}
		#floatControlStrip QPushButton#floatStop{border-radius:%3px;}
		#floatControlStrip QPushButton#floatStop[active="true"]{background:#53323a;border-color:#85505a;}
		#floatControlStrip QLabel#floatTime{color:#ecedf1;padding:0;}
		#floatControlStrip QFrame#floatDivider{background:#363c46;}
	)").arg(P(26)).arg(P(14)).arg(P(12));
	if (theme.isLight())
		style += QString(R"(
			#floatControlStrip{background:%1;border-color:%2;}
			#floatControlStrip QPushButton{background:%3;border-color:%4;}
			#floatControlStrip QPushButton:hover{background:%5;border-color:%2;}
			#floatControlStrip QPushButton:checked{background:%6;border-color:%7;}
			#floatControlStrip QPushButton:disabled{background:%1;border-color:%4;}
			#floatControlStrip QPushButton#floatStop[active="true"]{background:%8;border-color:%8;}
			#floatControlStrip QLabel#floatTime{color:%9;}
			#floatControlStrip QFrame#floatDivider{background:%4;}
		)").arg(theme.background.name(), theme.border.name(), theme.field.name(), theme.divider.name(),
			theme.hover.name(), theme.selected.name(), theme.selectedBorder.name(), theme.record.name(), theme.text.name());
	m_strip->setStyleSheet(style);
	m_strip->findChild<QLabel*>("floatDragGrip")->setPixmap(theme.icon("grip").pixmap(P(17), P(17)));
	for (auto button : m_strip->findChildren<QPushButton*>())
	{
		const auto name = button->icon().name();
		if (!name.isEmpty()) button->setIcon(theme.icon(name));
	}
}

void KRRRecordingToolbar::setSwitchToMinimal(bool minimal)
{
	m_modeSwitch->setIcon(m_theme.icon(minimal ? "minimal" : "screen"));
	m_modeSwitch->setObjectName(minimal ? "floatMinimal" : "floatWorkbench");
	m_modeSwitch->setToolTip(minimal ? Ui("极简模式") : Ui("录制工作台"));
	m_modeSwitch->setAccessibleName(m_modeSwitch->toolTip());
}

bool KRRRecordingToolbar::present(QString &error, bool requireExclusion)
{
	if (!SetWindowDisplayAffinity(reinterpret_cast<HWND>(winId()), 0x11) && requireExclusion)
	{
		error = Ui("此系统无法将悬浮条排除出抓屏，请使用主窗口控制。");
		return false;
	}
	adjustSize();
	auto screen = m_placed ? QGuiApplication::screenAt(captureAnchor()) :
		m_owner ? QGuiApplication::screenAt(m_owner->frameGeometry().center()) : QGuiApplication::primaryScreen();
	if (!screen)
		screen = QGuiApplication::primaryScreen();
	if (!screen)
	{
		error = Ui("无法确定悬浮条所在的屏幕");
		return false;
	}
	auto safe = screen->availableGeometry();
	if (!m_placed)
		move(safe.center().x() - width() / 2, safe.bottom() - height() - P(36));
	repositionOnScreen(screen);
	m_placed = true;
	show();
	raise();
	return true;
}
void KRRRecordingToolbar::repositionOnScreen(QScreen *screen)
{
	if (!screen) return;
	const auto area = screen->availableGeometry();
	move(qBound(area.left(), x(), qMax(area.left(), area.right() - width() + 1)),
		qBound(area.top(), y(), qMax(area.top(), area.bottom() - height() + 1)));
}
void KRRRecordingToolbar::setCollapsed(bool collapsed)
{
	auto screen = QGuiApplication::screenAt(captureAnchor());
	layout()->setContentsMargins(collapsed ? QMargins{} : QMargins(P(8), P(5), P(8), P(10)));
	m_strip->setVisible(!collapsed);
	m_emblem->setVisible(collapsed);
	adjustSize();
	repositionOnScreen(screen);
}
void KRRRecordingToolbar::setStatus(bool active, bool paused, bool saving, const QString &time)
{
	if (m_stop->property("active").toBool() != active)
	{
		m_stop->setProperty("active", active);
		m_stop->style()->unpolish(m_stop);
		m_stop->style()->polish(m_stop);
	}
	m_active = active;
	const QString color = active && !paused ? "#ff646f" : "#5bd6aa";
	if (m_statusDot->property("dotColor").toString() != color)
	{
		m_statusDot->setProperty("dotColor", color);
		m_statusDot->setStyleSheet(QString("background:%1;border-radius:%2px;").arg(color).arg(P(8) / 2.));
	}
	m_state->setText(time);
	m_state->setToolTip(saving ? Ui("正在保存") : paused ? Ui("已暂停") : active ? Ui("正在录制") : Ui("准备就绪"));
	m_pause->setIcon(m_theme.icon(paused ? "play" : "pause"));
	m_pause->setToolTip(paused ? Ui("继续") : Ui("暂停"));
	m_pause->setAccessibleName(m_pause->toolTip());
	m_stop->setIcon(m_theme.icon(active ? "floating-stop" : "floating-play"));
	m_stop->setToolTip(active ? Ui("停止并保存") : Ui("开始录制"));
	m_stop->setAccessibleName(m_stop->toolTip());
	m_pause->setEnabled(active && !saving);
	if (m_pause->isHidden() == active)
	{
		auto screen = QGuiApplication::screenAt(captureAnchor());
		m_pause->setVisible(active);
		adjustSize();
		if (isVisible()) repositionOnScreen(screen);
	}
	m_stop->setEnabled(!saving);
	m_shot->setEnabled(!saving);
}
void KRRRecordingToolbar::setAnnotationActive(bool active)
{
	m_lecture->setChecked(active);
}
void KRRRecordingToolbar::setCaptureButtons(bool camera, bool mic, bool cameraAvailable, bool ready)
{
	m_camera->setChecked(camera);
	m_mic->setChecked(mic);
	m_camera->setEnabled(cameraAvailable && ready);
	m_mic->setEnabled(ready);
}
void KRRRecordingToolbar::setMarkerEnabled(bool enabled)
{
	m_mark->setEnabled(enabled);
}
void KRRRecordingToolbar::markerFeedback(bool saved)
{
	if (!isVisible()) return;
	m_mark->setIcon(m_theme.icon(saved ? "check" : "close"));
	m_markerFeedback.start(1400);
}
bool KRRRecordingToolbar::eventFilter(QObject *object, QEvent *event)
{
	if (event->type() == QEvent::MouseButtonPress)
	{
		auto e = static_cast<QMouseEvent *>(event);
		if (e->button() == Qt::LeftButton)
		{
			m_drag = e->globalPos() - pos();
			m_dragStart = e->globalPos();
			m_dragging = true;
			m_moved = false;
			return true;
		}
	}
	if (event->type() == QEvent::MouseMove && m_dragging)
	{
		auto e = static_cast<QMouseEvent *>(event);
		m_moved |= (e->globalPos() - m_dragStart).manhattanLength() >= QApplication::startDragDistance();
		if (m_moved)
		{
			const auto desired = e->globalPos() - m_drag;
			if (auto screen = QGuiApplication::screenAt(e->globalPos()))
			{
				const auto area = screen->geometry();
				move(qBound(area.left(), desired.x(), qMax(area.left(), area.right() - width() + 1)),
					qBound(area.top(), desired.y(), qMax(area.top(), area.bottom() - height() + 1)));
			}
		}
		return true;
	}
	if (event->type() == QEvent::MouseButtonRelease && m_dragging)
	{
		m_dragging = false;
		if (object == m_emblem && !m_moved) m_emblem->click();
		return true;
	}
	return QWidget::eventFilter(object, event);
}
KRRPeakMeter::KRRPeakMeter(QWidget* parent) : QWidget(parent)
{
	setFixedHeight(P(12));
}

void KRRPeakMeter::setLevel(double level)
{
	m_level = qBound(0., level, 1.);
	setAccessibleDescription(QString::number(qRound(m_level * 100)) + "%");
	update();
}

void KRRPeakMeter::paintEvent(QPaintEvent*)
{
	QPainter painter(this);
	const int segments = qBound(12, width() / 3, 72);
	const double step = width() / double(segments);
	const int lit = qBound(0, int(std::ceil((60. + 20. * std::log10(qMax(.001, m_level))) * segments / 60.)), segments);
	for (int i = 0; i < segments; ++i)
	{
		const QColor color(i >= lit ? "#414752" : i * 10 >= segments * 9 ? "#e96667" : i * 4 >= segments * 3 ? "#e5ba55" : "#53cba7");
		painter.fillRect(QRectF(i * step, 1., qMax(1., step - 1), height() - 2.), color);
	}
}

KRRLiveCanvas::KRRLiveCanvas(QWidget *parent) : QLabel(parent)
{
	setMouseTracking(true);
}
void KRRLiveCanvas::setFrame(const QImage &image)
{
	m_image = image;
	update();
}
void KRRLiveCanvas::setPlacement(const QRectF &rect, bool enabled)
{
	if (!m_dragging)
		m_rect = rect;
	m_editable = enabled;
	if (!enabled)
		setCursor(Qt::ArrowCursor);
	update();
}
QRect KRRLiveCanvas::imageRect() const
{
	if (m_image.isNull())
		return {};
	QSize fitted = m_image.size().scaled(size(), Qt::KeepAspectRatio);
	return QRect(QPoint((width() - fitted.width()) / 2, (height() - fitted.height()) / 2), fitted);
}
QRect KRRLiveCanvas::pipRect() const
{
	auto area = imageRect();
	return QRect(area.x() + qRound(m_rect.x() * area.width()), area.y() + qRound(m_rect.y() * area.height()),
				 qRound(m_rect.width() * area.width()), qRound(m_rect.height() * area.height()));
}
void KRRLiveCanvas::paintEvent(QPaintEvent *event)
{
	if (m_image.isNull())
	{
		QLabel::paintEvent(event);
		return;
	}
	QPainter painter(this);
	painter.fillRect(rect(), QColor("#152133"));
	painter.setRenderHint(QPainter::SmoothPixmapTransform);
	painter.drawImage(imageRect(), m_image);
	if (m_editable)
	{
		auto r = pipRect();
		painter.setPen(QPen(QColor("#f06578"), P(2), Qt::DashLine));
		painter.drawRect(r);
		painter.fillRect(QRect(r.right() - P(9), r.bottom() - P(9), P(10), P(10)), QColor("#f06578"));
		painter.setPen(Qt::white);
		painter.drawText(r.adjusted(P(4), P(4), -P(4), -P(4)), Qt::AlignTop | Qt::AlignLeft, Ui("拖动位置 · 右下角缩放"));
	}
}
void KRRLiveCanvas::mousePressEvent(QMouseEvent *event)
{
	if (!m_editable || event->button() != Qt::LeftButton || !pipRect().contains(event->pos()))
		return;
	m_dragging = true;
	m_anchor = event->pos();
	m_start = m_rect;
	auto r = pipRect();
	m_resize = QRect(r.right() - P(18), r.bottom() - P(18), P(20), P(20)).contains(event->pos());
}
void KRRLiveCanvas::mouseMoveEvent(QMouseEvent *event)
{
	if (!m_editable)
		return;
	if (!m_dragging)
	{
		setCursor(pipRect().contains(event->pos()) ? Qt::SizeAllCursor : Qt::ArrowCursor);
		return;
	}
	auto area = imageRect();
	if (area.isEmpty())
		return;
	QPoint delta = event->pos() - m_anchor;
	if (m_resize)
		m_rect = QRectF(m_start.topLeft(),
						QSizeF(qBound(.05, m_start.width() + double(delta.x()) / area.width(), 1 - m_start.x()),
							   qBound(.05, m_start.height() + double(delta.y()) / area.height(), 1 - m_start.y())));
	else
		m_rect.moveTopLeft(QPointF(qBound(0., m_start.x() + double(delta.x()) / area.width(), 1 - m_start.width()),
								   qBound(0., m_start.y() + double(delta.y()) / area.height(), 1 - m_start.height())));
	emit placementChanged(m_rect);
	update();
}
void KRRLiveCanvas::mouseReleaseEvent(QMouseEvent *event)
{
	if (m_dragging)
	{
		mouseMoveEvent(event);
		m_dragging = false;
		emit placementChanged(m_rect);
	}
}
