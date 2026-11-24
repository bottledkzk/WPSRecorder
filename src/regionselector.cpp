#include "regionselector.h"
#include "framecapture.h"
#include "uilanguage.h"
#include "uitheme.h"
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>
#include <cmath>
#include <windows.h>

namespace
{
constexpr int MinPixels = 2;

QRect WorkArea(QPoint point, const QRect &fallback)
{
	MONITORINFO monitor{};
	monitor.cbSize = sizeof(monitor);
	if (!GetMonitorInfoW(MonitorFromPoint({point.x(), point.y()}, MONITOR_DEFAULTTONEAREST), &monitor))
		return fallback;
	const auto &r = monitor.rcWork;
	return QRect(r.left, r.top, r.right - r.left, r.bottom - r.top).intersected(fallback);
}
}

KRRRegionSelector::KRRRegionSelector(const QRect &initial, QWidget *parent)
	: QDialog(parent), m_desktop(KRRFrameCapture::VirtualDesktop())
{
	setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
	setAttribute(Qt::WA_TranslucentBackground);
	setGeometry(m_desktop);
	setMouseTracking(true);
	setCursor(Qt::CrossCursor);
	setWindowTitle(Ui("选择录制区域 · Esc 取消"));
	if (initial.isValid())
		m_selection = initial.intersected(m_desktop).translated(-m_desktop.topLeft());

	auto panel = new QFrame(this);
	m_panel = panel;
	panel->setObjectName("recordingRegionActions");
	panel->setCursor(Qt::ArrowCursor);
	panel->setStyleSheet("#recordingRegionActions{background:#f7faff;border:1px solid #b8cce0;border-radius:10px;}");
	auto layout = new QVBoxLayout(panel);
	layout->setContentsMargins(12, 8, 12, 8);
	auto hint = new QLabel(Ui("框内移动，拖边角缩放；Enter 确认，Esc 取消。"), panel);
	hint->setWordWrap(true);
	hint->setMaximumWidth(RecorderUi::Px(390));
	layout->addWidget(hint);
	auto actions = new QHBoxLayout;
	m_dimensions = new QLabel(panel);
	m_dimensions->setObjectName("recordingRegionDimensions");
	m_dimensions->setMinimumWidth(RecorderUi::Px(110));
	m_confirm = new QPushButton(Ui("确认选区"), panel);
	m_confirm->setObjectName("confirmRecordingRegion");
	m_confirm->setProperty("role", "primary");
	m_confirm->setDefault(true);
	auto cancel = new QPushButton(Ui("取消"), panel);
	cancel->setObjectName("cancelRecordingRegion");
	cancel->setAutoDefault(false);
	actions->addWidget(m_dimensions);
	actions->addStretch();
	actions->addWidget(cancel);
	actions->addWidget(m_confirm);
	layout->addLayout(actions);
	connect(m_confirm, &QPushButton::clicked, this, &KRRRegionSelector::accept);
	connect(cancel, &QPushButton::clicked, this, &KRRRegionSelector::reject);
	updateControls(true);
}

bool KRRRegionSelector::hasSelection() const
{
	return m_selection.width() >= MinPixels && m_selection.height() >= MinPixels;
}

QRect KRRRegionSelector::selectedRegion() const
{
	return hasSelection() ? QRect(mapToGlobal(m_selection.topLeft()), m_selection.size()).intersected(m_desktop)
						  : QRect();
}

void KRRRegionSelector::accept()
{
	if (m_drag == Drag::None && hasSelection())
		QDialog::accept();
}

QPoint KRRRegionSelector::bounded(const QPoint &point) const
{
	return {qBound(0, point.x(), width() - 1), qBound(0, point.y(), height() - 1)};
}

Qt::Edges KRRRegionSelector::hitEdges(const QPoint &point) const
{
	const int margin = RecorderUi::Px(8);
	if (!hasSelection() || !m_selection.adjusted(-margin, -margin, margin, margin).contains(point))
		return {};
	const int left = std::abs(point.x() - m_selection.left()), right = std::abs(point.x() - m_selection.right());
	const int top = std::abs(point.y() - m_selection.top()), bottom = std::abs(point.y() - m_selection.bottom());
	Qt::Edges edges;
	if (qMin(left, right) <= margin)
		edges |= left <= right ? Qt::LeftEdge : Qt::RightEdge;
	if (qMin(top, bottom) <= margin)
		edges |= top <= bottom ? Qt::TopEdge : Qt::BottomEdge;
	return edges;
}

void KRRRegionSelector::updateCursor(const QPoint &point)
{
	const auto edges = hitEdges(point);
	const bool horizontal = edges & (Qt::LeftEdge | Qt::RightEdge);
	const bool vertical = edges & (Qt::TopEdge | Qt::BottomEdge);
	if (horizontal && vertical)
		setCursor(edges.testFlag(Qt::LeftEdge) == edges.testFlag(Qt::TopEdge) ? Qt::SizeFDiagCursor : Qt::SizeBDiagCursor);
	else if (horizontal)
		setCursor(Qt::SizeHorCursor);
	else if (vertical)
		setCursor(Qt::SizeVerCursor);
	else
		setCursor(hasSelection() && m_selection.contains(point) ? Qt::SizeAllCursor : Qt::CrossCursor);
}

