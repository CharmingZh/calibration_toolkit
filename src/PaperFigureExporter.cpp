#include "PaperFigureExporter.h"

#include <QDir>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QSvgGenerator>
#include <QtMath>

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <numbers>
#include <vector>

#include <opencv2/imgproc.hpp>

namespace mycalib {
namespace {

// ===== Canvas & safety bleed =====
constexpr int kCanvasWidth  = 1920;
constexpr int kCanvasHeight = 1350;
constexpr int kBleedPx      = 12;

// Adaptive scalar field render cap
constexpr int kRenderMin = 512;
constexpr int kRenderMax = 2048;

// base grid alpha / line widths
constexpr double kFrameLineW = 1.2;

class PainterStateGuard {
public:
    explicit PainterStateGuard(QPainter &painter) : p_(painter) { p_.save(); }
    ~PainterStateGuard() { p_.restore(); }
private:
    QPainter &p_;
};

QColor interpolate(const QColor &a, const QColor &b, double t) {
    const double u = std::clamp(t, 0.0, 1.0);
    auto lerp = [&](int ca, int cb){ return int(std::round(ca + (cb - ca) * u)); };
    return QColor(lerp(a.red(), b.red()), lerp(a.green(), b.green()), lerp(a.blue(), b.blue()));
}

// ---------- Dynamic layout ----------
struct CanvasLayout {
    QRectF canvas;     // with bleed
    QRectF safe;       // without bleed
    QRectF plot;       // aspect-correct drawing rect
    QRectF cbarRight;  // right colorbar area (may be null if not used)
    double tickSizePx;
    double rightPad;
    double leftPad;
};

double percentile(std::vector<double> &values, double q)
{
    if (values.empty()) return 0.0;
    const double clampedQ = std::clamp(q, 0.0, 1.0);
    const size_t idx = static_cast<size_t>(std::round(clampedQ * (values.size() - 1)));
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(idx), values.end());
    return values[idx];
}

CanvasLayout computeLayout(const QSize &baseCanvas,
                           const QStringList & /*xTicks*/,
                           const QStringList &yTicks,
                           double dataAspect,
                           const QFont &axisFont,
                           const QFont &tickFont,
                           double rightExtraPad = 0.0,
                           double leftExtraPad  = 0.0)
{
    CanvasLayout L;
    L.canvas = QRectF(0, 0, baseCanvas.width() + 2*kBleedPx, baseCanvas.height() + 2*kBleedPx);
    L.safe   = L.canvas.adjusted(kBleedPx, kBleedPx, -kBleedPx, -kBleedPx);
    L.tickSizePx = 12.0;
    L.rightPad = rightExtraPad;
    L.leftPad  = leftExtraPad;

    QFontMetricsF axisFM(axisFont), tickFM(tickFont);

    const double yTitleW = axisFM.height() * 1.05;
    auto maxTextWidth = [&](const QStringList &lst){ double m=0; for (auto &s: lst) m = std::max(m, tickFM.horizontalAdvance(s)); return m; };
    const double maxYTickW = maxTextWidth(yTicks);
    const double maxXTickH = tickFM.height();

    const double marginLeft   = 14.0 + yTitleW + 6.0 + maxYTickW + 4.0 + L.tickSizePx + L.leftPad;
    const double marginRight  = 12.0 + L.tickSizePx + L.rightPad;
    const double marginTop    = 28.0;
    const double marginBottom = 30.0 + maxXTickH + 6.0 + axisFM.height();

    QRectF avail = L.safe.adjusted(marginLeft, marginTop, -marginRight, -marginBottom);

    // Keep aspect ratio
    double availW = avail.width(), availH = avail.height();
    double plotW = availW, plotH = availW / std::max(1e-12, dataAspect);
    if (plotH > availH) { plotH = availH; plotW = plotH * dataAspect; }
    const double plotX = avail.left() + (availW - plotW)*0.5;
    const double plotY = avail.top()  + (availH - plotH)*0.5;

    L.plot = QRectF(plotX, plotY, plotW, plotH);
    L.cbarRight = QRectF();

    return L;
}

} // namespace

