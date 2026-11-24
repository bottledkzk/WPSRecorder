#include "uilanguage.h"
#include "editwidgets.h"
#include "uitheme.h"
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>
#include <numeric>
static int P(int n)
{
	return RecorderUi::Px(n);
}
KRREditCanvas::KRREditCanvas(QWidget *parent) : QWidget(parent)
{
	setMinimumSize(P(300), P(170));
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	setMouseTracking(true);
	setObjectName("editCanvas");
}
QRectF KRREditCanvas::videoRect() const
{
	QSize source = m_project ? m_project->size : QSize(16, 9);
	if (m_project && ((m_previewCrop && !m_editing) || !m_still.isNull()))
		source = m_project->crop.size();
	QSize target = source.scaled(size() - QSize(P(24), P(24)), Qt::KeepAspectRatio);
	return QRectF((width() - target.width()) / 2.0, (height() - target.height()) / 2.0, target.width(),
				  target.height());
}
void KRREditCanvas::paintEvent(QPaintEvent *)
{
	m_subtitleHitAreas.clear();
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	p.fillRect(rect(), QColor("#121b2a"));
	if (!m_project || (m_frame.isNull() && m_still.isNull()))
	{
		p.setPen(QColor("#abb8cc"));
		p.drawText(rect(), Qt::AlignCenter, m_project ? Ui("正在读取当前画面…") : Ui("导入视频后，在这里预览与剪辑"));
		return;
	}
	QRectF target = videoRect(), source = m_frame.rect();
	if (m_previewCrop && !m_editing && m_still.isNull())
	{
		auto r = m_project->crop;
		source = QRectF(r.x() * m_frame.width() / double(m_project->size.width()),
						r.y() * m_frame.height() / double(m_project->size.height()),
						r.width() * m_frame.width() / double(m_project->size.width()),
						r.height() * m_frame.height() / double(m_project->size.height()));
	}
	if (m_still.isNull())
		p.drawImage(target, m_frame, source);
	else
	{
		p.fillRect(target, Qt::black);
		const auto fitted = m_still.size().scaled(target.size().toSize(), Qt::KeepAspectRatio);
		p.drawImage(QRectF(target.center() - QPointF(fitted.width()/2., fitted.height()/2.), fitted), m_still);
	}
	if (m_editing && m_still.isNull())
	{
		const auto crop = displayCrop(m_dragging ? m_dragCrop : m_project->crop);
		QPainterPath shade;
		shade.addRect(target);
		QPainterPath clear;
		clear.addRect(crop);
		p.fillPath(shade.subtracted(clear), QColor(0, 0, 0, 145));
		p.setPen(QPen(QColor("#ff6374"), P(2)));
		p.drawRect(crop);
		p.setPen(QPen(QColor(255, 255, 255, 130), 1, Qt::DashLine));
		for (int i = 1; i < 3; ++i)
		{
			p.drawLine(QPointF(crop.left() + crop.width() * i / 3, crop.top()),
					   QPointF(crop.left() + crop.width() * i / 3, crop.bottom()));
			p.drawLine(QPointF(crop.left(), crop.top() + crop.height() * i / 3),
					   QPointF(crop.right(), crop.top() + crop.height() * i / 3));
		}
		p.setBrush(Qt::white);
		p.setPen(QPen(QColor("#ea5270"), 1));
		for (qreal x : {crop.left(), crop.center().x(), crop.right()})
			for (qreal y : {crop.top(), crop.center().y(), crop.bottom()})
				if (x != crop.center().x() || y != crop.center().y())
					p.drawRoundedRect(QRectF(x-P(3),y-P(3),P(6),P(6)), 1, 1);
		p.setPen(Qt::white);
		p.drawText(rect().adjusted(P(12), P(6), -P(12), -P(6)), Qt::AlignTop | Qt::AlignLeft,
				   Ui("拖动边角缩放 · 框内移动 · 框外新建"));
	}
	else
	{
		const auto cues = !m_still.isNull() ? QVector<KRREditCaption>{{0, 1, m_stillCaption}} :
			m_project->captionsAt(m_position);
		for (const auto& cue : cues)
		{
			const auto style = m_project->styleFor(cue);
			const auto text = cue.displayText();
			if (text.isEmpty() || style.color.alpha() == 0) continue;
			QFont font(style.font);
			const int pixels = style.pixels ? style.pixels : qBound(14, m_project->crop.height() / 20, 80);
			font.setPixelSize(qMax(1, qRound(pixels * target.height() / m_project->crop.height())));
			p.setFont(font);
			const auto position = m_dragSubtitle == cue.track ? m_subtitlePosition :
				m_project->subtitlePositions.value(cue.track, QPointF(.5, .9375));
			QRectF area(target.left() + target.width() * (position.x() - .46), target.top(),
				target.width() * .92, target.height() * position.y());
			p.save();
			p.setClipRect(target);
			p.setPen(QColor(16, 21, 32, style.color.alpha()));
			const auto alignment = Qt::AlignHCenter | Qt::AlignBottom | Qt::TextWordWrap;
			if (m_still.isNull())
				m_subtitleHitAreas.append({p.boundingRect(area, alignment, text).adjusted(-P(5), -P(4), P(5), P(4))
					.intersected(target), cue.track});
			for (const QPointF &offset : {QPointF(-1,0), QPointF(1,0), QPointF(0,-1), QPointF(0,1)})
				p.drawText(area.translated(offset), alignment, text);
			p.setPen(style.color);
			p.drawText(area, alignment, text);
			p.restore();
		}
	}
}
int KRREditCanvas::subtitleTrackAt(const QPoint &point) const
{
	for (auto it = m_subtitleHitAreas.crbegin(); it != m_subtitleHitAreas.crend(); ++it)
		if (it->first.contains(point)) return it->second;
	return -1;
}
QRectF KRREditCanvas::displayCrop(const QRect &crop) const
{
	const auto v = videoRect();
	const double x = v.width() / m_project->size.width(), y = v.height() / m_project->size.height();
	return QRectF(v.left() + crop.x()*x, v.top() + crop.y()*y, crop.width()*x, crop.height()*y);
}
QPoint KRREditCanvas::pixelAt(QPoint point) const
{
	const auto v = videoRect();
	return {qBound(0,qRound((point.x()-v.left())*m_project->size.width()/v.width()),m_project->size.width()),
			qBound(0,qRound((point.y()-v.top())*m_project->size.height()/v.height()),m_project->size.height())};
}
Qt::Edges KRREditCanvas::edgesAt(QPoint point) const
{
	const auto r = displayCrop(m_project->crop);
	const int tolerance = P(8);
	if (!r.adjusted(-tolerance,-tolerance,tolerance,tolerance).contains(point)) return {};
	Qt::Edges edges;
	const auto left = std::abs(point.x()-r.left()), right = std::abs(point.x()-r.right());
	const auto top = std::abs(point.y()-r.top()), bottom = std::abs(point.y()-r.bottom());
	if (qMin(left,right) <= tolerance) edges |= left <= right ? Qt::LeftEdge : Qt::RightEdge;
	if (qMin(top,bottom) <= tolerance) edges |= top <= bottom ? Qt::TopEdge : Qt::BottomEdge;
	return edges;
}
void KRREditCanvas::mousePressEvent(QMouseEvent *e)
{
	if (!m_project || !m_still.isNull() || e->button() != Qt::LeftButton) return;
	if (!m_editing)
	{
		m_dragSubtitle = subtitleTrackAt(e->pos());
		if (m_dragSubtitle < 0 || m_dragSubtitle >= m_project->subtitlePositions.size())
		{
			m_dragSubtitle = -1;
			return;
		}
		m_subtitleStart = m_subtitlePosition = m_project->subtitlePositions[m_dragSubtitle];
		m_subtitleAnchor = e->pos();
		setCursor(Qt::ClosedHandCursor);
		return;
	}
	m_edges = edgesAt(e->pos());
	if (!m_edges && !videoRect().contains(e->pos())) return;
	m_anchor = pixelAt(e->pos());
	m_originalCrop = m_dragCrop = m_project->crop;
	m_move = !m_edges && displayCrop(m_originalCrop).contains(e->pos()) &&
		(m_originalCrop.size() != QSize(m_project->size.width()/2*2,m_project->size.height()/2*2));
	m_new = !m_edges && !m_move;
	m_dragging = true;
}
void KRREditCanvas::mouseMoveEvent(QMouseEvent *e)
{
	if (!m_project || !m_still.isNull()) return;
	if (!m_editing)
	{
		if (m_dragSubtitle < 0)
		{
			setCursor(subtitleTrackAt(e->pos()) >= 0 ? Qt::OpenHandCursor : Qt::ArrowCursor);
			return;
		}
		const auto frame = videoRect();
		const auto delta = e->pos() - m_subtitleAnchor;
		m_subtitlePosition = QPointF(qBound(0., m_subtitleStart.x() + delta.x() / qMax(1., frame.width()), 1.),
			qBound(0., m_subtitleStart.y() + delta.y() / qMax(1., frame.height()), 1.));
		update();
		return;
	}
	if (!m_dragging)
	{
		const auto edges = edgesAt(e->pos());
		const bool h = edges & (Qt::LeftEdge|Qt::RightEdge), v = edges & (Qt::TopEdge|Qt::BottomEdge);
		setCursor(h && v ? (edges.testFlag(Qt::LeftEdge) == edges.testFlag(Qt::TopEdge) ? Qt::SizeFDiagCursor : Qt::SizeBDiagCursor)
				  : h ? Qt::SizeHorCursor : v ? Qt::SizeVerCursor
				  : displayCrop(m_project->crop).contains(e->pos()) ? Qt::SizeAllCursor : Qt::CrossCursor);
		return;
	}
	const QPoint pos = pixelAt(e->pos()), delta = pos - m_anchor;
	if (m_move)
	{
		auto moved = m_originalCrop.topLeft() + delta;
		m_dragCrop = m_originalCrop;
		m_dragCrop.moveTo(qBound(0,moved.x(),m_project->size.width()-m_originalCrop.width())/2*2,
						  qBound(0,moved.y(),m_project->size.height()-m_originalCrop.height())/2*2);
	}
	else
	{
		int l=m_originalCrop.x(), r=l+m_originalCrop.width(), t=m_originalCrop.y(), b=t+m_originalCrop.height();
		const bool left = m_new ? pos.x()<m_anchor.x() : m_edges.testFlag(Qt::LeftEdge);
		const bool top = m_new ? pos.y()<m_anchor.y() : m_edges.testFlag(Qt::TopEdge);
		if (m_new)
		{
			l=qMin(pos.x(),m_anchor.x()); r=qMax(pos.x(),m_anchor.x());
			t=qMin(pos.y(),m_anchor.y()); b=qMax(pos.y(),m_anchor.y());
		}
		else
		{
			if (left) l=qMin(r-2,l+delta.x()); else if (m_edges.testFlag(Qt::RightEdge)) r=qMax(l+2,r+delta.x());
			if (top) t=qMin(b-2,t+delta.y()); else if (m_edges.testFlag(Qt::BottomEdge)) b=qMax(t+2,b+delta.y());
		}
		const bool horizontal = m_edges & (Qt::LeftEdge|Qt::RightEdge), vertical = m_edges & (Qt::TopEdge|Qt::BottomEdge);
		const auto ratio = m_project->cropRatio;
		const bool byHeight = (vertical && !horizontal) || ((m_new || (horizontal && vertical)) &&
			!ratio.isEmpty() && qint64(std::abs(delta.y()))*ratio.width()>qint64(std::abs(delta.x()))*ratio.height());
		auto fitted = FitEditCrop(QRect(l,t,r-l,b-t),m_project->size,ratio,byHeight,left,top);
		if (fitted.isValid()) m_dragCrop = fitted;
	}
	update();
}
void KRREditCanvas::mouseReleaseEvent(QMouseEvent *e)
{
	if (m_dragSubtitle >= 0 && !m_editing && e->button() == Qt::LeftButton)
	{
		mouseMoveEvent(e);
		const int track = m_dragSubtitle;
		m_dragSubtitle = -1;
		setCursor(Qt::OpenHandCursor);
		emit subtitlePositionRequested(track, m_subtitlePosition);
		update();
		return;
	}
	if (!m_dragging || e->button() != Qt::LeftButton) return;
	mouseMoveEvent(e);
	m_dragging = false;
	if (m_dragCrop != m_originalCrop) emit cropRequested(m_dragCrop);
	update();
}

