#include "camera/ImageView.h"

#include <QPainter>
#include <QMouseEvent>
#include <QLinearGradient>
#include <QPen>
#include <QSizePolicy>
#include <QFont>
#include <QMutexLocker>
#include <algorithm>


ImageView::ImageView(QWidget* parent) : QWidget(parent) {
	setMinimumSize(360, 240);
	setFocusPolicy(Qt::StrongFocus);
	setAttribute(Qt::WA_OpaquePaintEvent, true);
	setAttribute(Qt::WA_NoSystemBackground, true);
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}


void ImageView::setImage(const QImage& img) {
QMutexLocker lk(&m_mtx);
m_image = img;
update();
}


void ImageView::setGridOverlayEnabled(bool enabled)
{
	if (m_gridOverlayEnabled == enabled) {
		return;
	}
	m_gridOverlayEnabled = enabled;
	update();
}


void ImageView::setGridDimensions(int rows, int cols)
{
	rows = std::max(0, rows);
	cols = std::max(0, cols);
	if (m_gridRows == rows && m_gridCols == cols) {
		return;
	}
	m_gridRows = rows;
	m_gridCols = cols;
	if (m_gridRows <= 0 || m_gridCols <= 0) {
		m_gridCellCounts.clear();
	} else {
		m_gridCellCounts = QVector<QVector<int>>(m_gridRows, QVector<int>(m_gridCols, 0));
	}
	if (m_gridHighlightRow >= m_gridRows || m_gridHighlightCol >= m_gridCols) {
		m_gridHighlightRow = -1;
		m_gridHighlightCol = -1;
	}
	update();
}


void ImageView::setGridHighlight(int row, int col)
{
	if (row < 0 || col < 0 || row >= m_gridRows || col >= m_gridCols) {
		row = -1;
		col = -1;
	}
	if (m_gridHighlightRow == row && m_gridHighlightCol == col) {
		return;
	}
	m_gridHighlightRow = row;
	m_gridHighlightCol = col;
	update();
}


void ImageView::setGridCellCounts(const QVector<QVector<int>> &counts, int maxPerCell)
{
	m_gridMaxPerCell = std::max(0, maxPerCell);
	if (m_gridRows <= 0 || m_gridCols <= 0) {
		m_gridCellCounts.clear();
		update();
		return;
	}

	QVector<QVector<int>> sanitized(m_gridRows, QVector<int>(m_gridCols, 0));
	const int rows = std::min(m_gridRows, static_cast<int>(counts.size()));
	for (int r = 0; r < rows; ++r) {
		const QVector<int> &srcRow = counts.at(r);
		QVector<int> &dstRow = sanitized[r];
		const int cols = std::min(m_gridCols, static_cast<int>(srcRow.size()));
		for (int c = 0; c < cols; ++c) {
			dstRow[c] = std::max(0, srcRow.at(c));
		}
	}
	if (m_gridCellCounts == sanitized) {
		return;
	}
	m_gridCellCounts = sanitized;
	update();
}


