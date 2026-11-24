#ifndef WPSRECORDER_RECORDINGWIDGETS_H
#define WPSRECORDER_RECORDINGWIDGETS_H
#include "uitheme.h"
#include <QImage>
#include <QJsonObject>
#include <QLabel>
#include <QTimer>
#include <QWidget>
class QPushButton;
// Small native controls share the main window's commands; they never own a recording.
class KRRRecordingToolbar : public QWidget
{
	Q_OBJECT
  public:
	explicit KRRRecordingToolbar(QWidget *owner);
	bool present(QString &error, bool requireExclusion = true);
	void setTheme(const RecorderUi::ToolbarTheme& theme);
	QPoint captureAnchor() const { return frameGeometry().center(); }
	void setStatus(bool active, bool paused, bool saving, const QString &time);
	void setCaptureButtons(bool camera, bool mic, bool cameraAvailable, bool ready);
	void setAnnotationActive(bool active);
	void setMarkerEnabled(bool enabled);
	void markerFeedback(bool saved);
	void setSwitchToMinimal(bool minimal);
  signals:
	void pauseRequested();
	void startRequested();
	void annotationRequested();
	void screenshotRequested();
	void stopRequested();
	void returnRequested();
	void hideRequested();
	void cameraRequested(bool enabled);
	void micRequested(bool enabled);
	void markerRequested();

  protected:
	bool eventFilter(QObject *, QEvent *) override;

  private:
	void repositionOnScreen(QScreen *screen);
	void setCollapsed(bool collapsed);
	QWidget *m_owner;
	QWidget *m_strip = nullptr;
	QLabel *m_statusDot = nullptr;
	QPushButton *m_emblem = nullptr;
	QLabel *m_state;
	QPushButton *m_pause, *m_stop, *m_shot, *m_camera, *m_mic, *m_mark, *m_lecture;
	QPushButton *m_modeSwitch = nullptr;
	RecorderUi::ToolbarTheme m_theme;
	QTimer m_markerFeedback;
	QPoint m_drag;
	QPoint m_dragStart;
	bool m_dragging = false, m_moved = false, m_placed = false, m_active = false;
};
class KRRPeakMeter : public QWidget
{
public:
	explicit KRRPeakMeter(QWidget* parent = nullptr);
	void setLevel(double level);
	double level() const { return m_level; }
protected:
	void paintEvent(QPaintEvent*) override;
private:
	double m_level = 0;
};

// Placement is relative to the actual video, including letterboxed preview areas.
class KRRLiveCanvas : public QLabel
{
	Q_OBJECT
  public:
	explicit KRRLiveCanvas(QWidget *parent = nullptr);
	void setFrame(const QImage &image);
	void setPlacement(const QRectF &rect, bool enabled);
	QSize frameSize() const { return m_image.size(); }
	const QImage& frame() const { return m_image; }
	QRect imageRect() const;
  signals:
	void placementChanged(const QRectF &rect);

  protected:
	void paintEvent(QPaintEvent *) override;
	void mousePressEvent(QMouseEvent *) override;
	void mouseMoveEvent(QMouseEvent *) override;
	void mouseReleaseEvent(QMouseEvent *) override;

  private:
	QRect pipRect() const;
	QImage m_image;
	QRectF m_rect;
	QPoint m_anchor;
	QRectF m_start;
	bool m_editable = false, m_dragging = false, m_resize = false;
};
#endif
