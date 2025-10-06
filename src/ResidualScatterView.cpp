#include "ResidualScatterView.h"

#include <algorithm>
#include <array>
#include <cmath>

#include <QCursor>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QVector3D>
#include <QToolTip>
#include <QWheelEvent>

namespace mycalib {
namespace {

QColor viridisColor(float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    static const std::array<std::pair<float, QVector3D>, 6> stops = {
        std::pair<float, QVector3D>{0.0f, {68.0f, 1.0f, 84.0f}},
        {0.2f, {59.0f, 82.0f, 139.0f}},
        {0.4f, {33.0f, 145.0f, 140.0f}},
        {0.6f, {94.0f, 201.0f, 98.0f}},
        {0.8f, {253.0f, 231.0f, 37.0f}},
        {1.0f, {249.0f, 196.0f, 65.0f}},
    };

    for (size_t i = 1; i < stops.size(); ++i) {
        if (t <= stops[i].first) {
            const float span = stops[i].first - stops[i - 1].first;
            const float factor = span > 0.0f ? (t - stops[i - 1].first) / span : 0.0f;
            const QVector3D color = stops[i - 1].second + (stops[i].second - stops[i - 1].second) * factor;
            return QColor::fromRgbF(color.x() / 255.0f, color.y() / 255.0f, color.z() / 255.0f, 1.0f);
        }
    }
    const QVector3D last = stops.back().second;
    return QColor::fromRgbF(last.x() / 255.0f, last.y() / 255.0f, last.z() / 255.0f, 1.0f);
}

} // namespace

ResidualScatterView::ResidualScatterView(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
}

void ResidualScatterView::setSamples(std::vector<Sample> samples,
                                     float maxMagnitudePx,
                                     float maxMagnitudeMm)
{
    m_samples = std::move(samples);
    m_maxMagnitudePx = std::max(0.001f, maxMagnitudePx);
    m_maxMagnitudeMm = std::max(0.001f, maxMagnitudeMm);
    m_zoom = 1.0f;
    m_pan = QPointF(0.0, 0.0);
    recalcBounds();
    m_hoverIndex = -1;
    update();
}

void ResidualScatterView::clear()
{
    m_samples.clear();
    m_maxMagnitudePx = 1.0f;
    m_maxMagnitudeMm = 1.0f;
    m_baseRadius = 1.0f;
    m_zoom = 1.0f;
    m_pan = QPointF(0.0, 0.0);
    m_hoverIndex = -1;
    update();
}

void ResidualScatterView::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QRectF bounds = rect();
    QLinearGradient bg(bounds.topLeft(), bounds.bottomRight());
    bg.setColorAt(0.0, QColor(14, 18, 26));
    bg.setColorAt(1.0, QColor(8, 10, 18));
    painter.fillRect(bounds, bg);

    if (width() <= 0 || height() <= 0) {
        return;
    }

    const qreal leftMargin = 120.0;
    const qreal rightMargin = 180.0;
    const qreal topMargin = 80.0;
    const qreal bottomMargin = 90.0;
    QRectF plotRect(bounds.left() + leftMargin,
                    bounds.top() + topMargin,
                    std::max<qreal>(bounds.width() - leftMargin - rightMargin, 80.0),
                    std::max<qreal>(bounds.height() - topMargin - bottomMargin, 80.0));

    const float displayRadius = m_baseRadius > 0.0f ? m_baseRadius / m_zoom : 1.0f;
    const qreal scale = std::min(plotRect.width(), plotRect.height()) * 0.5 / std::max<qreal>(displayRadius, 1e-3);

    painter.save();
    painter.setClipRect(plotRect.adjusted(-1, -1, 1, 1));

    QRadialGradient focus(plotRect.center(), plotRect.width() * 0.65);
    focus.setColorAt(0.0, QColor(255, 255, 255, 22));
    focus.setColorAt(1.0, Qt::transparent);
    painter.fillRect(plotRect, focus);

