#ifndef WPSRECORDER_EDITWIDGETS_H
#define WPSRECORDER_EDITWIDGETS_H
#include "editproject.h"
#include <QImage>
#include <QMap>
#include <QSet>
#include <QWidget>
class KRREditCanvas : public QWidget
{
	Q_OBJECT
  public:
	explicit KRREditCanvas(QWidget *parent = nullptr);
	void setProject(const KRREditProject *p)
	{
		m_project = p;
		update();
	}
	void setFrame(const QImage &image)
	{
		m_frame = image;
		++m_frameCount;
		update();
	}
	void setPosition(qint64 t)
	{
		m_position = t;
		update();
	}
	void setCropEditing(bool editing)
	{
		m_editing = editing;
		m_dragSubtitle = -1;
		setCursor(editing ? Qt::CrossCursor : Qt::ArrowCursor);
		update();
	}
	void setCropPreview(bool preview)
	{
		m_previewCrop = preview;
		update();
	}
	QRectF videoRect() const;
	void setStill(const QImage &image, const QString &caption = {})
	{
		m_still = image; m_stillCaption = caption; update();
	}
	qint64 frameCount() const
	{
		return m_frameCount;
	}
	QImage currentFrame() const
	{
		return m_frame;
	}
  signals:
	void cropRequested(const QRect &crop);
	void subtitlePositionRequested(int track, const QPointF &position);

  protected:
	void paintEvent(QPaintEvent *) override;
	void mousePressEvent(QMouseEvent *) override;
	void mouseMoveEvent(QMouseEvent *) override;
	void mouseReleaseEvent(QMouseEvent *) override;

  private:
	const KRREditProject *m_project = nullptr;
	QImage m_frame, m_still;
	QString m_stillCaption;
	qint64 m_position = 0, m_frameCount = 0;
	bool m_editing = false, m_dragging = false, m_previewCrop = true;
	QRectF displayCrop(const QRect &crop) const;
	QPoint pixelAt(QPoint point) const;
	int subtitleTrackAt(const QPoint &point) const;
	Qt::Edges edgesAt(QPoint point) const;
	QPoint m_anchor;
	QRect m_originalCrop, m_dragCrop;
	Qt::Edges m_edges;
	bool m_move = false, m_new = false;
	QVector<QPair<QRectF, int>> m_subtitleHitAreas;
	int m_dragSubtitle = -1;
	QPoint m_subtitleAnchor;
	QPointF m_subtitleStart, m_subtitlePosition;
};
struct KRREditClipMove
{
	int track = -1, clip = -1, destination = -1;
	QVector<int> indexes;
	QVector<KRREditRange> ranges;
};

class KRREditTimeline : public QWidget
{
	Q_OBJECT
  public:
	explicit KRREditTimeline(QWidget *parent = nullptr);
	void setProject(const KRREditProject *p);
	void setPosition(qint64 time)
	{
		if (m_position == time) return;
		m_position = time;
		update();
		emit positionChanged(time);
	}
	void setSelection(qint64 begin, qint64 end)
	{
		m_begin = begin;
		m_end = end;
		update();
	}
	void setTrack(int track)
	{
		m_track = track;
		update();
	}
	int track() const
	{
		return m_track;
	}
	void setViewport(double zoom, qint64 offset)
	{
		m_zoom = zoom;
		m_offset = offset;
		update();
	}
	void setThumbnail(int index, const QImage &image);
	void setSnapEnabled(bool enabled) { m_snap = enabled; }
	void setLockedTracks(const QSet<int>& tracks) { m_lockedTracks = tracks; update(); }
	void setWaveform(int track, const QVector<float> &wave);
	void clearAssets()
	{
		m_thumbnails.clear();
		m_waves.clear();
		m_wavePeaks.clear();
	}
	int xForTime(qint64 time) const;
	int videoY() const;
	int waveformCount() const
	{
		return m_waves.size();
	}
	int thumbnailCount() const
	{
		return m_thumbnails.size();
	}
  signals:
	void seekRequested(qint64 time);
	void positionChanged(qint64 time);
	void selectionRequested(qint64 begin, qint64 end);
	void clipSelected(qint64 begin, qint64 end);
	void trackRequested(int track);
	void trackToggleRequested(int track);
	void captionRequested(int caption);
	void clipMoved(const KRREditClipMove& move);
	void stillRequested(int still);

  protected:
	void paintEvent(QPaintEvent *) override;
	void mousePressEvent(QMouseEvent *) override;
	void mouseMoveEvent(QMouseEvent *) override;
	void mouseReleaseEvent(QMouseEvent *) override;
	void keyPressEvent(QKeyEvent *) override;

  private:
	qint64 timeForX(int x) const;
	qint64 snapTime(qint64 time) const;
	qint64 snapClipStart(qint64 start) const;
	qint64 markerAt(int x, qint64 fallback) const;
	int rowForY(int y) const;
	int firstAudioRow() const;
	const QVector<KRREditRange>& clipRanges(int track) const;
	void calculateDrag(int x);
	void resetDrag();
	enum class Drag { None, Seek, Select, ResizeStart, ResizeEnd, MoveSelection, MoveVideo, MoveTrack };
	const KRREditProject *m_project = nullptr;
	QMap<int, QImage> m_thumbnails;
	QHash<QString, QImage> m_stillThumbnails;
	QMap<int, QVector<float>> m_waves;
	QMap<int, float> m_wavePeaks;
	QVector<KRREditRange> m_videoClips;
	QVector<QVector<KRREditRange>> m_audioClips;
	QVector<qint64> m_chapterMarks;
	QSet<int> m_lockedTracks;
	double m_zoom = 1;
	qint64 m_duration = 0;
	qint64 m_offset = 0, m_position = 0, m_begin = 0, m_end = 0, m_anchor = 0;
	int m_track = -1;
	Drag m_drag = Drag::None;
	KRREditClipMove m_move;
	QVector<KRREditRange> m_originalClips;
	QVector<KRREditSegment> m_dragSegments;
	KRREditRange m_dragRange, m_dragPreview;
	qint64 m_dragLimit = 0, m_insertAt = -1;
	bool m_dragMoved = false;
	bool m_snap = false;
	QPoint m_press;
};
#endif