void ImageView::paintEvent(QPaintEvent*) {
	QPainter p(this);

	QLinearGradient grad(rect().topLeft(), rect().bottomRight());
	grad.setColorAt(0.0, QColor(14, 19, 26));
	grad.setColorAt(1.0, QColor(11, 17, 24));
	p.fillRect(rect(), grad);

	QImage img;
	{
		QMutexLocker lk(&m_mtx);
		img = m_image;
	}

	if (!img.isNull()) {
		const QRect target = rect();
		const bool largeFrame = img.width() > 1920 || img.height() > 1920;
		const Qt::TransformationMode mode = largeFrame ? Qt::FastTransformation : Qt::SmoothTransformation;
		p.setRenderHint(QPainter::SmoothPixmapTransform, mode == Qt::SmoothTransformation);
		QImage scaled = img.scaled(target.size(), Qt::KeepAspectRatio, mode);
		const QPoint topLeft((width() - scaled.width()) / 2, (height() - scaled.height()) / 2);
		p.drawImage(topLeft, scaled);

		if (m_gridOverlayEnabled && m_gridRows > 0 && m_gridCols > 0) {
			p.save();
			p.setRenderHint(QPainter::Antialiasing, true);
			const QRectF imageRect(QPointF(topLeft), QSizeF(scaled.size()));
			const qreal cellWidth = imageRect.width() / static_cast<qreal>(m_gridCols);
			const qreal cellHeight = imageRect.height() / static_cast<qreal>(m_gridRows);

			if (m_gridHighlightRow >= 0 && m_gridHighlightCol >= 0) {
				const QRectF highlightRect(imageRect.left() + m_gridHighlightCol * cellWidth,
				                           imageRect.top() + m_gridHighlightRow * cellHeight,
				                           cellWidth,
				                           cellHeight);
				p.fillRect(highlightRect, QColor(80, 164, 255, 60));
				QPen highlightPen(QColor(80, 164, 255, 200));
				highlightPen.setWidthF(2.0);
				highlightPen.setJoinStyle(Qt::MiterJoin);
				p.setPen(highlightPen);
				p.drawRect(highlightRect);
			}

			QPen gridPen(QColor(168, 182, 210, 140));
			gridPen.setWidthF(1.2);
			p.setPen(gridPen);
			for (int c = 1; c < m_gridCols; ++c) {
				const qreal x = imageRect.left() + c * cellWidth;
				p.drawLine(QPointF(x, imageRect.top()), QPointF(x, imageRect.bottom()));
			}
			for (int r = 1; r < m_gridRows; ++r) {
				const qreal y = imageRect.top() + r * cellHeight;
				p.drawLine(QPointF(imageRect.left(), y), QPointF(imageRect.right(), y));
			}

			if (!m_gridCellCounts.isEmpty()) {
				QFont font = p.font();
				font.setPointSizeF(std::max(9.0, font.pointSizeF() - 1.0));
				font.setBold(true);
				p.setFont(font);
				for (int r = 0; r < m_gridRows; ++r) {
					if (r >= m_gridCellCounts.size()) {
						continue;
					}
					const QVector<int> &rowCounts = m_gridCellCounts.at(r);
					for (int c = 0; c < m_gridCols; ++c) {
						if (c >= rowCounts.size()) {
							continue;
						}
						const int count = rowCounts.at(c);
						const QRectF cellRect(imageRect.left() + c * cellWidth,
						                          imageRect.top() + r * cellHeight,
						                          cellWidth,
						                          cellHeight);
						QString label = QString::number(count);
						if (m_gridMaxPerCell > 0) {
							label = QStringLiteral("%1/%2").arg(count).arg(m_gridMaxPerCell);
						}
						p.setPen(QColor(238, 242, 255, 220));
						p.drawText(cellRect, Qt::AlignCenter, label);
					}
				}
			}
			p.restore();
		}

		if (!m_roi.isNull()) {
			QPen pen(QColor(85, 255, 192));
			pen.setWidth(2);
			pen.setStyle(Qt::DashLine);
			p.setPen(pen);
			p.drawRect(m_roi);
		}
	} else {
		p.setPen(QPen(QColor(95, 104, 122), 1, Qt::DashLine));
		const int w = width();
		const int h = height();
		for (int x = 24; x < w; x += 24) {
			p.drawLine(QPoint(x, 0), QPoint(x, h));
		}
		for (int y = 24; y < h; y += 24) {
			p.drawLine(QPoint(0, y), QPoint(w, y));
		}
		p.setPen(QPen(QColor(120, 130, 148)));
		p.drawText(rect(), Qt::AlignCenter, tr("等待图像帧..."));
	}
}


void ImageView::mousePressEvent(QMouseEvent* e) {
m_dragging = true; m_dragStart = e->pos(); m_roi = QRect(m_dragStart, QSize());
}


void ImageView::mouseMoveEvent(QMouseEvent* e) {
if (m_dragging) {
m_roi = QRect(m_dragStart, e->pos()).normalized();
update();
}
}


void ImageView::mouseReleaseEvent(QMouseEvent*) {
m_dragging = false;
Q_EMIT roiChanged(m_roi);
}


void ImageView::resizeEvent(QResizeEvent*) { update(); }
