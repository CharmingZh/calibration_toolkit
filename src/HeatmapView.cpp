#include "HeatmapView.h"

#include <algorithm>
#include <cmath>

#include <QPainter>
#include <QPaintEvent>

namespace mycalib {

HeatmapView::HeatmapView(QWidget *parent)
    : QFrame(parent)
{
    setFrameShape(QFrame::StyledPanel);
    setFrameShadow(QFrame::Raised);
    setMinimumHeight(240);
}

void HeatmapView::setTitle(const QString &title)
{
    m_title = title;
    update();
}

void HeatmapView::setHeatmap(const QImage &image, double minValue, double maxValue, const QString &legendLabel)
{
    m_heatmap = image;
    m_minValue = minValue;
    m_maxValue = maxValue;
    m_legendLabel = legendLabel;
    m_showLegend = !legendLabel.trimmed().isEmpty();
    m_warpedGridLines.clear();
    update();
}

void HeatmapView::clear()
{
    m_heatmap = QImage();
    m_legendLabel.clear();
    m_showLegend = false;
    m_warpedGridLines.clear();
    update();
}

void HeatmapView::setLegendUnit(const QString &unit)
{
    m_legendUnit = unit;
    update();
}

void HeatmapView::setLegendTickCount(int tickCount)
{
    m_tickCount = std::clamp(tickCount, 2, 8);
    update();
}

void HeatmapView::setLegendPrecision(int decimals)
{
    m_valuePrecision = std::clamp(decimals, 0, 5);
    update();
}

void HeatmapView::setGridOverlayEnabled(bool enabled)
{
    m_drawGrid = enabled;
    update();
}

void HeatmapView::setWarpedGridLines(const QVector<QPolygonF> &lines)
{
    m_warpedGridLines = lines;
    update();
}

void HeatmapView::paintEvent(QPaintEvent *event)
{
    QFrame::paintEvent(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const QRect content = rect().adjusted(8, 8, -8, -8);

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(32, 36, 53));
    painter.drawRoundedRect(content, 12, 12);

    QRect titleRect = content.adjusted(16, 12, -16, -content.height() + 44);
    QFont titleFont = painter.font();
    titleFont.setPointSizeF(titleFont.pointSizeF() + 1.5);
    titleFont.setBold(true);
    painter.setFont(titleFont);
    painter.setPen(QColor(202, 220, 255));
    painter.drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter, m_title);