    painter.setPen(QPen(QColor(70, 82, 112), 1.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(plotRect, 14, 14);

    painter.setPen(QPen(QColor(56, 68, 98), 1.2));
    const int gridSteps = 8;
    for (int i = -gridSteps; i <= gridSteps; ++i) {
        if (i == 0) {
            continue;
        }
        const qreal offset = static_cast<qreal>(i) / gridSteps;
        const qreal xVal = plotRect.center().x() + offset * plotRect.width() * 0.5;
        const qreal yVal = plotRect.center().y() + offset * plotRect.height() * 0.5;
        painter.drawLine(QPointF(xVal, plotRect.top()), QPointF(xVal, plotRect.bottom()));
        painter.drawLine(QPointF(plotRect.left(), yVal), QPointF(plotRect.right(), yVal));
    }

    painter.setPen(QPen(QColor(180, 196, 230), 1.4));
    painter.drawLine(QPointF(plotRect.left(), plotRect.center().y()), QPointF(plotRect.right(), plotRect.center().y()));
    painter.drawLine(QPointF(plotRect.center().x(), plotRect.top()), QPointF(plotRect.center().x(), plotRect.bottom()));

    const qreal pointRadiusBase = 5.0;
    int renderedPoints = 0;
    int hovered = m_hoverIndex;
    const int sampleCount = static_cast<int>(m_samples.size());
    for (int i = 0; i < sampleCount; ++i) {
        const auto &sample = m_samples[i];
        const QPointF screen = mapToScreen(sample.deltaPx - m_pan, scale, plotRect);
        if (!plotRect.contains(screen)) {
            continue;
        }
        const float normalized = std::clamp(sample.magnitudePx / m_maxMagnitudePx, 0.0f, 1.0f);
        QColor color = colorForMagnitude(sample.magnitudePx);
        if (i == hovered) {
            color = color.lighter(140);
        }
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        const qreal radius = pointRadiusBase * (0.75 + normalized * 0.6);
        painter.drawEllipse(screen, radius, radius);
        ++renderedPoints;
    }
    painter.restore();

    painter.setPen(QPen(QColor(200, 210, 235), 1));
    painter.setFont(QFont(painter.font().family(), painter.font().pointSizeF() + 0.5f, QFont::DemiBold));
    painter.drawText(QRectF(plotRect.left(), bounds.top() + 24, plotRect.width(), 24), Qt::AlignCenter,
                     tr("Reprojection Residual Scatter"));

    painter.setFont(QFont(painter.font().family(), painter.font().pointSizeF() - 0.5f));
    painter.drawText(QRectF(plotRect.left(), plotRect.bottom() + 16, plotRect.width(), 20), Qt::AlignCenter,
                     tr("Δx (px)"));
    painter.save();
    painter.translate(bounds.left() + 46, plotRect.center().y());
    painter.rotate(-90);
    painter.drawText(QRectF(-plotRect.height() * 0.5, -16, plotRect.height(), 20), Qt::AlignCenter, tr("Δy (px)"));
    painter.restore();

    // Colorbars
    const qreal barWidth = 44.0;
    const qreal barGap = 26.0;
    const QRectF pxBar(plotRect.left() - barGap - barWidth, plotRect.top(), barWidth, plotRect.height());
    const QRectF mmBar(plotRect.right() + barGap, plotRect.top(), barWidth, plotRect.height());

    auto drawBar = [&](const QRectF &rect, float maxValue, const QString &unitLabel, bool ticksOnRight) {
        painter.setPen(Qt::NoPen);
        const int height = static_cast<int>(rect.height());
        for (int y = 0; y < height; ++y) {
            const float t = 1.0f - static_cast<float>(y) / std::max(1, height - 1);
            painter.setBrush(viridisColor(t));
            painter.drawRect(QRectF(rect.left(), rect.top() + y, rect.width(), 1.0));
        }
    painter.setPen(QPen(QColor(210, 220, 238), 1.1));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(rect.adjusted(0, 0, -1, -1), 6, 6);

        QFont tickFont = painter.font();
        tickFont.setPointSizeF(tickFont.pointSizeF() - 0.5f);
        painter.setFont(tickFont);

        const int tickCount = 4;
        for (int i = 0; i < tickCount; ++i) {
            const float t = static_cast<float>(i) / (tickCount - 1);
            const qreal y = rect.bottom() - t * rect.height();
            const qreal tickStart = ticksOnRight ? rect.right() + 2.0 : rect.left() - 2.0;
            const qreal tickEnd = ticksOnRight ? rect.right() + 8.0 : rect.left() - 8.0;
            painter.drawLine(QPointF(tickStart, y), QPointF(tickEnd, y));

            const float value = maxValue * t;
            QString label = QString::number(value, 'f', value >= 10.0f ? 1 : 2);
            if (!unitLabel.isEmpty()) {
                label += QStringLiteral(" %1").arg(unitLabel);
            }
            const qreal labelX = ticksOnRight ? rect.right() + 10.0 : rect.left() - 74.0;
            QRectF labelRect(labelX, y - 8.0, 70.0, 16.0);
            const Qt::Alignment align = ticksOnRight ? (Qt::AlignLeft | Qt::AlignVCenter)
                                                     : (Qt::AlignRight | Qt::AlignVCenter);
            painter.drawText(labelRect, align, label);
        }
    };

    painter.save();
    painter.setClipRect(pxBar.adjusted(-4, -4, 4, 4));
    drawBar(pxBar, m_maxMagnitudePx, tr("px"), false);
    painter.restore();

    painter.save();
    painter.setClipRect(mmBar.adjusted(-4, -4, 4, 4));
    drawBar(mmBar, m_maxMagnitudeMm, tr("mm"), true);
    painter.restore();

    painter.setPen(QPen(QColor(168, 182, 214), 1));
    painter.setFont(QFont(painter.font().family(), painter.font().pointSizeF() - 0.3f, QFont::DemiBold));
    painter.drawText(QRectF(pxBar.left(), pxBar.top() - 22, pxBar.width(), 18), Qt::AlignCenter, tr("|Δ|"));
    painter.drawText(QRectF(mmBar.left(), mmBar.top() - 22, mmBar.width(), 18), Qt::AlignCenter, tr("|Δ|"));
    painter.drawText(QRectF(pxBar.left() - 4, pxBar.bottom() + 8, pxBar.width() + 8, 18), Qt::AlignCenter, tr("Pixels"));
    painter.drawText(QRectF(mmBar.left() - 4, mmBar.bottom() + 8, mmBar.width() + 8, 18), Qt::AlignCenter, tr("Millimeters"));

    if (hovered >= 0 && hovered < sampleCount) {
        const auto &sample = m_samples[hovered];
        const QPointF screen = mapToScreen(sample.deltaPx - m_pan, scale, plotRect);
        painter.setPen(QPen(QColor(255, 234, 120), 1.6));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(screen, 10.0, 10.0);

        QString info = tr("Δx: %1 px  Δy: %2 px\n|Δ|: %3 px  (%4 mm)")
                            .arg(sample.deltaPx.x(), 0, 'f', 3)
                            .arg(sample.deltaPx.y(), 0, 'f', 3)
                            .arg(sample.magnitudePx, 0, 'f', 3)
                            .arg(sample.magnitudeMm, 0, 'f', 3);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(12, 14, 22, 230));
        QRectF bubble(screen.x() + 14, screen.y() - 10, 210, 56);
        painter.drawRoundedRect(bubble, 8, 8);
        painter.setPen(QColor(235, 240, 250));
        painter.drawText(bubble.adjusted(12, 8, -12, -8), Qt::AlignLeft | Qt::AlignTop, info);
    }

    painter.setPen(QColor(140, 156, 188));
    painter.drawText(QRectF(plotRect.left(), bounds.bottom() - 28, plotRect.width(), 18), Qt::AlignCenter,
                     tr("Points: %1  Shown: %2").arg(m_samples.size()).arg(renderedPoints));
}

void ResidualScatterView::wheelEvent(QWheelEvent *event)
{
    if (event->angleDelta().y() == 0) {
        QWidget::wheelEvent(event);
        return;
    }
    const float factor = std::pow(1.15f, event->angleDelta().y() / 120.0f);
    m_zoom = std::clamp(m_zoom * factor, 0.4f, 12.0f);
    update();
}

void ResidualScatterView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_dragging = true;
        m_lastMousePos = event->pos();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void ResidualScatterView::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragging) {
        const QPoint delta = event->pos() - m_lastMousePos;
        m_lastMousePos = event->pos();
        const float displayRadius = m_baseRadius > 0.0f ? m_baseRadius / m_zoom : 1.0f;
        const qreal scale = std::min(width(), height()) * 0.5 / std::max<qreal>(displayRadius, 1e-3);
        m_pan.setX(m_pan.x() - static_cast<double>(delta.x()) / scale);
        m_pan.setY(m_pan.y() + static_cast<double>(delta.y()) / scale);
        update();
        event->accept();
        return;
    }
    updateHover(event->pos());
    QWidget::mouseMoveEvent(event);
}