KRREditTimeline::KRREditTimeline(QWidget *parent) : QWidget(parent)
{
	setObjectName("editTimeline");
	setMouseTracking(true);
	setFocusPolicy(Qt::ClickFocus);
	setMinimumWidth(P(400));
	setToolTip(Ui("点击片段选中，拖动调整位置。点击标尺或打点定位。"));
}
void KRREditTimeline::setProject(const KRREditProject *p)
{
	m_project = p;
	m_duration = p ? p->editDuration() : 0;
	m_offset = qBound<qint64>(0, m_offset, qMax<qint64>(0, m_duration - qRound64(m_duration / m_zoom)));
	resetDrag();
	m_videoClips.clear();
	m_audioClips.clear();
	m_chapterMarks.clear();
	if (!p) m_stillThumbnails.clear();
	if (p)
	{
		for (const auto& still : p->stills)
			if (!m_stillThumbnails.contains(still.image))
				m_stillThumbnails.insert(still.image, LoadEditStill(still.image).scaled(P(165), P(94), Qt::KeepAspectRatio, Qt::SmoothTransformation));
		m_videoClips = p->videoClips();
		if (p->arranged())
			for (auto& clip : m_videoClips)
				clip = {p->outputTime(clip.begin), p->outputTime(clip.end - 1) + 1};
		m_audioClips.resize(p->audio.size());
		for (int track = 0; track < p->audio.size(); ++track)
			for (const auto& clip : p->audioClips(track, true))
				m_audioClips[track].append({clip.begin, clip.end});
		for (const auto& chapter : (p->arranged() ? p->outputChapters() : p->chapters))
			m_chapterMarks.append(chapter.at);
	}
	setMinimumHeight(firstAudioRow() + P(p ? p->audio.size() * 48 : 0));
	updateGeometry();
	update();
}
void KRREditTimeline::setThumbnail(int i, const QImage &image)
{
	m_thumbnails[i] = image;
	update();
}
void KRREditTimeline::setWaveform(int i, const QVector<float> &wave)
{
	m_waves[i] = wave;
	const auto peak = std::max_element(wave.cbegin(), wave.cend());
	m_wavePeaks[i] = peak == wave.cend() ? .02f : qMax(.02f, *peak);
	update();
}
int KRREditTimeline::xForTime(qint64 t) const
{
	if (!m_project)
		return P(112);
	return P(112) + qRound((t - m_offset) * double(width() - P(124)) * m_zoom / qMax<qint64>(1, m_duration));
}
qint64 KRREditTimeline::timeForX(int x) const
{
	if (!m_project)
		return 0;
	return qBound<qint64>(0, m_offset + qRound64((x - P(112)) * m_duration / (qMax(1, width() - P(124)) * m_zoom)),
		m_duration);
}
int KRREditTimeline::videoY() const
{
	return P(68);
}
int KRREditTimeline::rowForY(int y) const
{
	if (y < P(34))
		return -100;
	if (y < P(104))
		return -1;
	if (y < firstAudioRow())
		return SubtitleTrackSelection(qMin(m_project->subtitlePositions.size() - 1, (y - P(104)) / P(44)));
	return (y - firstAudioRow()) / P(48);
}
int KRREditTimeline::firstAudioRow() const
{
	return P(150 + qMax(0, (m_project ? m_project->subtitlePositions.size() : 1) - 1) * 44);
}
const QVector<KRREditRange>& KRREditTimeline::clipRanges(int track) const
{
	static const QVector<KRREditRange> empty;
	if (track == -1) return m_videoClips;
	return track >= 0 && track < m_audioClips.size() ? m_audioClips[track] : empty;
}
void KRREditTimeline::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.fillRect(rect(), QColor("#202329"));
	if (!m_project)
		return;
	int left = P(112), right = width() - P(12);
	p.setPen(QColor("#b9c6da"));
	p.drawText(QRect(0, 0, left, P(32)), Qt::AlignCenter, m_project->arranged() ? Ui("成片时间轴") : Ui("源时间轴"));
	p.save();
	p.setClipRect(QRect(left, 0, right - left, height()));
	double visible = m_duration / m_zoom;
	double desired = visible / qMax(1, (right - left) / P(90));
	qint64 step = 100;
	for (qint64 n : QList<qint64>{100, 200, 500, 1000, 2000, 5000, 10000, 15000, 30000, 60000, 120000, 300000, 600000,
								  1800000, 3600000, 7200000})
	{
		step = n;
		if (n >= desired)
			break;
	}
	for (qint64 t = m_offset / step * step; t <= m_offset + visible + step; t += step)
	{
		int x = xForTime(t);
		p.setPen(QColor("#344259"));
		p.drawLine(x, P(25), x, height());
		p.setPen(QColor("#b9c6da"));
		p.drawText(x + P(4), P(19), EditTime(t).mid(t >= 3600000 ? 0 : 3, t >= 3600000 ? 8 : 5));
	}
	for (auto mark : m_chapterMarks)
	{
		const int x = xForTime(mark), y = P(26), s = P(4);
		p.setPen(Qt::NoPen);
		p.setBrush(QColor("#f2bc57"));
		p.setPen(QPen(QColor("#f2bc57"), 1));
		p.drawLine(x, y - s - 1, x, y + s);
		p.drawPolygon(QPolygon(QVector<QPoint>{{x, y - s}, {x + 2 * s, y - s}, {x + s, y}, {x, y}}));
	}
	auto paintRange = [&](const KRREditRange &r, int y, int h, const QColor &color)
	{
		QRect box(xForTime(r.begin), y, xForTime(r.end) - xForTime(r.begin), h);
		p.fillRect(box, color);
	};
	p.fillRect(QRect(left, P(36), right - left, P(64)), QColor("#25272c"));
	auto drawThumbnailStrip = [&](const QRect& block, qint64 sourceBegin, qint64 sourceEnd, const QImage& still = QImage{})
	{
		if (block.width() <= 0 || block.right() < left || block.left() > right) return;
		p.save();
		p.setClipRect(block, Qt::IntersectClip);
		p.fillRect(block, QColor("#303239"));
		const int stride = P(110);
		const int first = block.left() + qMax(0, (left - block.left()) / stride) * stride;
		for (int x = first; x <= qMin(block.right(), right); x += stride)
		{
			const qint64 sourceTime = sourceBegin + qRound64(double(x - block.left()) / block.width() * (sourceEnd - sourceBegin));
			const int sample = qBound(0, int(sourceTime * 12 / qMax<qint64>(1, m_project->duration)), 11);
			auto thumb = m_thumbnails.constFind(sample);
			for (int d = 1; thumb == m_thumbnails.cend() && d < 12; ++d)
			{
				thumb = m_thumbnails.constFind(sample - d);
				if (thumb == m_thumbnails.cend()) thumb = m_thumbnails.constFind(sample + d);
			}
			const auto& image = !still.isNull() ? still : (thumb == m_thumbnails.cend() ? still : thumb.value());
			if (image.isNull()) continue;
			const QRect tile(x, block.top(), stride - 1, block.height());
			auto area = QSizeF(tile.size());
			area.scale(image.size(), Qt::KeepAspectRatio);
			p.drawImage(tile, image, QRectF(QPointF((image.width() - area.width()) / 2, (image.height() - area.height()) / 2), area));
		}
		p.restore();
		p.setPen(QColor("#6d7684"));
		p.drawLine(block.left(), block.top(), block.left(), block.bottom());
	};
	if (m_project->arranged())
	{
		qint64 at = 0;
		for (const auto& clip : m_project->segments())
		{
			const auto end = at + clip.end - clip.begin;
			const QRect block(xForTime(at), P(36), xForTime(end) - xForTime(at), P(64));
			const auto still = clip.still >= 0 ? m_stillThumbnails.value(m_project->stills[clip.still].image) : QImage{};
			drawThumbnailStrip(block, clip.begin, clip.end, still);
			if (block.width() > P(46))
			{
				p.save();
				p.setClipRect(block, Qt::IntersectClip);
				p.setPen(Qt::white);
				p.fillRect(QRect(block.left(), block.bottom() - P(16), qMin(P(104), block.width()), P(17)), QColor(10, 12, 16, 180));
				p.drawText(block.adjusted(P(4), 0, -P(3), 0), Qt::AlignBottom | Qt::AlignLeft,
					clip.still >= 0 ? Ui("静止") : EditTime(clip.begin));
				p.restore();
			}
			at = end;
		}
	}
	else
	{
		for (const auto& range : m_project->kept)
			drawThumbnailStrip(QRect(xForTime(range.begin), P(36), xForTime(range.end) - xForTime(range.begin), P(64)), range.begin, range.end);
		for (const auto &still : m_project->stills)
		{
			const int x = xForTime(still.at);
			const QRect label(x - P(5), P(37), P(47), P(21));
			p.setPen(Qt::NoPen);
			p.setBrush(QColor("#2680b4"));
			p.drawRoundedRect(label, P(3), P(3));
			p.setPen(Qt::white);
			p.drawText(label, Qt::AlignCenter, Ui("静止"));
		}
		for (auto split : m_project->splits)
		{
			const int x = xForTime(split);
			p.setPen(QPen(QColor("#f1f4fa"), P(2)));
			p.drawLine(x, P(36), x, P(100));
		}
		for (const auto& r : m_project->protectedRanges)
		{
			paintRange(r, P(94), P(6), QColor("#efb74f"));
			const QRect label(xForTime(r.begin) + P(3), P(75), P(42), P(17));
			if (xForTime(r.end) < label.right()) continue;
			p.fillRect(label, QColor("#efb74f"));
			p.setPen(QColor("#261a05"));
			p.drawText(label, Qt::AlignCenter, Ui("必保"));
		}
	}
	for (int i = 0; i < m_project->captions.size(); ++i)
	{
		auto c = m_project->captions[i];
		QRect box(xForTime(c.begin), P(109 + c.track * 44), qMax(P(3), xForTime(c.end) - xForTime(c.begin)), P(32));
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(m_project->hiddenSubtitleTracks.contains(c.track) ? "#4e485e" : "#7962ac"));
		p.drawRoundedRect(box, P(4), P(4));
		p.setPen(Qt::white);
		p.drawText(box.adjusted(P(5), 0, -P(3), 0), Qt::AlignVCenter, c.text.simplified());
	}
	for (int a = 0; a < m_project->audio.size(); ++a)
	{
		const auto& track = m_project->audio[a];
		int y = firstAudioRow() + P(3 + a * 48);
		p.fillRect(QRect(left, y, right - left, P(40)), QColor("#293448"));
		const auto& ranges = clipRanges(a);
		for (auto r : ranges)
			paintRange(r, y, P(40), QColor(track.audible() ? "#234f4c" : "#364250"));
		const auto wave = m_waves.value(a);
		const float maximum = m_wavePeaks.value(a, .02f);
		p.setPen(QPen(QColor(track.audible() ? "#79d7bc" : "#69798d"), 1));
		int iRange = 0;
		for (int x = left; !wave.isEmpty() && x < right && iRange < ranges.size(); x += 2)
		{
			qint64 t = timeForX(x);
			while (iRange < ranges.size() && t >= ranges[iRange].end) ++iRange;
			if (iRange >= ranges.size() || t < ranges[iRange].begin)
				continue;
			if (m_project->arranged())
			{
				const auto& clip = track.placed[iRange];
				t = t - clip.begin + clip.mediaStart + track.offset;
			}
			int i = qBound(0, int(t * wave.size() / qMax<qint64>(1, m_project->duration)), wave.size() - 1);
			double h = qMin(double(P(17)), wave[i] / maximum * P(17) * track.gain);
			p.drawLine(QPointF(x, y + P(20) - h), QPointF(x, y + P(20) + h));
		}
	}
	qint64 skippedBegin = 0;
	for (int i = 0; !m_project->arranged() && i <= m_project->kept.size(); ++i)
	{
		qint64 skippedEnd = i < m_project->kept.size() ? m_project->kept[i].begin : m_project->duration;
		if (skippedEnd > skippedBegin)
			p.fillRect(
				QRect(xForTime(skippedBegin), P(36), xForTime(skippedEnd) - xForTime(skippedBegin), height() - P(36)),
				QColor(8, 13, 23, 175));
		if (i < m_project->kept.size())
			skippedBegin = m_project->kept[i].end;
	}
	const int rowY = m_track == -1 ? P(36) : m_track < -1 ? P(104 + SelectedSubtitleTrack(m_track) * 44) :
		firstAudioRow() + P(3 + m_track * 48);
	const int rowH = m_track == -1 ? P(64) : P(40);
	const bool movingClip = m_drag == Drag::MoveVideo || m_drag == Drag::MoveTrack;
	const auto selected = m_dragMoved && !movingClip && m_drag != Drag::Seek ? m_dragPreview : KRREditRange{m_begin, m_end};
	if (selected.end > selected.begin && !(m_dragMoved && movingClip))
	{
		QRect selection(xForTime(selected.begin), rowY, xForTime(selected.end) - xForTime(selected.begin), rowH);
		if (m_track != -1) p.fillRect(selection, QColor(239, 81, 104, 32));
		p.setPen(QPen(QColor("#ff677c"), P(2)));
		p.setBrush(Qt::NoBrush);
		p.drawRect(selection.adjusted(0, 0, -1, -1));
		for (int x : {selection.left(), selection.right()})
		{
			p.fillRect(QRect(x - P(3), rowY, P(6), rowH), QColor("#ff677c"));
		}
		if (selection.width() > P(24))
			p.fillRect(QRect(selection.left() + P(4), rowY, selection.width() - P(8), P(6)), QColor("#ff677c"));
	}
	if (m_dragMoved && m_drag == Drag::MoveTrack)
	{
		QRect target(xForTime(m_dragPreview.begin), rowY, qMax(P(3), xForTime(m_dragPreview.end) - xForTime(m_dragPreview.begin)), rowH);
		p.fillRect(target, QColor(116, 204, 232, 98));
		p.setPen(QPen(QColor("#9deaff"), P(2)));
		p.setBrush(Qt::NoBrush);
		p.drawRect(target.adjusted(0, 0, -1, -1));
	}
	if (m_dragMoved && m_drag == Drag::MoveVideo && m_insertAt >= 0)
	{
		const QRect target(xForTime(m_dragPreview.begin), rowY + P(4),
			qMax(P(4), xForTime(m_dragPreview.end) - xForTime(m_dragPreview.begin)), rowH - P(8));
		p.fillRect(target, QColor(116, 204, 232, 82));
		p.setPen(QPen(QColor("#8ac2d3"), 1));
		p.setBrush(Qt::NoBrush);
		p.drawRect(target);
		const int x = xForTime(m_insertAt);
		p.setPen(QPen(QColor("#9deaff"), P(3)));
		p.drawLine(x, rowY - P(4), x, rowY + rowH + P(4));
	}
	int head = xForTime(m_position);
	p.setPen(QPen(QColor("#ffcf68"), P(1)));
	p.drawLine(head, 0, head, height());
	p.setBrush(QColor("#ffcf68"));
	p.setPen(Qt::NoPen);
	p.drawPolygon(QPolygon(QVector<QPoint>{QPoint(head - P(5), 0), QPoint(head + P(5), 0), QPoint(head, P(8))}));
	p.restore();
	p.fillRect(QRect(0, P(34), left, height()), QColor("#272c33"));
	p.setPen(QColor("#e4ecf8"));
	p.drawText(QRect(P(10), P(36), left - P(15), P(64)), Qt::AlignVCenter,
		m_lockedTracks.contains(-1) ? Ui("视频 · 已锁定") : Ui("视频  01"));
	for (int i = 0; i < m_project->subtitlePositions.size(); ++i)
		p.drawText(QRect(P(10), P(104 + i * 44), left - P(15), P(44)), Qt::AlignVCenter,
			Ui("字幕 %1").arg(i + 1) + (m_lockedTracks.contains(SubtitleTrackSelection(i)) ? Ui(" · 锁定") : QString{}));
	for (int a = 0; a < m_project->audio.size(); ++a)
	{
		auto track = m_project->audio[a];
		int y = firstAudioRow() + P(3 + a * 48);
		p.setPen(QColor(track.audible() ? "#9de3cf" : "#78899f"));
		const auto name = track.needsRefresh ? Ui("配音轨待更新") : track.title;
		p.drawText(QRect(P(10), y, left - P(14), P(40)), Qt::AlignVCenter,
			(m_lockedTracks.contains(a) ? Ui("锁 ") : track.audible() ? "● " : "○ ") + name);
	}
	p.setPen(QPen(QColor("#ff6374"), P(2)));
	p.drawLine(1, rowY, 1, rowY + rowH);
}
namespace
{
QVector<KRREditRange> MoveTrackClip(const QVector<KRREditRange>& original, int from, qint64 start,
	qint64 pointer, qint64 limit)
{
	auto placed = original;
	const auto clip = original[from];
	const qint64 length = clip.end - clip.begin;
	start = qBound<qint64>(0, start, qMax<qint64>(0, limit - length));
	QVector<int> order(original.size());
	std::iota(order.begin(), order.end(), 0);
	order.removeAt(from);
	int to = 0;
	while (to < order.size())
	{
		const auto neighbor = original[order[to]];
		if (pointer <= neighbor.begin + (neighbor.end - neighbor.begin) / 2) break;
		++to;
	}
	const qint64 lower = to > 0 ? original[order[to - 1]].end : 0;
	const qint64 upper = to < order.size() ? original[order[to]].begin : limit;
	if (upper - lower >= length)
	{
		const auto at = qBound(lower, start, upper - length);
		placed[from] = {at, at + length};
		return placed;
	}
	if (from == to) return placed;

	// Reorder only the affected span when a gap is too narrow. Preserve its lengths and gaps.
	order.insert(to, from);
	const int first = qMin(from, to), last = qMax(from, to);
	qint64 at = original[first].begin;
	for (int i = first; i <= last; ++i)
	{
		const auto index = order[i];
		const auto duration = original[index].end - original[index].begin;
		placed[index] = {at, at + duration};
		at += duration;
		if (i < last) at += qMax<qint64>(0, original[i + 1].begin - original[i].end);
	}
	return placed;
}

qint64 SourceBoundaryOnTimeline(const QVector<KRREditSegment>& segments, qint64 source)
{
	qint64 at = 0;
	for (const auto& segment : segments)
	{
		if (source < segment.begin) return at;
		if (segment.still < 0 && source < segment.end)
			return at + source - segment.begin;
		at += segment.end - segment.begin;
	}
	return at;
}

qint64 DisplaySourceTime(const QVector<KRREditSegment>& segments, qint64 time, qint64 duration)
{
	for (const auto& segment : segments)
	{
		const auto length = segment.end - segment.begin;
		if (time < length) return segment.still < 0 ? segment.begin + time : segment.begin;
		time -= length;
	}
	return duration + time;
}
}