// ===================== Export All =====================
void PaperFigureExporter::exportAll(const CalibrationOutput &output, const QString &outputDirectory)
{
    QDir baseDir(outputDirectory);
    const QString paperDirPath = baseDir.absoluteFilePath(QStringLiteral("paper_figures"));
    QDir paperDir(paperDirPath);
    if (!paperDir.exists() && !paperDir.mkpath(QStringLiteral("."))) return;

    const auto exportScalar = [&](const cv::Mat &field,
                                  double minValue, double maxValue,
                                  const QString &fileStem,
                                  const QString &colorbarLabel,
                                  const cv::Mat *vectorField = nullptr,
                                  const std::vector<std::vector<cv::Point2f>> *gridLines = nullptr,
                                  bool drawVector = true,
                                  bool drawGrid   = false,
                                  ScalarColormap map = ScalarColormap::Viridis,
                                  const QString &xLabel = QStringLiteral("Image X (px)"),
                                  const QString &yLabel = QStringLiteral("Image Y (px)")) {
        exportScalarFieldFigure(field, minValue, maxValue,
                                QString(), xLabel, yLabel, colorbarLabel,
                                paperDir.filePath(fileStem),
                                vectorField, gridLines, drawVector, drawGrid, map);
    };

    if (!output.heatmaps.boardCoverageScalar.empty()) {
        exportScalar(output.heatmaps.boardCoverageScalar,
                     output.heatmaps.boardCoverageMin,
                     output.heatmaps.boardCoverageMax,
                     QStringLiteral("board_coverage_ratio"),
                     QStringLiteral("Detection probability"),
                     nullptr, nullptr, false, false, ScalarColormap::Turbo);
    }

    if (!output.heatmaps.pixelErrorScalar.empty()) {
        exportScalar(output.heatmaps.pixelErrorScalar,
                     output.heatmaps.pixelErrorMin,
                     output.heatmaps.pixelErrorMax,
                     QStringLiteral("reprojection_error_pixels"),
                     QStringLiteral("Mean |Δ| (px)"),
                     nullptr, nullptr, false, false, ScalarColormap::Turbo);
    }

    if (!output.heatmaps.boardErrorScalar.empty()) {
        exportScalar(output.heatmaps.boardErrorScalar,
                     output.heatmaps.boardErrorMin,
                     output.heatmaps.boardErrorMax,
                     QStringLiteral("board_plane_error_mm"),
                     QStringLiteral("Mean board-plane error (mm)"),
                     nullptr, nullptr, false, false, ScalarColormap::Viridis);
    }

    if (!output.heatmaps.distortionScalar.empty()) {
        exportScalar(output.heatmaps.distortionScalar,
                     output.heatmaps.distortionMin,
                     output.heatmaps.distortionMax,
                     QStringLiteral("distortion_magnitude"),
                     QStringLiteral("Radial drift (px)"),
                     nullptr, nullptr, false, false, ScalarColormap::Viridis);

        exportScalar(output.heatmaps.distortionScalar,
                     output.heatmaps.distortionMin,
                     output.heatmaps.distortionMax,
                     QStringLiteral("distortion_vector_overlay"),
                     QStringLiteral("Radial drift (px)"),
                     &output.heatmaps.distortionVectors,
                     &output.heatmaps.distortionGrid,
                     true, true, ScalarColormap::Viridis);

        exportScalar(output.heatmaps.distortionScalar,
                     output.heatmaps.distortionMin,
                     output.heatmaps.distortionMax,
                     QStringLiteral("distortion_grid_only"),
                     QStringLiteral("Radial drift (px)"),
                     nullptr, &output.heatmaps.distortionGrid,
                     false, true, ScalarColormap::Viridis);
    }

    exportResidualScatterFigure(output, paperDir.filePath(QStringLiteral("reprojection_residual_scatter")));
}

// ===================== Scalar Field Figure =====================
void PaperFigureExporter::exportScalarFieldFigure(const cv::Mat &field,
                                                  double minValue,
                                                  double maxValue,
                                                  const QString &title,
                                                  const QString &xLabel,
                                                  const QString &yLabel,
                                                  const QString &colorbarLabel,
                                                  const QString &fileBasePath,
                                                  const cv::Mat *vectorField,
                                                  const std::vector<std::vector<cv::Point2f>> *gridLines,
                                                  bool drawVectorField,
                                                  bool drawGrid,
                                                  ScalarColormap colormap)
{
    if (field.empty()) return;

    cv::Mat fieldDouble = (field.type() == CV_64F) ? field : [&]{
        cv::Mat tmp; field.convertTo(tmp, CV_64F); return tmp; }();

    const auto draw = [&](QPainter &painter) {
        drawScalarField(painter, fieldDouble, minValue, maxValue,
                        title, xLabel, yLabel, colorbarLabel,
                        vectorField, gridLines, drawVectorField, drawGrid, colormap);
    };
    writeSvgAndPng(fileBasePath, draw);
}

// ===================== Residual Scatter Figure =====================
void PaperFigureExporter::exportResidualScatterFigure(const CalibrationOutput &output,
                                                      const QString &fileBasePath)
{
    const auto draw = [&](QPainter &painter) { drawResidualScatter(painter, output); };
    writeSvgAndPng(fileBasePath, draw);
}