void ResidualScatterView::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_dragging) {
        m_dragging = false;
        setCursor(Qt::ArrowCursor);
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void ResidualScatterView::leaveEvent(QEvent *event)
{
    Q_UNUSED(event);
    if (!m_dragging && m_hoverIndex != -1) {
        m_hoverIndex = -1;
        update();
    }
}

QColor ResidualScatterView::colorForMagnitude(float valuePx) const
{
    const float normalized = m_maxMagnitudePx > 0.0f ? valuePx / m_maxMagnitudePx : 0.0f;
    return viridisColor(std::clamp(normalized, 0.0f, 1.0f));
}

QPointF ResidualScatterView::mapToScreen(const QPointF &deltaPx,
                                         qreal scale,
                                         const QRectF &plotRect) const
{
    return QPointF(plotRect.center().x() + deltaPx.x() * scale,
                   plotRect.center().y() - deltaPx.y() * scale);
}

void ResidualScatterView::recalcBounds()
{
    double maxComponent = 1.0;
    for (const auto &sample : m_samples) {
        maxComponent = std::max<double>(maxComponent, std::fabs(sample.deltaPx.x()));
        maxComponent = std::max<double>(maxComponent, std::fabs(sample.deltaPx.y()));
    }
    m_baseRadius = static_cast<float>(maxComponent * 1.2);
}

void ResidualScatterView::updateHover(const QPoint &cursorPos)
{
    const qreal margin = 80.0;
    QRectF plotRect = rect().adjusted(margin, margin, -margin * 2.4, -margin);
    if (plotRect.width() < 50 || plotRect.height() < 50) {
        plotRect = rect().adjusted(40, 40, -140, -40);
    }
    const float displayRadius = m_baseRadius > 0.0f ? m_baseRadius / m_zoom : 1.0f;
    const qreal scale = std::min(plotRect.width(), plotRect.height()) * 0.5 / std::max<qreal>(displayRadius, 1e-3);

    const qreal hoverThreshold = 9.0;
    int closest = -1;
    qreal bestDist = hoverThreshold * hoverThreshold;
    const int sampleCount = static_cast<int>(m_samples.size());
    for (int i = 0; i < sampleCount; ++i) {
        const auto &sample = m_samples[i];
        const QPointF screen = mapToScreen(sample.deltaPx - m_pan, scale, plotRect);
        const qreal dx = screen.x() - cursorPos.x();
        const qreal dy = screen.y() - cursorPos.y();
        const qreal dist2 = dx * dx + dy * dy;
        if (dist2 < bestDist) {
            bestDist = dist2;
            closest = i;
        }
    }
    if (closest != m_hoverIndex) {
        m_hoverIndex = closest;
        update();
    }
}

} // namespace mycalib