void KRREditTimeline::mousePressEvent(QMouseEvent *e)
{
	if (!m_project || e->button() != Qt::LeftButton)
		return;
	int row = rowForY(e->pos().y());
	if (row >= m_project->audio.size())
		return;
	if (e->pos().x() < P(112))
	{
		if (row >= 0)
		{
			emit trackRequested(row);
			if (e->pos().x() < P(30))
				emit trackToggleRequested(row);
		}
		else if (row != -100)
			emit trackRequested(row == -1 && !m_project->arranged() ? -1000 : row);
		return;
	}
	qint64 t = timeForX(e->pos().x());
	if (row == -1 && !m_project->arranged() && e->pos().y() <= P(58))
		for (int i=0;i<m_project->stills.size();++i)
		{
			const int x=xForTime(m_project->stills[i].at);
			if (e->pos().x() >= x-P(5) && e->pos().x() <= x+P(42)) { emit stillRequested(i); return; }
		}
	if (row == -100)
	{
		m_drag = Drag::Seek;
		emit seekRequested(markerAt(e->pos().x(), snapTime(t)));
		return;
	}
	if (row != m_track) emit trackRequested(row);
	m_track = row;
	if (m_lockedTracks.contains(row))
	{
		m_drag = Drag::Seek;
		emit seekRequested(t);
		return;
	}
	m_press = e->pos();
	m_anchor = snapTime(t);
	m_dragRange = m_dragPreview = {m_begin, m_end};
	m_drag = Drag::Select;
	if (e->modifiers() & Qt::ShiftModifier) return;
	const int top = row == -1 ? P(36) : row < -1 ? P(104 + SelectedSubtitleTrack(row) * 44) :
		firstAudioRow() + P(3 + row * 48);
	if (m_end > m_begin)
	{
		const int start = xForTime(m_begin), end = xForTime(m_end);
		const bool grip = e->pos().y() <= top + P(9) && e->modifiers().testFlag(Qt::AltModifier);
		if (grip && e->pos().x() >= start - P(6) && e->pos().x() <= end + P(6))
		{
			if (qAbs(e->pos().x() - start) <= P(5)) m_drag = Drag::ResizeStart;
			else if (qAbs(e->pos().x() - end) <= P(5)) m_drag = Drag::ResizeEnd;
			else
			{
				m_drag = Drag::MoveSelection;
				m_anchor = t - m_begin;
			}
			return;
		}
	}
	QVector<KRREditRange> hits = clipRanges(row);
	if (row < -1)
	{
		const auto& captions = m_project->captions;
		for (int i = 0; i < captions.size(); ++i)
			if (captions[i].track == SelectedSubtitleTrack(row)) m_move.indexes.append(i);
		std::sort(m_move.indexes.begin(), m_move.indexes.end(), [&captions](int a, int b)
		{
			return captions[a].begin < captions[b].begin;
		});
		for (int index : m_move.indexes) hits.append({captions[index].begin, captions[index].end});
	}
	for (int i = 0; i < hits.size(); ++i)
	{
		const auto& clip = hits[i];
		if (t < clip.begin || t >= clip.end) continue;
		m_drag = row == -1 ? Drag::MoveVideo : Drag::MoveTrack;
		m_move.track = row;
		m_move.clip = i;
		m_dragLimit = m_duration;
		m_dragRange = m_dragPreview = clip;
		m_anchor = t - clip.begin;
		m_originalClips = hits;
		if (row >= 0 && !m_project->arranged())
		{
			m_dragSegments = m_project->segments();
			m_originalClips.clear();
			for (const auto& placed : m_project->audioClips(row))
				m_originalClips.append({placed.begin, placed.end});
			m_dragLimit = m_project->outputDuration();
		}
		return;
	}
}
qint64 KRREditTimeline::snapTime(qint64 time) const
{
	if (!m_snap) return time;
	const int x = xForTime(time);
	int distance = P(7);
	qint64 best = time;
	const auto consider = [&](qint64 candidate)
	{
		const auto delta = std::abs(xForTime(candidate) - x);
		if (delta < distance) { distance = delta; best = candidate; }
	};
	for (const auto& clip : clipRanges(-1)) { consider(clip.begin); consider(clip.end); }
	for (const auto& track : m_audioClips)
		for (const auto& clip : track) { consider(clip.begin); consider(clip.end); }
	for (auto marker : m_chapterMarks) consider(marker);
	for (const auto& caption : m_project->captions)
	{
		consider(caption.begin);
		consider(caption.end);
	}
	for (const auto& range : m_project->protectedRanges)
	{
		consider(range.begin);
		consider(range.end);
	}
	return best;
}

