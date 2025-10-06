#pragma once

#include <QFrame>
#include <QImage>
#include <QPolygonF>
#include <QVector>

namespace mycalib {

class HeatmapView : public QFrame {
    Q_OBJECT

public:
    explicit HeatmapView(QWidget *parent = nullptr);

    void setTitle(const QString &title);
    void setHeatmap(const QImage &image, double minValue, double maxValue, const QString &legendLabel = QString());
    void clear();
    void setLegendUnit(const QString &unit);
    void setLegendTickCount(int tickCount);
    void setLegendPrecision(int decimals);
    void setGridOverlayEnabled(bool enabled);
    void setWarpedGridLines(const QVector<QPolygonF> &lines);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QString formatValue(double value) const;

    QString m_title;
    QImage m_heatmap;
    double m_minValue {0.0};
    double m_maxValue {1.0};
    QString m_legendLabel;
    bool m_showLegend {true};
    QString m_legendUnit;
    int m_tickCount {3};
    int m_valuePrecision {2};
    bool m_drawGrid {false};
    QVector<QPolygonF> m_warpedGridLines;
};

} // namespace mycalib
