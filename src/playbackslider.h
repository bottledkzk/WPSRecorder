#ifndef WPSRECORDER_PLAYBACKSLIDER_H
#define WPSRECORDER_PLAYBACKSLIDER_H
#include <QMediaPlayer>
#include <QMouseEvent>
#include <QPainter>
#include <QSlider>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QTimer>
#include <QVector>
#include <QtMath>
#include "uitheme.h"
class KRRPlaybackSlider : public QSlider
{
  public:
	explicit KRRPlaybackSlider(QWidget *parent = nullptr) : QSlider(Qt::Horizontal, parent)
	{
		setMinimumHeight(RecorderUi::Px(22));
	}
	void setHandleOnHover(bool enabled) { m_handleOnHover = enabled; update(); }
	void setMarkers(const QVector<int>& markers)
	{
		m_markers = markers;
		update();
	}

  protected:
	void paintEvent(QPaintEvent*) override
	{
		const auto g = geometryForSlider();
		QPainter painter(this);
		painter.setRenderHint(QPainter::Antialiasing);
		painter.setPen(Qt::NoPen);
		auto paintTrack = [&](qreal left, qreal right, const QColor& color)
		{
			painter.setBrush(color);
			painter.drawRoundedRect(QRectF(left, g.y - g.track / 2, right - left, g.track), g.track / 2, g.track / 2);
		};
		paintTrack(g.left, g.left + g.span, QColor("#424a57"));
		const QColor accent(isEnabled() ? "#61a4f0" : "#616a78");
		if (g.reversed) paintTrack(g.x, g.left + g.span, accent);
		else paintTrack(g.left, g.x, accent);
		painter.setBrush(QColor("#ff6374"));
		for (int mark : m_markers)
		{
			if (mark < minimum() || mark > maximum()) continue;
			const qreal x = g.left + QStyle::sliderPositionFromValue(minimum(), maximum(), mark, g.span, g.reversed);
			const qreal y = g.y, size = RecorderUi::Px(4);
			painter.drawEllipse(QPointF(x, y), size, size);
		}
		if (m_handleOnHover && !underMouse() && !isSliderDown()) return;
		const qreal border = g.radius / 4;
		painter.setBrush(Qt::white);
		painter.setPen(QPen(accent, hasFocus() ? border * 1.30 : border));
		painter.drawEllipse(QPointF(g.x, g.y), g.radius - border, g.radius - border);
	}
	void mousePressEvent(QMouseEvent *e) override
	{
		if (e->button() != Qt::LeftButton || !isEnabled() || maximum() <= minimum()) { e->ignore(); return; }
		const auto g = geometryForSlider();
		const bool onKnob = (!m_handleOnHover || underMouse()) && QRectF(g.x - g.radius, g.y - g.radius, 2 * g.radius, 2 * g.radius).contains(e->localPos());
		m_offset = onKnob ? e->localPos().x() - g.x : 0;
		m_markerPressed = !onKnob && moveToMarker(e->localPos().x());
		setFocus(Qt::MouseFocusReason);
		setSliderDown(true);
		if (!onKnob && !m_markerPressed) moveTo(e->localPos().x());
		m_pressedX = e->localPos().x();
		e->accept();
	}
	void mouseMoveEvent(QMouseEvent *e) override
	{
		if (!isSliderDown()) { e->ignore(); return; }
		if (m_markerPressed && qAbs(e->localPos().x() - m_pressedX) < RecorderUi::Px(4)) return;
		m_markerPressed = false;
		moveTo(e->localPos().x());
		e->accept();
	}
	void mouseReleaseEvent(QMouseEvent *e) override
	{
		if (!isSliderDown() || e->button() != Qt::LeftButton) { e->ignore(); return; }
		if (!m_markerPressed) moveTo(e->localPos().x());
		m_markerPressed = false;
		setSliderDown(false);
		e->accept();
	}

	void enterEvent(QEvent* event) override { update(); QSlider::enterEvent(event); }
	void leaveEvent(QEvent* event) override { update(); QSlider::leaveEvent(event); }

  private:
	struct Geometry
	{
		int left, span, x;
		qreal y, radius, track;
		bool reversed;
	};
	Geometry geometryForSlider() const
	{
		QStyleOptionSlider option;
		initStyleOption(&option);
		const QRect bounds = contentsRect();
		const qreal radius = qMax<qreal>(1, qMin<qreal>(RecorderUi::Px(8),
			qMin((bounds.height() - 4) / 2., (bounds.width() - 4) / 2.)));
		const int left = bounds.left() + qCeil(radius) + 1;
		const int span = qMax(1, bounds.width() - 2 * (left - bounds.left()) - 1);
		const int at = QStyle::sliderPositionFromValue(minimum(), maximum(), sliderPosition(), span, option.upsideDown);
		return {left, span, left + at, bounds.center().y() + .5, radius, qMax<qreal>(2, radius / 2), option.upsideDown};
	}
	void moveTo(qreal x)
	{
		const auto g = geometryForSlider();
		const int pixel = qBound(0, qRound(x - m_offset - g.left), g.span);
		setSliderPosition(QStyle::sliderValueFromPosition(minimum(), maximum(), pixel, g.span, g.reversed));
	}
	bool moveToMarker(qreal x)
	{
		const auto g = geometryForSlider();
		int nearest = -1;
		qreal distance = RecorderUi::Px(6);
		for (int mark : m_markers)
		{
			if (mark < minimum() || mark > maximum()) continue;
			const auto delta = qAbs(x - g.left - QStyle::sliderPositionFromValue(minimum(), maximum(), mark, g.span, g.reversed));
			if (delta <= distance) { distance = delta; nearest = mark; }
		}
		if (nearest < 0) return false;
		setSliderPosition(nearest);
		return true;
	}
	QVector<int> m_markers;
	qreal m_offset = 0;
	qreal m_pressedX = 0;
	bool m_markerPressed = false, m_handleOnHover = false;
};
inline void BindPlaybackSeek(QSlider *slider, QMediaPlayer *player)
{
	auto debounce = new QTimer(slider);
	debounce->setSingleShot(true);
	debounce->setInterval(45);
	QObject::connect(slider, &QSlider::sliderMoved, debounce, [debounce] { debounce->start(); });
	QObject::connect(slider, &QSlider::actionTriggered, debounce, [slider, debounce]
	{
		if (!slider->isSliderDown()) debounce->start();
	});
	QObject::connect(debounce, &QTimer::timeout, player, [slider, player] { player->setPosition(slider->value()); });
	QObject::connect(slider, &QSlider::sliderReleased, player,
					 [slider, player, debounce]
					 {
						 debounce->stop();
						 player->setPosition(slider->value());
					 });
}
#endif
