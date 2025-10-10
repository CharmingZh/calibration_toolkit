#include "camera/focus/FocusEvaluator.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

namespace {
constexpr double kBrightnessTarget = 115.0;
constexpr double kHighlightThreshold = 245.0;
constexpr double kShadowThreshold = 10.0;

inline double normalizeMetric(double value, double low, double high) {
    if (high <= low) {
        return 0.0;
    }
    const double t = (value - low) / (high - low);
    return std::clamp(t, 0.0, 1.0);
}

cv::Mat makeEvenCopy(const cv::Mat& input) {
    const int evenCols = input.cols - (input.cols % 2);
    const int evenRows = input.rows - (input.rows % 2);
    if (evenCols <= 0 || evenRows <= 0) {
        return cv::Mat();
    }
    if (evenCols == input.cols && evenRows == input.rows) {
        return input.clone();
    }
    return input(cv::Rect(0, 0, evenCols, evenRows)).clone();
}

double computeHighFrequencyRatio(const cv::Mat& floatImage) {
    if (floatImage.empty()) {
        return 0.0;
    }

    cv::Mat padded;
    const int optimalRows = cv::getOptimalDFTSize(floatImage.rows);
    const int optimalCols = cv::getOptimalDFTSize(floatImage.cols);
    if (optimalRows != floatImage.rows || optimalCols != floatImage.cols) {
        cv::copyMakeBorder(floatImage, padded,
                           0, optimalRows - floatImage.rows,
                           0, optimalCols - floatImage.cols,
                           cv::BORDER_CONSTANT, 0.0f);
    } else {
        padded = floatImage.clone();
    }

    cv::Mat window;
    cv::createHanningWindow(window, padded.size(), CV_32F);
    padded = padded.mul(window);

    cv::Mat complex;
    cv::dft(padded, complex, cv::DFT_COMPLEX_OUTPUT);

    cv::Mat planes[2];
    cv::split(complex, planes);
    cv::Mat magnitude;
    cv::magnitude(planes[0], planes[1], magnitude);
    cv::Mat powerSpectrum;
    cv::multiply(magnitude, magnitude, powerSpectrum);

    cv::Mat spectrum = makeEvenCopy(powerSpectrum);
    if (spectrum.empty()) {
        return 0.0;
    }

    const double totalEnergy = cv::sum(spectrum)[0];
    if (totalEnergy <= 1e-9) {
        return 0.0;
    }

    const int cx = spectrum.cols / 2;
    const int cy = spectrum.rows / 2;

    cv::Mat q0(spectrum, cv::Rect(0, 0, cx, cy));
    cv::Mat q1(spectrum, cv::Rect(cx, 0, cx, cy));
    cv::Mat q2(spectrum, cv::Rect(0, cy, cx, cy));
    cv::Mat q3(spectrum, cv::Rect(cx, cy, cx, cy));
    cv::Mat tmp;
    q0.copyTo(tmp);
    q3.copyTo(q0);
    tmp.copyTo(q3);
    q1.copyTo(tmp);
    q2.copyTo(q1);
    tmp.copyTo(q2);

    const double centerX = static_cast<double>(spectrum.cols) / 2.0 - 0.5;
    const double centerY = static_cast<double>(spectrum.rows) / 2.0 - 0.5;
    const double maxRadius = std::sqrt(centerX * centerX + centerY * centerY);
    const double threshold = maxRadius * 0.28;

    double highEnergy = 0.0;
    for (int y = 0; y < spectrum.rows; ++y) {
        const float* row = spectrum.ptr<float>(y);
        for (int x = 0; x < spectrum.cols; ++x) {
            const double dx = static_cast<double>(x) - centerX;
            const double dy = static_cast<double>(y) - centerY;
            const double radius = std::sqrt(dx * dx + dy * dy);
            if (radius >= threshold) {
                highEnergy += row[x];
            }
        }
    }

    return std::clamp(highEnergy / totalEnergy, 0.0, 1.0);
}
}

