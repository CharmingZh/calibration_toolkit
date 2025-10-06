#pragma once

#include <functional>
#include <vector>

#include <QColor>
#include <QString>

#include "CalibrationEngine.h"

class QPainter;

namespace mycalib {

class PaperFigureExporter {
public:
    static void exportAll(const CalibrationOutput &output, const QString &outputDirectory);

private:
    // 扩展了可用的配色
    enum class ScalarColormap {
        Viridis,
        Turbo,
        Cividis,
        Plasma
    };

    static void exportScalarFieldFigure(const cv::Mat &field,
                                        double minValue,
                                        double maxValue,
                                        const QString &title,
                                        const QString &xLabel,
                                        const QString &yLabel,
                                        const QString &colorbarLabel,
                                        const QString &fileBasePath,
                                        const cv::Mat *vectorField = nullptr,
                                        const std::vector<std::vector<cv::Point2f>> *gridLines = nullptr,
                                        bool drawVectorField = true,
                                        bool drawGrid = false,
                                        ScalarColormap colormap = ScalarColormap::Viridis);
    static void exportResidualScatterFigure(const CalibrationOutput &output,
                                            const QString &fileBasePath);

    static void drawScalarField(QPainter &painter,
                                const cv::Mat &field,
                                double minValue,
                                double maxValue,
                                const QString &title,
                                const QString &xLabel,
                                const QString &yLabel,
                                const QString &colorbarLabel,
                                const cv::Mat *vectorField = nullptr,
                                const std::vector<std::vector<cv::Point2f>> *gridLines = nullptr,
                                bool drawVectorField = true,
                                bool drawGrid = false,
                                ScalarColormap colormap = ScalarColormap::Viridis);
    static void drawResidualScatter(QPainter &painter,
                                    const CalibrationOutput &output);

    // colormaps
    static QColor viridisColor(double t);
    static QColor turboColor(double t);
    static QColor cividisColor(double t);
    static QColor plasmaColor(double t);
    static QColor colorForMap(ScalarColormap map, double t);

    static QColor blendTowardsWhite(const QColor &color, double weight);
    static void writeSvgAndPng(const QString &fileBasePath,
                               const std::function<void(QPainter &)> &drawFunction);
};

} // namespace mycalib