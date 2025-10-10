#pragma once

#include <QImage>
#include <QRect>

class FocusEvaluator {
public:
    struct Metrics {
        bool valid{false};
        double meanIntensity{0.0};
        double contrast{0.0};
        double laplacianVariance{0.0};
        double tenengrad{0.0};
        double highFrequencyRatio{0.0};
        double gradientUniformity{0.0};
        double highlightRatio{0.0};
        double shadowRatio{0.0};
        double compositeScore{0.0};
    };

    Metrics evaluate(const QImage& frame, const QRect& roi = {}) const;

private:
    static QRect clampRoi(const QRect& roi, const QSize& frameSize);
};
