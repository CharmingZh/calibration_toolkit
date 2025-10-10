#pragma once
#include <QWidget>
#include <QImage>
#include <QMutex>
#include <QVector>
#include <QObject>


class ImageView : public QWidget {
	Q_OBJECT

public:
	explicit ImageView(QWidget* parent = nullptr);
	void setImage(const QImage& img);

	// ROI 交互
	QRect currentRoi() const { return m_roi; }

	void setGridOverlayEnabled(bool enabled);
	void setGridDimensions(int rows, int cols);
	void setGridHighlight(int row, int col);
	void setGridCellCounts(const QVector<QVector<int>> &counts, int maxPerCell);

Q_SIGNALS:
	void roiChanged(const QRect& r);

protected:
	void paintEvent(QPaintEvent*) override;
	void mousePressEvent(QMouseEvent*) override;
	void mouseMoveEvent(QMouseEvent*) override;
	void mouseReleaseEvent(QMouseEvent*) override;
	void resizeEvent(QResizeEvent*) override;

private:
	QImage m_image;
	mutable QMutex m_mtx;
	QRect m_roi;
	bool m_dragging = false;
	QPoint m_dragStart;
	bool m_gridOverlayEnabled { false };
	int m_gridRows { 0 };
	int m_gridCols { 0 };
	int m_gridHighlightRow { -1 };
	int m_gridHighlightCol { -1 };
	QVector<QVector<int>> m_gridCellCounts;
	int m_gridMaxPerCell { 0 };
};
