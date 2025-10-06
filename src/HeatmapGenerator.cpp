#include "HeatmapGenerator.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <utility>

#include <opencv2/calib3d.hpp>
#include <opencv2/imgproc.hpp>

namespace mycalib {

namespace {
constexpr int kHistogramBinSize = 140;

cv::Mat applyColorMapTurbo(const cv::Mat &src)
{
    cv::Mat normalized;
    cv::normalize(src, normalized, 0, 255, cv::NORM_MINMAX, CV_8U);
    cv::Mat colored;
    cv::applyColorMap(normalized, colored, cv::COLORMAP_TURBO);
    return colored;
}

cv::Scalar viridisColor(double t)
{
    static const std::array<std::pair<double, cv::Vec3d>, 5> kStops = {
        std::pair<double, cv::Vec3d>{0.0, {84.0, 1.0, 68.0}},
        std::pair<double, cv::Vec3d>{0.25, {139.0, 82.0, 59.0}},
        std::pair<double, cv::Vec3d>{0.5, {140.0, 145.0, 33.0}},
        std::pair<double, cv::Vec3d>{0.75, {98.0, 201.0, 94.0}},
        std::pair<double, cv::Vec3d>{1.0, {37.0, 231.0, 253.0}},
    };

    t = std::clamp(t, 0.0, 1.0);
    for (size_t i = 1; i < kStops.size(); ++i) {
        if (t <= kStops[i].first) {
            const double span = kStops[i].first - kStops[i - 1].first;
            const double ratio = span > 0.0 ? (t - kStops[i - 1].first) / span : 0.0;
            const cv::Vec3d color = kStops[i - 1].second + (kStops[i].second - kStops[i - 1].second) * ratio;
            return cv::Scalar(color[0], color[1], color[2]);
        }
    }
    const cv::Vec3d &last = kStops.back().second;
    return cv::Scalar(last[0], last[1], last[2]);
}

} // namespace

cv::Mat HeatmapGenerator::buildBoardCoverage(const std::vector<DetectionResult> &detections,
                                             const cv::Size &imageSize,
                                             double *minValue,
                                             double *maxValue,
                                             cv::Mat *rawScalarOut) const
{
    cv::Mat coverage = cv::Mat::zeros(imageSize, CV_32F);
    for (const auto &rec : detections) {
        if (!rec.success || rec.imagePoints.empty()) {
            continue;
        }
        std::vector<cv::Point> hull;
        hull.reserve(rec.imagePoints.size());
        for (const auto &pt : rec.imagePoints) {
            hull.emplace_back(cv::Point(static_cast<int>(pt.x), static_cast<int>(pt.y)));
        }
        if (hull.size() >= 4) {
            std::vector<int> hullIndices;
            cv::convexHull(hull, hullIndices);
            std::vector<cv::Point> hullPoints;
            hullPoints.reserve(hullIndices.size());
            for (int idx : hullIndices) {
                hullPoints.emplace_back(hull[idx]);
            }
            cv::Mat mask = cv::Mat::zeros(imageSize, CV_8U);
            cv::fillConvexPoly(mask, hullPoints, cv::Scalar(255));
            coverage += mask / 255.0f;
        }
    }

    double minVal = 0.0, maxVal = 0.0;
    cv::minMaxLoc(coverage, &minVal, &maxVal);
    if (minValue) {
        *minValue = minVal;
    }
    if (maxValue) {
        *maxValue = maxVal;
    }

    if (rawScalarOut) {
        coverage.copyTo(*rawScalarOut);
    }

    return applyColorMapTurbo(coverage);
}

cv::Mat HeatmapGenerator::buildPixelErrorHeatmap(const std::vector<DetectionResult> &detections,
                                                 const cv::Size &imageSize,
                                                 double *minValue,
                                                 double *maxValue,
                                                 cv::Mat *rawScalarOut) const
{
    if (imageSize.width == 0 || imageSize.height == 0) {
        return cv::Mat();
    }

    const int binsX = std::max(12, imageSize.width / kHistogramBinSize);
    const int binsY = std::max(12, imageSize.height / kHistogramBinSize);

    cv::Mat sum = cv::Mat::zeros(binsY, binsX, CV_32F);
    cv::Mat count = cv::Mat::zeros(binsY, binsX, CV_32F);

    for (const auto &rec : detections) {
        if (!rec.success || rec.imagePoints.empty() || rec.residualsPx.empty()) {
            continue;
        }
        for (size_t i = 0; i < rec.imagePoints.size() && i < rec.residualsPx.size(); ++i) {
            const auto &pt = rec.imagePoints[i];
            const double err = rec.residualsPx[i];
            int xBin = static_cast<int>(pt.x / imageSize.width * binsX);
            int yBin = static_cast<int>(pt.y / imageSize.height * binsY);
            xBin = std::clamp(xBin, 0, binsX - 1);
            yBin = std::clamp(yBin, 0, binsY - 1);
            sum.at<float>(yBin, xBin) += static_cast<float>(err);
            count.at<float>(yBin, xBin) += 1.f;
        }
    }

    cv::Mat avg;
    cv::divide(sum, count, avg, 1.0, CV_32F);
    avg.setTo(0, count == 0);

    double minVal = 0.0;
    double maxVal = 0.0;
    cv::Mat mask = count > 0;
    if (cv::countNonZero(mask) > 0) {
        cv::minMaxLoc(avg, &minVal, &maxVal, nullptr, nullptr, mask);
    }
    if (minValue) {
        *minValue = minVal;
    }
    if (maxValue) {
        *maxValue = maxVal;
    }

    cv::Mat upscaled = renderHeatmapFromHistogram(avg, imageSize);
    if (rawScalarOut) {
        upscaled.copyTo(*rawScalarOut);
    }
    return applyColorMapTurbo(upscaled);
}

cv::Mat HeatmapGenerator::buildBoardErrorHeatmap(const std::vector<DetectionResult> &detections,
                                                 const cv::Size &imageSize,
                                                 double *minValue,
                                                 double *maxValue,
                                                 cv::Mat *rawScalarOut) const
{
    if (imageSize.width == 0 || imageSize.height == 0) {
        return cv::Mat();
    }

    cv::Mat accumulation = cv::Mat::zeros(imageSize, CV_32F);
    cv::Mat counter = cv::Mat::zeros(imageSize, CV_32F);

    for (const auto &rec : detections) {
        if (!rec.success || rec.imagePoints.empty() || rec.residualsPx.empty()) {
            continue;
        }
        for (size_t i = 0; i < rec.imagePoints.size() && i < rec.residualsPx.size(); ++i) {
            const auto &pt = rec.imagePoints[i];
            const double err = rec.residualsPx[i];
            int x = std::clamp(static_cast<int>(std::round(pt.x)), 0, imageSize.width - 1);
            int y = std::clamp(static_cast<int>(std::round(pt.y)), 0, imageSize.height - 1);
            accumulation.at<float>(y, x) += static_cast<float>(err);
            counter.at<float>(y, x) += 1.f;
        }
    }

    cv::Mat average;
    cv::divide(accumulation, counter, average, 1.0, CV_32F);
    average.setTo(0, counter == 0);

    double minVal = 0.0, maxVal = 0.0;
    cv::Mat mask = counter > 0;
    cv::minMaxLoc(average, &minVal, &maxVal, nullptr, nullptr, mask);
    if (minValue) {
        *minValue = minVal;
    }
    if (maxValue) {
        *maxValue = maxVal;
    }

    cv::Mat blurred;
    cv::GaussianBlur(average, blurred, cv::Size(0, 0), 6.0);
    if (rawScalarOut) {
        blurred.copyTo(*rawScalarOut);
    }
    return applyColorMapTurbo(blurred);
}

cv::Mat HeatmapGenerator::renderHeatmapFromHistogram(const cv::Mat &histogram, const cv::Size &targetSize)
{
    cv::Mat temp;
    cv::resize(histogram, temp, targetSize, 0, 0, cv::INTER_CUBIC);
    cv::Mat blurred;
    cv::GaussianBlur(temp, blurred, cv::Size(0, 0), 5.5);
    return blurred;
}

cv::Mat HeatmapGenerator::buildResidualScatter(const std::vector<DetectionResult> &detections,
                                               double *maxMagnitude) const
{
    std::vector<cv::Point2f> residuals;
    residuals.reserve(2048);
    for (const auto &rec : detections) {
        if (!rec.success || rec.residualVectors.empty()) {
            continue;
        }
        residuals.insert(residuals.end(), rec.residualVectors.begin(), rec.residualVectors.end());
    }

    if (residuals.empty()) {
        if (maxMagnitude) {
            *maxMagnitude = 0.0;
        }
        return cv::Mat();
    }

    double maxMag = 0.0;
    for (const auto &vec : residuals) {
        maxMag = std::max(maxMag, static_cast<double>(cv::norm(vec)));
    }
    if (maxMag < 1e-6) {
        maxMag = 1.0;
    }
    if (maxMagnitude) {
        *maxMagnitude = maxMag;
    }

    const int size = 640;
    cv::Mat canvas(size, size, CV_8UC3, cv::Scalar(24, 28, 42));
    const cv::Point center(size / 2, size / 2);
    const double scale = (size * 0.38) / maxMag;

    cv::Scalar gridColor(60, 70, 90);
    for (int step = -4; step <= 4; ++step) {
        if (step == 0) {
            continue;
        }
        const double value = (maxMag / 4.0) * static_cast<double>(step);
        const int offset = static_cast<int>(std::round(value * scale));
        cv::line(canvas, cv::Point(center.x + offset, 64), cv::Point(center.x + offset, size - 64), gridColor, 1,
                 cv::LINE_AA);
        cv::line(canvas, cv::Point(64, center.y - offset), cv::Point(size - 64, center.y - offset), gridColor, 1,
                 cv::LINE_AA);
    }

    cv::line(canvas, cv::Point(48, center.y), cv::Point(size - 48, center.y), cv::Scalar(150, 160, 200), 1, cv::LINE_AA);
    cv::line(canvas, cv::Point(center.x, 48), cv::Point(center.x, size - 48), cv::Scalar(150, 160, 200), 1, cv::LINE_AA);
    cv::rectangle(canvas, cv::Rect(48, 48, size - 96, size - 96), cv::Scalar(80, 90, 120), 1, cv::LINE_AA);

    std::vector<std::pair<cv::Point, double>> projected;
    projected.reserve(residuals.size());
    for (const auto &vec : residuals) {
        const double mag = cv::norm(vec);
        const int px = static_cast<int>(std::round(center.x + vec.x * scale));
        const int py = static_cast<int>(std::round(center.y - vec.y * scale));
        projected.emplace_back(cv::Point(px, py), mag);
    }

    for (const auto &[pt, mag] : projected) {
        const double normMag = std::clamp(mag / maxMag, 0.0, 1.0);
        const cv::Scalar color = viridisColor(normMag);
        cv::circle(canvas, pt, 4, color, cv::FILLED, cv::LINE_AA);
    }

    const cv::Scalar textColor(210, 220, 240);
    cv::putText(canvas, "dx (px)", cv::Point(size - 180, center.y + 24), cv::FONT_HERSHEY_SIMPLEX, 0.6,
                textColor, 1, cv::LINE_AA);
    cv::putText(canvas, "dy (px)", cv::Point(center.x + 16, 72), cv::FONT_HERSHEY_SIMPLEX, 0.6,
                textColor, 1, cv::LINE_AA);
    cv::putText(canvas, "Reprojection error scatter", cv::Point(60, 40), cv::FONT_HERSHEY_SIMPLEX, 0.75,
                textColor, 2, cv::LINE_AA);

    const int barWidth = 24;
    const int barHeight = size - 160;
    const cv::Rect barRect(size - 80, 80, barWidth, barHeight);
    for (int y = 0; y < barHeight; ++y) {
        const double t = 1.0 - static_cast<double>(y) / std::max(1, barHeight - 1);
        const cv::Scalar color = viridisColor(t);
        cv::line(canvas,
                 cv::Point(barRect.x, barRect.y + y),
                 cv::Point(barRect.x + barWidth - 1, barRect.y + y),
                 color,
                 1,
                 cv::LINE_AA);
    }
    cv::rectangle(canvas, barRect, cv::Scalar(200, 210, 230), 1, cv::LINE_AA);

    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.2f", maxMag);
    cv::putText(canvas, buffer, cv::Point(barRect.x + barWidth + 8, barRect.y + 8), cv::FONT_HERSHEY_SIMPLEX,
                0.5, textColor, 1, cv::LINE_AA);
    cv::putText(canvas, "0", cv::Point(barRect.x + barWidth + 8, barRect.y + barHeight), cv::FONT_HERSHEY_SIMPLEX,
                0.5, textColor, 1, cv::LINE_AA);
    cv::putText(canvas, "Magnitude (px)",
                cv::Point(barRect.x - 40, barRect.y + barHeight + 28), cv::FONT_HERSHEY_SIMPLEX, 0.55,
                textColor, 1, cv::LINE_AA);

    return canvas;
}

cv::Mat HeatmapGenerator::buildDistortionHeatmap(const cv::Mat &cameraMatrix,
                                                const cv::Mat &distCoeffs,
                                                const cv::Size &imageSize,
                                                double *minValue,
                                                double *maxValue,
                                                std::vector<std::vector<cv::Point2f>> *gridLines,
                                                cv::Mat *rawScalarOut,
                                                cv::Mat *vectorFieldOut) const
{
    if (imageSize.width <= 0 || imageSize.height <= 0 || cameraMatrix.empty()) {
        if (minValue) {
            *minValue = 0.0;
        }
        if (maxValue) {
            *maxValue = 0.0;
        }
        if (gridLines) {
            gridLines->clear();
        }
        if (rawScalarOut) {
            rawScalarOut->release();
        }
        if (vectorFieldOut) {
            vectorFieldOut->release();
        }
        return cv::Mat();
    }

    if (gridLines) {
        gridLines->clear();
    }

    cv::Mat camera64;
    cameraMatrix.convertTo(camera64, CV_64F);
    cv::Mat distCoeffs64;
    if (distCoeffs.empty()) {
        distCoeffs64 = cv::Mat::zeros(1, 5, CV_64F);
    } else {
        distCoeffs.convertTo(distCoeffs64, CV_64F);
    }

    cv::Mat map;
    cv::Mat map2;
    cv::initUndistortRectifyMap(camera64, distCoeffs64, cv::Mat(), camera64, imageSize, CV_32FC2, map, map2);

    cv::Mat magnitude(imageSize, CV_32F);
    cv::Mat *vectorDest = nullptr;
    if (vectorFieldOut) {
        vectorFieldOut->create(imageSize.height, imageSize.width, CV_32FC2);
        vectorDest = vectorFieldOut;
    }
    double localMin = std::numeric_limits<double>::infinity();
    double localMax = 0.0;

    for (int y = 0; y < imageSize.height; ++y) {
        const cv::Vec2f *row = map.ptr<cv::Vec2f>(y);
        float *dst = magnitude.ptr<float>(y);
        for (int x = 0; x < imageSize.width; ++x) {
            const cv::Vec2f src = row[x];
            const float dx = src[0] - static_cast<float>(x);
            const float dy = src[1] - static_cast<float>(y);
            const float mag = std::sqrt(dx * dx + dy * dy);
            dst[x] = mag;
            if (vectorDest) {
                vectorDest->at<cv::Vec2f>(y, x) = cv::Vec2f(dx, dy);
            }
            localMin = std::min<double>(localMin, mag);
            localMax = std::max<double>(localMax, mag);
        }
    }

    if (!std::isfinite(localMin)) {
        localMin = 0.0;
    }

    if (minValue) {
        *minValue = localMin;
    }
    if (maxValue) {
        *maxValue = localMax;
    }

    if (gridLines) {
        const int gridCount = 8;
        const int samplesPerLine = std::max(imageSize.width, imageSize.height) / 6;
        const int clampedSamples = std::clamp(samplesPerLine, 36, 160);
        const double width = static_cast<double>(imageSize.width - 1);
        const double height = static_cast<double>(imageSize.height - 1);
        const double fx = camera64.at<double>(0, 0);
        const double fy = camera64.at<double>(1, 1);
        const double cx = camera64.at<double>(0, 2);
        const double cy = camera64.at<double>(1, 2);

        gridLines->reserve((gridCount + 1) * 2);

        auto projectLine = [&](bool vertical, int index) {
            const int segments = clampedSamples;
            std::vector<cv::Point3f> objectPoints;
            objectPoints.reserve(segments);
            for (int s = 0; s < segments; ++s) {
                const double t = segments > 1 ? static_cast<double>(s) / (segments - 1) : 0.0;
                double xPix = vertical ? (static_cast<double>(index) / gridCount) * width : t * width;
                double yPix = vertical ? t * height : (static_cast<double>(index) / gridCount) * height;
                const double xNorm = (xPix - cx) / fx;
                const double yNorm = (yPix - cy) / fy;
                objectPoints.emplace_back(static_cast<float>(xNorm),
                                          static_cast<float>(yNorm),
                                          1.0f);
            }

            std::vector<cv::Point2f> projected;
            cv::projectPoints(objectPoints,
                              cv::Vec3d::zeros(),
                              cv::Vec3d::zeros(),
                              camera64,
                              distCoeffs64,
                              projected);

            std::vector<cv::Point2f> cleaned;
            cleaned.reserve(projected.size());
            for (const auto &pt : projected) {
                if (!std::isfinite(pt.x) || !std::isfinite(pt.y)) {
                    continue;
                }
                cleaned.push_back(pt);
            }
            if (cleaned.size() >= 2) {
                gridLines->push_back(std::move(cleaned));
            }
        };

        for (int i = 0; i <= gridCount; ++i) {
            projectLine(true, i);
            projectLine(false, i);
        }
    }

    cv::Mat blurred;
    cv::GaussianBlur(magnitude, blurred, cv::Size(0, 0), 3.0);
    if (rawScalarOut) {
        blurred.copyTo(*rawScalarOut);
    }
    return applyColorMapTurbo(blurred);
}

} // namespace mycalib