qint64 KRREditTimeline::markerAt(int x, qint64 fallback) const
{
	int distance = P(7);
	for (auto mark : m_chapterMarks)
	{
		const int delta = qAbs(xForTime(mark) - x);
		if (delta < distance) { distance = delta; fallback = mark; }
	}
	return fallback;
}

qint64 KRREditTimeline::snapClipStart(qint64 start) const
{
	if (!m_snap) return start;
	const qint64 length = m_dragRange.end - m_dragRange.begin;
	int distance = P(8);
	qint64 best = start;
	auto consider = [&](qint64 edge)
	{
		for (auto desired : {edge, edge - length})
		{
			if (desired < 0) continue;
			const int delta = qAbs(xForTime(start) - xForTime(desired));
			if (delta < distance) { distance = delta; best = desired; }
		}
	};
	auto clips = [&](const QVector<KRREditRange>& ranges, int track)
	{
		for (const auto& clip : ranges)
		{
			if (track == m_move.track && clip.begin == m_dragRange.begin && clip.end == m_dragRange.end) continue;
			consider(clip.begin);
			consider(clip.end);
		}
	};
	for (auto marker : m_chapterMarks) consider(marker);
	consider(m_position);
	clips(m_videoClips, -1);
	for (int track = 0; track < m_audioClips.size(); ++track) clips(m_audioClips[track], track);
	const auto movingCaption = m_move.track < -1 ? m_move.indexes.value(m_move.clip, -1) : -1;
	for (int index = 0; index < m_project->captions.size(); ++index)
		if (index != movingCaption)
		{
			consider(m_project->captions[index].begin);
			consider(m_project->captions[index].end);
		}
	return best;
}

