#include "CalibrationEngine.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <numeric>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QtConcurrent/QtConcurrent>

#include <opencv2/calib3d.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "HeatmapGenerator.h"
#include "PaperFigureExporter.h"
#include "ImageLoader.h"
#include "Logger.h"

namespace fs = std::filesystem;

namespace mycalib {

namespace {

QJsonArray matToJson(const cv::Mat &mat)
{
    QJsonArray outer;
    for (int r = 0; r < mat.rows; ++r) {
        QJsonArray row;
        for (int c = 0; c < mat.cols; ++c) {
            row.append(mat.at<double>(r, c));
        }
        outer.append(row);
    }
    return outer;
}

QString makeProgressBar(int current, int total)
{
    const int width = 30;
    if (total <= 0) {
        return QString(width, QChar('.'));
    }
    const double ratio = std::clamp(static_cast<double>(current) / static_cast<double>(total), 0.0, 1.0);
    int filled = static_cast<int>(std::round(ratio * width));
    filled = std::clamp(filled, 0, width);

    QString bar(width, QChar('.'));
    for (int i = 0; i < filled; ++i) {
        bar[i] = QChar('#');
    }
    return bar;
}

void computeResiduals(const cv::Mat &cameraMatrix,
                      const cv::Mat &distCoeffs,
                      const std::vector<std::vector<cv::Point3f>> &objectPoints,
                      const std::vector<std::vector<cv::Point2f>> &imagePoints,
                      std::vector<DetectionResult> &detections,
                      const std::vector<cv::Mat> &rvecs,
                      const std::vector<cv::Mat> &tvecs)
{
    Q_UNUSED(imagePoints);
    for (size_t idx = 0; idx < detections.size(); ++idx) {
        auto &rec = detections[idx];
        if (!rec.success) {
            continue;
        }
        cv::Mat projected;
        cv::projectPoints(objectPoints[idx], rvecs[idx], tvecs[idx], cameraMatrix, distCoeffs, projected);
        rec.residualsPx.clear();
        rec.residualVectors.clear();
        rec.residualCameraMm.clear();
        rec.residualCameraPercent.clear();
        for (int i = 0; i < projected.rows; ++i) {
            const cv::Point2f &obs = rec.imagePoints[i];
            const cv::Point2f proj = projected.at<cv::Point2f>(i);
            const cv::Point2f delta = obs - proj;
            const double err = cv::norm(delta);
            rec.residualsPx.push_back(err);
            rec.residualVectors.push_back(delta);
        }
        if (!rec.imagePoints.empty()) {
            std::vector<cv::Point2f> undistorted;
            cv::undistortPoints(rec.imagePoints, undistorted, cameraMatrix, distCoeffs);

            cv::Mat RMat;
            cv::Rodrigues(rvecs[idx], RMat);
            cv::Matx33d R(RMat);
            cv::Vec3d tvec;
            for (int i = 0; i < 3; ++i) {
                tvec[i] = tvecs[idx].at<double>(i, 0);
            }
            const cv::Vec3d planeNormal = R * cv::Vec3d(0.0, 0.0, 1.0);
            const double planeOffset = planeNormal.dot(tvec);

            rec.residualCameraMm.reserve(rec.imagePoints.size());
            rec.residualCameraPercent.reserve(rec.imagePoints.size());

            cv::Vec3d sumAbs{0.0, 0.0, 0.0};
            cv::Vec3d sumAbsPct{0.0, 0.0, 0.0};

            for (size_t i = 0; i < rec.imagePoints.size(); ++i) {
                cv::Vec3d dir(static_cast<double>(undistorted[i].x),
                              static_cast<double>(undistorted[i].y),
                              1.0);
                const double denom = planeNormal.dot(dir);
                cv::Vec3d deltaCam{0.0, 0.0, 0.0};
                cv::Vec3d percent{0.0, 0.0, 0.0};
                if (std::abs(denom) > 1e-9) {
                    const double lambda = planeOffset / denom;
                    const cv::Vec3d pcObserved = dir * lambda;
                    const cv::Point3f &obj = objectPoints[idx][i];
                    const cv::Vec3d pcExpected = R * cv::Vec3d(obj.x, obj.y, obj.z) + tvec;
                    deltaCam = pcObserved - pcExpected;

                    const cv::Vec3d denomVec{std::max(5.0, std::abs(pcExpected[0])),
                                             std::max(5.0, std::abs(pcExpected[1])),
                                             std::max(5.0, std::abs(pcExpected[2]))};
                    percent = cv::Vec3d(deltaCam[0] / denomVec[0],
                                         deltaCam[1] / denomVec[1],
                                         deltaCam[2] / denomVec[2]) * 100.0;
                }
                rec.residualCameraMm.push_back(deltaCam);
                rec.residualCameraPercent.push_back(percent);
                sumAbs += cv::Vec3d(std::abs(deltaCam[0]), std::abs(deltaCam[1]), std::abs(deltaCam[2]));
                sumAbsPct += cv::Vec3d(std::abs(percent[0]), std::abs(percent[1]), std::abs(percent[2]));
            }

            const double invCount = rec.residualCameraMm.empty() ? 0.0 : 1.0 / static_cast<double>(rec.residualCameraMm.size());
            rec.meanResidualCameraMm = sumAbs * invCount;
            rec.meanResidualCameraPercent = sumAbsPct * invCount;
        } else {
            rec.meanResidualCameraMm = cv::Vec3d(0.0, 0.0, 0.0);
            rec.meanResidualCameraPercent = cv::Vec3d(0.0, 0.0, 0.0);
        }
        cv::Vec3d trans;
        for (int i = 0; i < 3; ++i) {
            trans[i] = tvecs[idx].at<double>(i, 0);
        }
        rec.translationMm = trans;

        cv::Mat R;
        cv::Rodrigues(rvecs[idx], R);
        cv::Matx33d matrix(R);
    rec.rotationMatrix = matrix;
        const double sy = std::sqrt(matrix(0, 0) * matrix(0, 0) + matrix(1, 0) * matrix(1, 0));
        bool singular = sy < 1e-6;
        double x, y, z;
        if (!singular) {
            x = std::atan2(matrix(2, 1), matrix(2, 2));
            y = std::atan2(-matrix(2, 0), sy);
            z = std::atan2(matrix(1, 0), matrix(0, 0));
        } else {
            x = std::atan2(-matrix(1, 2), matrix(1, 1));
            y = std::atan2(-matrix(2, 0), sy);
            z = 0.0;
        }
        rec.rotationDeg = {x * 180.0 / CV_PI, y * 180.0 / CV_PI, z * 180.0 / CV_PI};
    }
}

CalibrationMetrics summarize(const std::vector<DetectionResult> &detections)
{
    CalibrationMetrics metrics;
    std::vector<double> residuals;
    std::vector<cv::Vec3d> translations;
    cv::Vec3d sumAbsResidualMm{0.0, 0.0, 0.0};
    cv::Vec3d sumSqResidualMm{0.0, 0.0, 0.0};
    cv::Vec3d sumAbsResidualPct{0.0, 0.0, 0.0};
    cv::Vec3d sumSqResidualPct{0.0, 0.0, 0.0};
    std::array<int, 3> componentCountMm{0, 0, 0};
    std::array<int, 3> componentCountPct{0, 0, 0};

    for (const auto &rec : detections) {
        if (!rec.success || rec.residualsPx.empty()) {
            continue;
        }
        residuals.insert(residuals.end(), rec.residualsPx.begin(), rec.residualsPx.end());
        translations.push_back(rec.translationMm);

        for (const auto &vec : rec.residualCameraMm) {
            for (int axis = 0; axis < 3; ++axis) {
                const double value = std::abs(vec[axis]);
                sumAbsResidualMm[axis] += value;
                sumSqResidualMm[axis] += vec[axis] * vec[axis];
                ++componentCountMm[axis];
            }
        }
        for (const auto &vec : rec.residualCameraPercent) {
            for (int axis = 0; axis < 3; ++axis) {
                const double value = std::abs(vec[axis]);
                sumAbsResidualPct[axis] += value;
                sumSqResidualPct[axis] += vec[axis] * vec[axis];
                ++componentCountPct[axis];
            }
        }
    }

    if (!residuals.empty()) {
        const double sum = std::accumulate(residuals.begin(), residuals.end(), 0.0);
        metrics.meanErrorPx = sum / static_cast<double>(residuals.size());

        std::vector<double> sorted = residuals;
        std::sort(sorted.begin(), sorted.end());
        const size_t midIdx = sorted.size() / 2;
        if (sorted.size() % 2 == 0 && sorted.size() >= 2) {
            metrics.medianErrorPx = 0.5 * (sorted[midIdx - 1] + sorted[midIdx]);
        } else {
            metrics.medianErrorPx = sorted[midIdx];
        }

        const size_t p95Index = static_cast<size_t>(std::ceil(0.95 * std::max<size_t>(sorted.size() - 1, 0))); // guard size=1
        metrics.p95ErrorPx = sorted[std::min(p95Index, sorted.size() - 1)];
        metrics.maxErrorPx = sorted.back();

        double variance = 0.0;
        for (double v : residuals) {
            const double diff = v - metrics.meanErrorPx;
            variance += diff * diff;
        }
        variance /= static_cast<double>(residuals.size());
        metrics.stdErrorPx = std::sqrt(std::max(0.0, variance));
    }
    if (!translations.empty()) {
        cv::Vec3d sum{0.0, 0.0, 0.0};
        for (const auto &t : translations) {
            sum += t;
        }
        metrics.meanTranslationMm = sum / static_cast<double>(translations.size());
        cv::Vec3d sq{0.0, 0.0, 0.0};
        for (const auto &t : translations) {
            cv::Vec3d diff = t - metrics.meanTranslationMm;
            sq[0] += diff[0] * diff[0];
            sq[1] += diff[1] * diff[1];
            sq[2] += diff[2] * diff[2];
        }
        metrics.stdTranslationMm = cv::Vec3d{std::sqrt(sq[0] / translations.size()),
                                             std::sqrt(sq[1] / translations.size()),
                                             std::sqrt(sq[2] / translations.size())};
    }

    for (int axis = 0; axis < 3; ++axis) {
        if (componentCountMm[axis] > 0) {
            metrics.meanResidualMm[axis] = sumAbsResidualMm[axis] / static_cast<double>(componentCountMm[axis]);
            metrics.rmsResidualMm[axis] = std::sqrt(sumSqResidualMm[axis] / static_cast<double>(componentCountMm[axis]));
        }
        if (componentCountPct[axis] > 0) {
            metrics.meanResidualPercent[axis] = sumAbsResidualPct[axis] / static_cast<double>(componentCountPct[axis]);
            metrics.rmsResidualPercent[axis] = std::sqrt(sumSqResidualPct[axis] / static_cast<double>(componentCountPct[axis]));
        }
    }
    return metrics;
}

} // namespace

QString CalibrationEngine::resolveOutputDirectory(const QString &requested)
{
    const QString trimmed = requested.trimmed();
    const QDir appDir(QCoreApplication::applicationDirPath());

    if (trimmed.isEmpty()) {
        return QDir(QDir::homePath() + QStringLiteral("/outputs")).absolutePath();
    }

    const QFileInfo info(trimmed);
    if (info.isAbsolute()) {
        return QDir(info.absoluteFilePath()).absolutePath();
    }

    const QString resolved = appDir.absoluteFilePath(trimmed);
    return QDir(QFileInfo(resolved).absoluteFilePath()).absolutePath();
}

CalibrationEngine::CalibrationEngine(QObject *parent)
    : QObject(parent)
    , m_watcher(new QFutureWatcher<CalibrationOutput>(this))
{
    connect(m_watcher, &QFutureWatcher<CalibrationOutput>::finished, this, [this]() {
        if (!m_future.isValid()) {
            return;
        }

        const CalibrationOutput result = m_future.result();
        m_future = QFuture<CalibrationOutput>();

        if (shouldAbort()) {
            return;
        }

        if (result.success) {
            Q_EMIT finished(result);
        } else {
            Q_EMIT failed(result.message);
        }
    });
}

CalibrationEngine::~CalibrationEngine()
{
    cancelAndWait();
}

void CalibrationEngine::run(const QString &imageDirectory,
                            const Settings &settings,
                            const QString &outputDirectory)
{
    if (isRunning()) {
        Logger::warning(QStringLiteral("Calibration already running; ignoring duplicate request."));
        return;
    }

    m_abortRequested.store(false, std::memory_order_release);
    m_directory = imageDirectory;
    m_settings = settings;
    m_outputDirectory = resolveOutputDirectory(outputDirectory);
    ensureDirectory(m_outputDirectory);

    m_future = QtConcurrent::run(&CalibrationEngine::executePipeline, this);
    m_watcher->setFuture(m_future);
}

CalibrationOutput CalibrationEngine::runBlocking(const QString &imageDirectory,
                                                 const Settings &settings,
                                                 const QString &outputDirectory)
{
    if (isRunning()) {
        cancelAndWait();
    }

    m_abortRequested.store(false, std::memory_order_release);
    m_directory = imageDirectory;
    m_settings = settings;
    m_outputDirectory = resolveOutputDirectory(outputDirectory);
    ensureDirectory(m_outputDirectory);
    return executePipeline();
}

void CalibrationEngine::cancelAndWait()
{
    m_abortRequested.store(true, std::memory_order_release);

    if (!m_watcher) {
        return;
    }

    if (m_watcher->isRunning()) {
        m_watcher->cancel();
        if (m_future.isValid()) {
            m_future.waitForFinished();
        } else {
            m_watcher->waitForFinished();
        }
    }

    if (m_future.isValid()) {
        m_future = QFuture<CalibrationOutput>();
    }
}

bool CalibrationEngine::isRunning() const
{
    return m_watcher && m_watcher->isRunning();
}

bool CalibrationEngine::shouldAbort() const
{
    return m_abortRequested.load(std::memory_order_acquire);
}

CalibrationOutput CalibrationEngine::executePipeline()
{
    CalibrationOutput output;
    output.success = false;

    bool abortLogged = false;
    auto abortGuard = [this, &output, &abortLogged]() -> bool {
        if (!shouldAbort()) {
            return false;
        }
        if (!abortLogged) {
            Logger::warning(QStringLiteral("Calibration aborted on request."));
            abortLogged = true;
        }
        output.message = tr("Calibration aborted");
        return true;
    };

    try {
        if (abortGuard()) {
            return output;
        }
    Logger::info(QStringLiteral("=== Calibration task started ==="));
    Logger::info(QStringLiteral("Input directory: %1").arg(m_directory));
    Logger::info(QStringLiteral("Output directory: %1").arg(m_outputDirectory));
    Logger::info(QStringLiteral("Board specification: small circle Ø=%1 mm, spacing=%2 mm, expected circles=%3")
                         .arg(m_settings.boardSpec.smallDiameterMm, 0, 'f', 2)
                         .arg(m_settings.boardSpec.centerSpacingMm, 0, 'f', 2)
                         .arg(static_cast<int>(m_settings.boardSpec.expectedCircleCount())));
    Logger::info(QStringLiteral("Detection settings: refinement=%1 | mean threshold=%2 px | point threshold=%3 px | min samples=%4 | max iterations=%5")
             .arg(m_settings.enableRefinement ? QStringLiteral("enabled") : QStringLiteral("disabled"))
                         .arg(m_settings.maxMeanErrorPx, 0, 'f', 2)
                         .arg(m_settings.maxPointErrorPx, 0, 'f', 2)
                         .arg(m_settings.minSamples)
                         .arg(m_settings.maxIterations));

        Q_EMIT statusChanged(tr("Collecting images"));
        const auto paths = collectImagePaths(m_directory);
        if (paths.empty()) {
            output.message = tr("No images found in directory");
            return output;
        }

        if (abortGuard()) {
            return output;
        }

        std::vector<DetectionResult> detections;
        detections.reserve(paths.size());

        const auto total = static_cast<int>(paths.size());
    Logger::info(QStringLiteral("Collected %1 images, starting detection...").arg(total));
        for (int idx = 0; idx < total; ++idx) {
            if (abortGuard()) {
                return output;
            }

            Q_EMIT statusChanged(tr("Detecting board %1/%2").arg(idx + 1).arg(total));
    DetectionResult result = detectBoard(paths[idx]);
            detections.push_back(result);
            Q_EMIT progressUpdated(idx + 1, total);

            const int processed = idx + 1;
            Logger::info(QStringLiteral("[Progress] [%1] %2/%3")
                             .arg(makeProgressBar(processed, total))
                             .arg(processed)
                             .arg(total));
        }

        int successCount = 0;
        int failureCount = 0;
        int totalSmallCircles = 0;
        int totalBigCircles = 0;
        std::vector<double> durationsMs;
        durationsMs.reserve(detections.size());

        for (const auto &rec : detections) {
            const double elapsed = std::chrono::duration<double, std::milli>(rec.elapsed).count();
            if (elapsed > 0.0) {
                durationsMs.push_back(elapsed);
            }
            if (rec.success) {
                ++successCount;
                totalSmallCircles += static_cast<int>(rec.imagePoints.size());
                totalBigCircles += rec.bigCircleCount;
            } else {
                ++failureCount;
            }
        }

        const double successRate = total > 0 ? static_cast<double>(successCount) * 100.0 / total : 0.0;
        Logger::info(QStringLiteral("=== Detection summary ==="));
        Logger::info(QStringLiteral("Processed images: %1 | success: %2 | failure: %3 | success rate: %4%%")
                         .arg(total)
                         .arg(successCount)
                         .arg(failureCount)
                         .arg(successRate, 0, 'f', 2));

        if (successCount > 0) {
            const double avgSmall = static_cast<double>(totalSmallCircles) / successCount;
            const double avgBig = successCount > 0 ? static_cast<double>(totalBigCircles) / successCount : 0.0;
            Logger::info(QStringLiteral("Small circle detections: total %1 | mean %2")
                             .arg(totalSmallCircles)
                             .arg(avgSmall, 0, 'f', 2));
            Logger::info(QStringLiteral("Big circle detections: total %1 | mean %2")
                             .arg(totalBigCircles)
                             .arg(avgBig, 0, 'f', 2));
        }

        if (!durationsMs.empty()) {
            const double sum = std::accumulate(durationsMs.begin(), durationsMs.end(), 0.0);
            const auto [minIt, maxIt] = std::minmax_element(durationsMs.begin(), durationsMs.end());
            const double avg = sum / durationsMs.size();
            Logger::info(QStringLiteral("Timing (ms): mean=%1 | fastest=%2 | slowest=%3 | samples=%4")
                             .arg(avg, 0, 'f', 2)
                             .arg(*minIt, 0, 'f', 2)
                             .arg(*maxIt, 0, 'f', 2)
                             .arg(durationsMs.size()));
        }

        if (failureCount > 0) {
            Logger::warning(QStringLiteral("Failed detections:"));
            for (const auto &rec : detections) {
                if (!rec.success) {
                    Logger::warning(QStringLiteral(" - %1: %2")
                                        .arg(QString::fromStdString(rec.name))
                                        .arg(QString::fromStdString(rec.message)));
                }
            }
        }
        Logger::info(QStringLiteral("=== Detection summary complete ==="));

        if (abortGuard()) {
            return output;
        }

        Q_EMIT statusChanged(tr("Calibrating camera"));
        output = calibrate(detections);
        if (!output.success) {
            return output;
        }

        if (abortGuard()) {
            return output;
        }

        Q_EMIT statusChanged(tr("Filtering outliers"));
        output = filterAndRecalibrate(std::move(output));
        if (!output.success) {
            return output;
        }

        if (abortGuard()) {
            return output;
        }

        Q_EMIT statusChanged(tr("Generating heatmaps"));
        HeatmapGenerator generator;
        output.heatmaps.boardCoverage = generator.buildBoardCoverage(output.keptDetections,
                                                                     output.imageSize,
                                                                     &output.heatmaps.boardCoverageMin,
                                                                     &output.heatmaps.boardCoverageMax,
                                                                     &output.heatmaps.boardCoverageScalar);
        output.heatmaps.pixelError = generator.buildPixelErrorHeatmap(output.keptDetections,
                                                                      output.imageSize,
                                                                      &output.heatmaps.pixelErrorMin,
                                                                      &output.heatmaps.pixelErrorMax,
                                                                      &output.heatmaps.pixelErrorScalar);
        output.heatmaps.boardError = generator.buildBoardErrorHeatmap(output.keptDetections,
                                                                      output.imageSize,
                                                                      &output.heatmaps.boardErrorMin,
                                                                      &output.heatmaps.boardErrorMax,
                                                                      &output.heatmaps.boardErrorScalar);
        output.heatmaps.residualScatter = generator.buildResidualScatter(output.keptDetections,
                                                                         &output.heatmaps.residualScatterMax);
        output.heatmaps.distortionMap = generator.buildDistortionHeatmap(output.cameraMatrix,
                                                                         output.distCoeffs,
                                                                         output.imageSize,
                                                                         &output.heatmaps.distortionMin,
                                                                         &output.heatmaps.distortionMax,
                                                                         &output.heatmaps.distortionGrid,
                                                                         &output.heatmaps.distortionScalar,
                                                                         &output.heatmaps.distortionVectors);

    Q_EMIT statusChanged(tr("Exporting report"));
        exportReport(output);

        if (!output.heatmaps.boardCoverage.empty()) {
            exportHeatmap(output.heatmaps.boardCoverage, m_outputDirectory + "/board_coverage_heatmap.png");
        }
        if (!output.heatmaps.pixelError.empty()) {
            exportHeatmap(output.heatmaps.pixelError, m_outputDirectory + "/reprojection_error_heatmap_pixels.png");
        }
        if (!output.heatmaps.boardError.empty()) {
            exportHeatmap(output.heatmaps.boardError, m_outputDirectory + "/reprojection_error_heatmap_board.png");
        }
        if (!output.heatmaps.residualScatter.empty()) {
            exportHeatmap(output.heatmaps.residualScatter, m_outputDirectory + "/reprojection_error_scatter.png");
        }
        if (!output.heatmaps.distortionMap.empty()) {
            exportHeatmap(output.heatmaps.distortionMap, m_outputDirectory + "/distortion_heatmap.png");
        }

        PaperFigureExporter::exportAll(output, m_outputDirectory);

        output.success = true;
        output.message = tr("Calibration complete");
        return output;
    } catch (const std::exception &ex) {
        output.success = false;
        output.message = QString::fromUtf8(ex.what());
        return output;
    }
}

std::vector<std::string> CalibrationEngine::collectImagePaths(const QString &directory) const
{
    ImageLoader loader;
    return loader.gatherImageFiles(directory.toStdString());
}

DetectionResult CalibrationEngine::detectBoard(const std::string &path) const
{
    DetectionResult result;
    result.name = fs::path(path).stem().string();

    const auto start = std::chrono::steady_clock::now();
    try {
        ImageLoader loader;
        cv::Mat gray = loader.loadImage(path);
        auto detection = m_detector.detect(gray, m_settings.boardSpec, result.name);
        const double elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        detection.elapsed = std::chrono::milliseconds(static_cast<int64_t>(elapsedMs));
        detection.resolution = gray.size();
        if (detection.success) {
            Logger::info(QStringLiteral("[OK] %1 completed in %2 ms (small circles=%3, large circles=%4)")
                             .arg(QString::fromStdString(result.name))
                             .arg(QString::number(elapsedMs, 'f', 2))
                             .arg(detection.imagePoints.size())
                             .arg(detection.bigCircleCount));
        } else {
            Logger::warning(QStringLiteral("[FAIL] %1: %2")
                                .arg(QString::fromStdString(result.name))
                                .arg(QString::fromStdString(detection.message)));
        }
        return detection;
    } catch (const std::exception &ex) {
        result.success = false;
        result.message = std::string("native_detection_exception: ") + ex.what();
        result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
        return result;
    } catch (...) {
        result.success = false;
        result.message = "native_detection_unknown_exception";
        result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
        return result;
    }
}

CalibrationOutput CalibrationEngine::calibrate(const std::vector<DetectionResult> &detections) const
{
    CalibrationOutput output;
    output.allDetections = detections;

    std::vector<std::vector<cv::Point3f>> objectPoints;
    std::vector<std::vector<cv::Point2f>> imagePoints;
    std::vector<DetectionResult> usable;

    for (const auto &rec : detections) {
        if (!rec.success || rec.imagePoints.empty()) {
            continue;
        }
        usable.push_back(rec);
        objectPoints.push_back(rec.objectPoints);
        imagePoints.push_back(rec.imagePoints);
    }

    if (usable.size() < 3) {
        output.success = false;
        output.message = tr("Not enough valid detections (%1)").arg(usable.size());
        return output;
    }

    cv::Mat cameraMatrix = cv::Mat::eye(3, 3, CV_64F);
    cameraMatrix.at<double>(0, 0) = 2000.0;
    cameraMatrix.at<double>(1, 1) = 2000.0;
    cameraMatrix.at<double>(0, 2) = usable.front().resolution.width / 2.0;
    cameraMatrix.at<double>(1, 2) = usable.front().resolution.height / 2.0;

    cv::Mat distCoeffs = cv::Mat::zeros(8, 1, CV_64F);
    std::vector<cv::Mat> rvecs;
    std::vector<cv::Mat> tvecs;

    const double rms = cv::calibrateCamera(objectPoints, imagePoints, usable.front().resolution,
                                           cameraMatrix, distCoeffs, rvecs, tvecs,
                                           cv::CALIB_RATIONAL_MODEL | cv::CALIB_THIN_PRISM_MODEL | cv::CALIB_TILTED_MODEL);

    std::vector<DetectionResult> enriched = usable;
    computeResiduals(cameraMatrix, distCoeffs, objectPoints, imagePoints, enriched, rvecs, tvecs);

    output.success = true;
    output.cameraMatrix = cameraMatrix;
    output.distCoeffs = distCoeffs;
    output.imageSize = usable.front().resolution;
    output.keptDetections = enriched;
    output.metrics = summarize(enriched);
    output.metrics.rms = rms;
    output.message = tr("Initial calibration complete");

    Logger::info(QStringLiteral("Initial calibration: samples=%1 | RMS=%2 px | Mean=%3 px | Median=%4 px | Max=%5 px")
                     .arg(static_cast<int>(usable.size()))
                     .arg(rms, 0, 'f', 3)
                     .arg(output.metrics.meanErrorPx, 0, 'f', 3)
                     .arg(output.metrics.medianErrorPx, 0, 'f', 3)
                     .arg(output.metrics.maxErrorPx, 0, 'f', 3));

    // merge enriched data back into all detections
    for (auto &rec : output.allDetections) {
        for (const auto &src : enriched) {
            if (rec.name == src.name) {
                rec = src;
                break;
            }
        }
    }

    return output;
}

CalibrationOutput CalibrationEngine::filterAndRecalibrate(CalibrationOutput &&input) const
{
    if (!input.success) {
        return input;
    }

    auto abortGuard = [this, &input]() -> bool {
        if (!shouldAbort()) {
            return false;
        }
        input.success = false;
        input.message = tr("Calibration aborted");
        return true;
    };

    if (abortGuard()) {
        return input;
    }

    auto kept = input.keptDetections;
    std::vector<DetectionResult> removed;

    for (int iteration = 0; iteration < m_settings.maxIterations; ++iteration) {
        if (abortGuard()) {
            return input;
        }
        if (static_cast<int>(kept.size()) < m_settings.minSamples) {
            break;
        }
        double thresholdMean = m_settings.maxMeanErrorPx;
        double thresholdMax = m_settings.maxPointErrorPx;

        std::vector<double> means;
        for (const auto &rec : kept) {
            means.push_back(rec.meanErrorPx());
        }
        if (!means.empty()) {
            std::vector<double> sorted = means;
            std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
            double median = sorted[sorted.size() / 2];
            double mad = 0.0;
            for (double v : means) {
                mad += std::abs(v - median);
            }
            mad /= means.size();
            thresholdMean = std::min(thresholdMean, median + 3.5 * std::max(mad, 1e-3));
        }

        std::vector<DetectionResult> next;
        std::vector<DetectionResult> removedThisIter;
        for (auto &rec : kept) {
            if (rec.meanErrorPx() > thresholdMean || rec.maxErrorPx() > thresholdMax) {
                rec.iterationRemoved = iteration + 1;
                removedThisIter.push_back(rec);
            } else {
                next.push_back(rec);
            }
        }
        if (removedThisIter.empty()) {
            break;
        }
        removed.insert(removed.end(), removedThisIter.begin(), removedThisIter.end());
        QStringList removedNames;
        for (const auto &rec : removedThisIter) {
            removedNames << QStringLiteral("%1 (mean=%2 px, max=%3 px)")
                                .arg(QString::fromStdString(rec.name))
                                .arg(rec.meanErrorPx(), 0, 'f', 3)
                                .arg(rec.maxErrorPx(), 0, 'f', 3);
        }
    Logger::warning(QStringLiteral("Iteration %1 removed %2 samples: %3")
                            .arg(iteration + 1)
                            .arg(removedThisIter.size())
                            .arg(removedNames.join(QStringLiteral(", "))));

        if (static_cast<int>(next.size()) < m_settings.minSamples) {
            break;
        }
        kept = next;

        // Recalibrate with kept samples
        CalibrationOutput recalibrated;
        recalibrated.allDetections = input.allDetections;
        recalibrated.keptDetections = kept;
        recalibrated.success = true;

        std::vector<std::vector<cv::Point3f>> objectPoints;
        std::vector<std::vector<cv::Point2f>> imagePoints;

            if (abortGuard()) {
                return input;
            }
        for (const auto &rec : kept) {
            objectPoints.push_back(rec.objectPoints);
            imagePoints.push_back(rec.imagePoints);
        }

        cv::Mat cameraMatrix = cv::Mat::eye(3, 3, CV_64F);
        cameraMatrix.at<double>(0, 0) = input.cameraMatrix.at<double>(0, 0);
        cameraMatrix.at<double>(1, 1) = input.cameraMatrix.at<double>(1, 1);
        cameraMatrix.at<double>(0, 2) = input.cameraMatrix.at<double>(0, 2);
        cameraMatrix.at<double>(1, 2) = input.cameraMatrix.at<double>(1, 2);

        cv::Mat distCoeffs = input.distCoeffs.clone();
        std::vector<cv::Mat> rvecs;
        std::vector<cv::Mat> tvecs;
        const double rms = cv::calibrateCamera(objectPoints, imagePoints, input.imageSize,
                                               cameraMatrix, distCoeffs, rvecs, tvecs,
                                               cv::CALIB_USE_INTRINSIC_GUESS | cv::CALIB_RATIONAL_MODEL |
                                               cv::CALIB_THIN_PRISM_MODEL | cv::CALIB_TILTED_MODEL);

        if (abortGuard()) {
            return input;
        }

        computeResiduals(cameraMatrix, distCoeffs, objectPoints, imagePoints, kept, rvecs, tvecs);

        input.cameraMatrix = cameraMatrix;
        input.distCoeffs = distCoeffs;
        input.metrics = summarize(kept);
        input.metrics.rms = rms;
    Logger::info(QStringLiteral("After iteration %1: samples=%2 | RMS=%3 px | Mean=%4 px | Median=%5 px | Max=%6 px")
                         .arg(iteration + 1)
                         .arg(static_cast<int>(kept.size()))
                         .arg(rms, 0, 'f', 3)
                         .arg(input.metrics.meanErrorPx, 0, 'f', 3)
                         .arg(input.metrics.medianErrorPx, 0, 'f', 3)
                         .arg(input.metrics.maxErrorPx, 0, 'f', 3));
    }

    input.keptDetections = kept;
    input.removedDetections = removed;
    input.success = true;
    input.message = tr("Robust calibration complete");
    Logger::info(QStringLiteral("Robust optimisation complete: kept %1 | removed %2 | final RMS=%3 px | Mean=%4 px | Median=%5 px | Max=%6 px")
                     .arg(static_cast<int>(input.keptDetections.size()))
                     .arg(static_cast<int>(input.removedDetections.size()))
                     .arg(input.metrics.rms, 0, 'f', 3)
                     .arg(input.metrics.meanErrorPx, 0, 'f', 3)
                     .arg(input.metrics.medianErrorPx, 0, 'f', 3)
                     .arg(input.metrics.maxErrorPx, 0, 'f', 3));
    Logger::info(QStringLiteral("Mean |ΔX,Y,Z| = (%1, %2, %3) mm | (%4, %5, %6) %%")
                     .arg(input.metrics.meanResidualMm[0], 0, 'f', 3)
                     .arg(input.metrics.meanResidualMm[1], 0, 'f', 3)
                     .arg(input.metrics.meanResidualMm[2], 0, 'f', 3)
                     .arg(input.metrics.meanResidualPercent[0], 0, 'f', 3)
                     .arg(input.metrics.meanResidualPercent[1], 0, 'f', 3)
                     .arg(input.metrics.meanResidualPercent[2], 0, 'f', 3));
    const double depthMm = input.metrics.meanTranslationMm[2];
    const double fx = input.cameraMatrix.at<double>(0, 0);
    if (std::abs(depthMm) > 1e-3 && fx > 1e-6) {
        const double pxPerMm = fx / std::abs(depthMm);
        const cv::Vec3d stdPx = input.metrics.stdTranslationMm * pxPerMm;
    const QString message = QStringLiteral("Translation σ ≈ (%1, %2, %3) mm | Depth ≈ %4 mm | ≈ (%5, %6, %7) px equivalent")
                                     .arg(input.metrics.stdTranslationMm[0], 0, 'f', 2)
                                     .arg(input.metrics.stdTranslationMm[1], 0, 'f', 2)
                                     .arg(input.metrics.stdTranslationMm[2], 0, 'f', 2)
                                     .arg(depthMm, 0, 'f', 1)
                                     .arg(stdPx[0], 0, 'f', 3)
                                     .arg(stdPx[1], 0, 'f', 3)
                                     .arg(stdPx[2], 0, 'f', 3);
        Logger::info(message);
    }
    return input;
}

void CalibrationEngine::exportReport(const CalibrationOutput &output) const
{
    ensureDirectory(m_outputDirectory);

    QJsonObject root;
    root.insert("success", output.success);
    root.insert("message", output.message);
    root.insert("num_samples", static_cast<int>(output.keptDetections.size()));
    root.insert("rms", output.metrics.rms);
    root.insert("mean_reprojection_px", output.metrics.meanErrorPx);
    root.insert("median_reprojection_px", output.metrics.medianErrorPx);
    root.insert("max_reprojection_px", output.metrics.maxErrorPx);
    root.insert("std_reprojection_px", output.metrics.stdErrorPx);
    root.insert("p95_reprojection_px", output.metrics.p95ErrorPx);
    root.insert("distortion_max_shift_px", output.heatmaps.distortionMax);

    QJsonObject translation;
    translation.insert("mean_x_mm", output.metrics.meanTranslationMm[0]);
    translation.insert("mean_y_mm", output.metrics.meanTranslationMm[1]);
    translation.insert("mean_z_mm", output.metrics.meanTranslationMm[2]);
    translation.insert("std_x_mm", output.metrics.stdTranslationMm[0]);
    translation.insert("std_y_mm", output.metrics.stdTranslationMm[1]);
    translation.insert("std_z_mm", output.metrics.stdTranslationMm[2]);
    root.insert("translation_stats", translation);

    if (output.cameraMatrix.cols >= 3 && output.cameraMatrix.rows >= 3) {
        const double fx = output.cameraMatrix.at<double>(0, 0);
        const double depthMm = output.metrics.meanTranslationMm[2];
        if (fx > 1e-6 && std::abs(depthMm) > 1e-3) {
            const double mmPerPixel = std::abs(depthMm) / fx;
            root.insert("approx_mm_per_pixel", mmPerPixel);
        }
    }

    root.insert("camera_matrix", matToJson(output.cameraMatrix));
    root.insert("distortion_coefficients", matToJson(output.distCoeffs));

    auto vecToJsonArray = [](const cv::Vec3d &vec) {
        return QJsonArray{vec[0], vec[1], vec[2]};
    };

    root.insert("mean_residual_mm", vecToJsonArray(output.metrics.meanResidualMm));
    root.insert("rms_residual_mm", vecToJsonArray(output.metrics.rmsResidualMm));
    root.insert("mean_residual_percent", vecToJsonArray(output.metrics.meanResidualPercent));
    root.insert("rms_residual_percent", vecToJsonArray(output.metrics.rmsResidualPercent));

    QJsonArray detectionsJson;
    for (const auto &rec : output.keptDetections) {
        QJsonObject item;
        item.insert("name", QString::fromStdString(rec.name));
        item.insert("mean_error_px", rec.meanErrorPx());
        item.insert("max_error_px", rec.maxErrorPx());
        item.insert("translation_mm", QJsonArray{rec.translationMm[0], rec.translationMm[1], rec.translationMm[2]});
        item.insert("rotation_deg", QJsonArray{rec.rotationDeg[0], rec.rotationDeg[1], rec.rotationDeg[2]});
        detectionsJson.append(item);
    }
    root.insert("kept_samples", detectionsJson);

    QJsonArray removedJson;
    for (const auto &rec : output.removedDetections) {
        QJsonObject item;
        item.insert("name", QString::fromStdString(rec.name));
        item.insert("iteration", rec.iterationRemoved);
        item.insert("mean_error_px", rec.meanErrorPx());
        item.insert("max_error_px", rec.maxErrorPx());
        removedJson.append(item);
    }
    root.insert("removed_samples", removedJson);

    const QString jsonPath = m_outputDirectory + "/calibration_report.json";
    QFile file(jsonPath);
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    }
}

void CalibrationEngine::exportHeatmap(const cv::Mat &heatmap, const QString &path) const
{
    if (heatmap.empty()) {
        return;
    }
    ensureDirectory(QFileInfo(path).absolutePath());
    cv::imwrite(path.toStdString(), heatmap);
}

void CalibrationEngine::ensureDirectory(const QString &path)
{
    QDir dir(path);
    if (!dir.exists()) {
        dir.mkpath(".");
    }
}

} // namespace mycalib
