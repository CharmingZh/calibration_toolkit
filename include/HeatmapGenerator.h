#pragma once

#include <vector>

#include <opencv2/core.hpp>

#include "DetectionResult.h"

namespace mycalib {
struct CalibrationMetrics;

class HeatmapGenerator {
public:
    HeatmapGenerator() = default;

    [[nodiscard]] cv::Mat buildBoardCoverage(const std::vector<DetectionResult> &detections,
                                             const cv::Size &imageSize,
                                             double *minValue = nullptr,
                                             double *maxValue = nullptr,
                                             cv::Mat *rawScalarOut = nullptr) const;

    [[nodiscard]] cv::Mat buildPixelErrorHeatmap(const std::vector<DetectionResult> &detections,
                                                 const cv::Size &imageSize,
                                                 double *minValue = nullptr,
                                                 double *maxValue = nullptr,
                                                 cv::Mat *rawScalarOut = nullptr) const;

    [[nodiscard]] cv::Mat buildBoardErrorHeatmap(const std::vector<DetectionResult> &detections,
                                                 const cv::Size &imageSize,
                                                 double *minValue = nullptr,
                                                 double *maxValue = nullptr,
                                                 cv::Mat *rawScalarOut = nullptr) const;

    [[nodiscard]] cv::Mat buildResidualScatter(const std::vector<DetectionResult> &detections,
                                               double *maxMagnitude = nullptr) const;

    [[nodiscard]] cv::Mat buildDistortionHeatmap(const cv::Mat &cameraMatrix,
                                                const cv::Mat &distCoeffs,
                                                const cv::Size &imageSize,
                                                double *minValue = nullptr,
                                                double *maxValue = nullptr,
                                                std::vector<std::vector<cv::Point2f>> *gridLines = nullptr,
                                                cv::Mat *rawScalarOut = nullptr,
                                                cv::Mat *vectorFieldOut = nullptr) const;

private:
    static cv::Mat renderHeatmapFromHistogram(const cv::Mat &histogram, const cv::Size &targetSize);
};

} // namespace mycalib