    if (!m_heatmap.isNull()) {
        const int legendAreaHeight = m_showLegend ? 82 : 24;
        QRect imageRect = content.adjusted(16, 56, -16, -legendAreaHeight);
        QImage scaled = m_heatmap.scaled(imageRect.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        QRect target(imageRect);
        target.setSize(scaled.size());
        target.moveCenter(imageRect.center());
        painter.drawImage(target.topLeft(), scaled);

        if (m_drawGrid) {
            painter.save();
            painter.setClipRect(target, Qt::IntersectClip);
            painter.setRenderHint(QPainter::Antialiasing, true);
            QPen gridPen(QColor(255, 255, 255, 45), 1.1, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
            painter.setPen(gridPen);

            if (!m_warpedGridLines.isEmpty() && !m_heatmap.isNull() && m_heatmap.width() > 0 && m_heatmap.height() > 0) {
                const qreal scaleX = static_cast<qreal>(target.width()) / static_cast<qreal>(m_heatmap.width());
                const qreal scaleY = static_cast<qreal>(target.height()) / static_cast<qreal>(m_heatmap.height());

                for (const auto &poly : m_warpedGridLines) {
                    if (poly.size() < 2) {
                        continue;
                    }
                    QPolygonF mapped;
                    mapped.reserve(poly.size());
                    for (const QPointF &pt : poly) {
                        if (!std::isfinite(pt.x()) || !std::isfinite(pt.y())) {
                            if (mapped.size() >= 2) {
                                painter.drawPolyline(mapped);
                            }
                            mapped.clear();
                            continue;
                        }
                        const qreal x = target.left() + pt.x() * scaleX;
                        const qreal y = target.top() + pt.y() * scaleY;
                        mapped.append(QPointF(x, y));
                    }
                    if (mapped.size() >= 2) {
                        painter.drawPolyline(mapped);
                    }
                }
            } else {
                const int gridLines = 12;
                for (int i = 1; i < gridLines; ++i) {
                    const qreal t = static_cast<qreal>(i) / gridLines;
                    const qreal x = target.left() + t * target.width();
                    const qreal y = target.top() + t * target.height();
                    painter.drawLine(QPointF(x, target.top()), QPointF(x, target.bottom()));
                    painter.drawLine(QPointF(target.left(), y), QPointF(target.right(), y));
                }
            }
            painter.restore();
        }

        if (m_showLegend) {
            QRect legendArea(content.left() + 32, content.bottom() - 70, content.width() - 64, 58);
            QRect labelRect = QRect(legendArea.left(), legendArea.top(), legendArea.width(), 18);
            painter.setPen(QColor(185, 198, 222));
            painter.setFont(QFont(painter.font().family(), painter.font().pointSize(), QFont::DemiBold));
            painter.drawText(labelRect, Qt::AlignCenter, m_legendLabel);

            QRect barRect = QRect(legendArea.left(), legendArea.top() + 22, legendArea.width(), 18);
            QLinearGradient gradient(barRect.topLeft(), barRect.topRight());
            gradient.setColorAt(0.0, QColor(37, 65, 178));
            gradient.setColorAt(0.25, QColor(29, 143, 225));
            gradient.setColorAt(0.5, QColor(98, 197, 105));
            gradient.setColorAt(0.75, QColor(255, 204, 72));
            gradient.setColorAt(1.0, QColor(220, 60, 52));
            painter.setBrush(gradient);
            painter.setPen(Qt::NoPen);
            painter.drawRoundedRect(barRect, 4, 4);
            painter.setPen(QPen(QColor(210, 220, 238), 1.2));
            painter.drawRoundedRect(barRect.adjusted(0, 0, -1, -1), 4, 4);

            const int ticks = std::max(2, m_tickCount);
            painter.setPen(QPen(QColor(210, 220, 238), 1));
            const qreal barYBottom = barRect.bottom();
            const qreal width = static_cast<qreal>(barRect.width());
            const double delta = m_maxValue - m_minValue;
            QRect tickLabelRect = QRect(barRect.left(), barRect.bottom() + 6, barRect.width(), 18);
            painter.setFont(QFont(painter.font().family(), painter.font().pointSize() - 1));

            for (int i = 0; i < ticks; ++i) {
                const qreal t = ticks == 1 ? 0.0 : static_cast<qreal>(i) / (ticks - 1);
                const qreal x = barRect.left() + t * width;
                painter.drawLine(QPointF(x, barYBottom + 1), QPointF(x, barYBottom + 6));

                const double value = m_minValue + delta * static_cast<double>(t);
                QString text = formatValue(value);
                if (!m_legendUnit.isEmpty()) {
                    text += QStringLiteral(" %1").arg(m_legendUnit);
                }

                QRectF label;
                if (i == 0) {
                    label = QRectF(barRect.left() - 4, barRect.bottom() + 6, 80, 18);
                    painter.drawText(label, Qt::AlignLeft | Qt::AlignVCenter, text);
                } else if (i == ticks - 1) {
                    label = QRectF(barRect.right() - 76, barRect.bottom() + 6, 80, 18);
                    painter.drawText(label, Qt::AlignRight | Qt::AlignVCenter, text);
                } else {
                    label = QRectF(x - 40, barRect.bottom() + 6, 80, 18);
                    painter.drawText(label, Qt::AlignHCenter | Qt::AlignVCenter, text);
                }
            }
        }
    } else {
        painter.setPen(QColor(120, 130, 160));
        painter.drawText(content, Qt::AlignCenter, tr("Heatmap will appear here"));
    }
}

QString HeatmapView::formatValue(double value) const
{
    return QString::number(value, 'f', m_valuePrecision);
}

} // namespace mycalib
