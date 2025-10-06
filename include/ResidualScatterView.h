#pragma once

#include <QPointF>
#include <QWidget>

#include <vector>

namespace mycalib {

class ResidualScatterView : public QWidget {
    Q_OBJECT

public:
    struct Sample {
        QPointF deltaPx {0.0, 0.0};
        float magnitudePx {0.0f};
        float magnitudeMm {0.0f};
    };

    explicit ResidualScatterView(QWidget *parent = nullptr);

    void setSamples(std::vector<Sample> samples, float maxMagnitudePx, float maxMagnitudeMm);
    void clear();

protected:
    void paintEvent(QPaintEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    QColor colorForMagnitude(float valuePx) const;
    QPointF mapToScreen(const QPointF &deltaPx, qreal scale, const QRectF &plotRect) const;
    void recalcBounds();
    void updateHover(const QPoint &cursorPos);

    std::vector<Sample> m_samples;
    float m_maxMagnitudePx {1.0f};
    float m_maxMagnitudeMm {1.0f};
    float m_baseRadius {1.0f};
    float m_zoom {1.0f};
    QPointF m_pan {0.0, 0.0};
    QPoint m_lastMousePos;
    bool m_dragging {false};
    int m_hoverIndex {-1};
};

} // namespace mycalib