void KRREditTimeline::calculateDrag(int x)
{
	const qint64 time = timeForX(x);
	switch (m_drag)
	{
	case Drag::MoveVideo:
	{
		const auto length = m_dragRange.end - m_dragRange.begin;
		const auto start = qBound<qint64>(0, snapClipStart(time - m_anchor), qMax<qint64>(0, m_duration - length));
		m_dragPreview = {start, start + length};
		m_move.destination = 0;
		m_insertAt = m_originalClips.constLast().end;
		for (int i = 0; i < m_originalClips.size(); ++i)
		{
			if (i == m_move.clip) continue;
			const auto clip = m_originalClips[i];
			if (time <= clip.begin + (clip.end - clip.begin) / 2)
			{
				m_insertAt = clip.begin;
				break;
			}
			++m_move.destination;
		}
		if (m_move.destination == m_move.clip) m_insertAt = m_dragRange.begin;
		break;
	}
	case Drag::MoveTrack:
	{
		qint64 target = snapClipStart(time - m_anchor), pointer = time;
		qint64 limit = m_dragLimit;
		if (!m_dragSegments.isEmpty())
		{
			target = SourceBoundaryOnTimeline(m_dragSegments, target);
			pointer = SourceBoundaryOnTimeline(m_dragSegments, pointer);
		}
		const auto original = m_originalClips[m_move.clip];
		if (m_move.track >= 0)
			limit = qMin<qint64>(86400000, qMax(limit, target + original.end - original.begin));
		m_move.ranges = MoveTrackClip(m_originalClips, m_move.clip, target, pointer, limit);
		m_dragPreview = m_move.ranges[m_move.clip];
		if (!m_dragSegments.isEmpty())
			m_dragPreview = {DisplaySourceTime(m_dragSegments, m_dragPreview.begin, m_duration),
							DisplaySourceTime(m_dragSegments, m_dragPreview.end - 1, m_duration) + 1};
		break;
	}
	case Drag::MoveSelection:
	{
		const auto length = m_dragRange.end - m_dragRange.begin;
		const auto start = qBound<qint64>(0, snapTime(time - m_anchor), qMax<qint64>(0, m_duration - length));
		m_dragPreview = {start, start + length};
		break;
	}
	case Drag::ResizeStart:
		m_dragPreview.begin = qBound<qint64>(0, snapTime(time), qMax<qint64>(0, m_dragRange.end - 20));
		break;
	case Drag::ResizeEnd:
		m_dragPreview.end = qBound(qMin(m_duration, m_dragRange.begin + 20), snapTime(time), m_duration);
		break;
	case Drag::Select:
	{
		const auto edge = qBound<qint64>(0, snapTime(time), m_duration);
		m_dragPreview = {qMin(edge, m_anchor), qMax(edge, m_anchor)};
		break;
	}
	default:
		break;
	}
}