FocusEvaluator::Metrics FocusEvaluator::evaluate(const QImage& frame, const QRect& roi) const {
    Metrics metrics;
    if (frame.isNull() || frame.format() == QImage::Format_Invalid) {
        return metrics;
    }

    QRect clippedRoi = clampRoi(roi, frame.size());
    if (clippedRoi.width() < 8 || clippedRoi.height() < 8) {
        clippedRoi = QRect(QPoint(0, 0), frame.size());
    }

    cv::Mat wrapped(frame.height(), frame.width(), CV_8UC1, const_cast<uchar*>(frame.bits()), frame.bytesPerLine());
    cv::Rect cvRoi(clippedRoi.x(), clippedRoi.y(), clippedRoi.width(), clippedRoi.height());
    cv::Mat gray = wrapped(cvRoi).clone();

    if (gray.empty()) {
        return metrics;
    }

    metrics.valid = true;

    cv::Scalar meanScalar;
    cv::Scalar stdScalar;
    cv::meanStdDev(gray, meanScalar, stdScalar);
    metrics.meanIntensity = meanScalar[0];
    metrics.contrast = stdScalar[0];

    cv::Mat grayFloat;
    gray.convertTo(grayFloat, CV_32F, 1.0 / 255.0);

    const std::array<double, 3> scaleWeights{0.5, 0.3, 0.2};
    cv::Mat current = grayFloat.clone();
    double tenengradAccum = 0.0;
    double laplacianAccum = 0.0;
    double weightAccum = 0.0;

    for (size_t i = 0; i < scaleWeights.size(); ++i) {
        const double weight = scaleWeights[i];
        if (current.rows < 16 || current.cols < 16) {
            break;
        }

        cv::Mat sobelX;
        cv::Mat sobelY;
        cv::Sobel(current, sobelX, CV_32F, 1, 0, 3);
        cv::Sobel(current, sobelY, CV_32F, 0, 1, 3);
        cv::Mat gradMag;
        cv::magnitude(sobelX, sobelY, gradMag);
        cv::Mat gradMagSq;
        cv::multiply(gradMag, gradMag, gradMagSq);
        const double tenVal = cv::mean(gradMagSq)[0];

        cv::Mat laplace;
        cv::Laplacian(current, laplace, CV_32F);
        cv::Scalar lapMean;
        cv::Scalar lapStd;
        cv::meanStdDev(laplace, lapMean, lapStd);
        const double lapVar = lapStd[0] * lapStd[0];

        tenengradAccum += weight * tenVal;
        laplacianAccum += weight * lapVar;
        weightAccum += weight;

        if (i + 1 < scaleWeights.size()) {
            cv::pyrDown(current, current);
        }
    }

    if (weightAccum > 0.0) {
        const double averagedTenengrad = tenengradAccum / weightAccum;
        const double averagedLaplacian = laplacianAccum / weightAccum;
        metrics.tenengrad = averagedTenengrad * 1000.0;
        metrics.laplacianVariance = averagedLaplacian * 1000.0;
    }

    metrics.highFrequencyRatio = computeHighFrequencyRatio(grayFloat);

    cv::Mat sobelXFull;
    cv::Mat sobelYFull;
    cv::Sobel(grayFloat, sobelXFull, CV_32F, 1, 0, 3);
    cv::Sobel(grayFloat, sobelYFull, CV_32F, 0, 1, 3);
    cv::Mat gradMagFull;
    cv::magnitude(sobelXFull, sobelYFull, gradMagFull);
    const double gradMagnitudeSum = cv::sum(gradMagFull)[0] + 1e-9;

    cv::Mat phase;
    cv::phase(sobelXFull, sobelYFull, phase, false);
    cv::Mat doublePhase;
    phase.convertTo(doublePhase, CV_32F, 2.0);
    cv::Mat cosComponent(doublePhase.size(), CV_32F);
    cv::Mat sinComponent(doublePhase.size(), CV_32F);
    for (int y = 0; y < doublePhase.rows; ++y) {
        const float* phaseRow = doublePhase.ptr<float>(y);
        float* cosRow = cosComponent.ptr<float>(y);
        float* sinRow = sinComponent.ptr<float>(y);
        for (int x = 0; x < doublePhase.cols; ++x) {
            cosRow[x] = std::cos(phaseRow[x]);
            sinRow[x] = std::sin(phaseRow[x]);
        }
    }

    const double cosAccum = cv::sum(cosComponent.mul(gradMagFull))[0];
    const double sinAccum = cv::sum(sinComponent.mul(gradMagFull))[0];
    const double coherence = std::sqrt(cosAccum * cosAccum + sinAccum * sinAccum) / gradMagnitudeSum;
    metrics.gradientUniformity = std::clamp(1.0 - coherence, 0.0, 1.0);

    const double totalPixels = static_cast<double>(gray.total());
    if (totalPixels > 0.0) {
        cv::Mat maskHighlight;
        cv::threshold(gray, maskHighlight, kHighlightThreshold - 1.0, 1.0, cv::THRESH_BINARY);
        cv::Mat maskShadow;
        cv::threshold(gray, maskShadow, kShadowThreshold, 1.0, cv::THRESH_BINARY_INV);
        metrics.highlightRatio = cv::sum(maskHighlight)[0] / totalPixels * 100.0;
        metrics.shadowRatio = cv::sum(maskShadow)[0] / totalPixels * 100.0;
    }

    const double brightnessError = (metrics.meanIntensity - kBrightnessTarget) / kBrightnessTarget;
    const double brightnessScore = std::exp(-3.0 * brightnessError * brightnessError);

    const double tenNorm = normalizeMetric(metrics.tenengrad, 6.0, 60.0);
    const double lapNorm = normalizeMetric(metrics.laplacianVariance, 8.0, 140.0);
    const double hfNorm = normalizeMetric(metrics.highFrequencyRatio, 0.10, 0.34);
    const double contrastNorm = normalizeMetric(metrics.contrast, 7.0, 42.0);
    const double uniformNorm = normalizeMetric(metrics.gradientUniformity, 0.35, 0.92);

    const double structureScore = 0.35 * tenNorm + 0.3 * lapNorm + 0.2 * hfNorm + 0.15 * contrastNorm;
    const double uniformityScore = uniformNorm;

    const double highlightPenalty = std::clamp(metrics.highlightRatio / 7.0, 0.0, 1.0);
    const double shadowPenalty = std::clamp(metrics.shadowRatio / 12.0, 0.0, 1.0);
    const double penaltyFactor = std::clamp(1.0 - (0.45 * highlightPenalty + 0.25 * shadowPenalty), 0.35, 1.0);

    const double composite = std::clamp((0.82 * structureScore + 0.18 * uniformityScore) * brightnessScore * penaltyFactor, 0.0, 1.15);
    metrics.compositeScore = std::clamp(composite * 100.0, 0.0, 100.0);

    return metrics;
}

QRect FocusEvaluator::clampRoi(const QRect& roi, const QSize& frameSize) {
    QRect frameRect(QPoint(0, 0), frameSize);
    return roi.intersected(frameRect);
}
