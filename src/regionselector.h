// Region selection in physical desktop pixels, including mixed-DPI monitors.
#ifndef WPSRECORDER_REGIONSELECTOR_H
#define WPSRECORDER_REGIONSELECTOR_H
#include <QDialog>

class QLabel;
class QPushButton;

class KRRRegionSelector : public QDialog
{
public:
	explicit KRRRegionSelector(const QRect &initial = {}, QWidget *parent = nullptr);
	QRect selectedRegion() const;
	void accept() override;

protected:
	void paintEvent(QPaintEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;

private:
	enum class Drag { None, Draw, Move, Resize };
	bool hasSelection() const;
	QPoint bounded(const QPoint &point) const;
	Qt::Edges hitEdges(const QPoint &point) const;
	void updateCursor(const QPoint &point);
	void updateControls(bool positionPanel);
	void updateDrag(const QPoint &point);
	void invalidateSelection(const QRect &previous);

	QRect m_desktop, m_selection, m_start;
	QPoint m_anchor;
	Qt::Edges m_edges;
	Drag m_drag = Drag::None;
	QWidget *m_panel = nullptr;
	QLabel *m_dimensions = nullptr;
	QPushButton *m_confirm = nullptr;
};
#endif