void KRREditTimeline::resetDrag()
{
	m_drag = Drag::None;
	m_dragMoved = false;
	m_insertAt = -1;
	m_move = {};
	m_originalClips.clear();
	m_dragSegments.clear();
	unsetCursor();
}

void KRREditTimeline::mouseMoveEvent(QMouseEvent *e)
{
	if (!m_project) return;
	const int x = e->pos().x();
	if (m_drag == Drag::Seek)
	{
		emit seekRequested(snapTime(timeForX(x)));
		return;
	}
	if (m_drag != Drag::None)
	{
		m_dragMoved = m_dragMoved || qAbs(x - m_press.x()) >= P(4);
		if (!m_dragMoved) return;
		calculateDrag(x);
		if (m_drag == Drag::MoveSelection || m_drag == Drag::MoveVideo || m_drag == Drag::MoveTrack)
			setCursor(Qt::ClosedHandCursor);
		update();
		return;
	}
	const int row = rowForY(e->pos().y());
	if (row == m_track && m_end > m_begin && x >= xForTime(m_begin) - P(5) && x <= xForTime(m_end) + P(5))
	{
		const int top = row == -1 ? P(36) : row < -1 ? P(104 + SelectedSubtitleTrack(row) * 44) :
			firstAudioRow() + P(3 + row * 48);
		if (e->pos().y() <= top + P(9) && e->modifiers().testFlag(Qt::AltModifier))
		{
			const bool edge = qAbs(x - xForTime(m_begin)) <= P(5) || qAbs(x - xForTime(m_end)) <= P(5);
			setCursor(edge ? Qt::SizeHorCursor : Qt::OpenHandCursor);
			return;
		}
	}
	unsetCursor();
}