void KRRRegionSelector::updateControls(bool positionPanel)
{
	m_confirm->setEnabled(hasSelection() && m_drag == Drag::None);
	m_dimensions->setText(hasSelection() ? QString("%1 × %2 px").arg(m_selection.width()).arg(m_selection.height())
										 : Ui("拖动框选区域"));
	if (!positionPanel)
		return;
	POINT cursor{};
	GetCursorPos(&cursor);
	const QPoint anchor = hasSelection() ? mapToGlobal(m_selection.center()) : QPoint(cursor.x, cursor.y);
	const auto work = WorkArea(anchor, m_desktop).translated(-m_desktop.topLeft());
	if (work.isEmpty())
		return;
	m_panel->adjustSize();
	const int gap = 14;
	const int maxX = qMax(work.left(), work.right() + 1 - m_panel->width());
	const int maxY = qMax(work.top(), work.bottom() + 1 - m_panel->height());
	int x = work.left() + gap, y = work.top() + gap;
	if (hasSelection())
	{
		x = m_selection.center().x() - m_panel->width() / 2;
		y = m_selection.bottom() + gap;
		if (y > maxY)
			y = m_selection.top() - m_panel->height() - gap;
		if (y < work.top())
			y = work.top() + gap;
	}
	m_panel->move(qBound(work.left(), x, maxX), qBound(work.top(), y, maxY));
}

void KRRRegionSelector::invalidateSelection(const QRect &previous)
{
	const QRect dirty = previous.united(m_selection);
	const int margin = RecorderUi::Px(7) + 2;
	if (dirty.isValid())
		update(dirty.adjusted(-margin, -margin, margin, margin));
}

void KRRRegionSelector::paintEvent(QPaintEvent *event)
{
	QPainter painter(this);
	painter.setCompositionMode(QPainter::CompositionMode_Source);
	painter.fillRect(event->rect(), QColor(0, 0, 0, 125));
	if (!hasSelection())
		return;
	// Windows sends clicks through zero-alpha pixels in a layered window.
	painter.fillRect(m_selection, QColor(80, 181, 245, 7));
	painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
	painter.setPen(QPen(QColor("#54c6ff"), 2));
	painter.setBrush(Qt::NoBrush);
	painter.drawRect(m_selection.adjusted(0, 0, -1, -1));
	const int size = RecorderUi::Px(7);
	painter.setPen(QPen(QColor("#2076ac"), 1));
	painter.setBrush(Qt::white);
	for (const int x : {m_selection.left(), m_selection.center().x(), m_selection.right()})
		for (const int y : {m_selection.top(), m_selection.center().y(), m_selection.bottom()})
			if (x != m_selection.center().x() || y != m_selection.center().y())
				painter.drawRect(QRect(x - size / 2, y - size / 2, size, size));
}

void KRRRegionSelector::mousePressEvent(QMouseEvent *event)
{
	if (event->button() != Qt::LeftButton || m_drag != Drag::None)
		return;
	m_anchor = bounded(event->pos());
	m_start = m_selection;
	m_edges = hitEdges(m_anchor);
	m_drag = m_edges ? Drag::Resize : hasSelection() && m_selection.contains(m_anchor) ? Drag::Move : Drag::Draw;
	if (m_drag == Drag::Draw)
		m_selection = {};
	updateControls(false);
	invalidateSelection(m_start);
	event->accept();
}

void KRRRegionSelector::updateDrag(const QPoint &point)
{
	const auto previous = m_selection;
	const QPoint p = bounded(point);
	const QPoint delta = p - m_anchor;
	if (m_drag == Drag::Draw)
		m_selection = QRect(m_anchor, p).normalized();
	else if (m_drag == Drag::Move)
		m_selection.moveTo(qBound(0, m_start.x() + delta.x(), width() - m_start.width()),
						   qBound(0, m_start.y() + delta.y(), height() - m_start.height()));
	else if (m_drag == Drag::Resize)
	{
		m_selection = m_start;
		if (m_edges.testFlag(Qt::LeftEdge))
			m_selection.setLeft(qBound(0, m_start.left() + delta.x(), m_start.right() - MinPixels + 1));
		if (m_edges.testFlag(Qt::RightEdge))
			m_selection.setRight(qBound(m_start.left() + MinPixels - 1, m_start.right() + delta.x(), width() - 1));
		if (m_edges.testFlag(Qt::TopEdge))
			m_selection.setTop(qBound(0, m_start.top() + delta.y(), m_start.bottom() - MinPixels + 1));
		if (m_edges.testFlag(Qt::BottomEdge))
			m_selection.setBottom(qBound(m_start.top() + MinPixels - 1, m_start.bottom() + delta.y(), height() - 1));
	}
	updateControls(false);
	invalidateSelection(previous);
}

void KRRRegionSelector::mouseMoveEvent(QMouseEvent *event)
{
	if (m_drag == Drag::None)
		updateCursor(event->pos());
	else
		updateDrag(event->pos());
}

void KRRRegionSelector::mouseReleaseEvent(QMouseEvent *event)
{
	if (m_drag == Drag::None || event->button() != Qt::LeftButton)
		return;
	updateDrag(event->pos());
	if (!hasSelection())
	{
		const auto previous = m_selection;
		m_selection = m_start;
		invalidateSelection(previous);
	}
	m_drag = Drag::None;
	updateCursor(event->pos());
	updateControls(true);
	event->accept();
}
