#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

namespace mycalib {

struct DetectionDebugImage {
    std::string label;
    std::string filePath;
};

struct DetectionResult {
    std::string name;
    bool success {false};
    std::string message;
    std::chrono::milliseconds elapsed {0};
    cv::Size resolution {0, 0};
    std::vector<cv::Point2f> imagePoints;
    std::vector<cv::Point3f> objectPoints;
    std::vector<cv::Point2f> bigCirclePoints;
    std::vector<float> circleRadiiPx;
    std::vector<float> bigCircleRadiiPx;
    int bigCircleCount {0};
    std::optional<std::vector<int>> inlierIndices;
    int iterationRemoved {0};

    // Cached scalar metrics, populated when detailed residual vectors are unavailable.
    double cachedMeanErrorPx {-1.0};
    double cachedMaxErrorPx {-1.0};

    [[nodiscard]] double meanErrorPx() const;
    [[nodiscard]] double maxErrorPx() const;
        std::vector<cv::Vec2i> logicalIndices;
        cv::Mat whiteRegionMask;
        cv::Mat warpHomography;
        cv::Mat warpHomographyInv;

    std::vector<double> residualsPx; // populated after calibration
    std::vector<cv::Point2f> residualVectors;
    std::vector<cv::Vec3d> residualCameraMm;
    std::vector<cv::Vec3d> residualCameraPercent;
    cv::Vec3d meanResidualCameraMm {0.0, 0.0, 0.0};
    cv::Vec3d meanResidualCameraPercent {0.0, 0.0, 0.0};
    cv::Vec3d translationMm {0.0, 0.0, 0.0};
    cv::Vec3d rotationDeg {0.0, 0.0, 0.0};
    cv::Matx33d rotationMatrix = cv::Matx33d::eye();

    std::vector<DetectionDebugImage> debugImages;
    std::string debugDirectory;
};

inline double DetectionResult::meanErrorPx() const
{
    if (cachedMeanErrorPx >= 0.0) {
        return cachedMeanErrorPx;
    }
    if (residualsPx.empty()) {
        return 0.0;
    }
    double sum = 0.0;
    for (double v : residualsPx) {
        sum += v;
    }
    return sum / static_cast<double>(residualsPx.size());
}

inline double DetectionResult::maxErrorPx() const
{
    if (cachedMaxErrorPx >= 0.0) {
        return cachedMaxErrorPx;
    }
    if (residualsPx.empty()) {
        return 0.0;
    }
    double m = residualsPx.front();
    for (double v : residualsPx) {
        m = std::max(m, v);
    }
    return m;
}

} // namespace mycalib