void KRREditTimeline::mouseReleaseEvent(QMouseEvent *e)
{
	if (!m_project || e->button() != Qt::LeftButton || m_drag == Drag::None) return;
	const auto drag = m_drag;
	const auto time = timeForX(e->pos().x());
	if (drag == Drag::Seek)
	{
		resetDrag();
		emit seekRequested(markerAt(e->pos().x(), snapTime(time)));
		return;
	}

	const bool moved = m_dragMoved || qAbs(e->pos().x() - m_press.x()) >= P(4);
	if (moved) calculateDrag(e->pos().x());
	const auto move = m_move;
	const auto range = m_dragRange;
	const auto preview = m_dragPreview;
	const auto anchor = m_anchor;
	const bool trackChanged = drag == Drag::MoveTrack && !move.ranges.isEmpty() &&
		move.ranges[move.clip].begin != m_originalClips[move.clip].begin;
	resetDrag();
	update();

	if (drag == Drag::MoveVideo || drag == Drag::MoveTrack)
	{
		if (moved && (drag == Drag::MoveVideo ? move.destination != move.clip : trackChanged))
			emit clipMoved(move);
		else if (move.track < -1)
		{
			emit captionRequested(move.indexes[move.clip]);
			emit clipSelected(range.begin, range.end);
		}
		else
		{
			emit selectionRequested(range.begin, range.end);
			emit clipSelected(range.begin, range.end);
			emit seekRequested(time);
		}
		return;
	}
	if (moved)
	{
		emit selectionRequested(preview.begin, preview.end);
		emit seekRequested(drag == Drag::Select ? time : preview.begin);
		return;
	}
	if (drag != Drag::Select) return;
	if (m_track < -1)
		emit selectionRequested(anchor, anchor);
	else
	{
		QVector<qint64> boundaries{0, m_duration};
		if (m_track < 0 && !m_project->arranged()) boundaries += m_project->splits;
		for (const auto clip : clipRanges(m_track)) boundaries << clip.begin << clip.end;
		std::sort(boundaries.begin(), boundaries.end());
		const auto end = std::upper_bound(boundaries.begin(), boundaries.end(), anchor);
		emit selectionRequested(end == boundaries.begin() ? 0 : *std::prev(end),
								end == boundaries.end() ? m_duration : *end);
	}
	emit seekRequested(time);
}

void KRREditTimeline::keyPressEvent(QKeyEvent *e)
{
	if (e->key() == Qt::Key_Escape && m_drag != Drag::None)
	{
		resetDrag();
		update();
		e->accept();
		return;
	}
	QWidget::keyPressEvent(e);
}
