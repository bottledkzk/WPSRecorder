#ifndef WPSRECORDER_ANNOTATIONOVERLAY_H
#define WPSRECORDER_ANNOTATIONOVERLAY_H
#include "recordingconfig.h"
#include <QJsonArray>
#include <QTimer>
#include <QWidget>
#include <memory>
class QToolBar;
class QToolButton;
class QAction;
class QComboBox;
class QLineEdit;
class QLabel;
class KRRLiveCapturePreview;
namespace RecorderUi { struct ToolbarTheme; }
class KRRAnnotationOverlay : public QWidget
{
	Q_OBJECT
  public:
	explicit KRRAnnotationOverlay();
	void setTheme(const RecorderUi::ToolbarTheme& theme);
	~KRRAnnotationOverlay() override;
	bool present(const QRect &area, const QJsonObject &effects, QString &error);
	QJsonObject effects() const
	{
		return m_effects;
	}
	void toggleInput();
	void dismiss();
  signals:
	void effectsChanged(const QJsonObject &effects);
	void activeChanged(bool active);

  protected:
	void paintEvent(QPaintEvent *) override;
	void mousePressEvent(QMouseEvent *) override;
	void mouseMoveEvent(QMouseEvent *) override;
	void mouseReleaseEvent(QMouseEvent *) override;
	bool eventFilter(QObject *object, QEvent *event) override;
	bool nativeEvent(const QByteArray &, void *message, long *result) override;

  private:
	void updateToolbarHole();
	void adjustToolbar();
	void updateBackgroundCapture();
	void revealTools();
	void setCompact(bool compact);
	QPointF normalized(const QPoint &point) const;
	void publish();
	void setTool(const QString &tool);
	void updateToolOptions();
	void startText(const QPoint &point);
	void finishText(bool accept);
	void updateStroke(const QPoint &point);
	bool appendCurrent(QJsonObject &effects) const;
	void publishStroke();
	void updatePointerTimer();
	void checkpoint();
	void undo(bool redo = false);
	void eraseTo(const QPoint &point);
	QJsonObject m_effects;
	QJsonObject m_current;
	QTimer m_backgroundTimer, m_pointerTimer;
	std::unique_ptr<KRRLiveCapturePreview> m_capture;
	QImage m_background, m_layer;
	QRect m_projection;
	QRegion m_desktopRegion;
	QPoint m_pointer;
	qint64 m_backgroundSequence = 0;
	QString m_tool = "pen";
	QString m_color = "#e9424c";
	QString m_highlightColor = "#ffe16f";
	bool m_drawing = false;
	bool m_input = true;
	QWidget* m_toolbarWindow = nullptr;
	QToolBar *m_toolbar = nullptr, *m_secondary = nullptr;
	QLabel* m_toolbarGrip = nullptr;
	QToolButton* m_colorButton = nullptr;
	QPoint m_toolbarDragOffset;
	bool m_draggingToolbar = false;
	QComboBox *m_widthChoice = nullptr, *m_textSizeChoice = nullptr, *m_highlightWidthChoice = nullptr;
	QComboBox *m_blurWidthChoice = nullptr, *m_blurStrengthChoice = nullptr;
	QAction *m_expandAction = nullptr;
	QAction *m_undoAction = nullptr, *m_redoAction = nullptr, *m_clearAction = nullptr;
	QLineEdit *m_textInput = nullptr;
	bool m_compact = false;
	int m_width = 3, m_textSize = 24, m_highlightWidth = 20;
	int m_clickStyle = 0;
	bool m_leftDown = false;
	int m_blurWidth = 48, m_blurStrength = 18;
	QVector<QJsonObject> m_undo, m_redo;
	QPoint m_erasePoint;
	bool m_erasing = false, m_eraseCheckpoint = false;
};
#endif