// ===================== Draw Scalar Field =====================
void PaperFigureExporter::drawScalarField(QPainter &painter,
                                          const cv::Mat &field,
                                          double minValue,
                                          double maxValue,
                                          const QString &title,
                                          const QString &xLabel,
                                          const QString &yLabel,
                                          const QString &colorbarLabel,
                                          const cv::Mat *vectorField,
                                          const std::vector<std::vector<cv::Point2f>> *gridLines,
                                          bool drawVectorField,
                                          bool drawGrid,
                                          ScalarColormap colormap)
{
    Q_UNUSED(title);

    painter.fillRect(QRect(0, 0, kCanvasWidth + 2*kBleedPx, kCanvasHeight + 2*kBleedPx), Qt::white);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    QFont axisFont("Times New Roman", 52, QFont::Bold);
    QFont tickFont("Times New Roman", 38);
    QFontMetricsF axisFM(axisFont), tickFM(tickFont);

    // Ticks for layout
    const int tickCount = 6;
    QStringList xTicks, yTicks, cbarTicks;
    for (int i = 0; i < tickCount; ++i) {
        xTicks << QString::number(double(i) * double(field.cols) / double(tickCount-1), 'f', 0);
        yTicks << QString::number(double(i) * double(field.rows) / double(tickCount-1), 'f', 0);
        const double v = minValue + (maxValue - minValue) * (double(i)/(tickCount-1));
        cbarTicks << QString::number(v, 'f', (maxValue - minValue > 1.0) ? 2 : 3);
    }

    // 右侧色条宽度与右边距预算
    double cbarTickW = 0.0; for (auto &t : cbarTicks) cbarTickW = std::max(cbarTickW, tickFM.horizontalAdvance(t));
    const double cbarCoreW = std::max(54.0, cbarTickW + 18.0);
    const double cbarGap   = 128.0;  // 与图的固定间距
    const double rightPad  = cbarGap + cbarCoreW + 18.0;

    // Aspect
    const double aspect = (field.cols>0 && field.rows>0) ? double(field.cols)/double(field.rows) : 1.0;

    // Layout
    CanvasLayout L = computeLayout(QSize(kCanvasWidth, kCanvasHeight),
                                   xTicks, yTicks, aspect, axisFont, tickFont,
                                   rightPad, 0.0);

    // 色条矩形（用于画梯度）
    L.cbarRight = QRectF(L.plot.right() + cbarGap, L.plot.top(), 46.0, L.plot.height());

    // ===== Render scalar field =====
    const int renderW = std::clamp(int(std::round(L.plot.width())),  kRenderMin, kRenderMax);
    const int renderH = std::clamp(int(std::round(L.plot.height())), kRenderMin, kRenderMax);
    cv::Mat resized;
    cv::resize(field, resized, cv::Size(renderW, renderH), 0, 0, cv::INTER_LANCZOS4);
    if (renderW >= 1024 && renderH >= 1024) {
        cv::GaussianBlur(resized, resized, cv::Size(3,3), 0.4);
    }

    QImage heat(resized.cols, resized.rows, QImage::Format_RGB32);
    const double range = std::max(maxValue - minValue, 1e-12);
    for (int y = 0; y < resized.rows; ++y) {
        const double *rowPtr = resized.ptr<double>(y);
        for (int x = 0; x < resized.cols; ++x) {
            const double t = std::clamp((rowPtr[x] - minValue) / range, 0.0, 1.0);
            const QColor base = colorForMap(colormap, t);
            heat.setPixelColor(x, y, blendTowardsWhite(base, 0.10));
        }
    }
    painter.drawImage(L.plot, heat);

    // Mapping function
    const double imgW = std::max(1, field.cols-1), imgH = std::max(1, field.rows-1);
    auto toPlot = [&](const cv::Point2f &pt){
        const double nx = pt.x / imgW;
        const double ny = pt.y / imgH;
        return QPointF(L.plot.left() + nx*L.plot.width(),
                       L.plot.top()  + ny*L.plot.height());
    };

    // Clip to plot for overlays
    painter.save();
    painter.setClipRect(L.plot);

    // Grid
    if (drawGrid && gridLines && !gridLines->empty()) {
        QPen gridPen(QColor(255,255,255,170), 1.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        painter.setPen(gridPen);
        for (const auto &line : *gridLines) {
            if (line.size() < 2) continue;
            QPainterPath path;
            path.moveTo(toPlot(line.front()));
            for (size_t i = 1; i < line.size(); ++i) path.lineTo(toPlot(line[i]));
            painter.drawPath(path);
        }
    }

    // Vector field (two-pass)
    if (drawVectorField && vectorField && !vectorField->empty()) {
        cv::Mat vecF;
        if (vectorField->type() == CV_32FC2) vecF = *vectorField;
        else vectorField->convertTo(vecF, CV_32FC2);

        const int samplesX=22, samplesY=16;
        const int stepX = std::max(1, vecF.cols / samplesX);
        const int stepY = std::max(1, vecF.rows / samplesY);
        const double arrowMax = std::min(L.plot.width(), L.plot.height()) * 0.055;
        const double maxMag = std::max(maxValue, 1e-9);

        auto drawArrow = [&](const QPointF &s, const QPointF &e){
            painter.setPen(QPen(QColor(0,0,0,70), 2.0));
            painter.drawLine(s,e);
            painter.setPen(QPen(QColor(255,210,90,220), 1.4));
            painter.drawLine(s,e);
            const double ang = std::atan2(s.y()-e.y(), s.x()-e.x());
            const double sz = 6.6;
            QPointF t1 = e + QPointF(std::cos(ang+0.42)*sz, std::sin(ang+0.42)*sz);
            QPointF t2 = e + QPointF(std::cos(ang-0.42)*sz, std::sin(ang-0.42)*sz);
            painter.drawLine(e, t1);
            painter.drawLine(e, t2);
        };

        for (int vy = stepY/2; vy < vecF.rows; vy += stepY) {
            for (int vx = stepX/2; vx < vecF.cols; vx += stepX) {
                const cv::Vec2f v = vecF.at<cv::Vec2f>(vy, vx);
                const double m = std::hypot(v[0], v[1]);
                if (m <= maxMag*0.01) continue;

                const QPointF start(L.plot.left() + (double(vx)/std::max(1,vecF.cols-1))*L.plot.width(),
                                    L.plot.top()  + (double(vy)/std::max(1,vecF.rows-1))*L.plot.height());
                const double len = arrowMax * std::clamp(m/maxMag, 0.0, 1.0);
                const double inv = (m>1e-6) ? 1.0/m : 0.0;
                const QPointF delta(v[0]*inv*len, v[1]*inv*len);
                drawArrow(start, start + delta);
            }
        }
    }

    painter.restore(); // end clip

    // Frame
    painter.setPen(QPen(QColor(70,70,70), kFrameLineW));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(L.plot);

    // ---- Axis titles ----
    painter.setFont(axisFont);
    painter.setPen(QColor(35,35,35));
    painter.drawText(QRectF(L.plot.left(), L.plot.bottom()+L.tickSizePx+16, L.plot.width(), axisFM.height()*1.8),
                     Qt::AlignCenter | Qt::TextWordWrap, xLabel);

    // 计算 Y 轴标题的**左侧**精确位置：在“刻度线+刻度文字”外侧居中
    double maxYTickW = 0.0; for (auto &s: yTicks) maxYTickW = std::max(maxYTickW, tickFM.horizontalAdvance(s));
    const double yTitleCenterX = L.plot.left() - (L.tickSizePx + 6.0 + maxYTickW + 30.0);
    {
        PainterStateGuard g(painter);
        painter.translate(yTitleCenterX, L.plot.center().y());
        painter.rotate(-90);
        painter.drawText(QRectF(-L.plot.height()/2.0, -axisFM.height()*0.9, L.plot.height(), axisFM.height()*1.8),
                         Qt::AlignCenter | Qt::TextWordWrap, yLabel);
    }

    // ---- Axis ticks ----
    painter.setFont(tickFont);
    painter.setPen(QColor(55,55,55));
    for (int i = 0; i < tickCount; ++i) {
        const double t = double(i)/(tickCount-1);

        const double x = L.plot.left() + t * L.plot.width();
        painter.drawLine(QPointF(x, L.plot.bottom()), QPointF(x, L.plot.bottom() + L.tickSizePx));
        painter.drawText(QRectF(x-58, L.plot.bottom()+L.tickSizePx+1, 116, tickFM.height()*1.6),
                         Qt::AlignHCenter | Qt::AlignTop, xTicks[i]);

        const double y = L.plot.bottom() - t * L.plot.height();
        painter.drawLine(QPointF(L.plot.left()-L.tickSizePx, y), QPointF(L.plot.left(), y));
        painter.drawText(QRectF(L.plot.left()-160, y - 0.9*tickFM.height(), 150, tickFM.height()*1.6),
                         Qt::AlignRight | Qt::AlignVCenter, yTicks[i]);
    }

    // ---- Colorbar (right) ----
    for (int y = 0; y < int(L.cbarRight.height()); ++y) {
        const double t = 1.0 - double(y)/std::max(1.0, L.cbarRight.height()-1.0);
        painter.fillRect(QRectF(L.cbarRight.left(), L.cbarRight.top()+y, L.cbarRight.width(), 1.0),
                         colorForMap(colormap, t));
    }
    painter.setPen(QPen(QColor(70,70,70), 1.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(L.cbarRight);

    // 色条刻度
    painter.setFont(tickFont);
    for (int i = 0; i < tickCount; ++i) {
        const double t = double(i)/(tickCount-1);
        const double y = L.cbarRight.bottom() - t * L.cbarRight.height();
        painter.drawLine(QPointF(L.cbarRight.right(), y), QPointF(L.cbarRight.right() + L.tickSizePx*0.9, y));
        painter.drawText(QRectF(L.cbarRight.right() + L.tickSizePx, y - tickFM.height()*0.9, 200, tickFM.height()*1.8),
                         Qt::AlignLeft | Qt::AlignVCenter, cbarTicks[i]);
    }

    // 色条标题：严格以色条宽度为盒子，居中在正下方（不可能侵入到 x 轴刻度区域）
    painter.setFont(QFont("Times New Roman", 44, QFont::DemiBold));
    const double labelWidth = L.cbarRight.width() + cbarTickW + 96.0;
    const double labelX = L.cbarRight.center().x() - labelWidth * 0.5;
    const QRectF labelBox(labelX, L.cbarRight.bottom() + 18, labelWidth, axisFM.height() * 2.4);
    painter.drawText(labelBox, Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, colorbarLabel);
}

// ===================== Draw Residual Scatter =====================
void PaperFigureExporter::drawResidualScatter(QPainter &painter,
                                              const CalibrationOutput &output)
{
    painter.fillRect(QRect(0, 0, kCanvasWidth + 2*kBleedPx, kCanvasHeight + 2*kBleedPx), Qt::white);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    struct Sample { QPointF dpx; double magPx; double magMm; };
    std::vector<Sample> samples; samples.reserve(8192);

    auto push = [&](const std::vector<DetectionResult> &vec){
        for (const auto &rec : vec) {
            if (!rec.success || rec.residualVectors.empty()) continue;
            const size_t n = std::min(rec.residualVectors.size(), rec.residualCameraMm.size());
            for (size_t i=0;i<n;++i) {
                Sample s;
                s.dpx = QPointF(rec.residualVectors[i].x, rec.residualVectors[i].y);
                s.magPx = std::hypot(s.dpx.x(), s.dpx.y());
                const auto &mm = rec.residualCameraMm[i];
                s.magMm = std::sqrt(mm[0]*mm[0] + mm[1]*mm[1] + mm[2]*mm[2]);
                samples.push_back(s);
            }
        }
    };
    push(output.keptDetections);
    push(output.removedDetections);

    if (samples.empty()) {
        painter.setPen(QColor(80,80,80));
        painter.setFont(QFont("Times New Roman", 36));
        painter.drawText(QRect(0,0,kCanvasWidth,kCanvasHeight), Qt::AlignCenter,
                         QStringLiteral("No residual samples available"));
        return;
    }

    // Ranges
    double minX=+1e9, maxX=-1e9, minY=+1e9, maxY=-1e9, maxMagPx=0.0, maxMagMm=0.0;
    for (auto &s: samples) {
        minX = std::min(minX, s.dpx.x()); maxX = std::max(maxX, s.dpx.x());
        minY = std::min(minY, s.dpx.y()); maxY = std::max(maxY, s.dpx.y());
        maxMagPx = std::max(maxMagPx, s.magPx); maxMagMm = std::max(maxMagMm, s.magMm);
    }
    if (!(std::isfinite(minX)&&std::isfinite(maxX))) { minX=-0.5; maxX=0.5; }
    if (!(std::isfinite(minY)&&std::isfinite(maxY))) { minY=-0.5; maxY=0.5; }
    const double padX = std::max(0.05*(maxX-minX), 0.015);
    const double padY = std::max(0.05*(maxY-minY), 0.015);
    minX -= padX; maxX += padX; minY -= padY; maxY += padY;

    // Fonts & ticks
    QFont axisFont("Times New Roman", 52, QFont::Bold);
    QFont tickFont("Times New Roman", 38);
    QFontMetricsF axisFM(axisFont), tickFM(tickFont);
    const int tickCount = 6;
    QStringList xTicks, yTicks, pxBarTicks, mmBarTicks;
    for (int i=0;i<tickCount;++i){
        const double t = double(i)/(tickCount-1);
        xTicks << QString::number(minX + (maxX-minX)*t, 'f', 2);
        yTicks << QString::number(minY + (maxY-minY)*t, 'f', 2);
        pxBarTicks << QString::number(t*maxMagPx, 'f', 3);
        mmBarTicks << QString::number(t*maxMagMm, 'f', 3);
    }

    double leftTickW = 0.0; for (auto &s: pxBarTicks) leftTickW = std::max(leftTickW, tickFM.horizontalAdvance(s));

    const double barGap = 96.0;
    const double barW   = 46.0;
    const double leftPad = barGap + barW + 16.0 + leftTickW + 20.0;
    const double rightPad = barGap + barW + 16.0 + 80.0;

    // Square plot
    CanvasLayout L = computeLayout(QSize(kCanvasWidth, kCanvasHeight),
                                   xTicks, yTicks, 1.0, axisFont, tickFont,
                                   rightPad, leftPad);

    // Background heat (local P75)
    const int gridN = 70;
    std::vector<std::vector<std::vector<double>>> grid(gridN, std::vector<std::vector<double>>(gridN));
    for (auto &s: samples) {
        const double nx = std::clamp((s.dpx.x()-minX)/(maxX-minX), 0.0, 1.0);
        const double ny = std::clamp((s.dpx.y()-minY)/(maxY-minY), 0.0, 1.0);
        const int gx = std::clamp(int(std::floor(nx*gridN)), 0, gridN-1);
        const int gy = std::clamp(int(std::floor(ny*gridN)), 0, gridN-1);
        grid[gy][gx].push_back(s.magPx);
    }
    cv::Mat qmap(gridN, gridN, CV_64F, cv::Scalar(0));
    for (int y=0;y<gridN;++y) for (int x=0;x<gridN;++x)
        if (!grid[y][x].empty()) qmap.at<double>(y,x) = percentile(grid[y][x], 0.75);
    cv::GaussianBlur(qmap, qmap, cv::Size(5,5), 1.2);

    painter.save();
    painter.setClipRect(L.plot);
    const double cellW = L.plot.width()/gridN, cellH = L.plot.height()/gridN;
    for (int y=0;y<gridN;++y){
        for (int x=0;x<gridN;++x){
            const double v = qmap.at<double>(y,x) / std::max(1e-6, maxMagPx);
            const QColor base = QColor::fromRgbF(0.15, 0.20, 0.28);
            const QColor tint = plasmaColor(std::clamp(v,0.0,1.0));
            const QColor blended = interpolate(base, tint, 0.25);
            painter.fillRect(QRectF(L.plot.left()+x*cellW, L.plot.top()+y*cellH, cellW+0.5, cellH+0.5),
                             blendTowardsWhite(blended, 0.88));
        }
    }
    painter.restore();

    // Minor grid
    painter.setPen(QPen(QColor(170,170,170), 0.9, Qt::DotLine));
    for (int i=1;i<4;++i){
        const double t = double(i)/4.0;
        const double x = L.plot.left() + t * L.plot.width();
        const double y = L.plot.top()  + t * L.plot.height();
        painter.drawLine(QPointF(x, L.plot.top()), QPointF(x, L.plot.bottom()));
        painter.drawLine(QPointF(L.plot.left(), y), QPointF(L.plot.right(), y));
    }

    // Frame
    painter.setPen(QPen(QColor(30,30,30), kFrameLineW));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(L.plot);

    // Axis titles —— y 标题放在左色条外侧
    painter.setFont(axisFont);
    painter.drawText(QRectF(L.plot.left(), L.plot.bottom()+16+16, L.plot.width(), axisFM.height()*1.8),
                     Qt::AlignCenter, QStringLiteral("Δx (px)"));

    const QRectF leftBarGeom(L.plot.left() - (barGap + barW) - 100.0, L.plot.top(), barW, L.plot.height());
    {
        PainterStateGuard g(painter);
        const double yTitleCenterX = leftBarGeom.left() + 80.0; // 在左色条外侧 18px
        painter.translate(yTitleCenterX, L.plot.center().y());
        painter.rotate(-90);
        painter.drawText(QRectF(-L.plot.height()/2.0, -axisFM.height()*0.9, L.plot.height(), axisFM.height()*1.8),
                         Qt::AlignCenter, QStringLiteral("Δy (px)"));
    }

    // Ticks
    painter.setFont(tickFont);
    painter.setPen(QColor(30,30,30));
    for (int i=0;i<tickCount;++i){
        const double t = double(i)/(tickCount-1);
        const double vx = minX + (maxX-minX)*t;
        const double vy = minY + (maxY-minY)*t;

        const double px = L.plot.left() + t * L.plot.width();
        painter.drawLine(QPointF(px, L.plot.bottom()), QPointF(px, L.plot.bottom()+L.tickSizePx));
        painter.drawText(QRectF(px-56, L.plot.bottom()+L.tickSizePx+1, 112, tickFont.pointSizeF()*2),
                         Qt::AlignHCenter | Qt::AlignTop, QString::number(vx,'f',2));

        const double py = L.plot.bottom() - t * L.plot.height();
        painter.drawLine(QPointF(L.plot.left()-L.tickSizePx, py), QPointF(L.plot.left(), py));
        painter.drawText(QRectF(L.plot.left()-130, py - tickFont.pointSizeF()*0.9, 110, tickFont.pointSizeF()*2),
                         Qt::AlignRight | Qt::AlignVCenter, QString::number(vy,'f',2));
    }

    // Zero lines
    painter.setPen(QPen(QColor(50,50,50), 1.2));
    if (minY < 0 && maxY > 0) {
        const double y0 = L.plot.bottom() - (-minY)/(maxY-minY)*L.plot.height();
        painter.drawLine(QPointF(L.plot.left(), y0), QPointF(L.plot.right(), y0));
    }
    if (minX < 0 && maxX > 0) {
        const double x0 = L.plot.left() + (-minX)/(maxX-minX)*L.plot.width();
        painter.drawLine(QPointF(x0, L.plot.top()), QPointF(x0, L.plot.bottom()));
    }

    // Points —— Plasma
    auto project = [&](const QPointF &r){
        const double nx = std::clamp((r.x()-minX)/(maxX-minX), 0.0, 1.0);
        const double ny = std::clamp((r.y()-minY)/(maxY-minY), 0.0, 1.0);
        return QPointF(L.plot.left()+nx*L.plot.width(), L.plot.bottom()-ny*L.plot.height());
    };
    painter.save();
    painter.setClipRect(L.plot);
    for (auto &s: samples) {
        const QPointF p = project(s.dpx);
        const double n = std::clamp(s.magPx/std::max(1e-6, maxMagPx), 0.0, 1.0);
        const QColor c = plasmaColor(n);
        painter.setPen(QPen(QColor(0,0,0,70), 0.8));
        painter.setBrush(blendTowardsWhite(c, 0.10));
        painter.drawEllipse(p, 3.0, 3.0);
    }
    painter.restore();

    // Dual colorbars
    const QRectF leftBar (leftBarGeom);
    const QRectF rightBar(L.plot.right() +  barGap, L.plot.top(), barW, L.plot.height());

    auto drawBar = [&](const QRectF &r, double vmax, const QString &label, bool ticksRight){
        for (int y=0; y<int(r.height()); ++y) {
            const double t = 1.0 - double(y)/std::max(1.0, r.height()-1.0);
            painter.fillRect(QRectF(r.left(), r.top()+y, r.width(), 1.0), plasmaColor(t));
        }
        painter.setPen(QPen(QColor(70,70,70), 1.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(r);

    painter.setFont(QFont("Times New Roman", 36, QFont::DemiBold));
    const double barLabelWidth = r.width() + 160.0;
    const double barLabelX = r.center().x() - barLabelWidth * 0.5;
    QRectF lab(barLabelX, r.bottom() + 18, barLabelWidth, axisFM.height() * 2.4);
    painter.drawText(lab, Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, label);

        painter.setFont(QFont("Times New Roman", 32));
        const int bt=5;
        for (int i=0;i<bt;++i){
            const double t = double(i)/(bt-1);
            const double y = r.bottom() - t * r.height();
            const double val = t * vmax;
            if (ticksRight) {
                painter.drawLine(QPointF(r.right(), y), QPointF(r.right()+16, y));
                painter.drawText(QRectF(r.right()+22, y-30, 160, 60), Qt::AlignLeft|Qt::AlignVCenter,
                                 QString::number(val,'f',3));
            } else {
                painter.drawLine(QPointF(r.left(), y), QPointF(r.left()-16, y));
                painter.drawText(QRectF(r.left()-170, y-30, 160, 60), Qt::AlignRight|Qt::AlignVCenter,
                                 QString::number(val,'f',3));
            }
        }
    };

    drawBar(leftBar,  maxMagPx, QStringLiteral("|Δ| (px)"), false);
    drawBar(rightBar, maxMagMm, QStringLiteral("|Δ| (mm)"), true);

    // Caption
    painter.setFont(QFont("Times New Roman", 32));
    painter.setPen(QColor(80,80,80));
    painter.drawText(QRectF(L.plot.left(), L.plot.bottom()+104, L.plot.width(), 56), Qt::AlignCenter,
                     QStringLiteral("Background: local 75th percentile of |Δ| (px). Points colored by |Δ| (Plasma)."));
}

// ===================== Colormaps & Utilities =====================
QColor PaperFigureExporter::viridisColor(double t)
{
    const std::array<QColor, 6> stops = {
        QColor(90, 20, 105),
        QColor(71, 44, 122),
        QColor(59, 81, 139),
        QColor(44, 113, 142),
        QColor(33, 144, 141),
        QColor(94, 201, 99)
    };
    if (t <= 0.0) return stops.front();
    if (t >= 1.0) return QColor(253, 231, 37);
    const double s = std::clamp(t,0.0,1.0) * (stops.size()-1);
    const int idx = int(std::floor(s));
    const double lt = s - idx;
    return interpolate(stops[size_t(idx)], stops[size_t(idx+1)], lt);
}

QColor PaperFigureExporter::turboColor(double t)
{
    const double x = std::clamp(t, 0.0, 1.0);
    const double x2 = x*x, x3 = x2*x, x4 = x3*x, x5 = x4*x;
    const double r = 0.13572138 + 4.61539260*x - 42.66032258*x2 + 132.13108234*x3 - 152.94239396*x4 + 59.28637943*x5;
    const double g = 0.09140261 + 2.19418839*x +  4.84296658*x2 -  14.18503333*x3 +  4.27729857*x4 +  2.82956604*x5;
    const double b = 0.10667330 + 12.64194608*x - 60.58204836*x2 + 110.36276771*x3 -  89.90310912*x4 + 27.34824973*x5;
    auto ch = [](double v){ return int(std::round(std::clamp(v,0.0,1.0)*255.0)); };
    return QColor(ch(r), ch(g), ch(b));
}

QColor PaperFigureExporter::cividisColor(double t)
{
    const double x = std::clamp(t, 0.0, 1.0);
    int r = int(std::round(  0 + 255 * (0.34*x)));
    int g = int(std::round( 34 + 255 * (0.53*x)));
    int b = int(std::round( 68 + 255 * (0.80*x)));
    return QColor(std::clamp(r,0,255), std::clamp(g,0,255), std::clamp(b,0,255));
}

QColor PaperFigureExporter::plasmaColor(double t)
{
    // 近似 Plasma
    const double x = std::clamp(t, 0.0, 1.0);
    const double r = std::clamp( 2.0*x - 0.5*x*x, 0.0, 1.0);
    const double g = std::clamp( 0.1 + 1.2*x - 1.2*x*x + 0.3*x*x*x, 0.0, 1.0);
    const double b = std::clamp( 0.9 - 1.1*x + 0.2*x*x, 0.0, 1.0);
    return QColor(int(r*255.0 + 0.5), int(g*255.0 + 0.5), int(b*255.0 + 0.5));
}

QColor PaperFigureExporter::colorForMap(ScalarColormap map, double t)
{
    switch (map) {
    case ScalarColormap::Turbo:   return turboColor(t);
    case ScalarColormap::Cividis: return cividisColor(t);
    case ScalarColormap::Plasma:  return plasmaColor(t);
    case ScalarColormap::Viridis:
    default:                      return viridisColor(t);
    }
}

QColor PaperFigureExporter::blendTowardsWhite(const QColor &color, double weight)
{
    return interpolate(color, QColor(255,255,255), std::clamp(weight, 0.0, 1.0));
}

// ===================== Write SVG & PNG (with bleed) =====================
void PaperFigureExporter::writeSvgAndPng(const QString &fileBasePath,
                                         const std::function<void(QPainter &)> &drawFunction)
{
    QSvgGenerator gen;
    gen.setFileName(fileBasePath + QStringLiteral(".svg"));
    gen.setSize(QSize(kCanvasWidth + 2*kBleedPx, kCanvasHeight + 2*kBleedPx));
    gen.setViewBox(QRect(0, 0, kCanvasWidth + 2*kBleedPx, kCanvasHeight + 2*kBleedPx));
    gen.setTitle(QStringLiteral("Calibration diagnostic figure"));
    {
        QPainter p(&gen);
        p.setRenderHint(QPainter::Antialiasing, true);
        drawFunction(p);
    }

    QImage img(kCanvasWidth + 2*kBleedPx, kCanvasHeight + 2*kBleedPx, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::white);
    {
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing, true);
        drawFunction(p);
    }
    img.save(fileBasePath + QStringLiteral(".png"));
}

} // namespace mycalib