#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <QObject>
#include <QString>
#include <QThreadPool>
#include <QtConcurrent>
#include <QFuture>
#include <QFutureWatcher>

#include <atomic>

#include <opencv2/core.hpp>

#include "BoardDetector.h"
#include "BoardSpec.h"
#include "DetectionResult.h"

namespace mycalib {

struct CalibrationMetrics {
    double rms {0.0};
    double meanErrorPx {0.0};
    double medianErrorPx {0.0};
    double maxErrorPx {0.0};
    double stdErrorPx {0.0};
    double p95ErrorPx {0.0};
    cv::Vec3d meanTranslationMm {0.0, 0.0, 0.0};
    cv::Vec3d stdTranslationMm {0.0, 0.0, 0.0};
    cv::Vec3d meanResidualMm {0.0, 0.0, 0.0};
    cv::Vec3d rmsResidualMm {0.0, 0.0, 0.0};
    cv::Vec3d meanResidualPercent {0.0, 0.0, 0.0};
    cv::Vec3d rmsResidualPercent {0.0, 0.0, 0.0};
};

struct HeatmapBundle {
    cv::Mat boardCoverage;
    cv::Mat pixelError;
    cv::Mat boardError;
    cv::Mat residualScatter;
    cv::Mat distortionMap;
    std::vector<std::vector<cv::Point2f>> distortionGrid;
    cv::Mat boardCoverageScalar;
    cv::Mat pixelErrorScalar;
    cv::Mat boardErrorScalar;
    cv::Mat distortionScalar;
    cv::Mat distortionVectors;
    double boardCoverageMin {0.0};
    double boardCoverageMax {1.0};
    double pixelErrorMin {0.0};
    double pixelErrorMax {1.0};
    double boardErrorMin {0.0};
    double boardErrorMax {1.0};
    double residualScatterMax {0.0};
    double distortionMin {0.0};
    double distortionMax {0.0};
};

struct CalibrationOutput {
    bool success {false};
    QString message;
    cv::Mat cameraMatrix;
    cv::Mat distCoeffs;
    cv::Size imageSize {0, 0};
    std::vector<DetectionResult> allDetections;
    std::vector<DetectionResult> keptDetections;
    std::vector<DetectionResult> removedDetections;
    CalibrationMetrics metrics;
    HeatmapBundle heatmaps;
};

class CalibrationEngine : public QObject {
    Q_OBJECT

public:
    struct Settings {
        BoardSpec boardSpec;
        double maxMeanErrorPx {3.0};
        double maxPointErrorPx {12.0};
        int maxIterations {3};
        int minSamples {12};
        bool enableRefinement {true};
    };

    static QString resolveOutputDirectory(const QString &requestedPath);

    explicit CalibrationEngine(QObject *parent = nullptr);
    ~CalibrationEngine() override;

    void run(const QString &imageDirectory,
             const Settings &settings,
             const QString &outputDirectory);

    CalibrationOutput runBlocking(const QString &imageDirectory,
                                  const Settings &settings,
                                  const QString &outputDirectory);

    void cancelAndWait();
    bool isRunning() const;

Q_SIGNALS:
    void progressUpdated(int processed, int total);
    void statusChanged(const QString &message);
    void finished(const CalibrationOutput &output);
    void failed(const QString &reason);

private:
    QString m_directory;
    Settings m_settings;
    QString m_outputDirectory;
    BoardDetector m_detector;
    QFutureWatcher<CalibrationOutput> *m_watcher {nullptr};
    QFuture<CalibrationOutput> m_future;
    std::atomic_bool m_abortRequested {false};

    CalibrationOutput executePipeline();
    std::vector<std::string> collectImagePaths(const QString &directory) const;
    DetectionResult detectBoard(const std::string &path) const;
    CalibrationOutput calibrate(const std::vector<DetectionResult> &detections) const;
    CalibrationOutput filterAndRecalibrate(CalibrationOutput &&input) const;
    void exportReport(const CalibrationOutput &output) const;
    void exportHeatmap(const cv::Mat &heatmap, const QString &path) const;

    static void ensureDirectory(const QString &path);
    bool shouldAbort() const;
};

} // namespace mycalib
