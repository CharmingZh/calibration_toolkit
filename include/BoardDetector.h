#pragma once

#include <optional>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

#include "BoardSpec.h"
#include "DetectionResult.h"

namespace mycalib {

struct DetectionConfig {
    double quadExpandScale {1.03};
    double quadExpandOffset {12.0};
    int warpMinShort {1400};
    int warpMinDim {400};
    double houghGaussianSigma {1.0};
    double houghCannyLowRatio {0.66};
    int houghCannyLowMin {10};
    double houghCannyHighRatio {2.0};
    int houghDilateKernel {3};
    int houghDilateIterations {1};
    double houghVotesRatio {0.006};
    double houghMinLineRatio {0.30};
    double houghMaxGapRatio {0.03};
    double houghOrientationTol {5.0};
    double houghOrthogonalityTol {10.0};
    double houghRhoNmsRatio {0.02};
    int houghKmeansMaxIter {200};
    double houghKmeansEps {1e-4};
    int houghKmeansAttempts {4};
    double quadMargin {0.005};
    double quadAreaMinRatio {0.002};
    double quadAreaMaxRatio {0.80};
    double quadAspectMin {0.90};
    double quadAspectMax {1.60};
    int quadEdgeHalf {6};
    int quadEdgeSamples {48};
    double quadEdgeMinContrast {0.5};
    double quadAreaBonus {300.0};
    double whiteGaussianSigma {1.2};
    int whiteMorphKernel {11};
    int whiteMorphIterations {1};
    double whiteApproxEpsRatio {0.0125};
    double whiteApproxExpand {1.3};
    double whiteApproxShrink {0.7};
    double areaRelaxDefault {0.12};
    double areaRelaxSmall {0.14};
    double areaRelaxBig {0.14};
    double areaRelaxReassignBig {0.20};
    int areaIterations {8};
    double claheClipLimit {2.0};
    cv::Size claheTileGrid {8, 8};
    cv::Size rectBlurKernel {3, 3};
    double blobMinArea {450.0};
    double blobMaxArea {26000.0};
    bool blobDark {true};
    double blobMinCircularity {0.45};
    double blobMinConvexity {0.45};
    double blobMinInertia {0.04};
    double blobMinThreshold {5.0};
    double blobMaxThreshold {220.0};
    double blobThresholdStep {5.0};
    double blobMinDist {10.0};
    double refineGate {0.6};
    double refineWinScale {3.0};
    double refineWinMin {30.0};
    double refineWinMax {220.0};
    int refineSegmentKsize {3};
    cv::Size refineOpenKernel {3, 3};
    int fallbackCannyLow {30};
    int fallbackCannyHigh {90};
};

class BoardDetector {
public:
    explicit BoardDetector(const DetectionConfig &config = DetectionConfig());

    DetectionResult detect(const cv::Mat &gray, const BoardSpec &spec, const std::string &name) const;

private:
    DetectionConfig m_cfg;
};

} // namespace mycalib
