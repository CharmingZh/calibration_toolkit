#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "MainWindow.h"

#include <QAction>
#include <QAbstractItemView>
#include <QApplication>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QComboBox>
#include <QBrush>
#include <QButtonGroup>
#include <QCheckBox>
#include <QColor>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QVariant>
#include <QLabel>
#include <QLineEdit>
#include <QHBoxLayout>
#include <QFrame>
#include <QBoxLayout>
#include <QMessageBox>
#include <QIcon>
#include <QSize>
#include <QProgressBar>
#include <QPushButton>
#include <QSizePolicy>
#include <QSplitter>
#include <QListWidget>
#include <QTabWidget>
#include <QStackedLayout>
#include <QStackedWidget>
#include <QScrollArea>
#include <QTextEdit>
#include <QDesktopServices>
#include <QUrl>
#include <QTextCursor>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QToolBar>
#include <QTreeWidget>
#include <QFileSystemWatcher>
#include <QVBoxLayout>
#include <QVector>
#include <QTableWidget>
#include <QDataStream>
#include <QSaveFile>
#include <QPolygonF>
#include <QStringList>
#include <QFile>
#include <QPoint>
#include <QDir>
#include <QMetaObject>
#include <QWidget>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QToolButton>
#include <QSet>
#include <QHash>
#include <QUuid>
#include <QtCore/Qt>
#include <QtMath>
#include <QMouseEvent>
#include <QKeyEvent>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <numeric>
#include <iterator>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <optional>
#include <cstring>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

#include "DetectionPreviewWidget.h"
#include "ImageEvaluationDialog.h"
#include "HeatmapGenerator.h"
#include "HeatmapView.h"
#include "Logger.h"
#include "Pose3DView.h"
#include "ResidualScatterView.h"
#include "ParameterDialog.h"

#if MYCALIB_HAVE_CONNECTED_CAMERA
#include "camera/CameraWindow.h"
#include "camera/ImageView.h"
#include "camera/FeaturePanel.h"
#endif

namespace mycalib {

namespace {

constexpr int kCaptureGridRows = MainWindow::kCaptureGridRows;
constexpr int kCaptureGridCols = MainWindow::kCaptureGridCols;
constexpr int kCaptureTargetPerCell = MainWindow::kCaptureTargetPerCell;
constexpr int kCaptureMinimumPerCell = MainWindow::kCaptureMinimumPerCell;

QImage cvMatToQImage(const cv::Mat &mat)
{
    if (mat.empty()) {
        return {};
    }
    cv::Mat rgb;
    if (mat.channels() == 3) {
        cv::cvtColor(mat, rgb, cv::COLOR_BGR2RGB);
        return QImage(rgb.data, rgb.cols, rgb.rows, rgb.step, QImage::Format_RGB888).copy();
    }
    if (mat.channels() == 1) {
        return QImage(mat.data, mat.cols, mat.rows, mat.step, QImage::Format_Grayscale8).copy();
    }
    return {};
}

QColor blendColors(const QColor &a, const QColor &b, double t)
{
    const double clamped = std::clamp(t, 0.0, 1.0);
    auto lerp = [&](int channelA, int channelB) {
        return static_cast<int>(std::round(channelA + (channelB - channelA) * clamped));
    };
    return QColor(lerp(a.red(), b.red()),
                  lerp(a.green(), b.green()),
                  lerp(a.blue(), b.blue()));
}

QString rgbaString(const QColor &color, double alpha)
{
    const double clampedAlpha = std::clamp(alpha, 0.0, 1.0);
    return QStringLiteral("rgba(%1,%2,%3,%4)")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(clampedAlpha, 0, 'f', 2);
}

void cleanupDebugArtifacts(const CalibrationOutput &output)
{
    std::unordered_set<std::filesystem::path> directories;
    std::unordered_set<std::filesystem::path> files;
    auto processDetection = [&](const DetectionResult &det) {
        if (!det.debugDirectory.empty()) {
            directories.insert(std::filesystem::path(det.debugDirectory));
        }
        for (const auto &img : det.debugImages) {
            if (img.filePath.empty()) {
                continue;
            }
            files.insert(std::filesystem::path(img.filePath));
        }
    };

    auto processList = [&](const std::vector<DetectionResult> &list) {
        for (const auto &det : list) {
            processDetection(det);
        }
    };

    processList(output.allDetections);
    processList(output.keptDetections);
    processList(output.removedDetections);

    const QString tempRoot = QDir::cleanPath(QDir::tempPath());
    const auto pathToQString = [](const std::filesystem::path &path) -> QString {
#ifdef _WIN32
        return QString::fromStdWString(path.native());
#else
        return QString::fromStdString(path.native());
#endif
    };

    for (const auto &file : files) {
        const QString absolute = QDir::cleanPath(pathToQString(file));
        if (!absolute.startsWith(tempRoot, Qt::CaseInsensitive)) {
            continue;
        }
        std::error_code ec;
        std::filesystem::remove(file, ec);
    }

    for (const auto &dir : directories) {
        const QString absolute = QDir::cleanPath(pathToQString(dir));
        if (!absolute.startsWith(tempRoot, Qt::CaseInsensitive)) {
            continue;
        }
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
}

QString dataSourceDisplayName(ProjectSession::DataSource source)
{
    switch (source) {
    case ProjectSession::DataSource::LocalDataset:
        return MainWindow::tr("Local images");
    case ProjectSession::DataSource::ConnectedCamera:
        return MainWindow::tr("Live capture");
    }
    return MainWindow::tr("Local images");
}

QString stageStatusDisplay(ProjectSession::StageStatus status)
{
    switch (status) {
    case ProjectSession::StageStatus::NotStarted:
        return MainWindow::tr("Not started");
    case ProjectSession::StageStatus::InProgress:
        return MainWindow::tr("In progress");
    case ProjectSession::StageStatus::Completed:
        return MainWindow::tr("Completed");
    }
    return MainWindow::tr("Not started");
}

struct PoseDescriptor {
    ProjectSession::CapturePose pose;
    const char *display;
    const char *hint;
};

const PoseDescriptor kPoseDescriptors[] = {
    {ProjectSession::CapturePose::Flat, QT_TR_NOOP("平拍"), QT_TR_NOOP("镜头与标定板平行" )},
    {ProjectSession::CapturePose::TiltUp, QT_TR_NOOP("上仰"), QT_TR_NOOP("相机略微向上俯视棋盘" )},
    {ProjectSession::CapturePose::TiltDown, QT_TR_NOOP("下俯"), QT_TR_NOOP("相机略微俯视棋盘" )},
    {ProjectSession::CapturePose::TiltLeft, QT_TR_NOOP("左倾"), QT_TR_NOOP("相机向左倾斜拍摄" )},
    {ProjectSession::CapturePose::TiltRight, QT_TR_NOOP("右倾"), QT_TR_NOOP("相机向右倾斜拍摄" )}
};

int capturePoseIndex(ProjectSession::CapturePose pose)
{
    for (int i = 0; i < static_cast<int>(std::size(kPoseDescriptors)); ++i) {
        if (kPoseDescriptors[i].pose == pose) {
            return i;
        }
    }
    return 0;
}

ProjectSession::CapturePose capturePoseFromIndex(int index)
{
    if (index < 0 || index >= static_cast<int>(std::size(kPoseDescriptors))) {
        return ProjectSession::CapturePose::Flat;
    }
    return kPoseDescriptors[index].pose;
}

QString sampleKeyFromName(const QString &name)
{
    if (name.isEmpty()) {
        return {};
    }
    QFileInfo info(name);
    const QString base = info.completeBaseName();
    if (!base.isEmpty()) {
        return base.toCaseFolded();
    }
    const QString fileName = info.fileName();
    if (!fileName.isEmpty()) {
        return fileName.toCaseFolded();
    }
    return name.toCaseFolded();
}

QString sampleKeyFromShot(const ProjectSession::CaptureShot &shot, const ProjectSession *session)
{
    QString candidate = shot.relativePath;
    if (candidate.isEmpty()) {
        const QVariant absoluteVariant = shot.metadata.value(QStringLiteral("absolute_path"));
        if (absoluteVariant.isValid()) {
            candidate = absoluteVariant.toString();
        }
    }
    if (candidate.isEmpty()) {
        const QVariant sourceVariant = shot.metadata.value(QStringLiteral("source_path"));
        if (sourceVariant.isValid()) {
            candidate = sourceVariant.toString();
        }
    }
    if (!candidate.isEmpty() && session && !session->rootPath().isEmpty()) {
        QFileInfo info(candidate);
        if (!info.isAbsolute()) {
            candidate = QDir(session->rootPath()).filePath(candidate);
        }
    }
    QString key = sampleKeyFromName(candidate);
    if (key.isEmpty()) {
        key = shot.id.toString(QUuid::WithoutBraces).toCaseFolded();
    }
    return key;
}

QString sampleDisplayNameFromShot(const ProjectSession::CaptureShot &shot, const ProjectSession *session)
{
    QString candidate = shot.relativePath;
    if (candidate.isEmpty()) {
        const QVariant absoluteVariant = shot.metadata.value(QStringLiteral("absolute_path"));
        if (absoluteVariant.isValid()) {
            candidate = absoluteVariant.toString();
        }
    }
    if (candidate.isEmpty()) {
        const QVariant sourceVariant = shot.metadata.value(QStringLiteral("source_path"));
        if (sourceVariant.isValid()) {
            candidate = sourceVariant.toString();
        }
    }
    if (!candidate.isEmpty() && session && !session->rootPath().isEmpty()) {
        QFileInfo info(candidate);
        if (!info.isAbsolute()) {
            candidate = QDir(session->rootPath()).filePath(candidate);
        }
    }
    if (!candidate.isEmpty()) {
        return QDir::toNativeSeparators(candidate);
    }
    return shot.id.toString(QUuid::WithoutBraces);
}

QString sampleKeyFromDetection(const DetectionResult &det)
{
    const QString name = QString::fromStdString(det.name);
    const QString key = sampleKeyFromName(name);
    if (!key.isEmpty()) {
        return key;
    }
    return name.toCaseFolded();
}

QString sanitizeDebugKey(const QString &raw)
{
    QString key = raw.trimmed().toLower();
    if (key.isEmpty()) {
        key = QStringLiteral("capture");
    }
    key.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral("_"));
    key.remove(QRegularExpression(QStringLiteral("^_+")));
    key.remove(QRegularExpression(QStringLiteral("_+$")));
    if (key.isEmpty()) {
        key = QStringLiteral("capture");
    }
    constexpr int kMaxLength = 48;
    if (key.size() > kMaxLength) {
        key.truncate(kMaxLength);
    }
    return key;
}

struct CaptureCoverageStats {
    std::array<std::array<int, kCaptureGridCols>, kCaptureGridRows> cellTotals {};
    std::array<int, std::size(kPoseDescriptors)> poseTotals {};
    int totalShots {0};
};

CaptureCoverageStats computeCaptureCoverage(const QVector<ProjectSession::CaptureShot> &shots)
{
    CaptureCoverageStats stats;
    for (const auto &shot : shots) {
        const int row = std::clamp(shot.gridRow, 0, kCaptureGridRows - 1);
        const int col = std::clamp(shot.gridCol, 0, kCaptureGridCols - 1);
        stats.cellTotals[row][col]++;
        const int poseIdx = capturePoseIndex(shot.pose);
        if (poseIdx >= 0 && poseIdx < static_cast<int>(stats.poseTotals.size())) {
            stats.poseTotals[poseIdx]++;
        }
        ++stats.totalShots;
    }
    return stats;
}

std::pair<int, int> inferGridCellFromDetection(const DetectionResult &detection)
{
    if (detection.imagePoints.empty() || detection.resolution.width <= 0 || detection.resolution.height <= 0) {
        return {kCaptureGridRows / 2, kCaptureGridCols / 2};
    }

    cv::Point2f center(0.0F, 0.0F);
    for (const auto &pt : detection.imagePoints) {
        center += pt;
    }
    center *= (1.0F / static_cast<float>(detection.imagePoints.size()));

    const float width = static_cast<float>(detection.resolution.width);
    const float height = static_cast<float>(detection.resolution.height);
    const float normalizedX = std::clamp(center.x / width, 0.0F, std::nextafter(1.0F, 0.0F));
    const float normalizedY = std::clamp(center.y / height, 0.0F, std::nextafter(1.0F, 0.0F));

    const int col = std::clamp(static_cast<int>(normalizedX * kCaptureGridCols), 0, kCaptureGridCols - 1);
    const int row = std::clamp(static_cast<int>(normalizedY * kCaptureGridRows), 0, kCaptureGridRows - 1);

    return {row, col};
}

ProjectSession::CapturePose inferPoseFromDetection(const DetectionResult &detection)
{
    constexpr double kTiltThreshold = 9.0;
    constexpr double kDominanceMargin = 2.5;
    constexpr double kFallbackTiltBias = 6.0;

    auto classifyFromAngles = [&](double pitchDeg, double yawDeg) {
        const double absPitch = std::abs(pitchDeg);
        const double absYaw = std::abs(yawDeg);
        if (absPitch <= kTiltThreshold && absYaw <= kTiltThreshold) {
            return ProjectSession::CapturePose::Flat;
        }
        if (absPitch > absYaw + kDominanceMargin) {
            return (pitchDeg >= 0.0) ? ProjectSession::CapturePose::TiltDown
                                    : ProjectSession::CapturePose::TiltUp;
        }
        if (absYaw >= absPitch - kDominanceMargin) {
            if (yawDeg > 0.0) {
                return ProjectSession::CapturePose::TiltRight;
            }
            if (yawDeg < 0.0) {
                return ProjectSession::CapturePose::TiltLeft;
            }
        }
        return (pitchDeg >= 0.0) ? ProjectSession::CapturePose::TiltDown
                                 : ProjectSession::CapturePose::TiltUp;
    };

    auto classifyWithFallback = [&](double pitchDeg, double yawDeg) {
        auto pose = classifyFromAngles(pitchDeg, yawDeg);
        if (pose == ProjectSession::CapturePose::Flat) {
            if (pitchDeg <= -kFallbackTiltBias) {
                pose = ProjectSession::CapturePose::TiltUp;
            } else if (pitchDeg >= kFallbackTiltBias) {
                pose = ProjectSession::CapturePose::TiltDown;
            } else if (yawDeg >= kFallbackTiltBias) {
                pose = ProjectSession::CapturePose::TiltRight;
            } else if (yawDeg <= -kFallbackTiltBias) {
                pose = ProjectSession::CapturePose::TiltLeft;
            }
        }
        return pose;
    };

    if (cv::norm(detection.rotationMatrix) > 0.0) {
        const cv::Vec3d normal = detection.rotationMatrix * cv::Vec3d(0.0, 0.0, 1.0);
        if (cv::norm(normal) > 0.0) {
            const double pitchDeg = qRadiansToDegrees(std::atan2(-normal[1], normal[2]));
            const double yawDeg = qRadiansToDegrees(std::atan2(normal[0], normal[2]));
            return classifyWithFallback(pitchDeg, yawDeg);
        }
    }

    if (cv::norm(detection.rotationDeg) > 0.0) {
        return classifyWithFallback(detection.rotationDeg[0], detection.rotationDeg[1]);
    }

    return ProjectSession::CapturePose::Flat;
}

int encodeGridId(int row, int col)
{
    return row * 10 + col;
}

int decodeGridRow(int id)
{
    return id / 10;
}

int decodeGridCol(int id)
{
    return id % 10;
}

QString toIso8601Utc(const QDateTime &dt)
{
    if (!dt.isValid()) {
        return {};
    }
    return dt.toUTC().toString(Qt::ISODateWithMs);
}

QJsonArray matToJsonArray(const cv::Mat &mat)
{
    QJsonArray result;
    if (mat.empty()) {
        return result;
    }

    const int rows = mat.rows;
    const int cols = mat.cols;
    const int type = mat.type();
    for (int r = 0; r < rows; ++r) {
        QJsonArray rowArray;
        for (int c = 0; c < cols; ++c) {
            double value = 0.0;
            switch (type) {
            case CV_32F:
                value = mat.at<float>(r, c);
                break;
            case CV_64F:
            default:
                value = mat.at<double>(r, c);
                break;
            }
            rowArray.append(value);
        }
        result.append(rowArray);
    }
    return result;
}

QJsonArray vec3dToJson(const cv::Vec3d &vec)
{
    return QJsonArray{vec[0], vec[1], vec[2]};
}

constexpr quint32 kSnapshotMagic = 0x4D43534E; // 'MCSN'
constexpr quint16 kSnapshotVersion = 1;

void writeMat(QDataStream &out, const cv::Mat &mat)
{
    out << qint32(mat.rows);
    out << qint32(mat.cols);
    out << qint32(mat.type());
    const bool hasData = !mat.empty();
    out << quint8(hasData ? 1 : 0);
    if (!hasData) {
        return;
    }

    const size_t rowSize = static_cast<size_t>(mat.cols) * mat.elemSize();
    const size_t totalSize = rowSize * static_cast<size_t>(mat.rows);
    QByteArray buffer(static_cast<int>(totalSize), Qt::Uninitialized);
    char *dst = buffer.data();
    for (int r = 0; r < mat.rows; ++r) {
        std::memcpy(dst + r * rowSize, mat.ptr(r), rowSize);
    }
    out << buffer;
}

cv::Mat readMat(QDataStream &in)
{
    qint32 rows = 0;
    qint32 cols = 0;
    qint32 type = 0;
    quint8 hasData = 0;
    in >> rows >> cols >> type >> hasData;
    if (in.status() != QDataStream::Ok || rows <= 0 || cols <= 0 || hasData == 0) {
        return cv::Mat();
    }
    QByteArray buffer;
    in >> buffer;
    if (in.status() != QDataStream::Ok) {
        return cv::Mat();
    }
    cv::Mat mat(rows, cols, type);
    const size_t expected = static_cast<size_t>(mat.total()) * mat.elemSize();
    const size_t toCopy = std::min(expected, static_cast<size_t>(buffer.size()));
    if (toCopy > 0) {
        std::memcpy(mat.data, buffer.constData(), toCopy);
    }
    if (toCopy < expected) {
        std::memset(mat.data + toCopy, 0, expected - toCopy);
    }
    return mat;
}

void writeVec3d(QDataStream &out, const cv::Vec3d &vec)
{
    out << vec[0] << vec[1] << vec[2];
}

cv::Vec3d readVec3d(QDataStream &in)
{
    cv::Vec3d vec{0.0, 0.0, 0.0};
    in >> vec[0] >> vec[1] >> vec[2];
    return vec;
}

void writeVec2iVector(QDataStream &out, const std::vector<cv::Vec2i> &vec)
{
    out << quint32(vec.size());
    for (const auto &value : vec) {
        out << value[0] << value[1];
    }
}

std::vector<cv::Vec2i> readVec2iVector(QDataStream &in)
{
    quint32 count = 0;
    in >> count;
    std::vector<cv::Vec2i> result;
    result.reserve(count);
    for (quint32 i = 0; i < count; ++i) {
        cv::Vec2i value;
        in >> value[0] >> value[1];
        result.push_back(value);
    }
    return result;
}

void writePoint2fVector(QDataStream &out, const std::vector<cv::Point2f> &points)
{
    out << quint32(points.size());
    for (const auto &pt : points) {
        out << pt.x << pt.y;
    }
}

std::vector<cv::Point2f> readPoint2fVector(QDataStream &in)
{
    quint32 count = 0;
    in >> count;
    std::vector<cv::Point2f> points;
    points.reserve(count);
    for (quint32 i = 0; i < count; ++i) {
        cv::Point2f pt;
        in >> pt.x >> pt.y;
        points.push_back(pt);
    }
    return points;
}

void writePoint3fVector(QDataStream &out, const std::vector<cv::Point3f> &points)
{
    out << quint32(points.size());
    for (const auto &pt : points) {
        out << pt.x << pt.y << pt.z;
    }
}

std::vector<cv::Point3f> readPoint3fVector(QDataStream &in)
{
    quint32 count = 0;
    in >> count;
    std::vector<cv::Point3f> points;
    points.reserve(count);
    for (quint32 i = 0; i < count; ++i) {
        cv::Point3f pt;
        in >> pt.x >> pt.y >> pt.z;
        points.push_back(pt);
    }
    return points;
}

void writeFloatVector(QDataStream &out, const std::vector<float> &values)
{
    out << quint32(values.size());
    for (float v : values) {
        out << v;
    }
}

std::vector<float> readFloatVector(QDataStream &in)
{
    quint32 count = 0;
    in >> count;
    std::vector<float> values;
    values.reserve(count);
    for (quint32 i = 0; i < count; ++i) {
        float v = 0.0F;
        in >> v;
        values.push_back(v);
    }
    return values;
}

void writeDoubleVector(QDataStream &out, const std::vector<double> &values)
{
    out << quint32(values.size());
    for (double v : values) {
        out << v;
    }
}

std::vector<double> readDoubleVector(QDataStream &in)
{
    quint32 count = 0;
    in >> count;
    std::vector<double> values;
    values.reserve(count);
    for (quint32 i = 0; i < count; ++i) {
        double v = 0.0;
        in >> v;
        values.push_back(v);
    }
    return values;
}

void writeVec3dVector(QDataStream &out, const std::vector<cv::Vec3d> &values)
{
    out << quint32(values.size());
    for (const auto &v : values) {
        writeVec3d(out, v);
    }
}

std::vector<cv::Vec3d> readVec3dVector(QDataStream &in)
{
    quint32 count = 0;
    in >> count;
    std::vector<cv::Vec3d> values;
    values.reserve(count);
    for (quint32 i = 0; i < count; ++i) {
        values.push_back(readVec3d(in));
    }
    return values;
}

void writeIntVector(QDataStream &out, const std::vector<int> &values)
{
    out << quint32(values.size());
    for (int v : values) {
        out << v;
    }
}

std::vector<int> readIntVector(QDataStream &in)
{
    quint32 count = 0;
    in >> count;
    std::vector<int> values;
    values.reserve(count);
    for (quint32 i = 0; i < count; ++i) {
        int v = 0;
        in >> v;
        values.push_back(v);
    }
    return values;
}

void writeMatx33d(QDataStream &out, const cv::Matx33d &mat)
{
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            out << mat(r, c);
        }
    }
}

cv::Matx33d readMatx33d(QDataStream &in)
{
    cv::Matx33d mat;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            in >> mat(r, c);
        }
    }
    return mat;
}

void writeCalibrationMetrics(QDataStream &out, const CalibrationMetrics &metrics)
{
    out << metrics.rms
        << metrics.meanErrorPx
        << metrics.medianErrorPx
        << metrics.maxErrorPx
        << metrics.stdErrorPx
        << metrics.p95ErrorPx;
    writeVec3d(out, metrics.meanTranslationMm);
    writeVec3d(out, metrics.stdTranslationMm);
    writeVec3d(out, metrics.meanResidualMm);
    writeVec3d(out, metrics.rmsResidualMm);
    writeVec3d(out, metrics.meanResidualPercent);
    writeVec3d(out, metrics.rmsResidualPercent);
}

CalibrationMetrics readCalibrationMetrics(QDataStream &in)
{
    CalibrationMetrics metrics;
    in >> metrics.rms
       >> metrics.meanErrorPx
       >> metrics.medianErrorPx
       >> metrics.maxErrorPx
       >> metrics.stdErrorPx
       >> metrics.p95ErrorPx;
    metrics.meanTranslationMm = readVec3d(in);
    metrics.stdTranslationMm = readVec3d(in);
    metrics.meanResidualMm = readVec3d(in);
    metrics.rmsResidualMm = readVec3d(in);
    metrics.meanResidualPercent = readVec3d(in);
    metrics.rmsResidualPercent = readVec3d(in);
    return metrics;
}

void writeDetectionList(QDataStream &out, const std::vector<DetectionResult> &detections)
{
    out << quint32(detections.size());
    for (const auto &det : detections) {
        out << QString::fromStdString(det.name);
        out << det.success;
        out << QString::fromStdString(det.message);
        out << qint64(det.elapsed.count());
        out << qint32(det.resolution.width) << qint32(det.resolution.height);
        writePoint2fVector(out, det.imagePoints);
        writePoint3fVector(out, det.objectPoints);
        writePoint2fVector(out, det.bigCirclePoints);
        writeFloatVector(out, det.circleRadiiPx);
        writeFloatVector(out, det.bigCircleRadiiPx);
        out << qint32(det.bigCircleCount);
        out << quint8(det.inlierIndices.has_value() ? 1 : 0);
        if (det.inlierIndices) {
            writeIntVector(out, *det.inlierIndices);
        }
        out << qint32(det.iterationRemoved);
        out << det.cachedMeanErrorPx;
        out << det.cachedMaxErrorPx;
        writeVec2iVector(out, det.logicalIndices);
        writeMat(out, det.whiteRegionMask);
        writeMat(out, det.warpHomography);
        writeMat(out, det.warpHomographyInv);
        writeDoubleVector(out, det.residualsPx);
        writePoint2fVector(out, det.residualVectors);
        writeVec3dVector(out, det.residualCameraMm);
        writeVec3dVector(out, det.residualCameraPercent);
        writeVec3d(out, det.meanResidualCameraMm);
        writeVec3d(out, det.meanResidualCameraPercent);
        writeVec3d(out, det.translationMm);
        writeVec3d(out, det.rotationDeg);
        writeMatx33d(out, det.rotationMatrix);

        out << quint32(det.debugImages.size());
        for (const auto &img : det.debugImages) {
            out << QString::fromStdString(img.label);
            out << QString::fromStdString(img.filePath);
        }
        out << QString::fromStdString(det.debugDirectory);
    }
}

std::vector<DetectionResult> readDetectionList(QDataStream &in)
{
    quint32 count = 0;
    in >> count;
    std::vector<DetectionResult> detections;
    detections.reserve(count);
    for (quint32 i = 0; i < count; ++i) {
        DetectionResult det;
        QString name;
        QString message;
        qint64 elapsedMs = 0;
        qint32 width = 0;
        qint32 height = 0;
        in >> name >> det.success >> message >> elapsedMs >> width >> height;
        det.name = name.toStdString();
        det.message = message.toStdString();
        det.elapsed = std::chrono::milliseconds(elapsedMs);
        det.resolution = cv::Size(width, height);
        det.imagePoints = readPoint2fVector(in);
        det.objectPoints = readPoint3fVector(in);
        det.bigCirclePoints = readPoint2fVector(in);
        det.circleRadiiPx = readFloatVector(in);
        det.bigCircleRadiiPx = readFloatVector(in);
        qint32 bigCount = 0;
        in >> bigCount;
        det.bigCircleCount = bigCount;
        quint8 hasInliers = 0;
        in >> hasInliers;
        if (hasInliers) {
            det.inlierIndices = readIntVector(in);
        }
        qint32 iteration = 0;
        in >> iteration;
        det.iterationRemoved = iteration;
        in >> det.cachedMeanErrorPx;
        in >> det.cachedMaxErrorPx;
        det.logicalIndices = readVec2iVector(in);
        det.whiteRegionMask = readMat(in);
        det.warpHomography = readMat(in);
        det.warpHomographyInv = readMat(in);
        det.residualsPx = readDoubleVector(in);
        det.residualVectors = readPoint2fVector(in);
        det.residualCameraMm = readVec3dVector(in);
        det.residualCameraPercent = readVec3dVector(in);
        det.meanResidualCameraMm = readVec3d(in);
        det.meanResidualCameraPercent = readVec3d(in);
        det.translationMm = readVec3d(in);
        det.rotationDeg = readVec3d(in);
        det.rotationMatrix = readMatx33d(in);

        quint32 debugCount = 0;
        in >> debugCount;
        det.debugImages.reserve(debugCount);
        for (quint32 j = 0; j < debugCount; ++j) {
            QString label;
            QString path;
            in >> label >> path;
            DetectionDebugImage img;
            img.label = label.toStdString();
            img.filePath = path.toStdString();
            det.debugImages.push_back(std::move(img));
        }
        QString debugDir;
        in >> debugDir;
        det.debugDirectory = debugDir.toStdString();

        detections.push_back(std::move(det));
    }
    return detections;
}

void writeHeatmapMeta(QDataStream &out, const HeatmapBundle &heatmaps)
{
    out << heatmaps.boardCoverageMin
        << heatmaps.boardCoverageMax
        << heatmaps.pixelErrorMin
        << heatmaps.pixelErrorMax
        << heatmaps.boardErrorMin
        << heatmaps.boardErrorMax
        << heatmaps.residualScatterMax
        << heatmaps.distortionMin
        << heatmaps.distortionMax;
}

HeatmapBundle readHeatmapMeta(QDataStream &in)
{
    HeatmapBundle bundle;
    in >> bundle.boardCoverageMin
       >> bundle.boardCoverageMax
       >> bundle.pixelErrorMin
       >> bundle.pixelErrorMax
       >> bundle.boardErrorMin
       >> bundle.boardErrorMax
       >> bundle.residualScatterMax
       >> bundle.distortionMin
       >> bundle.distortionMax;
    return bundle;
}

} // namespace

MainWindow::MainWindow(ProjectSession *session, QWidget *parent)
    : QMainWindow(parent)
    , m_session(session)
    , m_engine(new CalibrationEngine(this))
{
    setupUi();
    setupActions();
    applyTheme();

    m_inputWatcher = new QFileSystemWatcher(this);
    connect(m_inputWatcher, &QFileSystemWatcher::directoryChanged, this, &MainWindow::handleInputDirectoryChanged);
    connect(m_inputWatcher, &QFileSystemWatcher::fileChanged, this, &MainWindow::handleInputDirectoryChanged);

    bindSessionSignals();
    updateWindowTitle();

    refreshModeUi();
    restoreCalibrationSnapshot();
    persistProjectSummary(false);

    connect(m_engine, &CalibrationEngine::progressUpdated, this, &MainWindow::handleProgress);
    connect(m_engine, &CalibrationEngine::statusChanged, this, &MainWindow::handleStatus);
    connect(m_engine, &CalibrationEngine::finished, this, &MainWindow::handleFinished);
    connect(m_engine, &CalibrationEngine::failed, this, &MainWindow::handleFailed);

    QPointer<MainWindow> weakThis(this);
    Logger::setSink([weakThis](QtMsgType type, const QString &text) {
        if (!weakThis) {
            return;
        }
        QMetaObject::invokeMethod(weakThis, [weakThis, type, text]() {
            if (!weakThis) {
                return;
            }
            weakThis->appendLog(type, text);
        }, Qt::QueuedConnection);
    });
}

MainWindow::~MainWindow()
{
    if (m_engine) {
        m_engine->cancelAndWait();
    }
    cleanupDebugArtifacts(m_lastOutput);
    if (m_session) {
        QString error;
        if (!m_session->save(&error) && !error.isEmpty()) {
            Logger::warning(tr("Failed to save project session: %1").arg(error));
        }
    }
}

QString MainWindow::captureCellStyle(const CellQuality &quality,
                                     int totalShots,
                                     bool meetsMinimum,
                                     bool meetsRecommended) const
{
    const QColor neutral(118, 126, 158);
    const QColor progress(80, 143, 255);
    const QColor good(76, 175, 80);
    const QColor mid(255, 193, 7);
    const QColor bad(239, 83, 80);

    QColor baseColor;
    double baseAlpha = meetsMinimum ? 0.32 : 0.24;
    double checkedAlpha = meetsMinimum ? 0.46 : 0.34;
    double borderAlpha = 0.52;

    const int totalSamples = quality.total();
    if (totalSamples <= 0 && quality.pending == 0) {
        if (totalShots <= 0) {
            baseColor = neutral;
            baseAlpha = 0.14;
            checkedAlpha = 0.24;
            borderAlpha = 0.32;
        } else {
            baseColor = progress;
            baseAlpha = meetsMinimum ? 0.30 : 0.22;
            checkedAlpha = meetsMinimum ? 0.44 : 0.34;
            borderAlpha = meetsMinimum ? 0.45 : 0.33;
        }
    } else {
        const double avgError = quality.averageMeanErrorPx();
        double normalizedError = 0.0;
        if (totalSamples > 0) {
            const double goodThreshold = 0.25;
            const double badThreshold = 1.0;
            if (avgError <= goodThreshold) {
                normalizedError = 0.0;
            } else if (avgError >= badThreshold) {
                normalizedError = 1.0;
            } else {
                normalizedError = (avgError - goodThreshold) / (badThreshold - goodThreshold);
            }
        }

        const double removalRate = totalSamples > 0
                                        ? static_cast<double>(quality.removed) / static_cast<double>(std::max(totalSamples, 1))
                                        : 0.0;

        double residualSeverity = 0.0;
        if (quality.maxResidualMm > 0.0) {
            const double warnThreshold = 0.4;
            const double failThreshold = 1.2;
            if (quality.maxResidualMm <= warnThreshold) {
                residualSeverity = 0.0;
            } else if (quality.maxResidualMm >= failThreshold) {
                residualSeverity = 1.0;
            } else {
                residualSeverity = (quality.maxResidualMm - warnThreshold) / (failThreshold - warnThreshold);
            }
        }

        double score = std::max({normalizedError, removalRate, residualSeverity});
        if (quality.pending > 0) {
            score = std::max(score, 0.35);
        }
        if (totalSamples == 0 && quality.pending > 0) {
            score = 0.45;
        }

        if (score <= 0.5) {
            baseColor = blendColors(good, mid, score * 2.0);
        } else {
            baseColor = blendColors(mid, bad, (score - 0.5) * 2.0);
        }

        baseAlpha = meetsMinimum ? 0.34 : 0.26;
        checkedAlpha = meetsMinimum ? 0.48 : 0.38;
        borderAlpha = 0.50;
    }

    if (meetsRecommended) {
        baseAlpha = std::min(baseAlpha + 0.04, 0.48);
        checkedAlpha = std::min(checkedAlpha + 0.08, 0.64);
        borderAlpha = std::min(borderAlpha + 0.08, 0.68);
    }

    const QString base = rgbaString(baseColor, baseAlpha);
    const QString checked = rgbaString(baseColor, checkedAlpha);
    const QString border = rgbaString(baseColor, borderAlpha);
    return QStringLiteral("QToolButton { background: %1; border: 1px solid %2; color: #eaf1ff; font-weight: 600; } QToolButton:checked { background: %3; }")
        .arg(base, border, checked);
}

void MainWindow::bindSessionSignals()
{
    if (!m_session) {
        return;
    }
    connect(m_session, &ProjectSession::metadataChanged, this, &MainWindow::updateWindowTitle);
    connect(m_session, &ProjectSession::metadataChanged, this, &MainWindow::updateStageNavigator);
    connect(m_session, &ProjectSession::metadataChanged, this, [this]() {
        persistProjectSummary(false);
    });
    connect(m_session, &ProjectSession::metadataChanged, this, &MainWindow::refreshCapturePlanFromSession);
    connect(m_session, &ProjectSession::dataSourceChanged, this, [this](ProjectSession::DataSource) {
        refreshModeUi();
    });
}

void MainWindow::updateWindowTitle()
{
    if (!m_session) {
        setWindowTitle(tr("MyCalib GUI"));
        return;
    }

    const QString name = m_session->metadata().projectName;
    if (name.isEmpty()) {
        setWindowTitle(tr("MyCalib GUI"));
    } else {
        setWindowTitle(tr("%1 — MyCalib GUI").arg(name));
    }
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (auto *button = qobject_cast<QToolButton *>(watched)) {
        if (button->property("capture-grid").toBool()) {
            const int row = button->property("gridRow").toInt();
            const int col = button->property("gridCol").toInt();
            auto canShowDetails = [this, row, col]() {
                if (row < 0 || row >= kCaptureGridRows || col < 0 || col >= kCaptureGridCols) {
                    return false;
                }
                const auto &cell = m_cellQuality[row][col];
                return !cell.samples.isEmpty() || !cell.pendingSamples.isEmpty();
            };

            if (event->type() == QEvent::MouseButtonDblClick) {
                if (canShowDetails()) {
                    showCaptureCellDetails(row, col);
                }
                return true;
            }
            if (event->type() == QEvent::KeyPress) {
                const auto *keyEvent = static_cast<QKeyEvent *>(event);
                if (keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter) {
                    if (canShowDetails()) {
                        showCaptureCellDetails(row, col);
                    }
                }
            }
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

QString MainWindow::defaultInputDirectory() const
{
    if (!m_session) {
        return {};
    }

    const auto source = m_session->metadata().dataSource;
    if (source == ProjectSession::DataSource::LocalDataset) {
        return m_session->calibrationCaptureDir().absolutePath();
    }
    QDir captures = m_session->capturesRoot();
    captures.mkpath(QStringLiteral("live"));
    return captures.filePath(QStringLiteral("live"));
}

QString MainWindow::defaultOutputDirectory() const
{
    if (!m_session) {
        const QString candidate = QDir::homePath() + QStringLiteral("/outputs");
        return CalibrationEngine::resolveOutputDirectory(candidate);
    }
    return m_session->calibrationOutputDir().absolutePath();
}

void MainWindow::setupUi()
{
    m_toolBar = addToolBar(tr("Actions"));
    m_toolBar->setMovable(false);

    auto *central = new QWidget(this);
    auto *mainLayout = new QVBoxLayout(central);
    mainLayout->setContentsMargins(12, 10, 12, 10);
    mainLayout->setSpacing(10);

    auto *pathsWidget = new QWidget(central);
    auto *pathsLayout = new QGridLayout(pathsWidget);
    pathsLayout->setContentsMargins(0, 0, 0, 0);
    pathsLayout->setHorizontalSpacing(12);
    pathsLayout->setVerticalSpacing(6);

    auto *modeLabel = new QLabel(tr("Workflow"), pathsWidget);
    m_modeCombo = new QComboBox(pathsWidget);
    m_modeCombo->addItem(tr("Local images"), static_cast<int>(ProjectSession::DataSource::LocalDataset));
#if MYCALIB_HAVE_CONNECTED_CAMERA
    m_modeCombo->addItem(tr("Live capture"), static_cast<int>(ProjectSession::DataSource::ConnectedCamera));
#endif
    connect(m_modeCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, &MainWindow::handleModeChanged);

    m_inputPathEdit = new QLineEdit(pathsWidget);
    m_inputPathEdit->setPlaceholderText(tr("Images are stored inside the project"));
    m_inputPathEdit->setReadOnly(true);

    m_importButton = new QPushButton(tr("Import images"), pathsWidget);
    m_importButton->setProperty("accent", true);
    connect(m_importButton, &QPushButton::clicked, this, &MainWindow::importLocalImages);

    m_openInputButton = new QPushButton(tr("Open folder"), pathsWidget);
    connect(m_openInputButton, &QPushButton::clicked, this, &MainWindow::openInputLocation);

    m_outputPathEdit = new QLineEdit(pathsWidget);
    m_outputPathEdit->setPlaceholderText(tr("Outputs are stored inside the project"));
    m_outputPathEdit->setReadOnly(true);

    m_openOutputButton = new QPushButton(tr("Open folder"), pathsWidget);
    connect(m_openOutputButton, &QPushButton::clicked, this, &MainWindow::openOutputLocation);

    m_boardInfoLabel = new QLabel(tr("Board: Ø 5 mm · Spacing 25 mm (fixed)"), pathsWidget);

    m_modeChip = new QLabel(pathsWidget);
    m_modeChip->setObjectName(QStringLiteral("modeChip"));
    m_modeChip->setAlignment(Qt::AlignCenter);
    m_modeChip->setVisible(false);
    m_modeChip->setMinimumWidth(120);

    m_modeHeadline = new QLabel(pathsWidget);
    QFont modeHeadlineFont = m_modeHeadline->font();
    modeHeadlineFont.setPointSizeF(modeHeadlineFont.pointSizeF() + 1.0);
    modeHeadlineFont.setBold(true);
    m_modeHeadline->setFont(modeHeadlineFont);
    m_modeHeadline->setObjectName(QStringLiteral("modeHeadline"));

    m_modeDescription = new QLabel(pathsWidget);
    m_modeDescription->setWordWrap(true);
    m_modeDescription->setObjectName(QStringLiteral("modeDescription"));
    m_modeDescription->setTextFormat(Qt::RichText);
    m_modeDescription->setOpenExternalLinks(true);

    m_inputStatusLabel = new QLabel(pathsWidget);
    m_inputStatusLabel->setObjectName(QStringLiteral("inputStatus"));
    m_inputStatusLabel->setWordWrap(true);

    pathsLayout->addWidget(modeLabel, 0, 0);
    pathsLayout->addWidget(m_modeCombo, 0, 1, 1, 2);
    pathsLayout->addWidget(m_modeChip, 0, 3);

    pathsLayout->addWidget(new QLabel(tr("Project images"), pathsWidget), 1, 0);
    pathsLayout->addWidget(m_inputPathEdit, 1, 1, 1, 2);
    auto *inputButtons = new QWidget(pathsWidget);
    auto *inputButtonLayout = new QHBoxLayout(inputButtons);
    inputButtonLayout->setContentsMargins(0, 0, 0, 0);
    inputButtonLayout->setSpacing(6);
    inputButtonLayout->addWidget(m_importButton);
    inputButtonLayout->addWidget(m_openInputButton);
    pathsLayout->addWidget(inputButtons, 1, 3);

    pathsLayout->addWidget(new QLabel(tr("Output folder"), pathsWidget), 2, 0);
    pathsLayout->addWidget(m_outputPathEdit, 2, 1, 1, 2);
    pathsLayout->addWidget(m_openOutputButton, 2, 3);

    pathsLayout->addWidget(m_boardInfoLabel, 3, 0, 1, 4);
    pathsLayout->addWidget(m_modeHeadline, 4, 0, 1, 4);
    pathsLayout->addWidget(m_modeDescription, 5, 0, 1, 4);
    pathsLayout->addWidget(m_inputStatusLabel, 6, 0, 1, 4);
    pathsLayout->setColumnStretch(1, 3);
    pathsLayout->setColumnStretch(2, 1);
    pathsLayout->setColumnStretch(3, 1);
    pathsLayout->setRowStretch(6, 0);

    mainLayout->addWidget(pathsWidget);

    m_stageSplitter = new QSplitter(Qt::Horizontal, central);
    m_stageSplitter->setObjectName(QStringLiteral("stageSplitter"));
    m_stageSplitter->setHandleWidth(6);

    m_stageNavigator = new QListWidget(m_stageSplitter);
    m_stageNavigator->setObjectName(QStringLiteral("stageNavigator"));
    m_stageNavigator->setSelectionMode(QAbstractItemView::SingleSelection);
    m_stageNavigator->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_stageNavigator->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_stageNavigator->setSpacing(4);
    m_stageNavigator->setUniformItemSizes(false);
    m_stageNavigator->setMinimumWidth(180);
    m_stageNavigator->setMaximumWidth(320);

    m_stageStack = new QStackedWidget(m_stageSplitter);
    m_stageStack->setObjectName(QStringLiteral("stageStack"));

    m_stageSplitter->addWidget(m_stageNavigator);
    m_stageSplitter->addWidget(m_stageStack);
    m_stageSplitter->setStretchFactor(0, 0);
    m_stageSplitter->setStretchFactor(1, 1);
    m_stageSplitter->setCollapsible(0, false);
    m_stageSplitter->setCollapsible(1, false);

    mainLayout->addWidget(m_stageSplitter, 1);

    auto addStageNavItem = [&](const QString &title, const QString &subtitle, QListWidgetItem **store) {
        auto *item = new QListWidgetItem(title, m_stageNavigator);
        item->setData(Qt::UserRole, title);
        item->setData(Qt::UserRole + 1, subtitle);
        item->setText(QStringLiteral("%1\n%2").arg(title, subtitle));
        item->setSizeHint(QSize(200, 56));
        item->setToolTip(subtitle);
        if (store) {
            *store = item;
        }
        return item;
    };

    auto createScrollPage = [&](QScrollArea *&scrollPtr, QWidget *&pagePtr, const QMargins &margins) {
        scrollPtr = new QScrollArea(m_stageStack);
        scrollPtr->setWidgetResizable(true);
        scrollPtr->setFrameShape(QFrame::NoFrame);
        scrollPtr->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        scrollPtr->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        auto *container = new QWidget(scrollPtr);
        auto *layout = new QVBoxLayout(container);
        layout->setContentsMargins(margins);
        layout->setSpacing(16);
        container->setLayout(layout);
        scrollPtr->setWidget(container);
        pagePtr = container;
        m_stageStack->addWidget(scrollPtr);
        return layout;
    };

    auto *overviewLayout = createScrollPage(m_stageOverviewScroll, m_stageOverviewPage, QMargins(24, 24, 24, 24));
    addStageNavItem(tr("Overview"), tr("阶段总览与日志"), &m_stageOverviewItem);
    auto applyStageCardStyle = [](QGroupBox *box) {
        if (!box) {
            return;
        }
        box->setAlignment(Qt::AlignLeft);
        box->setStyleSheet(QString::fromUtf8(
            "QGroupBox {"
            "  background: rgba(18, 22, 36, 0.92);"
            "  border: 1px solid rgba(80, 96, 140, 0.55);"
            "  border-radius: 12px;"
            "  margin-top: 14px;"
            "}"
            "QGroupBox::title {"
            "  subcontrol-origin: margin;"
            "  left: 20px;"
            "  padding: 0 6px;"
            "  color: #8fa7ff;"
            "  font-weight: 600;"
            "  letter-spacing: 0.3px;"
            "}"));
    };

    m_stageTuningBox = new QGroupBox(tr("Stage one: Camera tuning"), m_stageOverviewPage);
    applyStageCardStyle(m_stageTuningBox);
    auto *tuningLayout = new QVBoxLayout(m_stageTuningBox);
    tuningLayout->setContentsMargins(18, 24, 18, 16);
    tuningLayout->setSpacing(8);

    auto *tuningHeader = new QHBoxLayout();
    tuningHeader->setContentsMargins(0, 0, 0, 0);
    tuningHeader->setSpacing(6);
    m_stageTuningStatusChip = new QLabel(m_stageTuningBox);
    m_stageTuningStatusChip->setObjectName(QStringLiteral("stageStatusChip"));
    m_stageTuningStatusChip->setVisible(false);
    tuningHeader->addWidget(m_stageTuningStatusChip);
    tuningHeader->addStretch(1);
    tuningLayout->addLayout(tuningHeader);

    m_stageTuningSummaryLabel = new QLabel(m_stageTuningBox);
    m_stageTuningSummaryLabel->setWordWrap(true);
    tuningLayout->addWidget(m_stageTuningSummaryLabel);

    m_stageTuningNotesLabel = new QLabel(m_stageTuningBox);
    m_stageTuningNotesLabel->setWordWrap(true);
    m_stageTuningNotesLabel->setStyleSheet(QStringLiteral("color:#9eb2d4; font-size:11px;"));
    tuningLayout->addWidget(m_stageTuningNotesLabel);

    auto *tuningButtons = new QHBoxLayout();
    tuningButtons->setSpacing(8);
    tuningButtons->setContentsMargins(0, 0, 0, 0);

    m_stageTuningReviewButton = new QPushButton(tr("Review timeline"), m_stageTuningBox);
    m_stageTuningReviewButton->setProperty("accent", true);
    connect(m_stageTuningReviewButton, &QPushButton::clicked, this, &MainWindow::showTuningTimeline);
    tuningButtons->addWidget(m_stageTuningReviewButton);

    m_stageTuningFolderButton = new QPushButton(tr("Open tuning folder"), m_stageTuningBox);
    connect(m_stageTuningFolderButton, &QPushButton::clicked, this, &MainWindow::openTuningFolder);
    tuningButtons->addWidget(m_stageTuningFolderButton);

    m_stageTuningCompleteButton = new QPushButton(tr("Mark stage complete"), m_stageTuningBox);
    connect(m_stageTuningCompleteButton, &QPushButton::clicked, this, &MainWindow::markCameraTuningCompleted);
    tuningButtons->addWidget(m_stageTuningCompleteButton);

    tuningButtons->addStretch(1);
    tuningLayout->addLayout(tuningButtons);

    overviewLayout->addWidget(m_stageTuningBox);

    m_stageCaptureBox = new QGroupBox(tr("Stage two: Calibration capture"), m_stageOverviewPage);
    applyStageCardStyle(m_stageCaptureBox);
    auto *captureLayout = new QVBoxLayout(m_stageCaptureBox);
    captureLayout->setContentsMargins(18, 24, 18, 16);
    captureLayout->setSpacing(8);

    auto *captureHeader = new QHBoxLayout();
    captureHeader->setContentsMargins(0, 0, 0, 0);
    captureHeader->setSpacing(6);
    m_stageCaptureStatusChip = new QLabel(m_stageCaptureBox);
    m_stageCaptureStatusChip->setObjectName(QStringLiteral("stageStatusChip"));
    m_stageCaptureStatusChip->setVisible(false);
    captureHeader->addWidget(m_stageCaptureStatusChip);
    captureHeader->addStretch(1);
    captureLayout->addLayout(captureHeader);

    m_stageCaptureSummaryLabel = new QLabel(m_stageCaptureBox);
    m_stageCaptureSummaryLabel->setWordWrap(true);
    captureLayout->addWidget(m_stageCaptureSummaryLabel);

    m_stageCaptureProgress = new QProgressBar(m_stageCaptureBox);
    m_stageCaptureProgress->setRange(0, kCaptureGridRows * kCaptureGridCols * kCaptureTargetPerCell);
    m_stageCaptureProgress->setFormat(QStringLiteral("%v / %m"));
    captureLayout->addWidget(m_stageCaptureProgress);

    m_stageCaptureTodoLabel = new QLabel(m_stageCaptureBox);
    m_stageCaptureTodoLabel->setWordWrap(true);
    m_stageCaptureTodoLabel->setTextFormat(Qt::RichText);
    captureLayout->addWidget(m_stageCaptureTodoLabel);

    m_stageCaptureModeHint = new QLabel(m_stageCaptureBox);
    m_stageCaptureModeHint->setWordWrap(true);
    m_stageCaptureModeHint->setStyleSheet(QStringLiteral("color:#9eb2d4; font-size:11px;"));
    captureLayout->addWidget(m_stageCaptureModeHint);

    auto *captureButtons = new QHBoxLayout();
    captureButtons->setSpacing(8);
    captureButtons->setContentsMargins(0, 0, 0, 0);

    m_stageCapturePrimaryButton = new QPushButton(tr("Open dataset"), m_stageCaptureBox);
    m_stageCapturePrimaryButton->setProperty("accent", true);
    connect(m_stageCapturePrimaryButton, &QPushButton::clicked, this, &MainWindow::handleStageCapturePrimaryAction);
    captureButtons->addWidget(m_stageCapturePrimaryButton);

    m_stageCaptureSecondaryButton = new QPushButton(tr("Review coverage"), m_stageCaptureBox);
    connect(m_stageCaptureSecondaryButton, &QPushButton::clicked, this, &MainWindow::showCaptureCoverageDialog);
    captureButtons->addWidget(m_stageCaptureSecondaryButton);

    captureButtons->addStretch(1);
    captureLayout->addLayout(captureButtons);

    overviewLayout->addWidget(m_stageCaptureBox);

    auto createMetricCard = [this](const QString &title,
                                   QLabel *&valueLabel,
                                   const QString &subtitle) {
    auto *frame = new QFrame(m_stageOverviewPage);
        frame->setFrameShape(QFrame::NoFrame);
        frame->setObjectName("metricCard");
        frame->setStyleSheet(QString::fromUtf8(
            "QFrame#metricCard {"
            "  background: qlineargradient(x1:0, y1:0, x2:1, y2:1,"
            "    stop:0 rgba(34, 40, 56, 240),"
            "    stop:1 rgba(18, 21, 32, 230));"
            "  border: 1px solid rgba(88, 98, 130, 120);"
            "  border-radius: 14px;"
            "}"
            "QLabel#metricTitle { color: #9FB6FA; font-weight: 600; letter-spacing: 0.4px; }"
            "QLabel#metricValue { color: white; font-size: 22px; font-weight: 700; }"
            "QLabel#metricSubtitle { color: #98A0B8; font-size: 12px; }") );

        auto *layout = new QVBoxLayout(frame);
        layout->setContentsMargins(18, 16, 18, 16);
        layout->setSpacing(6);

        auto *titleLabel = new QLabel(title, frame);
        titleLabel->setObjectName("metricTitle");
        layout->addWidget(titleLabel);

        valueLabel = new QLabel(tr("--"), frame);
        valueLabel->setObjectName("metricValue");
        layout->addWidget(valueLabel);

        if (!subtitle.isEmpty()) {
            auto *subtitleLabel = new QLabel(subtitle, frame);
            subtitleLabel->setObjectName("metricSubtitle");
            subtitleLabel->setWordWrap(true);
            layout->addWidget(subtitleLabel);
        }

        layout->addStretch(1);
        return frame;
    };

    auto addMetricRow = [&](std::initializer_list<std::tuple<QString, QLabel **, QString>> cards) {
        auto *row = new QHBoxLayout();
        row->setSpacing(14);
        row->setContentsMargins(0, 0, 0, 0);
        for (const auto &item : cards) {
            QLabel **labelPtr = std::get<1>(item);
            QWidget *card = createMetricCard(std::get<0>(item), *labelPtr, std::get<2>(item));
            card->setMinimumWidth(160);
            row->addWidget(card, 1);
        }
        overviewLayout->addLayout(row);
    };

    addMetricRow({
        {tr("Total images"), &m_metricTotalImages, tr("Processed images")},
        {tr("Kept samples"), &m_metricKeptImages, tr("Used in calibration")},
        {tr("Removed samples"), &m_metricRemovedImages, tr("Pruned as outliers")},
    });

    addMetricRow({
        {tr("RMS error"), &m_metricRms, tr("Root mean square · px")},
        {tr("Mean error"), &m_metricMeanPx, tr("Mean reprojection · px")},
        {tr("Median error"), &m_metricMedianPx, tr("Median reprojection · px")},
        {tr("P95 error"), &m_metricP95Px, tr("95th percentile · px")},
        {tr("Max error"), &m_metricMaxPx, tr("Worst-case reprojection · px")},
    });

    addMetricRow({
        {tr("Mean |Δ| [mm]"), &m_metricMeanResidualMm, tr("Residual magnitude in camera space")},
        {tr("Mean |Δ| [%]"), &m_metricMeanResidualPercent, tr("Percent residual to board scale")},
    });

    m_laserStageBox = new QGroupBox(tr("Stage three: Laser calibration"), m_stageOverviewPage);
    applyStageCardStyle(m_laserStageBox);
    auto *laserLayout = new QVBoxLayout(m_laserStageBox);
    laserLayout->setContentsMargins(18, 24, 18, 16);
    laserLayout->setSpacing(8);

    auto *laserHeader = new QHBoxLayout();
    laserHeader->setContentsMargins(0, 0, 0, 0);
    laserHeader->setSpacing(6);
    m_laserStageStatusChip = new QLabel(m_laserStageBox);
    m_laserStageStatusChip->setObjectName(QStringLiteral("stageStatusChip"));
    m_laserStageStatusChip->setVisible(false);
    laserHeader->addWidget(m_laserStageStatusChip);
    laserHeader->addStretch(1);
    laserLayout->addLayout(laserHeader);

    m_laserStageStatusLabel = new QLabel(m_laserStageBox);
    m_laserStageStatusLabel->setWordWrap(true);
    laserLayout->addWidget(m_laserStageStatusLabel);

    m_laserStageFrameLabel = new QLabel(m_laserStageBox);
    m_laserStageFrameLabel->setWordWrap(true);
    m_laserStageFrameLabel->setStyleSheet(QStringLiteral("color:#9fb6fa;font-weight:600;"));
    laserLayout->addWidget(m_laserStageFrameLabel);

    m_laserStageHintLabel = new QLabel(m_laserStageBox);
    m_laserStageHintLabel->setWordWrap(true);
    m_laserStageHintLabel->setTextFormat(Qt::RichText);
    m_laserStageHintLabel->setOpenExternalLinks(true);
    laserLayout->addWidget(m_laserStageHintLabel);

    auto *laserActionRow = new QHBoxLayout();
    laserActionRow->setSpacing(8);
    m_importLaserButton = new QPushButton(tr("Import laser frames"), m_laserStageBox);
    connect(m_importLaserButton, &QPushButton::clicked, this, &MainWindow::importLaserFrames);
    laserActionRow->addWidget(m_importLaserButton);
    m_openLaserCaptureButton = new QPushButton(tr("Open capture folder"), m_laserStageBox);
    connect(m_openLaserCaptureButton, &QPushButton::clicked, this, &MainWindow::openLaserCaptureFolder);
    laserActionRow->addWidget(m_openLaserCaptureButton);
    m_openLaserOutputButton = new QPushButton(tr("Open output folder"), m_laserStageBox);
    connect(m_openLaserOutputButton, &QPushButton::clicked, this, &MainWindow::openLaserOutputFolder);
    laserActionRow->addWidget(m_openLaserOutputButton);
    laserActionRow->addStretch(1);
    laserLayout->addLayout(laserActionRow);

    auto *laserStageRow = new QHBoxLayout();
    laserStageRow->setSpacing(8);
    m_markLaserCompletedButton = new QPushButton(tr("Mark stage complete"), m_laserStageBox);
    connect(m_markLaserCompletedButton, &QPushButton::clicked, this, &MainWindow::markLaserStageCompleted);
    laserStageRow->addWidget(m_markLaserCompletedButton);
    laserStageRow->addStretch(1);
    laserLayout->addLayout(laserStageRow);

    overviewLayout->addWidget(m_laserStageBox);

    auto *logTitle = new QLabel(tr("Activity log"), m_stageOverviewPage);
    QFont logFont = logTitle->font();
    logFont.setBold(true);
    logTitle->setFont(logFont);
    overviewLayout->addWidget(logTitle);

    m_logView = new QTextEdit(m_stageOverviewPage);
    m_logView->setReadOnly(true);
    m_logView->setMinimumHeight(160);
    if (m_logView->document()) {
        m_logView->document()->setMaximumBlockCount(2000);
    }
    overviewLayout->addWidget(m_logView, 1);
    overviewLayout->addStretch(1);

    auto *tuningLayoutContainer = createScrollPage(m_stageTuningScroll, m_stageTuningPage, QMargins(24, 24, 24, 24));
    addStageNavItem(tr("Camera tuning"), tr("采集前优化相机设置"), &m_stageTuningItem);

#if MYCALIB_HAVE_CONNECTED_CAMERA
    m_tuningMainColumn = new QWidget(m_stageTuningPage);
    m_tuningMainColumn->setObjectName(QStringLiteral("TuningStageContent"));
    m_tuningMainLayout = new QVBoxLayout(m_tuningMainColumn);
    m_tuningMainLayout->setContentsMargins(0, 0, 0, 0);
    m_tuningMainLayout->setSpacing(18);

    auto *cameraCard = new QFrame(m_tuningMainColumn);
    cameraCard->setObjectName(QStringLiteral("CameraTuningCard"));
    cameraCard->setStyleSheet(QStringLiteral(
        "#CameraTuningCard {"
        "  background: rgba(15, 20, 32, 0.95);"
        "  border: 1px solid rgba(86, 110, 168, 0.38);"
        "  border-radius: 16px;"
        "}"
        "#CameraTuningCard QLabel#CameraTuningTitle {"
        "  color: #f4f7ff;"
        "}"));
    auto *cameraCardLayout = new QVBoxLayout(cameraCard);
    cameraCardLayout->setContentsMargins(20, 20, 20, 20);
    cameraCardLayout->setSpacing(18);

    auto *cameraHeader = new QLabel(tr("相机可视化控制台"), cameraCard);
    cameraHeader->setObjectName(QStringLiteral("CameraTuningTitle"));
    QFont cameraHeaderFont = cameraHeader->font();
    cameraHeaderFont.setPointSizeF(cameraHeaderFont.pointSizeF() + 2.0);
    cameraHeaderFont.setBold(true);
    cameraHeader->setFont(cameraHeaderFont);
    cameraHeader->setStyleSheet(QStringLiteral("color:#f1f5ff;"));

    auto *headerRow = new QHBoxLayout();
    headerRow->setContentsMargins(0, 0, 0, 0);
    headerRow->setSpacing(16);
    headerRow->addWidget(cameraHeader, 0, Qt::AlignLeft);
    headerRow->addStretch(1);

    auto chipStyle = QStringLiteral(
        "padding:4px 14px;"
        "border-radius:14px;"
        "font-weight:600;"
        "letter-spacing:0.3px;"
        "background: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 rgba(90, 148, 255, 0.32), stop:1 rgba(132, 214, 255, 0.28));"
        "color:#f3f7ff;"
        "border:1px solid rgba(120, 196, 255, 0.55);");

    m_tuningCameraStateChip = new QLabel(cameraCard);
    m_tuningCameraStateChip->setObjectName(QStringLiteral("TuningCameraChip"));
    m_tuningCameraStateChip->setStyleSheet(chipStyle);
    m_tuningCameraStateChip->setVisible(false);
    headerRow->addWidget(m_tuningCameraStateChip, 0, Qt::AlignRight);

    m_tuningStreamStateChip = new QLabel(cameraCard);
    m_tuningStreamStateChip->setObjectName(QStringLiteral("TuningStreamChip"));
    m_tuningStreamStateChip->setStyleSheet(chipStyle);
    m_tuningStreamStateChip->setVisible(false);
    headerRow->addWidget(m_tuningStreamStateChip, 0, Qt::AlignRight);

    cameraCardLayout->addLayout(headerRow);

    m_tuningControlsPanel = new QWidget(this);
    m_tuningControlsPanel->setObjectName(QStringLiteral("TuningControlsPanel"));
    auto *controlsLayout = new QHBoxLayout(m_tuningControlsPanel);
    controlsLayout->setContentsMargins(0, 4, 0, 4);
    controlsLayout->setSpacing(14);
    m_tuningControlsPanel->setStyleSheet(QStringLiteral(
        "#TuningControlsPanel {"
        "  padding: 8px 12px 8px 12px;"
        "}"
        "#TuningControlsPanel QToolButton {"
        "  background: rgba(32, 46, 68, 0.68);"
        "  border: 1px solid rgba(110, 138, 190, 0.42);"
        "  border-radius: 14px;"
        "  color: #e6ecff;"
        "  font-weight: 600;"
        "  padding: 10px 12px;"
        "}"
        "#TuningControlsPanel QToolButton:hover {"
        "  background: rgba(80, 143, 255, 0.34);"
        "  border-color: rgba(120, 196, 255, 0.55);"
        "}"
        "#TuningControlsPanel QToolButton:pressed {"
        "  background: rgba(80, 143, 255, 0.46);"
        "}"
        "#TuningControlsPanel QToolButton:disabled {"
        "  background: rgba(40, 50, 70, 0.38);"
        "  color: rgba(210, 220, 240, 0.35);"
        "  border-color: rgba(90, 110, 150, 0.28);"
        "}"));

    auto configureButton = [&](QToolButton *&button,
                               const QString &iconPath,
                               const QString &text,
                               auto slot) {
        button = new QToolButton(m_tuningControlsPanel);
        button->setAutoRaise(true);
        button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        button->setIcon(QIcon(iconPath));
        button->setIconSize(QSize(28, 28));
        button->setMinimumWidth(84);
        button->setText(text);
        controlsLayout->addWidget(button);
        connect(button, &QToolButton::clicked, this, slot);
    };

    configureButton(m_tuningConnectButton,
                    QStringLiteral(":/icons/camera_open.svg"),
                    tr("连接"),
                    &MainWindow::connectCamera);
    configureButton(m_tuningDisconnectButton,
                    QStringLiteral(":/icons/camera_close.svg"),
                    tr("断开"),
                    &MainWindow::disconnectCamera);
    configureButton(m_tuningStartButton,
                    QStringLiteral(":/icons/stream_start.svg"),
                    tr("开始预览"),
                    &MainWindow::startCameraStream);
    configureButton(m_tuningStopButton,
                    QStringLiteral(":/icons/stream_stop.svg"),
                    tr("停止预览"),
                    &MainWindow::stopCameraStream);
    configureButton(m_tuningSnapshotButton,
                    QStringLiteral(":/icons/snapshot.svg"),
                    tr("快照"),
                    &MainWindow::captureFromCamera);
    configureButton(m_tuningRefreshButton,
                    QStringLiteral(":/icons/refresh.svg"),
                    tr("刷新列表"),
                    &MainWindow::refreshCameraDevices);

    controlsLayout->addStretch(1);
    m_tuningControlsPanel->hide();

    auto *controlRow = new QHBoxLayout();
    controlRow->setContentsMargins(0, 0, 0, 0);
    controlRow->setSpacing(12);

    m_tuningDeviceLabel = new QLabel(tr("可连接设备"), cameraCard);
    m_tuningDeviceLabel->setStyleSheet(QStringLiteral("color:#d5dfff;font-weight:600;"));
    controlRow->addWidget(m_tuningDeviceLabel);

    m_tuningDeviceCombo = m_cameraWindow ? m_cameraWindow->cameraSelector() : nullptr;
    if (m_tuningDeviceCombo) {
        m_tuningDeviceCombo->setParent(cameraCard);
        m_tuningDeviceCombo->setMinimumHeight(34);
        m_tuningDeviceCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        m_tuningDeviceCombo->setStyleSheet(QStringLiteral(
            "QComboBox {"
            "  padding: 6px 12px;"
            "  border-radius: 10px;"
            "  border: 1px solid rgba(110, 138, 190, 0.45);"
            "  background: rgba(32, 46, 68, 0.78);"
            "  color: #f1f5ff;"
            "  font-weight: 600;"
            "}"
            "QComboBox::drop-down { width: 28px; border-left: 1px solid rgba(110, 138, 190, 0.4); }"));
        controlRow->addWidget(m_tuningDeviceCombo, 1);
    } else {
        auto *placeholder = new QLabel(tr("未检测到相机"), cameraCard);
        placeholder->setStyleSheet(QStringLiteral("color:#8ea2c0;"));
        controlRow->addWidget(placeholder, 1);
    }

    m_tuningFeaturePopupButton = new QPushButton(tr("特征参数"), cameraCard);
    m_tuningFeaturePopupButton->setObjectName(QStringLiteral("FeaturePanelToggle"));
    m_tuningFeaturePopupButton->setCursor(Qt::PointingHandCursor);
    m_tuningFeaturePopupButton->setMinimumHeight(36);
    m_tuningFeaturePopupButton->setToolTip(tr("打开相机特征与属性调节面板"));
    m_tuningFeaturePopupButton->setStyleSheet(QStringLiteral(
        "QPushButton#FeaturePanelToggle {"
        "  padding: 8px 14px;"
        "  border-radius: 14px;"
        "  background: rgba(26, 38, 60, 0.86);"
        "  color: #ebf3ff;"
        "  font-weight: 600;"
        "  border: 1px solid rgba(120, 156, 230, 0.45);"
        "}"
        "QPushButton#FeaturePanelToggle:hover {"
        "  background: rgba(80, 143, 255, 0.42);"
        "  border-color: rgba(120, 196, 255, 0.6);"
        "  color: #0b1b36;"
        "}"
        "QPushButton#FeaturePanelToggle:pressed {"
        "  background: rgba(120, 196, 255, 0.72);"
        "  color: #071020;"
        "}"
        "QPushButton#FeaturePanelToggle:disabled {"
        "  background: rgba(32, 46, 68, 0.42);"
        "  color: rgba(210, 220, 240, 0.45);"
        "  border-color: rgba(80, 120, 180, 0.32);"
        "}"));
    connect(m_tuningFeaturePopupButton, &QPushButton::clicked, this, &MainWindow::showFeaturePanelPopup);
    controlRow->addWidget(m_tuningFeaturePopupButton, 0);

    m_tuningControlPopupButton = new QPushButton(tr("展开控制面板"), cameraCard);
    m_tuningControlPopupButton->setObjectName(QStringLiteral("TuningControlsToggle"));
    m_tuningControlPopupButton->setCursor(Qt::PointingHandCursor);
    m_tuningControlPopupButton->setMinimumHeight(36);
    m_tuningControlPopupButton->setToolTip(tr("连接、断开、取流与快照等操作"));
    m_tuningControlPopupButton->setStyleSheet(QStringLiteral(
        "QPushButton#TuningControlsToggle {"
        "  padding: 8px 16px;"
        "  border-radius: 14px;"
        "  background: rgba(80, 143, 255, 0.38);"
        "  color: #0b1b36;"
        "  font-weight: 600;"
        "}"
        "QPushButton#TuningControlsToggle:hover {"
        "  background: rgba(120, 196, 255, 0.68);"
        "}"
        "QPushButton#TuningControlsToggle:pressed {"
        "  background: rgba(120, 196, 255, 0.88);"
        "}"
        "QPushButton#TuningControlsToggle:disabled {"
        "  background: rgba(40, 68, 110, 0.45);"
        "  color: rgba(210, 220, 240, 0.45);"
        "}"));
    connect(m_tuningControlPopupButton, &QPushButton::clicked, this, &MainWindow::showTuningControlsPopup);
    controlRow->addWidget(m_tuningControlPopupButton, 0);

    cameraCardLayout->addLayout(controlRow);

    m_cameraWindow = new CameraWindow(cameraCard);
    m_cameraWindow->setObjectName(QStringLiteral("CameraWindowEmbed"));
    m_cameraWindow->setWindowFlag(Qt::Widget, true);
    m_cameraWindow->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_cameraWindow->setEmbeddedMode(true);
    cameraCardLayout->addWidget(m_cameraWindow, 1);

    m_tuningMainLayout->addWidget(cameraCard, 1);
    tuningLayoutContainer->addWidget(m_tuningMainColumn, 1);

    connect(m_cameraWindow, &CameraWindow::snapshotCaptured, this, &MainWindow::handleCameraSnapshotCaptured);
    connect(m_cameraWindow, &CameraWindow::connectionStateChanged, this, &MainWindow::handleCameraConnectionChanged);
    connect(m_cameraWindow, &CameraWindow::streamingStateChanged, this, &MainWindow::handleCameraStreamingChanged);
    connect(m_cameraWindow, &CameraWindow::tuningTimelineRequested, this, &MainWindow::showTuningTimeline);
#else
    m_tuningMainColumn = new QWidget(m_stageTuningPage);
    m_tuningMainColumn->setObjectName(QStringLiteral("TuningStageContent"));
    m_tuningMainLayout = new QVBoxLayout(m_tuningMainColumn);
    m_tuningMainLayout->setContentsMargins(0, 0, 0, 0);
    m_tuningMainLayout->setSpacing(18);
    tuningLayoutContainer->addWidget(m_tuningMainColumn, 1);
#endif

    m_stageCapturePage = new QWidget(m_stageStack);
    auto *capturePageLayout = new QVBoxLayout(m_stageCapturePage);
    capturePageLayout->setContentsMargins(12, 12, 12, 12);
    capturePageLayout->setSpacing(12);

    auto *captureIntro = new QLabel(tr("左侧列表列出了所有样本，右侧可快速检查检测结果。"), m_stageCapturePage);
    captureIntro->setWordWrap(true);
    captureIntro->setStyleSheet(QStringLiteral("color:#9eb2d4;"));
    capturePageLayout->addWidget(captureIntro);

    m_captureTabs = new QTabWidget(m_stageCapturePage);
    m_captureTabs->setDocumentMode(true);
    m_captureTabs->setTabPosition(QTabWidget::North);
    m_captureTabs->setMovable(false);

    m_datasetPage = new QWidget(m_captureTabs);
    auto *datasetLayout = new QVBoxLayout(m_datasetPage);
    datasetLayout->setContentsMargins(0, 0, 0, 0);
    datasetLayout->setSpacing(0);

    auto *imageSplitter = new QSplitter(Qt::Horizontal, m_datasetPage);
    imageSplitter->setHandleWidth(6);

    auto *imageLeftPane = new QWidget(imageSplitter);
    auto *leftLayout = new QVBoxLayout(imageLeftPane);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(8);

    if (!m_capturePlannerHost) {
        m_capturePlannerHost = new QWidget(imageLeftPane);
        m_capturePlannerHostLayout = new QVBoxLayout(m_capturePlannerHost);
        m_capturePlannerHostLayout->setContentsMargins(0, 0, 0, 0);
        m_capturePlannerHostLayout->setSpacing(8);
    }
    leftLayout->addWidget(m_capturePlannerHost, 0);

    setupCapturePlanner();
    ensureCapturePlannerMounted();
    refreshCapturePlanFromSession();

    m_detectionTree = new QTreeWidget(imageLeftPane);
    m_detectionTree->setHeaderLabels({tr("Image"), tr("Mean px"), tr("Max px"), tr("|ΔX| mm"), tr("|ΔY| mm"), tr("|ΔZ| mm"), tr("Status")});
    m_detectionTree->header()->setStretchLastSection(false);
    m_detectionTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int col = 1; col <= 5; ++col) {
        m_detectionTree->header()->setSectionResizeMode(col, QHeaderView::ResizeToContents);
    }
    m_detectionTree->header()->setSectionResizeMode(6, QHeaderView::ResizeToContents);
    m_detectionTree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_detectionTree->setSortingEnabled(true);
    m_detectionTree->header()->setSortIndicatorShown(true);
    m_detectionTree->header()->setSectionsClickable(true);
    m_detectionTree->header()->setSortIndicator(0, Qt::AscendingOrder);
    connect(m_detectionTree->header(), &QHeaderView::sectionClicked, this, [this](int section) {
        if (!m_detectionTree) {
            return;
        }
        if (m_lastSortColumn == section) {
            m_lastSortOrder = (m_lastSortOrder == Qt::AscendingOrder) ? Qt::DescendingOrder : Qt::AscendingOrder;
        } else {
            m_lastSortColumn = section;
            m_lastSortOrder = (section == 0) ? Qt::AscendingOrder : Qt::DescendingOrder;
        }
        m_detectionTree->sortItems(section, m_lastSortOrder);
        m_detectionTree->header()->setSortIndicator(section, m_lastSortOrder);
    });
    connect(m_detectionTree, &QTreeWidget::itemSelectionChanged, this, &MainWindow::handleDetectionSelectionChanged);
    leftLayout->addWidget(m_detectionTree, 1);

    m_captureFeedbackBox = new QGroupBox(tr("Stage-two capture feedback"), imageLeftPane);
    auto *feedbackLayout = new QVBoxLayout(m_captureFeedbackBox);
    feedbackLayout->setContentsMargins(12, 10, 12, 10);
    feedbackLayout->setSpacing(6);
    m_captureFeedbackSummary = new QLabel(m_captureFeedbackBox);
    m_captureFeedbackSummary->setWordWrap(true);
    feedbackLayout->addWidget(m_captureFeedbackSummary);
    m_captureFeedbackPose = new QLabel(m_captureFeedbackBox);
    m_captureFeedbackPose->setWordWrap(true);
    m_captureFeedbackPose->setStyleSheet(QStringLiteral("color:#a8b9da;"));
    feedbackLayout->addWidget(m_captureFeedbackPose);
    m_captureFeedbackActions = new QLabel(m_captureFeedbackBox);
    m_captureFeedbackActions->setWordWrap(true);
    m_captureFeedbackActions->setStyleSheet(QStringLiteral("color:#ffb86c;"));
    feedbackLayout->addWidget(m_captureFeedbackActions);
    feedbackLayout->addStretch(1);
    leftLayout->addWidget(m_captureFeedbackBox, 0);

    auto *detailBox = new QGroupBox(tr("Sample details"), imageLeftPane);
    auto *detailLayout = new QVBoxLayout(detailBox);
    detailLayout->setContentsMargins(12, 8, 12, 8);
    detailLayout->setSpacing(6);
    m_detectionMetaLabel = new QLabel(tr("Select an image on the left to inspect residuals."), detailBox);
    m_detectionMetaLabel->setWordWrap(true);
    m_detectionResidualMmLabel = new QLabel(detailBox);
    m_detectionResidualMmLabel->setWordWrap(true);
    m_detectionResidualPercentLabel = new QLabel(detailBox);
    m_detectionResidualPercentLabel->setWordWrap(true);
    detailLayout->addWidget(m_detectionMetaLabel);
    detailLayout->addWidget(m_detectionResidualMmLabel);
    detailLayout->addWidget(m_detectionResidualPercentLabel);
    detailLayout->addStretch(1);
    leftLayout->addWidget(detailBox, 0);

    imageSplitter->addWidget(imageLeftPane);

    auto *imageRightPane = new QWidget(imageSplitter);
    auto *rightLayout = new QVBoxLayout(imageRightPane);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(6);
    m_detectionPreview = new DetectionPreviewWidget(imageRightPane);
    rightLayout->addWidget(m_detectionPreview, 1);
    imageSplitter->addWidget(imageRightPane);
    imageSplitter->setStretchFactor(0, 1);
    imageSplitter->setStretchFactor(1, 2);

    datasetLayout->addWidget(imageSplitter);
    m_captureTabs->addTab(m_datasetPage, tr("Samples"));

    capturePageLayout->addWidget(m_captureTabs, 1);
    m_stageStack->addWidget(m_stageCapturePage);
    addStageNavItem(tr("Capture & dataset"), tr("样本管理与覆盖跟踪"), &m_stageCaptureItem);

    m_stageAnalysisPage = new QWidget(m_stageStack);
    auto *analysisLayout = new QVBoxLayout(m_stageAnalysisPage);
    analysisLayout->setContentsMargins(24, 24, 24, 24);
    analysisLayout->setSpacing(16);

    auto *analysisIntro = new QLabel(tr("浏览所有阶段的数值指标、热力图与三维检视。"), m_stageAnalysisPage);
    analysisIntro->setWordWrap(true);
    analysisIntro->setStyleSheet(QStringLiteral("color:#9eb2d4;"));
    analysisLayout->addWidget(analysisIntro);

    m_analysisTabs = new QTabWidget(m_stageAnalysisPage);
    m_analysisTabs->setDocumentMode(true);
    m_analysisTabs->setTabPosition(QTabWidget::North);

    m_heatmapBoard = new HeatmapView(m_analysisTabs);
    m_heatmapBoard->setTitle(tr("Coverage"));
    m_analysisTabs->addTab(m_heatmapBoard, tr("Coverage"));

    if (m_heatmapBoard) {
        m_heatmapBoard->setLegendUnit(tr("ratio"));
        m_heatmapBoard->setLegendTickCount(5);
        m_heatmapBoard->setLegendPrecision(2);
    }

    m_heatmapPixel = new HeatmapView(m_analysisTabs);
    m_heatmapPixel->setTitle(tr("Pixel error"));
    m_analysisTabs->addTab(m_heatmapPixel, tr("Pixel error"));

    if (m_heatmapPixel) {
        m_heatmapPixel->setLegendUnit(tr("px"));
        m_heatmapPixel->setLegendTickCount(5);
        m_heatmapPixel->setLegendPrecision(2);
    }

    m_scatterView = new ResidualScatterView(m_analysisTabs);
    m_analysisTabs->addTab(m_scatterView, tr("Residual scatter"));

    m_distortionMap = new HeatmapView(m_analysisTabs);
    m_distortionMap->setTitle(tr("Distortion"));
    m_analysisTabs->addTab(m_distortionMap, tr("Distortion"));

    if (m_distortionMap) {
        m_distortionMap->setLegendUnit(tr("px"));
        m_distortionMap->setLegendTickCount(5);
        m_distortionMap->setLegendPrecision(2);
        m_distortionMap->setGridOverlayEnabled(true);
    }

    m_poseTab = new QWidget(m_analysisTabs);
    m_poseStack = new QStackedLayout(m_poseTab);
    m_poseStack->setContentsMargins(0, 0, 0, 0);
    m_posePlaceholder = new QLabel(tr("The calibrated board pose will appear here once processing is done."), m_poseTab);
    m_posePlaceholder->setAlignment(Qt::AlignCenter);
    m_posePlaceholder->setWordWrap(true);
    m_posePlaceholder->setMargin(24);
    m_poseStack->addWidget(m_posePlaceholder);
    m_analysisTabs->addTab(m_poseTab, tr("3D inspection"));

    analysisLayout->addWidget(m_analysisTabs, 1);

    m_stageStack->addWidget(m_stageAnalysisPage);
    addStageNavItem(tr("Analytics"), tr("热力图、残差与激光流程"), &m_stageAnalysisItem);

    connect(m_stageNavigator, &QListWidget::currentRowChanged, this, [this](int row) {
        if (!m_stageStack) {
            return;
        }
        if (row >= 0 && row < m_stageStack->count()) {
            m_stageStack->setCurrentIndex(row);
        }
    });

    if (m_stageNavigator->count() > 0) {
        m_stageNavigator->setCurrentRow(0);
    }

    m_progressBar = new QProgressBar(central);
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    mainLayout->addWidget(m_progressBar);

    setCentralWidget(central);
    setupTuningSection();
    updateStageNavigator();
    updateCaptureFeedback(CalibrationOutput{});
}

void MainWindow::setupTuningSection()
{
    if (m_tuningTimelineBox) {
        return;
    }
    if (!m_stageTuningPage) {
        return;
    }

    auto *pageLayout = qobject_cast<QVBoxLayout *>(m_stageTuningPage->layout());
    if (!pageLayout) {
        pageLayout = new QVBoxLayout(m_stageTuningPage);
        pageLayout->setContentsMargins(20, 20, 20, 20);
        pageLayout->setSpacing(12);
    }

    m_tuningTimelineBox = new QGroupBox(tr("Tuning timeline"), m_stageTuningPage);
    m_tuningTimelineBox->setObjectName(QStringLiteral("tuningTimelineBox"));
    m_tuningTimelineBox->setStyleSheet(QStringLiteral(
        "QGroupBox#tuningTimelineBox {"
        "  background: rgba(15, 20, 32, 0.95);"
        "  border: 1px solid rgba(86, 110, 168, 0.38);"
        "  border-radius: 16px;"
        "  margin-top: 8px;"
        "}"
        "QGroupBox#tuningTimelineBox::title {"
        "  subcontrol-origin: margin;"
        "  left: 20px;"
        "  padding: 0 6px;"
        "  color: #9fb4ff;"
        "  font-weight: 600;"
        "  letter-spacing: 0.3px;"
        "}"));
    m_tuningTimelineBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    auto *layout = new QVBoxLayout(m_tuningTimelineBox);
    layout->setContentsMargins(18, 18, 18, 18);
    layout->setSpacing(10);

    m_tuningStageLabel = new QLabel(m_tuningTimelineBox);
    m_tuningStageLabel->setObjectName(QStringLiteral("tuningStage"));
    m_tuningStageLabel->setTextFormat(Qt::RichText);
    m_tuningStageLabel->setWordWrap(true);
    layout->addWidget(m_tuningStageLabel);

    m_tuningCountLabel = new QLabel(m_tuningTimelineBox);
    m_tuningCountLabel->setObjectName(QStringLiteral("tuningCount"));
    m_tuningCountLabel->setStyleSheet(QStringLiteral("color:#8ea2c0;font-size:11px;"));
    m_tuningCountLabel->setWordWrap(true);
    layout->addWidget(m_tuningCountLabel);

    m_tuningTimeline = new QTreeWidget(m_tuningTimelineBox);
    m_tuningTimeline->setObjectName(QStringLiteral("tuningTimeline"));
    m_tuningTimeline->setColumnCount(5);
    m_tuningTimeline->setHeaderLabels({tr("Captured"), tr("Exposure"), tr("Gain"), tr("Frame rate"), tr("Location")});
    m_tuningTimeline->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tuningTimeline->setSortingEnabled(true);
    if (auto *header = m_tuningTimeline->header()) {
        header->setStretchLastSection(true);
        header->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        header->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        header->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        header->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    }
    layout->addWidget(m_tuningTimeline, 1);
    connect(m_tuningTimeline, &QTreeWidget::itemActivated, this, &MainWindow::handleTuningItemActivated);
    connect(m_tuningTimeline, &QTreeWidget::itemDoubleClicked, this, &MainWindow::handleTuningItemActivated);

    auto *buttonRow = new QHBoxLayout();
    buttonRow->setContentsMargins(0, 0, 0, 0);
    buttonRow->setSpacing(8);

    auto *openFolderButton = new QPushButton(tr("Open tuning folder"), m_tuningTimelineBox);
    connect(openFolderButton, &QPushButton::clicked, this, &MainWindow::openTuningFolder);
    buttonRow->addWidget(openFolderButton);

    buttonRow->addStretch(1);

    m_markTuningDoneButton = new QPushButton(tr("Mark stage complete"), m_tuningTimelineBox);
    m_markTuningDoneButton->setProperty("accent", true);
    connect(m_markTuningDoneButton, &QPushButton::clicked, this, &MainWindow::markCameraTuningCompleted);
    buttonRow->addWidget(m_markTuningDoneButton);

    layout->addLayout(buttonRow);

    QVBoxLayout *hostLayout = m_tuningMainLayout ? m_tuningMainLayout : pageLayout;
    if (hostLayout) {
        hostLayout->addWidget(m_tuningTimelineBox);
        hostLayout->addStretch(1);
    }

    refreshTuningPanel();
    updateTuningStageUi();
}

void MainWindow::setupCapturePlanner()
{
    if (m_capturePlannerWidget) {
        return;
    }

    m_capturePlannerWidget = new QFrame;
    m_capturePlannerWidget->setObjectName(QStringLiteral("capturePlanner"));
    m_capturePlannerWidget->setFrameShape(QFrame::StyledPanel);
    m_capturePlannerWidget->setStyleSheet(QStringLiteral("#capturePlanner { background: rgba(12,18,28,0.72); border: 1px solid rgba(80,143,255,0.22); border-radius: 12px; }"));

    auto *outerLayout = new QVBoxLayout(m_capturePlannerWidget);
    outerLayout->setContentsMargins(16, 14, 16, 14);
    outerLayout->setSpacing(10);

    m_capturePlanTitleLabel = new QLabel(tr("阶段二 · 九宫格采集"), m_capturePlannerWidget);
    QFont titleFont = m_capturePlanTitleLabel->font();
    titleFont.setBold(true);
    m_capturePlanTitleLabel->setFont(titleFont);
    outerLayout->addWidget(m_capturePlanTitleLabel);

    m_capturePlanHintLabel = new QLabel(tr("选择取景格和姿态后按下“快照”。建议每格采集 %1 张（至少 %2 张），覆盖五种姿态。")
                                            .arg(kCaptureTargetPerCell)
                                            .arg(kCaptureMinimumPerCell),
                                        m_capturePlannerWidget);
    m_capturePlanHintLabel->setWordWrap(true);
    m_capturePlanHintLabel->setStyleSheet(QStringLiteral("color: rgba(210, 223, 245, 0.78);"));
    outerLayout->addWidget(m_capturePlanHintLabel);

    m_capturePlanToggle = new QCheckBox(tr("将快照记录到阶段二采集计划"), m_capturePlannerWidget);
    m_capturePlanToggle->setChecked(false);
    outerLayout->addWidget(m_capturePlanToggle);

    auto *gridContainer = new QWidget(m_capturePlannerWidget);
    auto *gridLayout = new QGridLayout(gridContainer);
    gridLayout->setContentsMargins(0, 0, 0, 0);
    gridLayout->setSpacing(6);

    m_captureGridGroup = new QButtonGroup(this);
    m_captureGridGroup->setExclusive(true);

    for (int r = 0; r < kCaptureGridRows; ++r) {
        for (int c = 0; c < kCaptureGridCols; ++c) {
            auto *button = new QToolButton(gridContainer);
            button->setCheckable(true);
            button->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
            button->setMinimumSize(80, 64);
            button->setText(captureCellDisplayName(r, c));
            button->setProperty("capture-grid", true);
            button->setProperty("gridRow", r);
            button->setProperty("gridCol", c);
            button->installEventFilter(this);
            m_captureGridGroup->addButton(button, encodeGridId(r, c));
            gridLayout->addWidget(button, r, c);
        }
    }
    outerLayout->addWidget(gridContainer);

    m_capturePoseContainer = new QWidget(m_capturePlannerWidget);
    auto *poseLayout = new QHBoxLayout(m_capturePoseContainer);
    poseLayout->setContentsMargins(0, 0, 0, 0);
    poseLayout->setSpacing(8);

    m_capturePoseGroup = new QButtonGroup(this);
    m_capturePoseGroup->setExclusive(true);

    for (int i = 0; i < static_cast<int>(std::size(kPoseDescriptors)); ++i) {
        const auto &descriptor = kPoseDescriptors[i];
        auto *button = new QToolButton(m_capturePoseContainer);
        button->setCheckable(true);
        button->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
        button->setText(tr(descriptor.display));
        button->setToolTip(tr(descriptor.hint));
        m_capturePoseGroup->addButton(button, i);
        poseLayout->addWidget(button, 1);
    }
    outerLayout->addWidget(m_capturePoseContainer);

    m_capturePlanSelectionLabel = new QLabel(m_capturePlannerWidget);
    m_capturePlanSelectionLabel->setStyleSheet(QStringLiteral("color:#9eb2d4;"));
    m_capturePlanSelectionLabel->setWordWrap(true);
    outerLayout->addWidget(m_capturePlanSelectionLabel);

    m_capturePlanPoseLabel = new QLabel(m_capturePlannerWidget);
    m_capturePlanPoseLabel->setStyleSheet(QStringLiteral("color:#9eb2d4;"));
    m_capturePlanPoseLabel->setWordWrap(true);
    outerLayout->addWidget(m_capturePlanPoseLabel);

    m_capturePlanSummary = new QLabel(m_capturePlannerWidget);
    m_capturePlanSummary->setStyleSheet(QStringLiteral("color:#d8e2f1;font-weight:600;"));
    m_capturePlanSummary->setWordWrap(true);
    outerLayout->addWidget(m_capturePlanSummary);

    m_capturePlanDetailButton = new QPushButton(tr("查看格位详情"), m_capturePlannerWidget);
    m_capturePlanDetailButton->setToolTip(tr("双击九宫格或点击此按钮查看采集详情。"));
    m_capturePlanDetailButton->setVisible(false);
    outerLayout->addWidget(m_capturePlanDetailButton);

    outerLayout->addStretch(1);

    connect(m_capturePlanToggle, &QCheckBox::toggled, this, [this](bool enabled) {
        handleCapturePlanToggled(enabled);
    });
    connect(m_captureGridGroup, QOverload<int>::of(&QButtonGroup::idClicked), this, [this](int id) {
        handleCaptureGridSelection(id);
    });
    connect(m_capturePoseGroup, QOverload<int>::of(&QButtonGroup::idClicked), this, [this](int id) {
        handleCapturePoseSelection(id);
    });

    if (auto *button = m_captureGridGroup->button(encodeGridId(0, 0))) {
        button->setChecked(true);
    }
    if (auto *poseButton = m_capturePoseGroup->button(0)) {
        poseButton->setChecked(true);
    }
    m_captureSelectedRow = 0;
    m_captureSelectedCol = 0;
    m_captureSelectedPose = ProjectSession::CapturePose::Flat;

    if (m_capturePlanDetailButton) {
        connect(m_capturePlanDetailButton, &QPushButton::clicked, this, [this]() {
            if (m_captureSelectedRow < 0 || m_captureSelectedRow >= kCaptureGridRows ||
                m_captureSelectedCol < 0 || m_captureSelectedCol >= kCaptureGridCols) {
                return;
            }
            showCaptureCellDetails(m_captureSelectedRow, m_captureSelectedCol);
        });
    }

    refreshCapturePlanUi();
    recomputeCellQuality();
}

QVector<ProjectSession::CaptureShot> MainWindow::aggregateCoverageShots() const
{
    QVector<ProjectSession::CaptureShot> shots;
    if (m_session) {
        shots = m_session->calibrationShots();
        if (m_session->metadata().dataSource == ProjectSession::DataSource::LocalDataset) {
            shots.reserve(shots.size() + m_datasetDerivedShots.size());
            for (const auto &derived : m_datasetDerivedShots) {
                shots.append(derived);
            }
        }
    }
    return shots;
}

void MainWindow::updateDerivedCoverageFromOutput(const CalibrationOutput &output)
{
    m_datasetDerivedShots.clear();

    if (!m_session || m_session->metadata().dataSource != ProjectSession::DataSource::LocalDataset) {
        return;
    }

    const auto &sourceDetections = !output.keptDetections.empty() ? output.keptDetections : output.allDetections;
    for (const auto &det : sourceDetections) {
        if (!det.success) {
            continue;
        }

        const auto [row, col] = inferGridCellFromDetection(det);
        ProjectSession::CaptureShot shot;
        shot.gridRow = row;
        shot.gridCol = col;
        shot.pose = inferPoseFromDetection(det);
        shot.relativePath = QString::fromStdString(det.name);
        shot.accepted = true;
        shot.metadata.insert(QStringLiteral("coverage_source"), QStringLiteral("detection"));
        m_datasetDerivedShots.append(shot);
    }
}

void MainWindow::ensureCapturePlannerMounted()
{
    if (!m_capturePlannerWidget) {
        return;
    }

    QWidget *targetParent = nullptr;
    if (m_capturePlannerHost) {
        targetParent = m_capturePlannerHost;
    }

    if (!targetParent) {
        m_capturePlannerWidget->setVisible(false);
        return;
    }

    if (m_capturePlannerWidget->parentWidget() != targetParent) {
        m_capturePlannerWidget->setParent(targetParent);
    }

    if (targetParent == m_capturePlannerHost && m_capturePlannerHostLayout) {
        if (m_capturePlannerHostLayout->indexOf(m_capturePlannerWidget) < 0) {
            m_capturePlannerHostLayout->insertWidget(0, m_capturePlannerWidget);
        }
    }

    if (m_capturePlannerHost) {
        m_capturePlannerHost->setVisible(targetParent == m_capturePlannerHost);
    }

    configureCapturePlannerForMode(m_activeSource);
    m_capturePlannerWidget->show();
    refreshCapturePlanUi();
    updateCaptureOverlay();
}

void MainWindow::configureCapturePlannerForMode(ProjectSession::DataSource source)
{
    const bool cameraMode = (source == ProjectSession::DataSource::ConnectedCamera);

    if (m_capturePlanTitleLabel) {
        m_capturePlanTitleLabel->setText(cameraMode
                                             ? tr("阶段二 · 九宫格采集")
                                             : tr("阶段二 · 九宫格统计"));
    }

    if (m_capturePlanHintLabel) {
        if (cameraMode) {
            m_capturePlanHintLabel->setText(tr("选择取景格和姿态后按下“快照”。建议每格采集 %1 张（至少 %2 张），覆盖五种姿态。")
                                               .arg(kCaptureTargetPerCell)
                                               .arg(kCaptureMinimumPerCell));
        } else {
            m_capturePlanHintLabel->setText(tr("运行标定后，系统会根据检测结果自动映射九宫格覆盖。点击任意格查看样本数量。"));
        }
    }

    if (m_capturePlanToggle) {
        if (!cameraMode && m_capturePlanToggle->isChecked()) {
            {
                QSignalBlocker blocker(m_capturePlanToggle);
                m_capturePlanToggle->setChecked(false);
            }
            handleCapturePlanToggled(false);
        }
        m_capturePlanToggle->setVisible(cameraMode);
        m_capturePlanToggle->setEnabled(cameraMode);
    }

    if (m_capturePoseContainer) {
        m_capturePoseContainer->setVisible(cameraMode);
    }

    if (m_capturePlanSelectionLabel) {
        m_capturePlanSelectionLabel->setVisible(cameraMode);
    }

    if (m_capturePlanPoseLabel) {
        m_capturePlanPoseLabel->setVisible(cameraMode);
    }
}

void MainWindow::refreshCapturePlanFromSession()
{
    for (auto &row : m_capturePlanState) {
        for (auto &cell : row) {
            cell.total = 0;
            cell.poseCounts.fill(0);
        }
    }

    const auto shots = aggregateCoverageShots();
    if (shots.isEmpty()) {
        refreshCapturePlanUi();
        return;
    }

    for (const auto &shot : shots) {
        if (shot.gridRow < 0 || shot.gridRow >= kCaptureGridRows ||
            shot.gridCol < 0 || shot.gridCol >= kCaptureGridCols) {
            continue;
        }
        auto &cell = m_capturePlanState[shot.gridRow][shot.gridCol];
        ++cell.total;
        const int poseIdx = capturePoseIndex(shot.pose);
        if (poseIdx >= 0 && poseIdx < static_cast<int>(cell.poseCounts.size())) {
            ++cell.poseCounts[poseIdx];
        }
    }

    refreshCapturePlanUi();
}

void MainWindow::refreshCapturePlanUi()
{
    if (!m_capturePlannerWidget) {
        return;
    }
    refreshCaptureGridButtons();
    refreshCapturePoseButtons();

    const int total = captureTotalShots();
    if (m_capturePlanSummary) {
        if (m_activeSource == ProjectSession::DataSource::ConnectedCamera) {
            const int targetTotal = kCaptureGridRows * kCaptureGridCols * kCaptureTargetPerCell;
            m_capturePlanSummary->setText(tr("阶段二进度：%1 / %2 张样本")
                                              .arg(total)
                                              .arg(targetTotal));
        } else {
            m_capturePlanSummary->setText(tr("基于检测结果的九宫格统计：%1 张样本" )
                                              .arg(total));
        }
    }

    updateCaptureOverlay();
    updateCaptureFeedback(m_lastOutput);
}

void MainWindow::refreshCaptureGridButtons()
{
    if (!m_captureGridGroup) {
        return;
    }

    for (int r = 0; r < kCaptureGridRows; ++r) {
        for (int c = 0; c < kCaptureGridCols; ++c) {
            auto *button = qobject_cast<QToolButton *>(m_captureGridGroup->button(encodeGridId(r, c)));
            if (!button) {
                continue;
            }
            const auto &cellState = m_capturePlanState[r][c];
            const auto &quality = m_cellQuality[r][c];
            const bool meetsRecommended = cellState.total >= kCaptureTargetPerCell;
            const bool meetsMinimum = cellState.total >= kCaptureMinimumPerCell;

            QStringList lines;
            lines << captureCellDisplayName(r, c);
            lines << tr("%1/%2").arg(cellState.total).arg(kCaptureTargetPerCell);
            if (quality.total() > 0) {
                lines << tr("保%1 剔%2").arg(quality.kept).arg(quality.removed);
            } else if (quality.pending > 0) {
                lines << tr("待%1").arg(quality.pending);
            } else if (!meetsMinimum && cellState.total > 0) {
                lines << tr("缺%1").arg(kCaptureMinimumPerCell - cellState.total);
            } else if (!meetsRecommended) {
                lines << tr("建议+%1").arg(kCaptureTargetPerCell - cellState.total);
            }
            button->setText(lines.join(QStringLiteral("\n")));

            QStringList detail;
            detail << tr("该格已记录 %1 张样本。").arg(cellState.total);
            if (!meetsMinimum) {
                detail << tr("距离最低要求还差 %1 张。")
                           .arg(std::max(0, kCaptureMinimumPerCell - cellState.total));
            } else if (!meetsRecommended) {
                detail << tr("已满足最低要求，如需更稳健可补拍 %1 张。")
                           .arg(kCaptureTargetPerCell - cellState.total);
            } else {
                detail << tr("已达到建议数量，可根据需要补拍。");
            }
            if (quality.total() > 0) {
                detail << tr("保留 %1，剔除 %2").arg(quality.kept).arg(quality.removed);
                detail << tr("平均误差 %1 px，最大误差 %2 px")
                            .arg(QString::number(quality.averageMeanErrorPx(), 'f', 3))
                            .arg(QString::number(quality.maxMeanErrorPx, 'f', 3));
                const double avgResidualMm = quality.averageResidualMm();
                if (!qFuzzyIsNull(avgResidualMm)) {
                    detail << tr("平均残差 %1 mm，最大残差 %2 mm")
                                .arg(QString::number(avgResidualMm, 'f', 3))
                                .arg(QString::number(quality.maxResidualMm, 'f', 3));
                }
            }
            if (quality.pending > 0) {
                detail << tr("仍有 %1 张待标定样本。")
                            .arg(quality.pending);
            }
            QStringList poseBreakdown;
            for (int i = 0; i < static_cast<int>(std::size(kPoseDescriptors)); ++i) {
                const int count = cellState.poseCounts[i];
                if (count <= 0) {
                    continue;
                }
                poseBreakdown << tr("%1 × %2").arg(tr(kPoseDescriptors[i].display)).arg(count);
            }
            if (!poseBreakdown.isEmpty()) {
                detail << tr("姿态分布：%1").arg(poseBreakdown.join(QStringLiteral("，")));
            }
            button->setToolTip(detail.join(QStringLiteral("\n")));

            button->setStyleSheet(captureCellStyle(quality, cellState.total, meetsMinimum, meetsRecommended));
        }
    }
    updateCaptureSelectionSummary();
}

void MainWindow::updateCaptureSelectionSummary()
{
    if (!m_capturePlanSelectionLabel) {
        return;
    }
    if (m_captureSelectedRow < 0 || m_captureSelectedRow >= kCaptureGridRows ||
        m_captureSelectedCol < 0 || m_captureSelectedCol >= kCaptureGridCols) {
        m_capturePlanSelectionLabel->clear();
        return;
    }

    const auto &state = m_capturePlanState[m_captureSelectedRow][m_captureSelectedCol];
    const auto &quality = m_cellQuality[m_captureSelectedRow][m_captureSelectedCol];

    QStringList parts;
    parts << tr("当前取景格：%1").arg(captureCellDisplayName(m_captureSelectedRow, m_captureSelectedCol));
    parts << tr("采集 %1/%2").arg(state.total).arg(kCaptureTargetPerCell);

    if (state.total < kCaptureMinimumPerCell) {
        parts << tr("距离最低目标缺 %1 张")
                    .arg(kCaptureMinimumPerCell - state.total);
    } else if (state.total < kCaptureTargetPerCell) {
        parts << tr("最低要求已满足，建议再补 %1 张")
                    .arg(kCaptureTargetPerCell - state.total);
    } else {
        parts << tr("已满足建议数量");
    }

    if (quality.total() > 0) {
        parts << tr("保/剔 %1/%2").arg(quality.kept).arg(quality.removed);
        parts << tr("均误差 %1 px")
                    .arg(QString::number(quality.averageMeanErrorPx(), 'f', 3));
        const double avgResidualMm = quality.averageResidualMm();
        if (!qFuzzyIsNull(avgResidualMm)) {
            parts << tr("残差 %1 mm").arg(QString::number(avgResidualMm, 'f', 3));
        }
    } else if (quality.pending > 0) {
        parts << tr("待标定 %1").arg(quality.pending);
    }

    m_capturePlanSelectionLabel->setText(parts.join(QStringLiteral(" · ")));
}

void MainWindow::refreshCapturePoseButtons()
{
    if (!m_capturePoseGroup) {
        return;
    }
    const int row = std::clamp(m_captureSelectedRow, 0, kCaptureGridRows - 1);
    const int col = std::clamp(m_captureSelectedCol, 0, kCaptureGridCols - 1);
    const auto &cell = m_capturePlanState[row][col];

    for (int i = 0; i < static_cast<int>(std::size(kPoseDescriptors)); ++i) {
        auto *button = qobject_cast<QToolButton *>(m_capturePoseGroup->button(i));
        if (!button) {
            continue;
        }
        button->setText(QStringLiteral("%1\n(%2)")
                            .arg(tr(kPoseDescriptors[i].display))
                            .arg(cell.poseCounts[i]));
        button->setToolTip(tr("%1\n%2")
                               .arg(tr(kPoseDescriptors[i].display))
                               .arg(tr(kPoseDescriptors[i].hint)));
    }

    if (m_capturePlanPoseLabel) {
        const int poseIdx = capturePoseIndex(m_captureSelectedPose);
        const int count = cell.poseCounts[poseIdx];
        m_capturePlanPoseLabel->setText(tr("当前姿态：%1 · 已拍 %2 张")
                                            .arg(capturePoseDisplayName(m_captureSelectedPose))
                                            .arg(count));
    }
}

void MainWindow::updateCaptureOverlay()
{
#if MYCALIB_HAVE_CONNECTED_CAMERA
    if (!m_cameraWindow) {
        return;
    }
    if (ImageView *view = m_cameraWindow->liveView()) {
        view->setGridDimensions(kCaptureGridRows, kCaptureGridCols);
        view->setGridCellCounts(captureTotalsMatrix(), kCaptureTargetPerCell);
        if (isCapturePlanActive()) {
            view->setGridOverlayEnabled(true);
            view->setGridHighlight(m_captureSelectedRow, m_captureSelectedCol);
        } else {
            view->setGridHighlight(-1, -1);
            view->setGridOverlayEnabled(false);
        }
    }
#else
    // No-op when camera streaming is unavailable.
#endif
}

void MainWindow::handleCapturePlanToggled(bool enabled)
{
    if (enabled) {
        ensureCalibrationStageStarted();
    }
    updateCaptureOverlay();
    refreshCapturePoseButtons();
    refreshCaptureGridButtons();
}

void MainWindow::handleCaptureGridSelection(int id)
{
    const int row = decodeGridRow(id);
    const int col = decodeGridCol(id);
    if (row < 0 || row >= kCaptureGridRows || col < 0 || col >= kCaptureGridCols) {
        return;
    }
    m_captureSelectedRow = row;
    m_captureSelectedCol = col;
    refreshCapturePoseButtons();
    refreshCaptureGridButtons();
    if (isCapturePlanActive()) {
        updateCaptureOverlay();
    }
    if (m_capturePlanDetailButton) {
        const auto &cell = m_cellQuality[row][col];
        const bool hasSamples = !cell.samples.isEmpty() || !cell.pendingSamples.isEmpty();
        if (m_capturePlanDetailButton->isVisible()) {
            m_capturePlanDetailButton->setEnabled(hasSamples);
            if (hasSamples) {
                m_capturePlanDetailButton->setToolTip(QString());
            } else {
                m_capturePlanDetailButton->setToolTip(tr("请先选择有样本的格位。"));
            }
        }
    }
    updateCaptureSelectionSummary();
}

void MainWindow::handleCapturePoseSelection(int id)
{
    m_captureSelectedPose = capturePoseFromIndex(id);
    refreshCapturePoseButtons();
}

QString MainWindow::capturePoseDisplayName(ProjectSession::CapturePose pose) const
{
    const int idx = capturePoseIndex(pose);
    return tr(kPoseDescriptors[std::clamp(idx, 0, static_cast<int>(std::size(kPoseDescriptors)) - 1)].display);
}

QString MainWindow::capturePoseHint(ProjectSession::CapturePose pose) const
{
    const int idx = capturePoseIndex(pose);
    return tr(kPoseDescriptors[std::clamp(idx, 0, static_cast<int>(std::size(kPoseDescriptors)) - 1)].hint);
}

QString MainWindow::captureCellDisplayName(int row, int col) const
{
    return tr("第%1行 · 第%2列").arg(row + 1).arg(col + 1);
}

QVector<QVector<int>> MainWindow::captureTotalsMatrix() const
{
    QVector<QVector<int>> totals(kCaptureGridRows, QVector<int>(kCaptureGridCols, 0));
    for (int r = 0; r < kCaptureGridRows; ++r) {
        for (int c = 0; c < kCaptureGridCols; ++c) {
            totals[r][c] = m_capturePlanState[r][c].total;
        }
    }
    return totals;
}

int MainWindow::captureTotalShots() const
{
    int total = 0;
    for (const auto &row : m_capturePlanState) {
        for (const auto &cell : row) {
            total += cell.total;
        }
    }
    return total;
}

bool MainWindow::isCapturePlanActive() const
{
    return m_capturePlanToggle && m_capturePlanToggle->isChecked();
}

QString MainWindow::debugArtifactRoot() const
{
    if (m_session) {
        QDir configDir = m_session->configDir();
        const QString path = configDir.filePath(QStringLiteral("debug_artifacts"));
        return QDir::cleanPath(path);
    }
    if (!m_outputDir.isEmpty()) {
        QDir output(QDir::cleanPath(m_outputDir));
        return output.filePath(QStringLiteral("debug_artifacts"));
    }
    return {};
}

void MainWindow::materializeDebugArtifacts(CalibrationOutput &output)
{
    const QString rootPath = QDir::cleanPath(debugArtifactRoot());
    if (rootPath.isEmpty()) {
        return;
    }

    QDir root(rootPath);
    if (!root.exists()) {
        root.mkpath(QStringLiteral("."));
    }

    const QString cleanRoot = QDir::cleanPath(root.absolutePath());
    const QString tempRoot = QDir::cleanPath(QDir::tempPath());

    QHash<QString, QString> directoryRemap;
    QHash<QString, int> baseCounters;
    QHash<QString, QString> fileRemap;
    QSet<QString> tempDirectories;

    auto ensureDirectory = [&](const QString &mapKey, const QString &baseKey) {
        if (directoryRemap.contains(mapKey)) {
            return directoryRemap.value(mapKey);
        }

        QString sanitized = sanitizeDebugKey(baseKey);
        if (sanitized.isEmpty()) {
            sanitized = QStringLiteral("capture");
        }

        int counter = baseCounters.value(sanitized, 0);
        QString candidate;
        do {
            candidate = (counter == 0)
                ? sanitized
                : QStringLiteral("%1_%2").arg(sanitized).arg(counter, 3, 10, QChar('0'));
            ++counter;
        } while (QDir(root.filePath(candidate)).exists());
        baseCounters.insert(sanitized, counter);

        const QString absolute = QDir::cleanPath(root.filePath(candidate));
        QDir().mkpath(absolute);
        directoryRemap.insert(mapKey, absolute);
        return absolute;
    };

    auto copyToPersistent = [&](const QString &sourcePath, const QDir &destDir) -> QString {
        QString cleanSource = QDir::cleanPath(sourcePath);
        if (cleanSource.isEmpty()) {
            return {};
        }
        if (cleanSource.startsWith(cleanRoot, Qt::CaseInsensitive)) {
            return QDir::toNativeSeparators(cleanSource);
        }
        if (fileRemap.contains(cleanSource)) {
            return fileRemap.value(cleanSource);
        }

        QFileInfo srcInfo(cleanSource);
        if (!srcInfo.exists()) {
            const QString candidate = destDir.filePath(srcInfo.fileName());
            if (QFileInfo::exists(candidate)) {
                const QString normalized = QDir::toNativeSeparators(QDir::cleanPath(candidate));
                fileRemap.insert(cleanSource, normalized);
                return normalized;
            }
            return {};
        }

        QString baseName = srcInfo.completeBaseName();
        QString suffix = srcInfo.completeSuffix();
        QString destPath = destDir.filePath(srcInfo.fileName());
        int counter = 1;
        while (QFileInfo::exists(destPath)) {
            const QString numbered = suffix.isEmpty()
                ? QStringLiteral("%1_%2").arg(baseName).arg(counter)
                : QStringLiteral("%1_%2.%3").arg(baseName).arg(counter).arg(suffix);
            destPath = destDir.filePath(numbered);
            ++counter;
        }

        if (!destDir.exists()) {
            destDir.mkpath(QStringLiteral("."));
        }

        if (QFile::copy(cleanSource, destPath)) {
            const QString normalized = QDir::toNativeSeparators(QDir::cleanPath(destPath));
            fileRemap.insert(cleanSource, normalized);
            if (cleanSource.startsWith(tempRoot, Qt::CaseInsensitive)) {
                tempDirectories.insert(QFileInfo(cleanSource).absolutePath());
            }
            return normalized;
        }

        return {};
    };

    auto processList = [&](std::vector<DetectionResult> &list) {
        for (auto &det : list) {
            if (det.debugImages.empty()) {
                continue;
            }

            QString originalDir = QDir::cleanPath(QString::fromStdString(det.debugDirectory));
            if (!originalDir.isEmpty() && originalDir.startsWith(cleanRoot, Qt::CaseInsensitive)) {
                for (auto &img : det.debugImages) {
                    QString normalized = QDir::cleanPath(QString::fromStdString(img.filePath));
                    img.filePath = QDir::toNativeSeparators(normalized).toStdString();
                }
                det.debugDirectory = QDir::toNativeSeparators(originalDir).toStdString();
                continue;
            }

            if (originalDir.isEmpty() && !det.debugImages.empty()) {
                originalDir = QDir::cleanPath(QFileInfo(QString::fromStdString(det.debugImages.front().filePath)).absolutePath());
            }

            const QString sampleKey = sampleKeyFromDetection(det);
            QString mapKey = originalDir;
            if (mapKey.isEmpty()) {
                mapKey = QDir::cleanPath(sampleKey);
            }
            if (mapKey.isEmpty()) {
                mapKey = QStringLiteral("capture_%1").arg(directoryRemap.size());
            }

            const QString targetDir = ensureDirectory(mapKey, sampleKey);
            QDir destDir(targetDir);
            for (auto &img : det.debugImages) {
                const QString copied = copyToPersistent(QString::fromStdString(img.filePath), destDir);
                if (!copied.isEmpty()) {
                    img.filePath = copied.toStdString();
                }
            }
            det.debugDirectory = QDir::toNativeSeparators(QDir::cleanPath(targetDir)).toStdString();
        }
    };

    processList(output.allDetections);
    processList(output.keptDetections);
    processList(output.removedDetections);

    for (const QString &dirPath : std::as_const(tempDirectories)) {
        if (dirPath.startsWith(tempRoot, Qt::CaseInsensitive)) {
            QDir tempDir(dirPath);
            tempDir.removeRecursively();
        }
    }
}

void MainWindow::setupActions()
{
    m_actionImportImages = new QAction(QIcon(":/icons/folder.svg"), tr("Import images"), this);
    connect(m_actionImportImages, &QAction::triggered, this, &MainWindow::importLocalImages);

    m_actionRun = new QAction(QIcon(":/icons/play.svg"), tr("Run calibration"), this);
    connect(m_actionRun, &QAction::triggered, this, &MainWindow::runCalibration);

    m_actionExport = new QAction(QIcon(":/icons/export.svg"), tr("Export JSON"), this);
    connect(m_actionExport, &QAction::triggered, this, &MainWindow::exportJson);
    m_actionExport->setEnabled(false);

    m_actionShowParameters = new QAction(QIcon(":/icons/app.png"), tr("Show parameters"), this);
    connect(m_actionShowParameters, &QAction::triggered, this, &MainWindow::showParameters);
    m_actionShowParameters->setEnabled(false);

    m_actionEvaluate = new QAction(QIcon(":/icons/evaluate.svg"), tr("Evaluate images"), this);
    connect(m_actionEvaluate, &QAction::triggered, this, &MainWindow::showEvaluationDialog);
    m_actionEvaluate->setEnabled(false);

    m_actionReset = new QAction(QIcon(":/icons/reset.svg"), tr("Reset"), this);
    connect(m_actionReset, &QAction::triggered, this, &MainWindow::resetUi);

    m_actionAbout = new QAction(tr("About"), this);
    connect(m_actionAbout, &QAction::triggered, this, &MainWindow::showAuthorInfo);

    if (m_toolBar) {
        m_toolBar->addAction(m_actionImportImages);
        m_toolbarPrimarySeparator = m_toolBar->addSeparator();
        m_toolBar->addAction(m_actionRun);
        m_toolBar->addAction(m_actionExport);
        m_toolBar->addAction(m_actionShowParameters);
        m_toolBar->addAction(m_actionEvaluate);
        m_toolBar->addSeparator();
        m_toolBar->addAction(m_actionReset);

#if MYCALIB_HAVE_CONNECTED_CAMERA
        m_toolbarCameraSeparator = m_toolBar->addSeparator();

        m_actionCameraConnect = new QAction(QIcon(QStringLiteral(":/icons/camera_open.svg")), tr("Connect camera"), this);
        connect(m_actionCameraConnect, &QAction::triggered, this, &MainWindow::connectCamera);
        m_toolBar->addAction(m_actionCameraConnect);

        m_actionCameraDisconnect = new QAction(QIcon(QStringLiteral(":/icons/camera_close.svg")), tr("Disconnect"), this);
        connect(m_actionCameraDisconnect, &QAction::triggered, this, &MainWindow::disconnectCamera);
        m_toolBar->addAction(m_actionCameraDisconnect);

        m_actionCameraStart = new QAction(QIcon(QStringLiteral(":/icons/stream_start.svg")), tr("Start stream"), this);
    m_actionCameraStart->setCheckable(true);
        connect(m_actionCameraStart, &QAction::triggered, this, &MainWindow::startCameraStream);
        m_toolBar->addAction(m_actionCameraStart);

        m_actionCameraStop = new QAction(QIcon(QStringLiteral(":/icons/stream_stop.svg")), tr("Stop stream"), this);
    m_actionCameraStop->setCheckable(true);
        connect(m_actionCameraStop, &QAction::triggered, this, &MainWindow::stopCameraStream);
        m_toolBar->addAction(m_actionCameraStop);

        m_actionCameraSnapshot = new QAction(QIcon(QStringLiteral(":/icons/snapshot.svg")), tr("Capture frame"), this);
        connect(m_actionCameraSnapshot, &QAction::triggered, this, &MainWindow::captureFromCamera);
        m_toolBar->addAction(m_actionCameraSnapshot);

        m_actionCameraRefresh = new QAction(QIcon(QStringLiteral(":/icons/refresh.svg")), tr("Refresh devices"), this);
        connect(m_actionCameraRefresh, &QAction::triggered, this, &MainWindow::refreshCameraDevices);
        m_toolBar->addAction(m_actionCameraRefresh);

        m_cameraStatusChip = new QLabel(tr("Camera offline"), m_toolBar);
        m_cameraStatusChip->setObjectName(QStringLiteral("CameraStatusChip"));
        m_cameraStatusChip->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
        m_cameraStatusChip->setVisible(false);
        m_cameraStatusChip->setStyleSheet(QStringLiteral("padding: 4px 10px; border-radius: 12px; background: rgba(72,132,255,0.08); color: #d4e3ff; font-weight: 600;"));
        m_toolBar->addWidget(m_cameraStatusChip);
#endif

        QWidget *spacer = new QWidget(m_toolBar);
        spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        m_toolBar->addWidget(spacer);
        m_toolBar->addAction(m_actionAbout);
    }

#if MYCALIB_HAVE_CONNECTED_CAMERA
    syncCameraActions();
#endif
}

void MainWindow::applyTheme()
{
    qApp->setWindowIcon(QIcon(QStringLiteral(":/icons/app.png")));
}

void MainWindow::refreshTuningPanel()
{
    if (!m_tuningTimeline) {
        return;
    }

    m_tuningTimeline->clear();

    if (!m_session) {
        if (m_tuningCountLabel) {
            m_tuningCountLabel->setText(tr("No project session loaded."));
        }
        updateTuningStageUi();
        return;
    }

    const auto snapshots = m_session->tuningSnapshots();
    for (const auto &snapshot : snapshots) {
        appendTuningSnapshotRow(snapshot);
    }

    m_tuningTimeline->sortItems(0, Qt::DescendingOrder);
    if (auto *first = m_tuningTimeline->topLevelItem(0)) {
        m_tuningTimeline->setCurrentItem(first);
    }

    updateTuningStageUi();
}

QTreeWidgetItem *MainWindow::appendTuningSnapshotRow(const ProjectSession::TuningSnapshot &snapshot)
{
    if (!m_tuningTimeline) {
        return nullptr;
    }

    auto *item = new QTreeWidgetItem(m_tuningTimeline);

    const QDateTime capturedLocal = snapshot.capturedAt.isValid()
        ? snapshot.capturedAt.toLocalTime()
        : QDateTime();
    item->setText(0, capturedLocal.isValid()
                        ? capturedLocal.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
                        : tr("--"));
    item->setData(0, Qt::UserRole, snapshot.capturedAt);

    const QVariantMap metrics = snapshot.metrics;

    auto ensureDisplay = [](const QString &text) {
        return text.isEmpty() ? QStringLiteral("--") : text;
    };

    QString exposure = metrics.value(QStringLiteral("exposure")).toString();
    if (exposure.isEmpty() && metrics.contains(QStringLiteral("ExposureTime"))) {
        exposure = metrics.value(QStringLiteral("ExposureTime")).toString();
    }
    item->setText(1, ensureDisplay(exposure));

    QString gain = metrics.value(QStringLiteral("gain")).toString();
    if (gain.isEmpty() && metrics.contains(QStringLiteral("gainNumeric"))) {
        gain = tr("%1 dB").arg(QString::number(metrics.value(QStringLiteral("gainNumeric")).toDouble(), 'f', 1));
    }
    item->setText(2, ensureDisplay(gain));

    QString fps = metrics.value(QStringLiteral("frameRate")).toString();
    if (fps.isEmpty()) {
        const double numericFps = metrics.value(QStringLiteral("frameRateNumeric")).toDouble();
        if (numericFps > 0.0) {
            fps = tr("%1 FPS").arg(QString::number(numericFps, 'f', 2));
        }
    }
    item->setText(3, ensureDisplay(fps));

    const QString relativePath = snapshot.relativePath;
    const QString absolutePath = absoluteSessionPath(relativePath);
    const QString displayPath = QDir::toNativeSeparators(QDir::isRelativePath(relativePath)
                                                             ? relativePath
                                                             : absolutePath);
    item->setText(4, ensureDisplay(displayPath));
    item->setData(0, Qt::UserRole + 1, absolutePath);
    item->setData(0, Qt::UserRole + 2, snapshot.id.toString(QUuid::WithoutBraces));

    return item;
}

QString MainWindow::absoluteSessionPath(const QString &relative) const
{
    if (relative.isEmpty()) {
        return relative;
    }
    QFileInfo info(relative);
    if (info.isAbsolute()) {
        return info.absoluteFilePath();
    }
    if (!m_session) {
        return info.absoluteFilePath();
    }
    QDir root(m_session->rootPath());
    return QDir::toNativeSeparators(root.filePath(relative));
}

QString MainWindow::relativeSessionPath(const QString &absolute) const
{
    if (!m_session || absolute.isEmpty()) {
        return absolute;
    }

    QFileInfo info(absolute);
    const QString absolutePath = info.isAbsolute() ? info.absoluteFilePath() : info.filePath();
    const QString relative = m_session->relativePath(absolutePath);
    if (relative.isEmpty() || relative.startsWith(QStringLiteral(".."))) {
        return QDir::toNativeSeparators(absolutePath);
    }
    return relative;
}

void MainWindow::updateTuningStageUi()
{
    if (!m_tuningStageLabel) {
        return;
    }

    if (!m_session) {
        m_tuningStageLabel->setText(tr("Camera tuning stage is unavailable without an open project."));
        if (m_tuningCountLabel) {
            m_tuningCountLabel->setText(tr("Create or open a project to begin tuning."));
        }
        if (m_markTuningDoneButton) {
            m_markTuningDoneButton->setEnabled(false);
        }
        return;
    }

    const auto state = m_session->stageState(ProjectSession::ProjectStage::CameraTuning);
    QStringList lines;
    lines << tr("Stage status: %1").arg(stageStatusDisplay(state.status));
    if (state.startedAt.isValid()) {
        lines << tr("Started: %1").arg(state.startedAt.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")));
    }
    if (state.completedAt.isValid()) {
        lines << tr("Completed: %1").arg(state.completedAt.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")));
    }
    if (!state.notes.isEmpty()) {
        QString notesHtml = state.notes.toHtmlEscaped();
        notesHtml.replace(QLatin1Char('\n'), QStringLiteral("<br/>"));
        lines << tr("Notes: %1").arg(notesHtml);
    }
    m_tuningStageLabel->setText(lines.join(QStringLiteral("<br/>")));

    const int snapshotCount = m_session->tuningSnapshots().size();
    if (m_tuningCountLabel) {
        m_tuningCountLabel->setText(tr("%n tuning snapshot(s) recorded.", "", snapshotCount));
    }
    if (m_markTuningDoneButton) {
        m_markTuningDoneButton->setEnabled(snapshotCount > 0 && state.status != ProjectSession::StageStatus::Completed);
    }
}

void MainWindow::ensureCameraTuningStageStarted()
{
    if (!m_session) {
        return;
    }
    auto state = m_session->stageState(ProjectSession::ProjectStage::CameraTuning);
    if (state.status != ProjectSession::StageStatus::NotStarted) {
        return;
    }
    state.status = ProjectSession::StageStatus::InProgress;
    if (state.notes.isEmpty()) {
        state.notes = tr("Stage progressed automatically after first tuning snapshot.");
    }
    m_session->updateStageState(ProjectSession::ProjectStage::CameraTuning, state);
    if (m_logView) {
        m_logView->append(tr("Camera tuning stage marked as in progress."));
    }
    updateTuningStageUi();
    updateStageNavigator();
    persistProjectSummary(false);
}

void MainWindow::ensureCalibrationStageStarted()
{
    if (!m_session) {
        return;
    }
    auto state = m_session->stageState(ProjectSession::ProjectStage::CalibrationCapture);
    if (state.status != ProjectSession::StageStatus::NotStarted) {
        return;
    }
    state.status = ProjectSession::StageStatus::InProgress;
    if (state.notes.isEmpty()) {
        state.notes = tr("Stage progressed automatically after first calibration capture.");
    }
    m_session->updateStageState(ProjectSession::ProjectStage::CalibrationCapture, state);
    if (m_logView) {
        m_logView->append(tr("Calibration capture stage marked as in progress."));
    }
    updateStageNavigator();
    persistProjectSummary(false);
}

void MainWindow::ensureLaserStageStarted()
{
    if (!m_session) {
        return;
    }
    auto state = m_session->stageState(ProjectSession::ProjectStage::LaserCalibration);
    if (state.status != ProjectSession::StageStatus::NotStarted) {
        return;
    }
    state.status = ProjectSession::StageStatus::InProgress;
    if (state.notes.isEmpty()) {
        state.notes = tr("Stage progressed automatically after first laser frame import.");
    }
    m_session->updateStageState(ProjectSession::ProjectStage::LaserCalibration, state);
    if (m_logView) {
        m_logView->append(tr("Laser calibration stage marked as in progress."));
    }
    updateStageNavigator();
    persistProjectSummary(false);
}

void MainWindow::handleTuningItemActivated(QTreeWidgetItem *item, int column)
{
    Q_UNUSED(column);
    if (!item) {
        return;
    }
    const QString absolutePath = item->data(0, Qt::UserRole + 1).toString();
    if (absolutePath.isEmpty()) {
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(absolutePath));
}

void MainWindow::markCameraTuningCompleted()
{
    if (!m_session) {
        return;
    }
    auto state = m_session->stageState(ProjectSession::ProjectStage::CameraTuning);
    if (state.status == ProjectSession::StageStatus::Completed) {
        return;
    }
    state.status = ProjectSession::StageStatus::Completed;
    const QString note = tr("Marked complete on %1").arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    if (state.notes.isEmpty()) {
        state.notes = note;
    } else {
        state.notes += QLatin1Char('\n');
        state.notes += note;
    }
    m_session->updateStageState(ProjectSession::ProjectStage::CameraTuning, state);
    if (m_logView) {
        m_logView->append(tr("Camera tuning stage marked as completed."));
    }
    updateTuningStageUi();
    updateStageNavigator();
    persistProjectSummary(false);
}

void MainWindow::openTuningFolder()
{
    if (!m_session) {
        return;
    }
    const QString path = m_session->tuningCaptureDir().absolutePath();
    openDirectory(path);
}

void MainWindow::showTuningControlsPopup()
{
#if MYCALIB_HAVE_CONNECTED_CAMERA
    if (!m_tuningControlPopupButton || !m_tuningControlsPanel || !m_cameraWindow) {
        return;
    }

    if (m_tuningControlPopup && m_tuningControlPopup->isVisible()) {
        m_tuningControlPopup->close();
        return;
    }

    auto *popup = new QDialog(this);
    popup->setObjectName(QStringLiteral("TuningControlsPopup"));
    popup->setWindowFlags(Qt::Popup | Qt::FramelessWindowHint);
    popup->setAttribute(Qt::WA_TranslucentBackground);
    popup->setModal(false);

    auto *outerLayout = new QVBoxLayout(popup);
    outerLayout->setContentsMargins(0, 0, 0, 0);
    outerLayout->setSpacing(0);

    auto *frame = new QFrame(popup);
    frame->setObjectName(QStringLiteral("TuningControlsPopupFrame"));
    frame->setStyleSheet(QStringLiteral(
        "QFrame#TuningControlsPopupFrame {"
        "  background: rgba(12, 18, 32, 0.98);"
        "  border-radius: 18px;"
        "  border: 1px solid rgba(86, 110, 168, 0.45);"
        "}"
        "QFrame#TuningControlsPopupFrame QToolButton {"
        "  background: rgba(32, 46, 68, 0.68);"
        "  border: 1px solid rgba(110, 138, 190, 0.42);"
        "  border-radius: 14px;"
        "  color: #e6ecff;"
        "  font-weight: 600;"
        "  padding: 10px 12px;"
        "}"
        "QFrame#TuningControlsPopupFrame QToolButton:hover {"
        "  background: rgba(80, 143, 255, 0.34);"
        "  border-color: rgba(120, 196, 255, 0.55);"
        "}"
        "QFrame#TuningControlsPopupFrame QToolButton:pressed {"
        "  background: rgba(80, 143, 255, 0.46);"
        "}"
        "QFrame#TuningControlsPopupFrame QToolButton:disabled {"
        "  background: rgba(40, 50, 70, 0.38);"
        "  color: rgba(210, 220, 240, 0.35);"
        "  border-color: rgba(90, 110, 150, 0.28);"
        "}"));

    auto *innerLayout = new QVBoxLayout(frame);
    innerLayout->setContentsMargins(18, 18, 18, 18);
    innerLayout->setSpacing(14);

    if (m_tuningControlsPanel->parent() != frame) {
        m_tuningControlsPanel->setParent(frame);
    }
    m_tuningControlsPanel->show();
    innerLayout->addWidget(m_tuningControlsPanel);

    outerLayout->addWidget(frame);

    m_tuningControlPopup = popup;

    connect(popup, &QDialog::finished, this, [this]() {
        if (m_tuningControlsPanel) {
            m_tuningControlsPanel->setParent(this);
            m_tuningControlsPanel->hide();
        }
        if (m_tuningControlPopupButton) {
            m_tuningControlPopupButton->setDown(false);
        }
        if (m_tuningControlPopup) {
            m_tuningControlPopup->deleteLater();
            m_tuningControlPopup = nullptr;
        }
    });

    popup->adjustSize();
    if (m_tuningControlPopupButton) {
        const QSize hint = popup->sizeHint();
        QPoint anchor = m_tuningControlPopupButton->mapToGlobal(QPoint(m_tuningControlPopupButton->width(), m_tuningControlPopupButton->height()));
        anchor.rx() -= hint.width();
        anchor.ry() += 6;
        popup->move(anchor);
    }

    if (m_tuningControlPopupButton) {
        m_tuningControlPopupButton->setDown(true);
    }

    popup->show();
#endif
}

void MainWindow::showFeaturePanelPopup()
{
#if MYCALIB_HAVE_CONNECTED_CAMERA
    if (!m_cameraWindow || !m_tuningFeaturePopupButton) {
        return;
    }

    if (m_featurePopup && m_featurePopup->isVisible()) {
        m_featurePopup->close();
        return;
    }

    auto *panel = m_cameraWindow->featurePanel();
    if (!panel) {
        return;
    }

    QWidget *originalParent = panel->parentWidget();
    if (!originalParent) {
        return;
    }

    if (auto *layout = originalParent->layout()) {
        m_featurePanelOriginalIndex = -1;
        for (int i = 0; i < layout->count(); ++i) {
            if (layout->itemAt(i) && layout->itemAt(i)->widget() == panel) {
                m_featurePanelOriginalIndex = i;
                break;
            }
        }
        layout->removeWidget(panel);
    } else {
        m_featurePanelOriginalIndex = -1;
    }

    m_featurePanelOriginalParent = originalParent;
    originalParent->setVisible(false);

    auto *popup = new QDialog(this);
    popup->setObjectName(QStringLiteral("FeaturePanelPopup"));
    popup->setWindowFlags(Qt::Popup | Qt::FramelessWindowHint);
    popup->setAttribute(Qt::WA_TranslucentBackground);
    popup->setModal(false);

    auto *outerLayout = new QVBoxLayout(popup);
    outerLayout->setContentsMargins(0, 0, 0, 0);
    outerLayout->setSpacing(0);

    auto *frame = new QFrame(popup);
    frame->setObjectName(QStringLiteral("FeaturePanelPopupFrame"));
    frame->setStyleSheet(QStringLiteral(
        "QFrame#FeaturePanelPopupFrame {"
        "  background: rgba(12, 18, 32, 0.98);"
        "  border-radius: 18px;"
        "  border: 1px solid rgba(86, 110, 168, 0.45);"
        "}"
        "QWidget#FeaturePanel {"
        "  background: transparent;"
        "  color: #d5e0ff;"
        "}"
        "FeaturePanel QLabel {"
        "  color: #d4deee;"
        "  font-weight: 500;"
        "}"
        "FeaturePanel QFrame#FeatureSection {"
        "  background: rgba(16, 24, 36, 0.86);"
        "  border-radius: 14px;"
        "  border: 1px solid rgba(80, 143, 255, 0.18);"
        "  padding: 14px 18px 18px;"
        "}"
    "FeaturePanel QTabWidget#FeatureTabs::pane {"
    "  border: none;"
    "  margin-top: 4px;"
    "}"
    "FeaturePanel QTabBar::tab {"
    "  background: rgba(24, 36, 54, 0.78);"
    "  color: #c9daff;"
    "  padding: 8px 18px;"
    "  margin-right: 6px;"
    "  border: 1px solid rgba(94, 150, 255, 0.28);"
    "  border-bottom-color: transparent;"
    "  border-top-left-radius: 10px;"
    "  border-top-right-radius: 10px;"
    "  font-size: 12.5px;"
    "}"
    "FeaturePanel QTabBar::tab:selected {"
    "  background: rgba(80, 143, 255, 0.32);"
    "  color: #f1f6ff;"
    "  border-color: rgba(124, 182, 255, 0.7);"
    "}"
    "FeaturePanel QTabBar::tab:hover {"
    "  background: rgba(80, 143, 255, 0.22);"
    "  color: #f7fbff;"
    "}"
        "FeaturePanel QLabel#FeatureSectionHeader {"
        "  color: #8ec5ff;"
        "  font-size: 13px;"
        "  font-weight: 700;"
        "  letter-spacing: 0.35px;"
        "  margin-bottom: 10px;"
        "}"
        "FeaturePanel QLineEdit,"
        "FeaturePanel QDoubleSpinBox,"
        "FeaturePanel QSpinBox,"
        "FeaturePanel QComboBox,"
        "FeaturePanel QCheckBox,"
        "FeaturePanel QPushButton {"
        "  background: rgba(17, 24, 34, 0.86);"
        "  border: 1px solid rgba(88, 172, 255, 0.2);"
        "  border-radius: 10px;"
        "  color: #e5ecf8;"
        "  padding: 6px 8px;"
        "}"
        "FeaturePanel QCheckBox::indicator {"
        "  width: 16px;"
        "  height: 16px;"
        "}"
        "FeaturePanel QPushButton {"
        "  padding: 8px 14px;"
        "  font-weight: 600;"
        "  color: #7bd8ff;"
        "}"
        "FeaturePanel QPushButton:hover {"
        "  background: rgba(80, 143, 255, 0.24);"
        "}"));
    outerLayout->addWidget(frame);

    auto *frameLayout = new QVBoxLayout(frame);
    frameLayout->setContentsMargins(22, 22, 22, 22);
    frameLayout->setSpacing(14);

    auto *titleLabel = new QLabel(tr("相机特征与属性"), frame);
    titleLabel->setStyleSheet(QStringLiteral("color:#f3f7ff;font-size:16px;font-weight:700;letter-spacing:0.4px;"));
    frameLayout->addWidget(titleLabel);

    auto *subtitleLabel = new QLabel(tr("调整曝光、采集、IO 以及更多高级参数"), frame);
    subtitleLabel->setStyleSheet(QStringLiteral("color:rgba(210,223,245,0.68);font-size:12px;"));
    subtitleLabel->setWordWrap(true);
    frameLayout->addWidget(subtitleLabel);

    auto *scrollArea = new QScrollArea(frame);
    scrollArea->setObjectName(QStringLiteral("FeaturePanelScroll"));
    scrollArea->setWidgetResizable(true);
    scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setStyleSheet(QStringLiteral(
        "QScrollArea#FeaturePanelScroll { border: none; background: transparent; }"
        "QScrollArea#FeaturePanelScroll > QWidget { background: transparent; }"));

    auto *host = new QWidget(scrollArea);
    host->setObjectName(QStringLiteral("FeaturePanelPopupHost"));
    auto *hostLayout = new QVBoxLayout(host);
    hostLayout->setContentsMargins(0, 0, 0, 0);
    hostLayout->setSpacing(0);
    panel->setParent(host);
    panel->show();
    hostLayout->addWidget(panel);
    scrollArea->setWidget(host);
    frameLayout->addWidget(scrollArea, 1);

    m_featurePopup = popup;

    connect(popup, &QDialog::finished, this, [this, panel]() {
        if (m_featurePanelOriginalParent) {
            if (auto *layout = m_featurePanelOriginalParent->layout()) {
                if (auto *box = qobject_cast<QBoxLayout *>(layout)) {
                    if (m_featurePanelOriginalIndex >= 0 && m_featurePanelOriginalIndex <= box->count()) {
                        box->insertWidget(m_featurePanelOriginalIndex, panel);
                    } else {
                        box->addWidget(panel);
                    }
                } else {
                    layout->addWidget(panel);
                }
            } else {
                panel->setParent(m_featurePanelOriginalParent);
            }
            if (m_cameraWindow && !m_cameraWindow->isEmbeddedMode()) {
                m_featurePanelOriginalParent->setVisible(true);
            } else if (m_cameraWindow && m_cameraWindow->isEmbeddedMode()) {
                m_featurePanelOriginalParent->setVisible(false);
            }
        } else {
            panel->setParent(nullptr);
        }

        m_featurePanelOriginalParent = nullptr;
        m_featurePanelOriginalIndex = -1;

        if (m_tuningFeaturePopupButton) {
            m_tuningFeaturePopupButton->setDown(false);
        }

        if (m_featurePopup) {
            m_featurePopup->deleteLater();
            m_featurePopup = nullptr;
        }
    });

    QSize desiredSize = popup->sizeHint();
    desiredSize.setWidth(std::max(desiredSize.width(), 700));
    desiredSize.setHeight(std::clamp(desiredSize.height(), 600, 820));
    popup->resize(desiredSize);

    if (m_tuningFeaturePopupButton) {
        const QSize popupSize = popup->size();
        QPoint anchor = m_tuningFeaturePopupButton->mapToGlobal(QPoint(m_tuningFeaturePopupButton->width(), m_tuningFeaturePopupButton->height()));
        anchor.rx() -= popupSize.width();
        anchor.ry() += 6;
        popup->move(anchor);
        m_tuningFeaturePopupButton->setDown(true);
    }

    popup->show();
#endif
}
void MainWindow::importLocalImages()
{
    if (!m_session) {
        QMessageBox::warning(this, tr("No project"), tr("Create or open a project before importing images."));
        return;
    }
    if (m_session->metadata().dataSource != ProjectSession::DataSource::LocalDataset) {
        QMessageBox::information(this,
                                 tr("Unavailable"),
                                 tr("Image import is only available in the local images workflow."));
        return;
    }

    const QString targetDirPath = defaultInputDirectory();
    if (targetDirPath.isEmpty()) {
        QMessageBox::warning(this, tr("Invalid project"), tr("The project media directory is not available."));
        return;
    }

    if (!QDir().mkpath(targetDirPath)) {
        QMessageBox::warning(this,
                             tr("Import failed"),
                             tr("Could not create the project media directory: %1").arg(targetDirPath));
        return;
    }

    static QString lastImportDir;
    const QString startDir = lastImportDir.isEmpty() ? QDir::homePath() : lastImportDir;
    const QStringList files = QFileDialog::getOpenFileNames(this,
                                                            tr("Select images to import"),
                                                            startDir,
                                                            tr("Images (*.png *.jpg *.jpeg *.bmp *.tif *.tiff);;All files (*)"));
    if (files.isEmpty()) {
        return;
    }
    lastImportDir = QFileInfo(files.constFirst()).absolutePath();

    QDir targetDir(targetDirPath);
    int copiedCount = 0;
    QStringList failed;
    for (const QString &file : files) {
        QFileInfo info(file);
        if (!info.exists() || !info.isFile()) {
            failed << file;
            continue;
        }

        if (info.absoluteFilePath() == QFileInfo(targetDir.filePath(info.fileName())).absoluteFilePath()) {
            // Already in target directory; skip silently.
            continue;
        }

        QString baseName = info.completeBaseName();
        QString suffix = info.suffix();
        QString candidate = info.fileName();
        QString destination = targetDir.filePath(candidate);
        int counter = 1;
        while (QFileInfo::exists(destination)) {
            const QString numbered = suffix.isEmpty()
                ? QStringLiteral("%1_%2").arg(baseName).arg(counter)
                : QStringLiteral("%1_%2.%3").arg(baseName).arg(counter).arg(suffix);
            destination = targetDir.filePath(numbered);
            ++counter;
        }

        if (QFile::copy(info.absoluteFilePath(), destination)) {
            ++copiedCount;
        } else {
            failed << file;
        }
    }

    refreshModeUi();

    if (copiedCount > 0 && m_logView) {
        m_logView->append(tr("Imported %1 file(s) into %2")
                              .arg(copiedCount)
                              .arg(QDir::toNativeSeparators(targetDirPath)));
    }
    if (!failed.isEmpty() && m_logView) {
        m_logView->append(tr("Skipped %1 file(s) during import.").arg(failed.size()));
    }
}

void MainWindow::openInputLocation()
{
    if (m_inputDir.isEmpty()) {
        return;
    }
    openDirectory(m_inputDir);
}

void MainWindow::openOutputLocation()
{
    if (m_outputDir.isEmpty()) {
        return;
    }
    openDirectory(m_outputDir);
}

void MainWindow::importLaserFrames()
{
    if (!m_session) {
        QMessageBox::warning(this, tr("No project"), tr("Create or open a project before importing laser frames."));
        return;
    }

    static QString s_lastLaserImportDir;
    const QString startDir = s_lastLaserImportDir.isEmpty() ? QDir::homePath() : s_lastLaserImportDir;
    const QStringList files = QFileDialog::getOpenFileNames(this,
                                                            tr("Select laser frames"),
                                                            startDir,
                                                            tr("Images (*.png *.jpg *.jpeg *.bmp *.tif *.tiff);;All files (*)"));
    if (files.isEmpty()) {
        return;
    }

    s_lastLaserImportDir = QFileInfo(files.constFirst()).absolutePath();

    int imported = 0;
    QStringList failed;

    for (const QString &file : files) {
        QFileInfo info(file);
        if (!info.exists() || !info.isFile()) {
            failed << file;
            continue;
        }

        QVariantMap annotations;
        annotations.insert(QStringLiteral("source_path"), QDir::toNativeSeparators(info.absoluteFilePath()));
        annotations.insert(QStringLiteral("imported_at"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));

        const ProjectSession::LaserFrame frame = m_session->recordLaserFrame(info.absoluteFilePath(), annotations);
        if (frame.id.isNull()) {
            failed << file;
        } else {
            ++imported;
        }
    }

    if (imported > 0) {
        ensureLaserStageStarted();
        if (m_logView) {
            m_logView->append(tr("Imported %1 laser frame(s) into the project.").arg(imported));
        }
    }

    if (!failed.isEmpty()) {
        QStringList display;
        for (const QString &path : failed) {
            display << QDir::toNativeSeparators(path);
        }
        QMessageBox::warning(this,
                             tr("Import issues"),
                             tr("The following files could not be imported:\n%1").arg(display.join(QStringLiteral("\n"))));
    }

    updateStageNavigator();
    persistProjectSummary(false);
}

void MainWindow::openLaserCaptureFolder()
{
    if (!m_session) {
        QMessageBox::information(this, tr("No project"), tr("Create or open a project to open the laser capture folder."));
        return;
    }
    const QString path = m_session->laserCaptureDir().absolutePath();
    if (path.isEmpty()) {
        QMessageBox::warning(this, tr("Unavailable"), tr("The laser capture directory is not available."));
        return;
    }
    openDirectory(path);
}

void MainWindow::openLaserOutputFolder()
{
    if (!m_session) {
        QMessageBox::information(this, tr("No project"), tr("Create or open a project to open the laser output folder."));
        return;
    }
    const QString path = m_session->laserOutputDir().absolutePath();
    if (path.isEmpty()) {
        QMessageBox::warning(this, tr("Unavailable"), tr("The laser output directory is not available."));
        return;
    }
    openDirectory(path);
}

void MainWindow::markLaserStageCompleted()
{
    if (!m_session) {
        QMessageBox::information(this, tr("No project"), tr("Create or open a project before updating the laser stage."));
        return;
    }

    const QVector<ProjectSession::LaserFrame> frames = m_session->laserFrames();
    if (frames.isEmpty()) {
        QMessageBox::information(this,
                                 tr("No laser frames"),
                                 tr("Import at least one laser frame before marking the stage complete."));
        return;
    }

    auto state = m_session->stageState(ProjectSession::ProjectStage::LaserCalibration);
    if (state.status == ProjectSession::StageStatus::Completed) {
        QMessageBox::information(this, tr("Already complete"), tr("Laser calibration is already marked as completed."));
        return;
    }

    state.status = ProjectSession::StageStatus::Completed;
    const QString note = tr("Stage marked complete on %1").arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    if (state.notes.isEmpty()) {
        state.notes = note;
    } else {
        state.notes += QLatin1Char('\n');
        state.notes += note;
    }
    m_session->updateStageState(ProjectSession::ProjectStage::LaserCalibration, state);
    if (m_logView) {
        m_logView->append(tr("Laser calibration stage marked as completed."));
    }
    updateStageNavigator();
    persistProjectSummary(false);
}

void MainWindow::handleModeChanged(int index)
{
    if (!m_modeCombo) {
        return;
    }
    const QVariant modeData = m_modeCombo->itemData(index);
    if (!modeData.isValid()) {
        return;
    }
    const auto selected = static_cast<ProjectSession::DataSource>(modeData.toInt());

#if !MYCALIB_HAVE_CONNECTED_CAMERA
    if (selected == ProjectSession::DataSource::ConnectedCamera) {
        QMessageBox::information(this,
                                 tr("Unavailable"),
                                 tr("This build of Calibration Studio was compiled without connected camera support."));
        QSignalBlocker blocker(m_modeCombo);
        const int fallbackIdx = m_modeCombo->findData(static_cast<int>(ProjectSession::DataSource::LocalDataset));
        if (fallbackIdx >= 0) {
            m_modeCombo->setCurrentIndex(fallbackIdx);
        }
        return;
    }
#endif

    if (m_session && m_session->metadata().dataSource != selected) {
        QString error;
        if (!m_session->setDataSource(selected, &error)) {
            QMessageBox::warning(this, tr("Switch failed"), error);
            QSignalBlocker blocker(m_modeCombo);
            const int current = m_modeCombo->findData(static_cast<int>(m_session->metadata().dataSource));
            if (current >= 0) {
                m_modeCombo->setCurrentIndex(current);
            }
            return;
        }
    }

    refreshModeUi();

#if MYCALIB_HAVE_CONNECTED_CAMERA
    syncCameraActions();
#endif
}

#if MYCALIB_HAVE_CONNECTED_CAMERA

void MainWindow::connectCamera()
{
    if (m_activeSource != ProjectSession::DataSource::ConnectedCamera || !m_cameraWindow) {
        return;
    }
    m_cameraWindow->connectSelectedCamera();
}

void MainWindow::disconnectCamera()
{
    if (m_activeSource != ProjectSession::DataSource::ConnectedCamera || !m_cameraWindow) {
        return;
    }
    m_cameraWindow->disconnectCamera();
}

void MainWindow::startCameraStream()
{
    if (m_activeSource != ProjectSession::DataSource::ConnectedCamera || !m_cameraWindow) {
        return;
    }
    m_cameraWindow->startStreaming();
}

void MainWindow::stopCameraStream()
{
    if (m_activeSource != ProjectSession::DataSource::ConnectedCamera || !m_cameraWindow) {
        return;
    }
    m_cameraWindow->stopStreaming();
}

void MainWindow::captureFromCamera()
{
    if (m_activeSource != ProjectSession::DataSource::ConnectedCamera || !m_cameraWindow) {
        return;
    }
    m_cameraWindow->triggerSnapshot();
}

void MainWindow::refreshCameraDevices()
{
    if (m_activeSource != ProjectSession::DataSource::ConnectedCamera || !m_cameraWindow) {
        return;
    }
    m_cameraWindow->refreshCameraList();
}

void MainWindow::handleCameraConnectionChanged(bool connected, const QString &id, const QString &model)
{
    const bool previous = m_cameraConnected;
    const QString previousDescriptor = m_cameraDescriptor;

    m_cameraConnected = connected;
    if (!connected) {
        m_cameraDescriptor.clear();
        m_cameraStreaming = false;
    } else {
        QString descriptor;
        if (!model.isEmpty() && !id.isEmpty()) {
            descriptor = tr("%1 · %2").arg(model, id);
        } else if (!model.isEmpty()) {
            descriptor = model;
        } else {
            descriptor = id;
        }
        if (descriptor.isEmpty()) {
            descriptor = tr("Camera");
        }
        m_cameraDescriptor = descriptor;
        updateCameraSnapshotTarget();
    }

    if (m_logView && (connected != previous || m_cameraDescriptor != previousDescriptor)) {
        if (connected) {
            m_logView->append(tr("Connected to camera: %1").arg(m_cameraDescriptor));
        } else {
            m_logView->append(tr("Camera disconnected."));
        }
    }

    syncCameraActions();
}

void MainWindow::handleCameraStreamingChanged(bool streaming)
{
    const bool previous = m_cameraStreaming;
    m_cameraStreaming = streaming;

    if (m_logView && streaming != previous) {
        if (streaming) {
            m_logView->append(tr("Camera streaming started."));
        } else {
            m_logView->append(tr("Camera streaming stopped."));
        }
    }

    syncCameraActions();
}

void MainWindow::syncCameraActions()
{
    if (!m_toolBar) {
        return;
    }

    const bool cameraMode = (m_activeSource == ProjectSession::DataSource::ConnectedCamera);
    const bool windowReady = cameraMode && m_cameraWindow;
    const bool connected = windowReady && m_cameraWindow->isCameraConnected();
    const bool streaming = windowReady && m_cameraWindow->isStreaming();

    auto configureAction = [&](QAction *action, bool visible, bool enabled) {
        if (!action) {
            return;
        }
        action->setVisible(visible);
        action->setEnabled(enabled);
    };

    const bool showToolbarControls = false;
    configureAction(m_actionCameraConnect, showToolbarControls, windowReady && !connected);
    configureAction(m_actionCameraDisconnect, showToolbarControls, windowReady && connected && !streaming);
    configureAction(m_actionCameraStart, showToolbarControls, windowReady && connected && !streaming);
    configureAction(m_actionCameraStop, showToolbarControls, windowReady && streaming);
    configureAction(m_actionCameraSnapshot, showToolbarControls, windowReady && connected);
    configureAction(m_actionCameraRefresh, showToolbarControls, windowReady);

    if (m_actionCameraStart) {
        m_actionCameraStart->setChecked(cameraMode && streaming);
    }
    if (m_actionCameraStop) {
        m_actionCameraStop->setChecked(cameraMode && streaming);
    }

    if (m_toolbarCameraSeparator) {
        m_toolbarCameraSeparator->setVisible(showToolbarControls);
    }

    auto configureButton = [&](QToolButton *button, bool visible, bool enabled) {
        if (!button) {
            return;
        }
        button->setVisible(visible);
        button->setEnabled(enabled);
    };

    configureButton(m_tuningConnectButton, cameraMode, windowReady && !connected);
    configureButton(m_tuningDisconnectButton, cameraMode, windowReady && connected && !streaming);
    configureButton(m_tuningStartButton, cameraMode, windowReady && connected && !streaming);
    configureButton(m_tuningStopButton, cameraMode, windowReady && streaming);
    configureButton(m_tuningSnapshotButton, cameraMode, windowReady && connected);
    configureButton(m_tuningRefreshButton, cameraMode, windowReady);

    if (!cameraMode && m_tuningControlPopup) {
        m_tuningControlPopup->close();
    }
    if (!cameraMode && m_featurePopup) {
        m_featurePopup->close();
    }

    if (m_tuningControlPopupButton) {
        m_tuningControlPopupButton->setEnabled(windowReady);
        m_tuningControlPopupButton->setVisible(cameraMode);
    }
    if (m_tuningFeaturePopupButton) {
        m_tuningFeaturePopupButton->setEnabled(windowReady);
        m_tuningFeaturePopupButton->setVisible(cameraMode);
    }

    if (m_tuningDeviceCombo) {
        m_tuningDeviceCombo->setEnabled(windowReady);
        m_tuningDeviceCombo->setVisible(cameraMode);
    }
    if (m_tuningDeviceLabel) {
        m_tuningDeviceLabel->setVisible(cameraMode);
    }

    if (m_cameraStatusChip) {
        if (!cameraMode) {
            m_cameraStatusChip->setVisible(false);
        } else {
            QString descriptor = m_cameraDescriptor;
            if (descriptor.isEmpty() && connected) {
                descriptor = tr("Camera");
            }

            QString text;
            QString background;
            QString foreground;
            if (!connected) {
                text = tr("Camera offline");
                background = QStringLiteral("rgba(255, 134, 150, 0.28)");
                foreground = QStringLiteral("#fff3f6");
            } else if (streaming) {
                text = descriptor.isEmpty() ? tr("Streaming") : tr("Streaming · %1").arg(descriptor);
                background = QStringLiteral("rgba(64, 238, 210, 0.32)");
                foreground = QStringLiteral("#02281c");
            } else {
                text = descriptor.isEmpty() ? tr("Connected") : tr("Connected · %1").arg(descriptor);
                background = QStringLiteral("rgba(120, 196, 255, 0.26)");
                foreground = QStringLiteral("#0c233e");
            }

            const QString style = QStringLiteral("padding: 4px 12px; border-radius: 12px; font-weight: 600; letter-spacing: 0.3px; background:%1; color:%2;")
                                       .arg(background.isEmpty() ? QStringLiteral("rgba(72,132,255,0.08)") : background,
                                            foreground.isEmpty() ? QStringLiteral("#d4e3ff") : foreground);
            m_cameraStatusChip->setStyleSheet(style);
            m_cameraStatusChip->setText(text);
            m_cameraStatusChip->setVisible(true);
        }
    }

    auto applyTuningChip = [&](QLabel *chip, const QString &text, const QString &background, const QString &foreground) {
        if (!chip) {
            return;
        }
        if (!cameraMode) {
            chip->setVisible(false);
            return;
        }
        const QString style = QStringLiteral("padding:4px 12px;border-radius:12px;font-weight:600;letter-spacing:0.3px;background:%1;color:%2;")
                                   .arg(background, foreground);
        chip->setStyleSheet(style);
        chip->setText(text);
        chip->setVisible(true);
    };

    if (cameraMode) {
        QString descriptor = m_cameraDescriptor;
        if (descriptor.isEmpty() && connected) {
            descriptor = tr("Camera");
        }

        if (!connected) {
            applyTuningChip(m_tuningCameraStateChip,
                            tr("未连接"),
                            QStringLiteral("rgba(255, 134, 150, 0.3)"),
                            QStringLiteral("#fff3f6"));
            applyTuningChip(m_tuningStreamStateChip,
                            tr("未开流"),
                            QStringLiteral("rgba(120, 132, 170, 0.32)"),
                            QStringLiteral("#dde5ff"));
        } else if (streaming) {
            applyTuningChip(m_tuningCameraStateChip,
                            descriptor.isEmpty() ? tr("已连接") : tr("已连接 · %1").arg(descriptor),
                            QStringLiteral("rgba(120, 196, 255, 0.26)"),
                            QStringLiteral("#0c233e"));
            applyTuningChip(m_tuningStreamStateChip,
                            tr("实时预览"),
                            QStringLiteral("rgba(64, 238, 210, 0.34)"),
                            QStringLiteral("#043528"));
        } else {
            applyTuningChip(m_tuningCameraStateChip,
                            descriptor.isEmpty() ? tr("已连接") : tr("已连接 · %1").arg(descriptor),
                            QStringLiteral("rgba(120, 196, 255, 0.26)"),
                            QStringLiteral("#0c233e"));
            applyTuningChip(m_tuningStreamStateChip,
                            tr("等待预览"),
                            QStringLiteral("rgba(128, 152, 210, 0.24)"),
                            QStringLiteral("#0f2140"));
        }
    }
}

#endif // MYCALIB_HAVE_CONNECTED_CAMERA

void MainWindow::openDirectory(const QString &path)
{
    if (path.isEmpty()) {
        return;
    }
    QDir dir(path);
    if (!dir.exists()) {
        QDir().mkpath(dir.absolutePath());
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir.absolutePath()));
}

void MainWindow::refreshModeUi()
{
    ProjectSession::DataSource source = m_session ? m_session->metadata().dataSource
                                                  : ProjectSession::DataSource::LocalDataset;
#if !MYCALIB_HAVE_CONNECTED_CAMERA
    const bool cameraRequested = (source == ProjectSession::DataSource::ConnectedCamera);
    if (cameraRequested) {
        source = ProjectSession::DataSource::LocalDataset;
    }
#endif
    const auto previous = m_activeSource;
    m_activeSource = source;
    const bool cameraMode = (source == ProjectSession::DataSource::ConnectedCamera);

    if (m_modeChip) {
        m_modeChip->setVisible(true);
        if (cameraMode) {
            m_modeChip->setText(tr("Live capture"));
            m_modeChip->setStyleSheet(QStringLiteral("padding: 4px 14px; border-radius: 14px; font-weight: 600; background: rgba(40, 190, 140, 0.28); color: #ccffe8;"));
        } else {
            m_modeChip->setText(tr("Local images"));
            m_modeChip->setStyleSheet(QStringLiteral("padding: 4px 14px; border-radius: 14px; font-weight: 600; background: rgba(90, 120, 255, 0.30); color: #e0e7ff;"));
        }
    }

    if (m_actionRun) {
        m_actionRun->setText(cameraMode ? tr("Process live cache") : tr("Run calibration"));
    }

    if (m_modeCombo) {
        const int idx = m_modeCombo->findData(static_cast<int>(source));
        QSignalBlocker blocker(m_modeCombo);
        if (idx >= 0) {
            m_modeCombo->setCurrentIndex(idx);
        }
        m_modeCombo->setEnabled(m_session != nullptr && !m_running);
    }

    m_inputDir = defaultInputDirectory();
    QString inputDisplay = cameraMode
        ? tr("Camera stream cache: %1").arg(QDir::toNativeSeparators(m_inputDir))
        : QDir::toNativeSeparators(m_inputDir);
    if (m_inputPathEdit) {
        m_inputPathEdit->setText(inputDisplay);
    }

    const bool allowLocal = (source == ProjectSession::DataSource::LocalDataset) && !m_running;
    if (m_importButton) {
        m_importButton->setVisible(source == ProjectSession::DataSource::LocalDataset);
        m_importButton->setEnabled(allowLocal);
    }
    if (m_actionImportImages) {
        m_actionImportImages->setVisible(source == ProjectSession::DataSource::LocalDataset);
        m_actionImportImages->setEnabled(allowLocal);
    }
    if (m_toolbarPrimarySeparator) {
        m_toolbarPrimarySeparator->setVisible(source == ProjectSession::DataSource::LocalDataset);
    }
    if (m_openInputButton) {
    m_openInputButton->setText(cameraMode ? tr("Open cache") : tr("Open folder"));
        m_openInputButton->setEnabled(!m_inputDir.isEmpty());
    }

    m_outputDir = defaultOutputDirectory();
    if (m_outputPathEdit) {
        m_outputPathEdit->setText(QDir::toNativeSeparators(m_outputDir));
    }
    if (m_openOutputButton) {
        m_openOutputButton->setEnabled(!m_outputDir.isEmpty());
    }

    updateModeExplainer();
    updateInputSummary();
    updateRunAvailability();
    syncInputWatcher();

#if MYCALIB_HAVE_CONNECTED_CAMERA
    if (m_cameraWindow) {
        const bool cameraActive = (source == ProjectSession::DataSource::ConnectedCamera);
        if (!cameraActive && previous == ProjectSession::DataSource::ConnectedCamera) {
            bool anyChange = false;
            if (m_cameraWindow->isStreaming()) {
                m_cameraWindow->stopStreaming();
                anyChange = true;
            }
            if (m_cameraWindow->isCameraConnected()) {
                m_cameraWindow->disconnectCamera();
                anyChange = true;
            }
            if (anyChange && m_logView) {
                m_logView->append(tr("Stopped live capture session while leaving the camera workflow."));
            }
        }
        m_cameraWindow->setEnabled(cameraActive);
        if (cameraActive) {
            updateCameraSnapshotTarget();
            m_cameraWindow->refreshCameraList();
        }
        if (!cameraActive && m_capturePlanToggle && m_capturePlanToggle->isChecked()) {
            QSignalBlocker blocker(m_capturePlanToggle);
            m_capturePlanToggle->setChecked(false);
            handleCapturePlanToggled(false);
        }
    }

    syncCameraActions();
    updateCaptureOverlay();
#endif

    refreshCaptureTabs();
    ensureCapturePlannerMounted();

    if (previous != source && m_logView) {
        m_logView->append(tr("Switched workflow mode to %1").arg(dataSourceDisplayName(source)));
    }

#if !MYCALIB_HAVE_CONNECTED_CAMERA
    if (cameraRequested && previous != source && m_logView) {
        m_logView->append(tr("Live capture workflow is unavailable in this build."));
    }
#endif

    reconcileStageStates();
    refreshTuningPanel();
    updateStageNavigator();
}

void MainWindow::syncInputWatcher()
{
    if (!m_inputWatcher) {
        return;
    }

    const QStringList watchedDirs = m_inputWatcher->directories();
    if (!watchedDirs.isEmpty()) {
        m_inputWatcher->removePaths(watchedDirs);
    }
    const QStringList watchedFiles = m_inputWatcher->files();
    if (!watchedFiles.isEmpty()) {
        m_inputWatcher->removePaths(watchedFiles);
    }

    if (m_inputDir.isEmpty()) {
        return;
    }

    QDir dir(m_inputDir);
    if (!dir.exists()) {
        dir.mkpath(QStringLiteral("."));
    }

    const QString absolute = dir.absolutePath();
    if (absolute.isEmpty()) {
        return;
    }

    if (m_inputWatcher->addPaths({absolute}).isEmpty() && m_logView) {
        m_logView->append(tr("Unable to watch input directory: %1").arg(QDir::toNativeSeparators(absolute)));
    }
}

void MainWindow::handleInputDirectoryChanged(const QString &path)
{
    Q_UNUSED(path);

    const int previous = m_lastInputImageCount;
    updateInputSummary();
    updateRunAvailability();
    const int current = m_lastInputImageCount;

    if (m_logView && current != previous) {
        auto countMessage = [current](const QString &singular, const QString &plural) {
            return (current == 1) ? singular : plural.arg(current);
        };

        if (m_activeSource == ProjectSession::DataSource::ConnectedCamera) {
            QStringList fragments;
            fragments << (current > previous
                               ? tr("New frame detected in live cache.")
                               : tr("Live cache updated."));
            fragments << countMessage(tr("Live cache now holds 1 frame."),
                                      tr("Live cache now holds %1 frames."));
            m_logView->append(fragments.join(QLatin1Char(' ')));
        } else {
            QStringList fragments;
            fragments << (current > previous
                               ? tr("New image detected in project dataset.")
                               : tr("Project dataset refreshed."));
            fragments << countMessage(tr("Dataset now contains 1 image."),
                                      tr("Dataset now contains %1 images."));
            m_logView->append(fragments.join(QLatin1Char(' ')));
        }
    }

    syncInputWatcher();
}

void MainWindow::updateLaserStageUi()
{
    if (!m_laserStageBox) {
        return;
    }

    auto setLabelText = [](QLabel *label, const QString &text) {
        if (label) {
            label->setText(text);
        }
    };

    if (!m_session) {
        if (m_laserStageStatusChip) {
            setStatusChip(m_laserStageStatusChip,
                          ProjectSession::StageStatus::NotStarted,
                          stageStatusDisplay(ProjectSession::StageStatus::NotStarted));
        }
        setLabelText(m_laserStageStatusLabel, tr("Laser calibration stage is unavailable without an open project."));
        setLabelText(m_laserStageFrameLabel, QString());
        setLabelText(m_laserStageHintLabel, tr("Create or open a project to manage laser sweep captures."));
        if (m_importLaserButton) {
            m_importLaserButton->setEnabled(false);
        }
        if (m_openLaserCaptureButton) {
            m_openLaserCaptureButton->setEnabled(false);
        }
        if (m_openLaserOutputButton) {
            m_openLaserOutputButton->setEnabled(false);
        }
        if (m_markLaserCompletedButton) {
            m_markLaserCompletedButton->setEnabled(false);
            m_markLaserCompletedButton->setToolTip({});
        }
        return;
    }

    const auto state = m_session->stageState(ProjectSession::ProjectStage::LaserCalibration);
    if (m_laserStageStatusChip) {
        setStatusChip(m_laserStageStatusChip, state.status, stageStatusDisplay(state.status));
    }
    const QVector<ProjectSession::LaserFrame> frames = m_session->laserFrames();

    QStringList statusLines;
    statusLines << tr("Stage status: %1").arg(stageStatusDisplay(state.status));
    if (state.startedAt.isValid()) {
        statusLines << tr("Started: %1").arg(state.startedAt.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")));
    }
    if (state.completedAt.isValid()) {
        statusLines << tr("Completed: %1").arg(state.completedAt.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")));
    }
    if (!state.notes.isEmpty()) {
        QString notesHtml = state.notes.toHtmlEscaped();
        notesHtml.replace(QLatin1Char('\n'), QStringLiteral("<br/>"));
        statusLines << tr("Notes: %1").arg(notesHtml);
    }
    setLabelText(m_laserStageStatusLabel, statusLines.join(QStringLiteral("<br/>")));

    setLabelText(m_laserStageFrameLabel, tr("%n laser frame(s) recorded.", "", frames.size()));

    const QString capturePath = QDir::toNativeSeparators(m_session->laserCaptureDir().absolutePath());
    const QString outputPath = QDir::toNativeSeparators(m_session->laserOutputDir().absolutePath());
    const QString captureLink = QUrl::fromLocalFile(capturePath).toString();
    const QString outputLink = QUrl::fromLocalFile(outputPath).toString();
    const QString captureDisplay = capturePath.toHtmlEscaped();
    const QString outputDisplay = outputPath.toHtmlEscaped();
    const QString hintHtml = tr("<p>Drop laser sweep images into <a href=\"%1\">%2</a> or use the import button above. "
                                "When the solver is ready, write plane estimates and visualisations to <a href=\"%3\">%4</a>.</p>"
                                "<p>Mark the stage complete after solving to capture timestamps for the project log.</p>")
                          .arg(captureLink,
                              captureDisplay,
                              outputLink,
                              outputDisplay);
    setLabelText(m_laserStageHintLabel, hintHtml);

    const bool hasFrames = !frames.isEmpty();
    if (m_laserStageFrameLabel) {
        m_laserStageFrameLabel->setStyleSheet(hasFrames
                                                  ? QStringLiteral("color:#7de0b2;font-weight:600;")
                                                  : QStringLiteral("color:#ffb38a;font-weight:600;"));
    }

    if (m_importLaserButton) {
        m_importLaserButton->setEnabled(true);
    }
    if (m_openLaserCaptureButton) {
        m_openLaserCaptureButton->setEnabled(true);
    }
    if (m_openLaserOutputButton) {
        m_openLaserOutputButton->setEnabled(true);
    }
    if (m_markLaserCompletedButton) {
        const bool canComplete = state.status != ProjectSession::StageStatus::Completed && hasFrames;
        m_markLaserCompletedButton->setEnabled(canComplete);
        m_markLaserCompletedButton->setToolTip(canComplete ? QString() : tr("Import laser frames before completing the stage."));
    }
}

void MainWindow::persistProjectSummary(bool announce)
{
    if (!m_session) {
        return;
    }

    const QString summaryPath = m_session->exportsDir().absoluteFilePath(QStringLiteral("project_summary.json"));
    QString error;
    if (writeProjectSummary(summaryPath, &error)) {
        if (announce && m_logView) {
            m_logView->append(tr("Project summary saved to %1").arg(QDir::toNativeSeparators(summaryPath)));
        }
    } else if (!error.isEmpty() && m_logView) {
        m_logView->append(tr("Failed to write project summary: %1").arg(error));
    }
}

QJsonObject MainWindow::stageStateToJson(const ProjectSession::StageState &state) const
{
    QJsonObject obj;
    obj.insert(QStringLiteral("status"), ProjectSession::toString(state.status));
    if (state.startedAt.isValid()) {
        obj.insert(QStringLiteral("started_at"), toIso8601Utc(state.startedAt));
    }
    if (state.completedAt.isValid()) {
        obj.insert(QStringLiteral("completed_at"), toIso8601Utc(state.completedAt));
    }
    if (!state.notes.isEmpty()) {
        obj.insert(QStringLiteral("notes"), state.notes);
    }
    return obj;
}

QJsonArray MainWindow::calibrationShotsToJson(const QVector<ProjectSession::CaptureShot> &shots) const
{
    QJsonArray array;
    for (const auto &shot : shots) {
        QJsonObject obj;
        obj.insert(QStringLiteral("id"), shot.id.toString(QUuid::WithoutBraces));
        if (shot.capturedAt.isValid()) {
            obj.insert(QStringLiteral("captured_at"), toIso8601Utc(shot.capturedAt));
        }
        obj.insert(QStringLiteral("grid_row"), shot.gridRow);
        obj.insert(QStringLiteral("grid_col"), shot.gridCol);
        obj.insert(QStringLiteral("pose"), ProjectSession::toString(shot.pose));
        obj.insert(QStringLiteral("accepted"), shot.accepted);
        if (!shot.rejectionReason.isEmpty()) {
            obj.insert(QStringLiteral("rejection_reason"), shot.rejectionReason);
        }

        QString relativePath = shot.relativePath;
        QString absolutePath;

        if (!relativePath.isEmpty()) {
            absolutePath = absoluteSessionPath(relativePath);
        }

        if (absolutePath.isEmpty()) {
            const QVariant absoluteVariant = shot.metadata.value(QStringLiteral("absolute_path"));
            if (absoluteVariant.isValid()) {
                absolutePath = QDir::toNativeSeparators(QFileInfo(absoluteVariant.toString()).absoluteFilePath());
            }
        }

        if (absolutePath.isEmpty()) {
            const QVariant sourceVariant = shot.metadata.value(QStringLiteral("source_path"));
            if (sourceVariant.isValid()) {
                absolutePath = QDir::toNativeSeparators(QFileInfo(sourceVariant.toString()).absoluteFilePath());
            }
        }

        if (!absolutePath.isEmpty() && relativePath.isEmpty()) {
            relativePath = relativeSessionPath(absolutePath);
        }

        if (!relativePath.isEmpty()) {
            obj.insert(QStringLiteral("relative_path"), relativePath);
        }
        if (!absolutePath.isEmpty()) {
            obj.insert(QStringLiteral("absolute_path"), QDir::toNativeSeparators(absolutePath));
        }

        if (!shot.metadata.isEmpty()) {
            obj.insert(QStringLiteral("metadata"), QJsonObject::fromVariantMap(shot.metadata));
        }

        array.append(obj);
    }
    return array;
}

QJsonArray MainWindow::laserFramesToJson(const QVector<ProjectSession::LaserFrame> &frames) const
{
    QJsonArray array;
    for (const auto &frame : frames) {
        QJsonObject obj;
        obj.insert(QStringLiteral("id"), frame.id.toString(QUuid::WithoutBraces));
        if (frame.capturedAt.isValid()) {
            obj.insert(QStringLiteral("captured_at"), toIso8601Utc(frame.capturedAt));
        }
        QString relativePath = frame.relativePath;
        QString absolutePath;

        if (!relativePath.isEmpty()) {
            absolutePath = absoluteSessionPath(relativePath);
        }

        if (absolutePath.isEmpty()) {
            const QVariant sourceVariant = frame.annotations.value(QStringLiteral("source_path"));
            if (sourceVariant.isValid()) {
                absolutePath = QDir::toNativeSeparators(QFileInfo(sourceVariant.toString()).absoluteFilePath());
            }
        }

        if (!absolutePath.isEmpty() && relativePath.isEmpty()) {
            relativePath = relativeSessionPath(absolutePath);
        }

        if (!relativePath.isEmpty()) {
            obj.insert(QStringLiteral("relative_path"), relativePath);
        }
        if (!absolutePath.isEmpty()) {
            obj.insert(QStringLiteral("absolute_path"), QDir::toNativeSeparators(absolutePath));
        }
        if (!frame.annotations.isEmpty()) {
            obj.insert(QStringLiteral("annotations"), QJsonObject::fromVariantMap(frame.annotations));
        }
        array.append(obj);
    }
    return array;
}

QJsonObject MainWindow::laserPlaneToJson(const ProjectSession::LaserPlaneEstimate &plane) const
{
    QJsonObject obj;
    obj.insert(QStringLiteral("solved"), plane.solved);
    obj.insert(QStringLiteral("distance"), plane.distance);
    obj.insert(QStringLiteral("normal"), QJsonObject{
        {QStringLiteral("x"), plane.normal.x()},
        {QStringLiteral("y"), plane.normal.y()},
        {QStringLiteral("z"), plane.normal.z()}
    });
    if (!plane.extra.isEmpty()) {
        obj.insert(QStringLiteral("extra"), QJsonObject::fromVariantMap(plane.extra));
    }
    return obj;
}

QJsonObject MainWindow::calibrationOutputToJson(const CalibrationOutput &output) const
{
    QJsonObject obj;
    obj.insert(QStringLiteral("success"), output.success);
    obj.insert(QStringLiteral("message"), output.message);
    obj.insert(QStringLiteral("total_images"), static_cast<int>(output.allDetections.size()));
    obj.insert(QStringLiteral("kept_images"), static_cast<int>(output.keptDetections.size()));
    obj.insert(QStringLiteral("removed_images"), static_cast<int>(output.removedDetections.size()));

    QJsonObject metrics;
    metrics.insert(QStringLiteral("rms"), output.metrics.rms);
    metrics.insert(QStringLiteral("mean_px"), output.metrics.meanErrorPx);
    metrics.insert(QStringLiteral("median_px"), output.metrics.medianErrorPx);
    metrics.insert(QStringLiteral("max_px"), output.metrics.maxErrorPx);
    metrics.insert(QStringLiteral("p95_px"), output.metrics.p95ErrorPx);
    metrics.insert(QStringLiteral("std_px"), output.metrics.stdErrorPx);
    metrics.insert(QStringLiteral("mean_residual_mm"), vec3dToJson(output.metrics.meanResidualMm));
    metrics.insert(QStringLiteral("rms_residual_mm"), vec3dToJson(output.metrics.rmsResidualMm));
    metrics.insert(QStringLiteral("mean_residual_percent"), vec3dToJson(output.metrics.meanResidualPercent));
    metrics.insert(QStringLiteral("rms_residual_percent"), vec3dToJson(output.metrics.rmsResidualPercent));
    metrics.insert(QStringLiteral("mean_translation_mm"), vec3dToJson(output.metrics.meanTranslationMm));
    metrics.insert(QStringLiteral("std_translation_mm"), vec3dToJson(output.metrics.stdTranslationMm));
    metrics.insert(QStringLiteral("distortion_max_shift_px"), output.heatmaps.distortionMax);
    obj.insert(QStringLiteral("metrics"), metrics);

    obj.insert(QStringLiteral("camera_matrix"), matToJsonArray(output.cameraMatrix));
    obj.insert(QStringLiteral("distortion_coefficients"), matToJsonArray(output.distCoeffs));

    QJsonArray kept;
    for (const auto &rec : output.keptDetections) {
        QJsonObject det;
        det.insert(QStringLiteral("name"), QString::fromStdString(rec.name));
        det.insert(QStringLiteral("mean_error_px"), rec.meanErrorPx());
        det.insert(QStringLiteral("max_error_px"), rec.maxErrorPx());
        det.insert(QStringLiteral("translation_mm"), QJsonArray{rec.translationMm[0], rec.translationMm[1], rec.translationMm[2]});
        det.insert(QStringLiteral("rotation_deg"), QJsonArray{rec.rotationDeg[0], rec.rotationDeg[1], rec.rotationDeg[2]});
        kept.append(det);
    }
    obj.insert(QStringLiteral("kept_samples"), kept);

    QJsonArray removed;
    for (const auto &rec : output.removedDetections) {
        QJsonObject det;
        det.insert(QStringLiteral("name"), QString::fromStdString(rec.name));
        det.insert(QStringLiteral("iteration"), rec.iterationRemoved);
        det.insert(QStringLiteral("mean_error_px"), rec.meanErrorPx());
        det.insert(QStringLiteral("max_error_px"), rec.maxErrorPx());
        removed.append(det);
    }
    obj.insert(QStringLiteral("removed_samples"), removed);

    return obj;
}

QJsonObject MainWindow::buildProjectSummaryJson() const
{
    QJsonObject root;

    if (!m_session) {
        return root;
    }

    const auto &meta = m_session->metadata();

    QJsonObject project;
    project.insert(QStringLiteral("name"), meta.projectName);
    project.insert(QStringLiteral("id"), meta.projectId);
    if (meta.createdAt.isValid()) {
        project.insert(QStringLiteral("created_at"), toIso8601Utc(meta.createdAt));
    }
    if (meta.lastOpenedAt.isValid()) {
        project.insert(QStringLiteral("last_opened_at"), toIso8601Utc(meta.lastOpenedAt));
    }
    project.insert(QStringLiteral("data_source"), ProjectSession::toString(meta.dataSource));
    project.insert(QStringLiteral("root_path"), QDir::toNativeSeparators(m_session->rootPath()));
    if (!meta.cameraVendor.isEmpty()) {
        project.insert(QStringLiteral("camera_vendor"), meta.cameraVendor);
    }
    if (!meta.cameraModel.isEmpty()) {
        project.insert(QStringLiteral("camera_model"), meta.cameraModel);
    }
    root.insert(QStringLiteral("project"), project);

    QJsonObject stages;
    stages.insert(QStringLiteral("camera_tuning"), stageStateToJson(meta.cameraTuning));
    stages.insert(QStringLiteral("calibration_capture"), stageStateToJson(meta.calibrationCapture));
    stages.insert(QStringLiteral("laser_calibration"), stageStateToJson(meta.laserCalibration));
    root.insert(QStringLiteral("stages"), stages);

    const QVector<ProjectSession::CaptureShot> shots = meta.calibrationShots;
    int acceptedShots = 0;
    std::array<std::array<int, kCaptureGridCols>, kCaptureGridRows> gridTotals {};
    std::array<int, static_cast<int>(std::size(kPoseDescriptors))> poseTotals {};
    poseTotals.fill(0);

    for (const auto &shot : shots) {
        if (shot.accepted) {
            ++acceptedShots;
        }
        if (shot.gridRow >= 0 && shot.gridRow < kCaptureGridRows && shot.gridCol >= 0 && shot.gridCol < kCaptureGridCols) {
            ++gridTotals[shot.gridRow][shot.gridCol];
        }
        const int poseIdx = capturePoseIndex(shot.pose);
        if (poseIdx >= 0 && poseIdx < static_cast<int>(poseTotals.size())) {
            ++poseTotals[poseIdx];
        }
    }

    QJsonObject capture;
    capture.insert(QStringLiteral("total_shots"), shots.size());
    capture.insert(QStringLiteral("accepted_shots"), acceptedShots);
    capture.insert(QStringLiteral("shots"), calibrationShotsToJson(shots));
    capture.insert(QStringLiteral("dataset_image_count"), countImageFiles(m_session->calibrationCaptureDir().absolutePath()));
    capture.insert(QStringLiteral("dataset_directory"), relativeSessionPath(m_session->calibrationCaptureDir().absolutePath()));

    QJsonArray gridArray;
    for (int r = 0; r < kCaptureGridRows; ++r) {
        QJsonArray row;
        for (int c = 0; c < kCaptureGridCols; ++c) {
            row.append(gridTotals[r][c]);
        }
        gridArray.append(row);
    }
    capture.insert(QStringLiteral("grid_totals"), gridArray);

    QJsonArray poseCounts;
    for (const auto &descriptor : kPoseDescriptors) {
        const int idx = capturePoseIndex(descriptor.pose);
        if (idx >= 0 && idx < static_cast<int>(poseTotals.size())) {
            QJsonObject poseObj;
            poseObj.insert(QStringLiteral("pose"), ProjectSession::toString(descriptor.pose));
            poseObj.insert(QStringLiteral("label"), tr(descriptor.display));
            poseObj.insert(QStringLiteral("count"), poseTotals[idx]);
            poseCounts.append(poseObj);
        }
    }
    capture.insert(QStringLiteral("pose_totals"), poseCounts);

    capture.insert(QStringLiteral("live_cache_image_count"), countImageFiles(m_session->liveCacheDir().absolutePath()));
    capture.insert(QStringLiteral("live_cache_directory"), relativeSessionPath(m_session->liveCacheDir().absolutePath()));
    capture.insert(QStringLiteral("tuning_directory"), relativeSessionPath(m_session->tuningCaptureDir().absolutePath()));
    root.insert(QStringLiteral("capture"), capture);

    if (m_lastOutput.success) {
        QJsonObject calibration = calibrationOutputToJson(m_lastOutput);
        calibration.insert(QStringLiteral("output_directory"), relativeSessionPath(m_outputDir));
        calibration.insert(QStringLiteral("report_path"), relativeSessionPath(QDir(m_outputDir).absoluteFilePath(QStringLiteral("calibration_report.json"))));
        calibration.insert(QStringLiteral("paper_figures_directory"), relativeSessionPath(QDir(m_outputDir).absoluteFilePath(QStringLiteral("paper_figures"))));
        root.insert(QStringLiteral("calibration"), calibration);
    }

    const QVector<ProjectSession::LaserFrame> laserFrames = meta.laserFrames;
    QJsonObject laser;
    laser.insert(QStringLiteral("frame_count"), laserFrames.size());
    laser.insert(QStringLiteral("frames"), laserFramesToJson(laserFrames));
    laser.insert(QStringLiteral("plane_estimate"), laserPlaneToJson(meta.laserPlane));
    laser.insert(QStringLiteral("capture_directory"), relativeSessionPath(m_session->laserCaptureDir().absolutePath()));
    laser.insert(QStringLiteral("output_directory"), relativeSessionPath(m_session->laserOutputDir().absolutePath()));
    root.insert(QStringLiteral("laser"), laser);

    QJsonObject directories;
    directories.insert(QStringLiteral("calibration_output"), relativeSessionPath(m_session->calibrationOutputDir().absolutePath()));
    directories.insert(QStringLiteral("laser_output"), relativeSessionPath(m_session->laserOutputDir().absolutePath()));
    directories.insert(QStringLiteral("exports"), relativeSessionPath(m_session->exportsDir().absolutePath()));
    directories.insert(QStringLiteral("logs"), relativeSessionPath(m_session->logsDir().absolutePath()));
    root.insert(QStringLiteral("directories"), directories);

    root.insert(QStringLiteral("summary_generated_at"), toIso8601Utc(QDateTime::currentDateTimeUtc()));

    return root;
}

bool MainWindow::writeProjectSummary(const QString &filePath, QString *errorMessage) const
{
    if (!m_session) {
        if (errorMessage) {
            *errorMessage = tr("No project session available");
        }
        return false;
    }

    const QJsonObject summary = buildProjectSummaryJson();
    if (summary.isEmpty()) {
        if (errorMessage) {
            *errorMessage = tr("Summary is empty");
        }
        return false;
    }

    const QFileInfo info(filePath);
    const QString directoryPath = info.absolutePath();
    if (!directoryPath.isEmpty()) {
        QDir().mkpath(directoryPath);
    }

    QFile file(info.absoluteFilePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (errorMessage) {
            *errorMessage = file.errorString();
        }
        return false;
    }

    const QJsonDocument doc(summary);
    const qint64 bytesWritten = file.write(doc.toJson(QJsonDocument::Indented));
    if (bytesWritten <= 0) {
        if (errorMessage) {
            *errorMessage = tr("Failed to write summary");
        }
        return false;
    }

    return true;
}

void MainWindow::persistCalibrationSnapshot(const CalibrationOutput &output) const
{
    if (!m_session || !output.success) {
        return;
    }

    const QString snapshotPath = m_session->configDir().absoluteFilePath(QStringLiteral("last_calibration_snapshot.bin"));
    QSaveFile file(snapshotPath);
    if (!file.open(QIODevice::WriteOnly)) {
        Logger::warning(tr("无法写入标定快照：%1").arg(file.errorString()));
        return;
    }

    QDataStream out(&file);
    out.setVersion(QDataStream::Qt_6_5);
    out << kSnapshotMagic << kSnapshotVersion;
    out << quint8(output.success ? 1 : 0);
    out << output.message;
    writeMat(out, output.cameraMatrix);
    writeMat(out, output.distCoeffs);
    out << qint32(output.imageSize.width) << qint32(output.imageSize.height);
    writeCalibrationMetrics(out, output.metrics);
    writeDetectionList(out, output.allDetections);
    writeDetectionList(out, output.keptDetections);
    writeDetectionList(out, output.removedDetections);
    writeHeatmapMeta(out, output.heatmaps);
    out << m_outputDir;

    if (out.status() != QDataStream::Ok) {
        Logger::warning(tr("写入标定快照时发生错误。"));
        return;
    }

    if (!file.commit()) {
        Logger::warning(tr("无法保存标定快照：%1").arg(file.errorString()));
    }
}

bool MainWindow::loadCalibrationSnapshot(const QString &filePath, CalibrationOutput *output, QString *outputDir) const
{
    if (!output) {
        return false;
    }

    QFile file(filePath);
    if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
        return false;
    }

    QDataStream in(&file);
    in.setVersion(QDataStream::Qt_6_5);

    quint32 magic = 0;
    quint16 version = 0;
    in >> magic >> version;
    if (magic != kSnapshotMagic || version != kSnapshotVersion) {
        return false;
    }

    quint8 successByte = 0;
    QString message;
    in >> successByte >> message;
    output->success = (successByte != 0);
    output->message = message;
    output->cameraMatrix = readMat(in);
    output->distCoeffs = readMat(in);
    qint32 width = 0;
    qint32 height = 0;
    in >> width >> height;
    output->imageSize = cv::Size(width, height);
    output->metrics = readCalibrationMetrics(in);
    output->allDetections = readDetectionList(in);
    output->keptDetections = readDetectionList(in);
    output->removedDetections = readDetectionList(in);
    output->heatmaps = readHeatmapMeta(in);
    QString storedOutputDir;
    in >> storedOutputDir;
    if (outputDir) {
        *outputDir = storedOutputDir;
    }

    return in.status() == QDataStream::Ok;
}

void MainWindow::restoreCalibrationSnapshot()
{
    if (!m_session) {
        return;
    }

    const QString snapshotPath = m_session->configDir().absoluteFilePath(QStringLiteral("last_calibration_snapshot.bin"));
    CalibrationOutput snapshot;
    QString storedOutputDir;
    if (!loadCalibrationSnapshot(snapshotPath, &snapshot, &storedOutputDir) || !snapshot.success) {
        return;
    }

    m_lastOutput = snapshot;
    materializeDebugArtifacts(m_lastOutput);
    m_lastOutputFromSnapshot = true;

    if (!storedOutputDir.isEmpty()) {
        const QString resolved = absoluteSessionPath(storedOutputDir);
        m_outputDir = resolved.isEmpty() ? storedOutputDir : resolved;
        if (m_outputPathEdit) {
            m_outputPathEdit->setText(QDir::toNativeSeparators(m_outputDir));
        }
    }

    regenerateHeatmaps(m_lastOutput);
    reloadHeatmapsFromDisk(m_outputDir, &m_lastOutput.heatmaps);

    updateSummaryPanel(m_lastOutput);
    populateDetectionTree(m_lastOutput);
    updateDerivedCoverageFromOutput(m_lastOutput);
    refreshCapturePlanFromSession();
    showHeatmaps(m_lastOutput);
    ensurePoseView();
    if (m_poseView) {
        m_poseView->setDetections(m_lastOutput.allDetections);
    }
    refreshState(false);
    updateStageNavigator();

    if (m_logView) {
        m_logView->append(tr("已加载上次标定结果。"));
    }
}

void MainWindow::updateModeExplainer()
{
    if (!m_modeHeadline || !m_modeDescription) {
        return;
    }

    const QString inputPath = QDir::toNativeSeparators(m_inputDir);
    const QString cachePath = inputPath;

    if (m_activeSource == ProjectSession::DataSource::ConnectedCamera) {
        m_modeHeadline->setText(tr("Live capture workflow"));
        const QString link = QUrl::fromLocalFile(cachePath).toString();
        const QString html = tr("Use the camera toolbar to connect, start streaming, and capture frames. Snapshots are cached in <a href=\"%1\">%2</a> and appear here automatically.")
                                 .arg(link, cachePath);
        m_modeDescription->setText(html);
    } else {
        m_modeHeadline->setText(tr("Local dataset workflow"));
        const QString link = QUrl::fromLocalFile(inputPath).toString();
        const QString html = tr("Calibration images are read from <a href=\"%1\">%2</a>. Use “Import images” or drop files into the folder before running calibration.")
                                 .arg(link, inputPath);
        m_modeDescription->setText(html);
    }
}

void MainWindow::setStatusChip(QLabel *chip, ProjectSession::StageStatus status, const QString &labelText)
{
    if (!chip) {
        return;
    }

    QString background;
    QString foreground;
    switch (status) {
    case ProjectSession::StageStatus::NotStarted:
        background = QStringLiteral("rgba(118, 126, 158, 0.28)");
        foreground = QStringLiteral("#d6dcf2");
        break;
    case ProjectSession::StageStatus::InProgress:
        background = QStringLiteral("rgba(255, 188, 92, 0.30)");
        foreground = QStringLiteral("#2d1b00");
        break;
    case ProjectSession::StageStatus::Completed:
        background = QStringLiteral("rgba(90, 210, 140, 0.30)");
        foreground = QStringLiteral("#09331a");
        break;
    }

    const QString style = QStringLiteral("padding:4px 12px; border-radius:12px; font-weight:600; background:%1; color:%2;")
                              .arg(background, foreground);
    chip->setStyleSheet(style);
    chip->setText(labelText);
    chip->setVisible(true);
}

QString MainWindow::formatStageTimestamp(const QDateTime &dt) const
{
    if (!dt.isValid()) {
        return {};
    }
    return dt.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

QString MainWindow::stageStateLongSummary(const ProjectSession::StageState &state) const
{
    QStringList parts;
    parts << tr("状态：%1").arg(stageStatusDisplay(state.status));
    if (state.startedAt.isValid()) {
        parts << tr("开始：%1").arg(formatStageTimestamp(state.startedAt));
    }
    if (state.completedAt.isValid()) {
        parts << tr("完成：%1").arg(formatStageTimestamp(state.completedAt));
    }
    if (!state.notes.isEmpty()) {
        QString notes = state.notes;
        notes.replace(QLatin1Char('\n'), QStringLiteral(" · "));
        parts << tr("备注：%1").arg(notes);
    }
    return parts.join(QStringLiteral("<br/>"));
}

void MainWindow::updateStageNavigator()
{
    auto composedTitle = [](QListWidgetItem *item, int role) -> QString {
        if (!item) {
            return {};
        }
        const QVariant data = item->data(role);
        if (data.isValid()) {
            return data.toString();
        }
        const QString text = item->text();
        const int newline = text.indexOf(QLatin1Char('\n'));
        if (newline >= 0) {
            return (role == Qt::UserRole) ? text.left(newline) : text.mid(newline + 1);
        }
        return text;
    };

    auto joinParts = [](const QStringList &parts) {
        QStringList filtered;
        filtered.reserve(parts.size());
        for (const QString &part : parts) {
            const QString trimmed = part.trimmed();
            if (!trimmed.isEmpty()) {
                filtered << trimmed;
            }
        }
        return filtered.join(QStringLiteral(" · "));
    };

    auto applyItemText = [&](QListWidgetItem *item, const QStringList &secondLineParts, const QString &tooltip) {
        if (!item) {
            return;
        }
        const QString title = composedTitle(item, Qt::UserRole);
        const QString secondLine = joinParts(secondLineParts);
        if (!title.isEmpty() && !secondLine.isEmpty()) {
            item->setText(QStringLiteral("%1\n%2").arg(title, secondLine));
        } else if (!secondLine.isEmpty()) {
            item->setText(secondLine);
        }
        if (!tooltip.isEmpty()) {
            item->setToolTip(tooltip);
        }
    };

    auto applyStageItem = [&](QListWidgetItem *item, ProjectSession::ProjectStage stage, const QStringList &extraParts) {
        if (!item) {
            return;
        }
        QStringList parts;
        parts << composedTitle(item, Qt::UserRole + 1);
        QString tooltip;

        if (!m_session) {
            parts << tr("未打开项目");
            tooltip = tr("Create or open a project to begin.");
        } else {
            const auto state = m_session->stageState(stage);
            parts << stageStatusDisplay(state.status);
            parts << extraParts;
            QString detail = stageStateLongSummary(state);
            if (!detail.isEmpty()) {
                tooltip = detail;
            }
        }

        applyItemText(item, parts, tooltip);
    };

    if (m_stageOverviewItem) {
        QStringList overviewParts;
        overviewParts << composedTitle(m_stageOverviewItem, Qt::UserRole + 1);
        QString tooltip;
        if (!m_session) {
            overviewParts << tr("未打开项目");
            tooltip = tr("Open or create a project to view stage progress.");
        } else {
            const auto meta = m_session->metadata();
            const QString projectName = meta.projectName.isEmpty() ? tr("未命名项目") : meta.projectName;
            overviewParts << tr("项目：%1").arg(projectName);
            tooltip = tr("当前项目：%1").arg(projectName);
        }
        applyItemText(m_stageOverviewItem, overviewParts, tooltip);
    }

    if (m_stageTuningItem) {
        QStringList extras;
        if (m_session) {
            const int snapshotCount = m_session->tuningSnapshots().size();
            if (snapshotCount > 0) {
                extras << tr("%n snapshot(s)", "", snapshotCount);
            }
        }
        applyStageItem(m_stageTuningItem, ProjectSession::ProjectStage::CameraTuning, extras);
    }

    if (m_stageCaptureItem) {
        QStringList extras;
        if (m_session) {
            const int shotCount = aggregateCoverageShots().size();
            if (shotCount > 0) {
                extras << tr("%n capture(s)", "", shotCount);
            }
        }
        applyStageItem(m_stageCaptureItem, ProjectSession::ProjectStage::CalibrationCapture, extras);
    }

    if (m_stageAnalysisItem) {
        QStringList extras;
        if (m_session) {
            const int frameCount = m_session->laserFrames().size();
            if (frameCount > 0) {
                extras << tr("%n laser frame(s)", "", frameCount);
            }
        }
        applyStageItem(m_stageAnalysisItem, ProjectSession::ProjectStage::LaserCalibration, extras);
    }

    updateStageOneCard();
    updateStageTwoCard();
    updateLaserStageUi();
}

void MainWindow::updateStageOneCard()
{
    if (!m_stageTuningBox) {
        return;
    }

    if (!m_session) {
        if (m_stageTuningStatusChip) {
            setStatusChip(m_stageTuningStatusChip,
                          ProjectSession::StageStatus::NotStarted,
                          stageStatusDisplay(ProjectSession::StageStatus::NotStarted));
        }
        if (m_stageTuningSummaryLabel) {
            m_stageTuningSummaryLabel->setText(tr("创建或打开项目以开始调参阶段。"));
        }
        if (m_stageTuningNotesLabel) {
            m_stageTuningNotesLabel->clear();
        }
        if (m_stageTuningReviewButton) {
            m_stageTuningReviewButton->setEnabled(false);
        }
        if (m_stageTuningFolderButton) {
            m_stageTuningFolderButton->setEnabled(false);
        }
        if (m_stageTuningCompleteButton) {
            m_stageTuningCompleteButton->setEnabled(false);
        }
        return;
    }

    const auto state = m_session->stageState(ProjectSession::ProjectStage::CameraTuning);
    if (m_stageTuningStatusChip) {
        setStatusChip(m_stageTuningStatusChip, state.status, stageStatusDisplay(state.status));
    }

    const QVector<ProjectSession::TuningSnapshot> snapshots = m_session->tuningSnapshots();
    QString summary;
    if (snapshots.isEmpty()) {
        summary = tr("尚未记录调参快照。连接相机后点击“快照”记录曝光、增益与帧率。");
    } else {
        const ProjectSession::TuningSnapshot &latest = snapshots.constLast();
        const QString captured = formatStageTimestamp(latest.capturedAt);
        summary = tr("已记录 %1 条调参快照。最近一次：%2。")
                      .arg(snapshots.size())
                      .arg(captured.isEmpty() ? tr("未知") : captured);
    }
    if (m_stageTuningSummaryLabel) {
        m_stageTuningSummaryLabel->setText(summary);
    }

    if (m_stageTuningNotesLabel) {
        m_stageTuningNotesLabel->setText(stageStateLongSummary(state));
    }

    if (m_stageTuningReviewButton) {
        m_stageTuningReviewButton->setEnabled(true);
    }
    if (m_stageTuningFolderButton) {
        m_stageTuningFolderButton->setEnabled(true);
    }
    if (m_stageTuningCompleteButton) {
        const bool allowComplete = (state.status != ProjectSession::StageStatus::Completed);
        m_stageTuningCompleteButton->setEnabled(allowComplete);
        m_stageTuningCompleteButton->setVisible(true);
    }
}

void MainWindow::updateStageTwoCard()
{
    if (!m_stageCaptureBox) {
        return;
    }

    const int totalCells = kCaptureGridRows * kCaptureGridCols;
    const int targetShots = totalCells * kCaptureTargetPerCell;

    if (!m_session) {
        if (m_stageCaptureStatusChip) {
            setStatusChip(m_stageCaptureStatusChip,
                          ProjectSession::StageStatus::NotStarted,
                          stageStatusDisplay(ProjectSession::StageStatus::NotStarted));
        }
        if (m_stageCaptureSummaryLabel) {
            m_stageCaptureSummaryLabel->setText(tr("创建或打开项目以跟踪阶段二采集。"));
        }
        if (m_stageCaptureProgress) {
            m_stageCaptureProgress->setMaximum(targetShots);
            m_stageCaptureProgress->setValue(0);
        }
        if (m_stageCaptureTodoLabel) {
            m_stageCaptureTodoLabel->clear();
        }
        if (m_stageCaptureModeHint) {
            m_stageCaptureModeHint->setText(tr("当前无项目，无法显示采集计划。"));
        }
        if (m_stageCapturePrimaryButton) {
            m_stageCapturePrimaryButton->setEnabled(false);
        }
        if (m_stageCaptureSecondaryButton) {
            m_stageCaptureSecondaryButton->setEnabled(false);
        }
        return;
    }

    const auto state = m_session->stageState(ProjectSession::ProjectStage::CalibrationCapture);
    if (m_stageCaptureStatusChip) {
        setStatusChip(m_stageCaptureStatusChip, state.status, stageStatusDisplay(state.status));
    }

    const bool cameraMode = (m_session->metadata().dataSource == ProjectSession::DataSource::ConnectedCamera);
    const QVector<ProjectSession::CaptureShot> shots = aggregateCoverageShots();
    const CaptureCoverageStats stats = computeCaptureCoverage(shots);
    const int totalShots = stats.totalShots;
    const int inputImageCount = countImageFiles(m_inputDir);

    if (m_stageCaptureProgress) {
        if (cameraMode) {
            m_stageCaptureProgress->setMaximum(targetShots);
            m_stageCaptureProgress->setValue(std::min(totalShots, targetShots));
            m_stageCaptureProgress->setFormat(QStringLiteral("%v / %m"));
        } else {
            const int maximum = std::max(inputImageCount, 1);
            m_stageCaptureProgress->setMaximum(maximum);
            m_stageCaptureProgress->setValue(inputImageCount);
            m_stageCaptureProgress->setFormat(tr("%v 张图像"));
        }
    }

    int completedCells = 0;
    QStringList missingCells;
    QStringList advisoryCells;
    for (int r = 0; r < kCaptureGridRows; ++r) {
        for (int c = 0; c < kCaptureGridCols; ++c) {
            const int count = stats.cellTotals[r][c];
            const bool meetsMinimum = count >= kCaptureMinimumPerCell;
            const bool meetsRecommended = count >= kCaptureTargetPerCell;
            if (meetsMinimum) {
                ++completedCells;
                if (!meetsRecommended) {
                    advisoryCells << tr("第%1行 · 第%2列 (%3/%4)")
                                      .arg(r + 1)
                                      .arg(c + 1)
                                      .arg(count)
                                      .arg(kCaptureTargetPerCell);
                }
            } else {
                missingCells << tr("第%1行 · 第%2列 (%3/%4)")
                                 .arg(r + 1)
                                 .arg(c + 1)
                                 .arg(count)
                                 .arg(kCaptureMinimumPerCell);
            }
        }
    }

    QStringList missingPoses;
    for (int i = 0; i < static_cast<int>(std::size(kPoseDescriptors)); ++i) {
        if (stats.poseTotals[i] == 0) {
            missingPoses << tr(kPoseDescriptors[i].display);
        }
    }

    QString summary;
    if (cameraMode) {
        if (totalShots == 0) {
            summary = tr("尚未记录阶段二样本。补齐九宫格后再运行标定。");
        } else {
            summary = tr("%1 / %2 计划样本完成 · %3 / %4 个格位达标（最低 %5 张/格）")
                          .arg(totalShots)
                          .arg(targetShots)
                          .arg(completedCells)
                          .arg(totalCells)
                          .arg(kCaptureMinimumPerCell);
        }
    } else {
        if (totalShots > 0) {
            summary = tr("已从标定结果映射 %1 张样本至九宫格（最低 %2 张/格统计），项目数据集共 %3 张图像。")
                          .arg(totalShots)
                          .arg(kCaptureMinimumPerCell)
                          .arg(inputImageCount);
        } else {
            summary = tr("项目数据集中当前有 %1 张图像。运行标定后，可在 Analytics 页检查覆盖与残差。")
                          .arg(inputImageCount);
        }
    }
    if (m_stageCaptureSummaryLabel) {
        m_stageCaptureSummaryLabel->setText(summary);
    }

    if (m_stageCaptureTodoLabel) {
        if (cameraMode) {
            QStringList todoItems;
            if (!missingCells.isEmpty()) {
                const QString preview = missingCells.mid(0, 4).join(QStringLiteral("，"));
                QString line = tr("补拍九宫格（最低覆盖）：%1").arg(preview);
                if (missingCells.size() > 4) {
                    line += tr(" … 共 %1 个格位").arg(missingCells.size());
                }
                todoItems << line;
            }
            if (!advisoryCells.isEmpty()) {
                const QString preview = advisoryCells.mid(0, 4).join(QStringLiteral("，"));
                QString line = tr("补足建议覆盖：%1").arg(preview);
                if (advisoryCells.size() > 4) {
                    line += tr(" … 共 %1 个格位").arg(advisoryCells.size());
                }
                todoItems << line;
            }
            if (!missingPoses.isEmpty()) {
                todoItems << tr("补齐姿态：%1").arg(missingPoses.join(QStringLiteral("，")));
            }

            if (todoItems.isEmpty()) {
                m_stageCaptureTodoLabel->setStyleSheet(QStringLiteral("color:#7de0b2;font-weight:600;"));
                m_stageCaptureTodoLabel->setText(tr("覆盖良好，可直接运行标定。"));
            } else {
                m_stageCaptureTodoLabel->setStyleSheet(QStringLiteral("color:#ffb38a;font-weight:600;"));
                m_stageCaptureTodoLabel->setText(QStringLiteral("<ul><li>%1</li></ul>")
                                                     .arg(todoItems.join(QStringLiteral("</li><li>"))));
            }
        } else {
            if (totalShots == 0) {
                m_stageCaptureTodoLabel->setStyleSheet(QStringLiteral("color:#9eb2d4;"));
                m_stageCaptureTodoLabel->setText(tr("运行一次标定即可生成九宫格覆盖统计，并在“Analytics”页查看热力图与残差。"));
            } else {
                QStringList todoItems;
                if (!missingCells.isEmpty()) {
                    const QString preview = missingCells.mid(0, 4).join(QStringLiteral("，"));
                    QString line = tr("覆盖不足的格位（低于最低要求）：%1").arg(preview);
                    if (missingCells.size() > 4) {
                        line += tr(" … 共 %1 个格位").arg(missingCells.size());
                    }
                    todoItems << line;
                }
                if (!advisoryCells.isEmpty()) {
                    const QString preview = advisoryCells.mid(0, 4).join(QStringLiteral("，"));
                    QString line = tr("建议补拍以提升覆盖的格位：%1").arg(preview);
                    if (advisoryCells.size() > 4) {
                        line += tr(" … 共 %1 个格位").arg(advisoryCells.size());
                    }
                    todoItems << line;
                }
                if (!missingPoses.isEmpty()) {
                    todoItems << tr("缺少姿态：%1").arg(missingPoses.join(QStringLiteral("，")));
                }
                if (todoItems.isEmpty()) {
                    m_stageCaptureTodoLabel->setStyleSheet(QStringLiteral("color:#7de0b2;font-weight:600;"));
                    m_stageCaptureTodoLabel->setText(tr("覆盖分布良好，可继续分析或导出结果。"));
                } else {
                    m_stageCaptureTodoLabel->setStyleSheet(QStringLiteral("color:#ffb38a;font-weight:600;"));
                    m_stageCaptureTodoLabel->setText(QStringLiteral("<ul><li>%1</li></ul>")
                                                         .arg(todoItems.join(QStringLiteral("</li><li>"))));
                }
            }
        }
    }

    if (m_stageCaptureModeHint) {
#if MYCALIB_HAVE_CONNECTED_CAMERA
        if (m_session->metadata().dataSource == ProjectSession::DataSource::ConnectedCamera) {
            m_stageCaptureModeHint->setText(tr("实时采集模式：缓存中已有 %1 张快照。使用九宫格规划补齐覆盖。")
                                                .arg(inputImageCount));
        } else
#endif
        {
            m_stageCaptureModeHint->setText(tr("本地图像模式：项目数据集中当前有 %1 张图片。导入或补拍后可再次运行标定。")
                                                .arg(inputImageCount));
        }
    }

    if (m_stageCapturePrimaryButton) {
        m_stageCapturePrimaryButton->setEnabled(true);
#if MYCALIB_HAVE_CONNECTED_CAMERA
        if (m_session->metadata().dataSource == ProjectSession::DataSource::ConnectedCamera) {
            m_stageCapturePrimaryButton->setText(tr("打开相机采集器"));
        } else {
            m_stageCapturePrimaryButton->setText(tr("打开图像目录"));
        }
#else
        m_stageCapturePrimaryButton->setText(tr("打开图像目录"));
#endif
    }

    if (m_stageCaptureSecondaryButton) {
    const bool anySamples = !shots.isEmpty() || inputImageCount > 0;
    m_stageCaptureSecondaryButton->setVisible(true);
    m_stageCaptureSecondaryButton->setText(tr("覆盖概览"));
    m_stageCaptureSecondaryButton->setEnabled(anySamples);
    m_stageCaptureSecondaryButton->setToolTip(anySamples ? QString() : tr("导入或采集样本后即可查看九宫格统计。"));
    }
}

void MainWindow::refreshCaptureTabs()
{
    if (!m_captureTabs) {
        return;
    }

    if (m_datasetPage) {
        int datasetIndex = m_captureTabs->indexOf(m_datasetPage);
        if (datasetIndex < 0) {
            datasetIndex = m_captureTabs->insertTab(0, m_datasetPage, tr("Samples"));
        } else {
            m_captureTabs->setTabText(datasetIndex, tr("Samples"));
        }
        const bool requiresSelection = (m_captureTabs->currentIndex() == -1);
        if (requiresSelection) {
            m_captureTabs->setCurrentIndex(datasetIndex);
        }
    }
}

void MainWindow::reconcileStageStates()
{
    if (!m_session) {
        return;
    }

    const auto metadata = m_session->metadata();
    const bool localWorkflow = (metadata.dataSource == ProjectSession::DataSource::LocalDataset);

    auto promoteStage = [&](ProjectSession::ProjectStage stage, ProjectSession::StageStatus desiredStatus) {
        auto state = m_session->stageState(stage);
        if (state.status == desiredStatus) {
            return;
        }
        if (desiredStatus == ProjectSession::StageStatus::InProgress && state.status != ProjectSession::StageStatus::NotStarted) {
            return;
        }
        if (state.status == ProjectSession::StageStatus::Completed && desiredStatus != ProjectSession::StageStatus::Completed) {
            return;
        }
        state.status = desiredStatus;
        m_session->updateStageState(stage, state);
    };

    if (localWorkflow) {
        promoteStage(ProjectSession::ProjectStage::CameraTuning, ProjectSession::StageStatus::Completed);
    } else if (!m_session->tuningSnapshots().isEmpty()) {
        promoteStage(ProjectSession::ProjectStage::CameraTuning, ProjectSession::StageStatus::InProgress);
    }

    const bool hasShots = !m_session->calibrationShots().isEmpty();
    int imageCount = 0;
    if (localWorkflow) {
        imageCount = countImageFiles(m_session->calibrationCaptureDir().absolutePath());
    } else {
        imageCount = countImageFiles(m_session->liveCacheDir().absolutePath());
    }
    if (hasShots || imageCount > 0) {
        promoteStage(ProjectSession::ProjectStage::CalibrationCapture, ProjectSession::StageStatus::InProgress);
    }

    if (!m_session->laserFrames().isEmpty()) {
        promoteStage(ProjectSession::ProjectStage::LaserCalibration, ProjectSession::StageStatus::InProgress);
    }
}

void MainWindow::handleStageCapturePrimaryAction()
{
    if (!m_session) {
        QMessageBox::information(this, tr("无项目"), tr("请先创建或打开项目。"));
        return;
    }

#if MYCALIB_HAVE_CONNECTED_CAMERA
    if (m_session->metadata().dataSource == ProjectSession::DataSource::ConnectedCamera) {
        if (m_stageNavigator && m_stageTuningItem) {
            m_stageNavigator->setCurrentItem(m_stageTuningItem);
        }
        if (m_stageStack && m_stageTuningPage) {
            m_stageStack->setCurrentWidget(m_stageTuningPage);
        }
        if (m_capturePlanToggle && !m_capturePlanToggle->isChecked()) {
            QSignalBlocker blocker(m_capturePlanToggle);
            m_capturePlanToggle->setChecked(true);
            handleCapturePlanToggled(true);
        }

        const CaptureCoverageStats stats = computeCaptureCoverage(m_session->calibrationShots());
        std::optional<std::pair<int, int>> nextCell;
        for (int r = 0; r < kCaptureGridRows && !nextCell; ++r) {
            for (int c = 0; c < kCaptureGridCols; ++c) {
                if (stats.cellTotals[r][c] < kCaptureMinimumPerCell) {
                    nextCell = std::make_pair(r, c);
                    break;
                }
            }
        }
        if (!nextCell) {
            for (int r = 0; r < kCaptureGridRows && !nextCell; ++r) {
                for (int c = 0; c < kCaptureGridCols; ++c) {
                    if (stats.cellTotals[r][c] < kCaptureTargetPerCell) {
                        nextCell = std::make_pair(r, c);
                        break;
                    }
                }
            }
        }
        if (!nextCell) {
            nextCell = std::make_pair(1, 1);
        }

        if (nextCell && m_captureGridGroup) {
            const int id = encodeGridId(nextCell->first, nextCell->second);
            if (auto *button = m_captureGridGroup->button(id)) {
                QSignalBlocker blocker(m_captureGridGroup);
                button->setChecked(true);
                handleCaptureGridSelection(id);
            }
        }

        if (m_capturePoseGroup && !m_capturePoseGroup->checkedButton()) {
            if (auto *button = m_capturePoseGroup->button(0)) {
                QSignalBlocker blocker(m_capturePoseGroup);
                button->setChecked(true);
                handleCapturePoseSelection(0);
            }
        }

        refreshCapturePlanUi();
        updateCaptureOverlay();
        return;
    }
#endif

    openInputLocation();
    if (m_stageNavigator && m_stageCaptureItem) {
        m_stageNavigator->setCurrentItem(m_stageCaptureItem);
    }
    refreshCaptureTabs();
    if (m_captureTabs && m_datasetPage) {
        const int index = m_captureTabs->indexOf(m_datasetPage);
        if (index >= 0) {
            m_captureTabs->setCurrentIndex(index);
        }
    }
}

void MainWindow::showCaptureCoverageDialog()
{
    if (!m_session) {
        QMessageBox::information(this, tr("无项目"), tr("请先创建或打开项目。"));
        return;
    }

    const QVector<ProjectSession::CaptureShot> shots = aggregateCoverageShots();

    const CaptureCoverageStats stats = computeCaptureCoverage(shots);
    const int targetPerCell = kCaptureTargetPerCell;
    const int minimumPerCell = kCaptureMinimumPerCell;
    const int totalTarget = kCaptureGridRows * kCaptureGridCols * targetPerCell;
    const int imageCount = countImageFiles(m_inputDir);

    int completedCells = 0;
    QStringList cellLines;
    for (int r = 0; r < kCaptureGridRows; ++r) {
        for (int c = 0; c < kCaptureGridCols; ++c) {
            const int count = stats.cellTotals[r][c];
            QString line = tr("第%1行 · 第%2列：%3/%4（最低 %5）")
                               .arg(r + 1)
                               .arg(c + 1)
                               .arg(count)
                               .arg(targetPerCell)
                               .arg(minimumPerCell);
            if (count < minimumPerCell) {
                line = tr("%1 · 缺%2").arg(line).arg(minimumPerCell - count);
                line = QStringLiteral("<span style='color:#ffb38a;font-weight:600;'>%1</span>").arg(line);
            } else {
                ++completedCells;
                if (count < targetPerCell) {
                    line = tr("%1 · 建议再补%2")
                               .arg(line)
                               .arg(targetPerCell - count);
                    line = QStringLiteral("<span style='color:#ffd27f;font-weight:600;'>%1</span>").arg(line);
                }
            }
            cellLines << line;
        }
    }

    QStringList poseLines;
    for (int i = 0; i < static_cast<int>(std::size(kPoseDescriptors)); ++i) {
        QString line = tr("%1：%2 张")
                           .arg(tr(kPoseDescriptors[i].display))
                           .arg(stats.poseTotals[i]);
        if (stats.poseTotals[i] == 0) {
            line = QStringLiteral("<span style='color:#ffb38a;font-weight:600;'>%1</span>").arg(line);
        }
        poseLines << line;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(tr("阶段二覆盖概览"));
    dialog.setModal(true);
    dialog.setMinimumSize(440, 380);

    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(26, 24, 26, 20);
    layout->setSpacing(16);

    QString summary;
    if (stats.totalShots == 0) {
        summary = tr("尚未记录九宫格样本。项目当前包含 %1 张图像，可继续导入或运行标定以生成覆盖统计。")
                      .arg(imageCount);
    } else {
        if (m_session->metadata().dataSource == ProjectSession::DataSource::ConnectedCamera) {
            summary = tr("已记录 %1 / %2 张样本，%3 / %4 个格位达到最低 %5 张/格目标。")
                          .arg(stats.totalShots)
                          .arg(totalTarget)
                          .arg(completedCells)
                          .arg(kCaptureGridRows * kCaptureGridCols)
                          .arg(minimumPerCell);
        } else {
            summary = tr("覆盖映射：共 %1 张样本（%2 / %3 个格位达到最低 %4 张/格，基于标定结果）。项目图像总数：%5 张。")
                          .arg(stats.totalShots)
                          .arg(completedCells)
                          .arg(kCaptureGridRows * kCaptureGridCols)
                          .arg(minimumPerCell)
                          .arg(imageCount);
        }
    }
    auto *summaryLabel = new QLabel(summary, &dialog);
    summaryLabel->setWordWrap(true);
    layout->addWidget(summaryLabel);

    auto *cellLabel = new QLabel(QStringLiteral("<ul><li>%1</li></ul>")
                                     .arg(cellLines.join(QStringLiteral("</li><li>"))),
                                 &dialog);
    cellLabel->setTextFormat(Qt::RichText);
    cellLabel->setWordWrap(true);
    layout->addWidget(cellLabel);

    auto *poseLabel = new QLabel(QStringLiteral("<ul><li>%1</li></ul>")
                                     .arg(poseLines.join(QStringLiteral("</li><li>"))),
                                 &dialog);
    poseLabel->setTextFormat(Qt::RichText);
    poseLabel->setWordWrap(true);
    layout->addWidget(poseLabel);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("关闭"));

    auto *openDatasetButton = buttons->addButton(tr("打开图像目录"), QDialogButtonBox::ActionRole);
    connect(openDatasetButton, &QPushButton::clicked, this, &MainWindow::openInputLocation);

#if MYCALIB_HAVE_CONNECTED_CAMERA
    if (m_session->metadata().dataSource == ProjectSession::DataSource::ConnectedCamera) {
        auto *plannerButton = buttons->addButton(tr("打开相机采集器"), QDialogButtonBox::ActionRole);
        connect(plannerButton, &QPushButton::clicked, &dialog, [this, &dialog]() {
            dialog.accept();
            handleStageCapturePrimaryAction();
        });
    }
#endif

    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);

    dialog.exec();
}

void MainWindow::showTuningTimeline()
{
    if (m_stageNavigator && m_stageTuningItem) {
        m_stageNavigator->setCurrentItem(m_stageTuningItem);
    }
    if (m_tuningTimeline) {
        m_tuningTimeline->setFocus();
    }
}

int MainWindow::countImageFiles(const QString &directory) const
{
    if (directory.isEmpty()) {
        return 0;
    }
    QDir dir(directory);
    if (!dir.exists()) {
        return 0;
    }
    const QStringList filters = {
        QStringLiteral("*.png"), QStringLiteral("*.jpg"), QStringLiteral("*.jpeg"),
        QStringLiteral("*.bmp"), QStringLiteral("*.tif"), QStringLiteral("*.tiff")
    };
    return dir.entryList(filters, QDir::Files | QDir::Readable).size();
}

bool MainWindow::hasInputImages() const
{
    return countImageFiles(m_inputDir) > 0;
}

void MainWindow::updateInputSummary()
{
    if (!m_inputStatusLabel) {
        return;
    }
    const int count = countImageFiles(m_inputDir);
    const bool cameraMode = (m_activeSource == ProjectSession::DataSource::ConnectedCamera);

    QString text;
    QString style;

    if (cameraMode) {
        if (count > 0) {
            text = tr("%n captured frame(s) available in the live cache.", "", count);
            style = QStringLiteral("color:#7de0b2;font-weight:600;");
        } else {
            text = tr("No captured frames yet. Connect the camera and capture at least one frame to enable calibration.");
            style = QStringLiteral("color:#ffb38a;font-weight:600;");
        }
    } else {
        if (count > 0) {
            text = tr("%n image(s) ready in the project dataset.", "", count);
            style = QStringLiteral("color:#7de0b2;font-weight:600;");
        } else {
            text = tr("Dataset is empty. Import or add images into the project folder before calibration.");
            style = QStringLiteral("color:#ffb38a;font-weight:600;");
        }
    }

    m_inputStatusLabel->setText(text);
    m_inputStatusLabel->setStyleSheet(style);
    m_lastInputImageCount = count;
}

void MainWindow::updateRunAvailability()
{
    if (!m_actionRun) {
        return;
    }
    const bool imagesReady = hasInputImages();
    const bool enable = !m_running && imagesReady;
    m_actionRun->setEnabled(enable);

    QString tooltip;
    if (m_running) {
        tooltip = tr("Calibration is currently running.");
    } else if (!imagesReady) {
        if (m_activeSource == ProjectSession::DataSource::ConnectedCamera) {
            tooltip = tr("Capture at least one frame before running calibration.");
        } else {
            tooltip = tr("Import calibration images before running calibration.");
        }
    } else {
        tooltip = tr("Run the calibration pipeline.");
    }
    m_actionRun->setToolTip(tooltip);
}

#if MYCALIB_HAVE_CONNECTED_CAMERA
void MainWindow::updateCameraSnapshotTarget()
{
    if (!m_cameraWindow) {
        return;
    }

    QString targetDir = m_inputDir;
    if (targetDir.isEmpty()) {

        targetDir = defaultInputDirectory();
    }
    if (targetDir.isEmpty()) {
        return;
    }

    QDir dir(targetDir);
    if (!dir.exists()) {
        dir.mkpath(QStringLiteral("."));
    }

    const QString snapshotDir = dir.absolutePath();
    m_cameraWindow->setSnapshotDirectory(snapshotDir);

    QString prefix = QStringLiteral("capture");
    if (m_session && !m_session->metadata().projectId.isEmpty()) {
        prefix = m_session->metadata().projectId.left(6);
    }
    if (prefix.isEmpty()) {
        prefix = QStringLiteral("capture");
    }
    m_cameraSnapshotPrefix = prefix;
    m_cameraSnapshotSequence = 0;
    m_cameraWindow->setSnapshotNamingPrefix(prefix);
    m_cameraWindow->setSnapshotPathProvider([this, snapshotDir]() -> QString {
        QDir dir(snapshotDir);
        if (!dir.exists()) {
            dir.mkpath(QStringLiteral("."));
        }

        const QString basePrefix = m_cameraSnapshotPrefix.isEmpty() ? QStringLiteral("capture") : m_cameraSnapshotPrefix;
        const QString timestamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmsszzz"));
        const int seqBase = ++m_cameraSnapshotSequence;

        QString candidate;
        int guard = 0;
        do {
            const int sequence = seqBase + guard;
            const QString fileName = QStringLiteral("%1_%2_%3.png")
                                         .arg(basePrefix,
                                              timestamp,
                                              QString::number(sequence).rightJustified(3, QLatin1Char('0')));
            candidate = dir.filePath(fileName);
            ++guard;
        } while (QFileInfo::exists(candidate) && guard < 1000);

        return candidate;
    });
}

void MainWindow::handleCameraSnapshotCaptured(const QString &filePath)
{
    if (filePath.isEmpty()) {
        return;
    }

    QString finalPath = filePath;

    if (!m_inputDir.isEmpty()) {
        QDir inputDir(m_inputDir);
        QFileInfo captured(filePath);
        const QString captureDir = QDir::cleanPath(captured.absolutePath());
        const QString targetDir = QDir::cleanPath(inputDir.absolutePath());
        if (captured.exists() && captured.isFile() && captureDir != targetDir) {
            const QString destination = inputDir.filePath(captured.fileName());
            if (!QFileInfo::exists(destination)) {
                if (QFile::copy(captured.absoluteFilePath(), destination)) {
                    finalPath = destination;
                }
            } else {
                finalPath = destination;
            }
        }
    }

    QString displayPath = QDir::toNativeSeparators(finalPath);
    if (m_session) {
        const QString relative = m_session->relativePath(finalPath);
        if (!relative.isEmpty() && !relative.startsWith(QStringLiteral(".."))) {
            displayPath = relative;
        }
    }

    const bool planActive = isCapturePlanActive();
    bool discardSnapshot = false;
    bool recordedCalibrationShot = false;

    QVariantMap metrics;
    if (m_cameraWindow) {
        metrics = m_cameraWindow->currentSnapshotMetrics();
    }

    if (planActive && m_session) {
        const int row = std::clamp(m_captureSelectedRow, 0, kCaptureGridRows - 1);
        const int col = std::clamp(m_captureSelectedCol, 0, kCaptureGridCols - 1);
        const auto pose = m_captureSelectedPose;
        const auto &cell = m_capturePlanState[row][col];

        if (cell.total >= kCaptureTargetPerCell) {
            const QMessageBox::StandardButton choice = QMessageBox::question(
                this,
                tr("超过建议数量"),
                tr("九宫格 %1 已记录 %2 张样本。仍要继续记录吗？")
                    .arg(captureCellDisplayName(row, col))
                    .arg(cell.total),
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::Yes);
            if (choice == QMessageBox::No) {
                discardSnapshot = true;
            }
        }

        if (!discardSnapshot) {
            ensureCalibrationStageStarted();
            QVariantMap shotMetadata = metrics;
            shotMetadata.insert(QStringLiteral("grid_row"), row);
            shotMetadata.insert(QStringLiteral("grid_col"), col);
            shotMetadata.insert(QStringLiteral("pose_label"), toString(pose));
            shotMetadata.insert(QStringLiteral("capture_stage"), QStringLiteral("calibration"));

            const ProjectSession::CaptureShot shot = m_session->addCalibrationShot(row, col, pose, finalPath, shotMetadata);
            if (shot.id.isNull()) {
                QMessageBox::warning(this,
                                     tr("记录失败"),
                                     tr("无法将样本写入项目，请检查目录权限。"));
            } else {
                recordedCalibrationShot = true;
                auto &state = m_capturePlanState[row][col];
                ++state.total;
                const int poseIdx = capturePoseIndex(pose);
                if (poseIdx >= 0 && poseIdx < static_cast<int>(state.poseCounts.size())) {
                    ++state.poseCounts[poseIdx];
                }

                QString planPath = shot.relativePath.isEmpty() ? displayPath : shot.relativePath;
                refreshCapturePlanUi();
                if (m_logView) {
                    m_logView->append(tr("记录阶段二样本：%1 · %2 → %3")
                                          .arg(captureCellDisplayName(row, col),
                                               capturePoseDisplayName(pose),
                                               planPath));
                }
            }
        }
    }

    if (discardSnapshot) {
        QFile::remove(finalPath);
        if (m_logView) {
            m_logView->append(tr("已放弃快照 %1").arg(displayPath));
        }
        return;
    }

    if (m_logView) {
        m_logView->append(tr("Captured snapshot saved to %1").arg(displayPath));
    }

    if (recordedCalibrationShot) {
        updateInputSummary();
        updateRunAvailability();
        syncInputWatcher();
        return;
    }

    ProjectSession::TuningSnapshot recordedSnapshot;

    if (m_session) {
        ensureCameraTuningStageStarted();
        recordedSnapshot = m_session->recordTuningSnapshot(finalPath, metrics);
        if (!recordedSnapshot.id.isNull()) {
            if (auto *item = appendTuningSnapshotRow(recordedSnapshot)) {
                m_tuningTimeline->sortItems(0, Qt::DescendingOrder);
                m_tuningTimeline->setCurrentItem(item);
                m_tuningTimeline->scrollToItem(item);
            }
            if (m_logView) {
                const QString summary = metrics.value(QStringLiteral("exposure")).toString();
                m_logView->append(summary.isEmpty()
                                      ? tr("Logged tuning snapshot with current parameters.")
                                      : tr("Logged tuning snapshot (exposure %1)." ).arg(summary));
            }
        }
        updateTuningStageUi();
    }

    updateInputSummary();
    updateRunAvailability();
    syncInputWatcher();
    updateStageNavigator();
}
#else

void MainWindow::connectCamera()
{
    QMessageBox::information(this,
                             tr("Live capture"),
                             tr("This build of Calibration Studio was compiled without connected camera support."));
}

void MainWindow::disconnectCamera()
{
    QMessageBox::information(this,
                             tr("Live capture"),
                             tr("This build of Calibration Studio was compiled without connected camera support."));
}

void MainWindow::startCameraStream()
{
    QMessageBox::information(this,
                             tr("Live capture"),
                             tr("This build of Calibration Studio was compiled without connected camera support."));
}

void MainWindow::stopCameraStream()
{
    QMessageBox::information(this,
                             tr("Live capture"),
                             tr("This build of Calibration Studio was compiled without connected camera support."));
}

void MainWindow::captureFromCamera()
{
    QMessageBox::information(this,
                             tr("Live capture"),
                             tr("This build of Calibration Studio was compiled without connected camera support."));
}

void MainWindow::refreshCameraDevices()
{
    QMessageBox::information(this,
                             tr("Live capture"),
                             tr("This build of Calibration Studio was compiled without connected camera support."));
}

void MainWindow::handleCameraConnectionChanged(bool connected, const QString &id, const QString &model)
{
    Q_UNUSED(connected);
    Q_UNUSED(id);
    Q_UNUSED(model);
}

void MainWindow::handleCameraStreamingChanged(bool streaming)
{
    Q_UNUSED(streaming);
}

void MainWindow::syncCameraActions()
{
}

void MainWindow::updateCameraSnapshotTarget()
{
}

void MainWindow::handleCameraSnapshotCaptured(const QString &filePath)
{
    Q_UNUSED(filePath);
}

#endif

void MainWindow::runCalibration()
{
    if (m_running) {
        return;
    }
    if (m_inputDir.isEmpty()) {
        QMessageBox::warning(this, tr("Missing input"), tr("Please import or capture images first."));
        return;
    }
    if (!QDir(m_inputDir).exists()) {
        QMessageBox::warning(this, tr("Invalid path"), tr("The input directory does not exist."));
        return;
    }

#if !MYCALIB_HAVE_CONNECTED_CAMERA
    if (m_session && m_session->metadata().dataSource == ProjectSession::DataSource::ConnectedCamera) {
        QMessageBox::information(this,
                                 tr("Live capture"),
                                 tr("This build of Calibration Studio was compiled without connected camera support."));
        return;
    }
#else
    if (m_session && m_session->metadata().dataSource == ProjectSession::DataSource::ConnectedCamera) {
        QDir inputDir(m_inputDir);
        const QStringList capturedImages = inputDir.entryList({QStringLiteral("*.png"), QStringLiteral("*.jpg"), QStringLiteral("*.jpeg"), QStringLiteral("*.bmp"), QStringLiteral("*.tif"), QStringLiteral("*.tiff")},
                                                              QDir::Files);
        if (capturedImages.isEmpty()) {
            QMessageBox::information(this,
                                     tr("Live capture"),
                                     tr("Capture at least one frame before running calibration."));
            return;
        }
        if (m_logView) {
            m_logView->append(tr("Running calibration with %1 live-captured frame(s).").arg(capturedImages.size()));
        }
    }
#endif

    CalibrationEngine::Settings settings;
    settings.boardSpec.smallDiameterMm = 5.0;
    settings.boardSpec.centerSpacingMm = 25.0;
    // board layout (7x6 with centre gap) follows the Python reference implementation

    resetUi();

    refreshState(true);
    m_progressBar->setValue(0);
    m_logView->append(QStringLiteral("[%1] Starting calibration ...")
                           .arg(QDateTime::currentDateTime().toString("hh:mm:ss")));

    const QString resolvedOutput = CalibrationEngine::resolveOutputDirectory(m_outputDir);
    m_outputDir = resolvedOutput;
    if (m_outputPathEdit) {
        m_outputPathEdit->setText(QDir::toNativeSeparators(resolvedOutput));
    }

    m_engine->run(m_inputDir, settings, resolvedOutput);
}

void MainWindow::resetUi()
{
    if (m_running) {
        return;
    }
    if (m_evaluationDialog) {
        m_evaluationDialog->close();
        m_evaluationDialog->deleteLater();
        m_evaluationDialog.clear();
    }
    cleanupDebugArtifacts(m_lastOutput);
    m_detectionTree->clear();
    m_logView->clear();
    m_lastLogKey.clear();
    m_lastLogHtml.clear();
    m_lastLogRepeat = 0;
    auto resetMetric = [](QLabel *label) {
        if (label) {
            label->setText(QStringLiteral("--"));
        }
    };
    resetMetric(m_metricTotalImages);
    resetMetric(m_metricKeptImages);
    resetMetric(m_metricRemovedImages);
    resetMetric(m_metricRms);
    resetMetric(m_metricMeanPx);
    resetMetric(m_metricMedianPx);
    resetMetric(m_metricP95Px);
    resetMetric(m_metricMaxPx);
    resetMetric(m_metricMeanResidualMm);
    resetMetric(m_metricMeanResidualPercent);

    if (m_heatmapBoard) {
        m_heatmapBoard->clear();
    }
    if (m_heatmapPixel) {
        m_heatmapPixel->clear();
    }
    if (m_scatterView) {
        m_scatterView->clear();
    }
    if (m_detectionPreview) {
        m_detectionPreview->clear();
    }
    if (m_distortionMap) {
        m_distortionMap->clear();
    }
    if (m_poseView) {
        m_poseView->deleteLater();
        m_poseView = nullptr;
    }
    if (m_poseStack && m_posePlaceholder) {
        m_poseStack->setCurrentWidget(m_posePlaceholder);
    }
    updateDetectionDetailPanel(nullptr);
    if (m_stageNavigator && m_stageOverviewItem) {
        m_stageNavigator->setCurrentItem(m_stageOverviewItem);
    }
    m_lastOutput = CalibrationOutput{};
    if (m_actionShowParameters) {
        m_actionShowParameters->setEnabled(false);
    }
    updateCaptureFeedback(m_lastOutput);
    m_datasetDerivedShots.clear();
    refreshCapturePlanFromSession();
}

void MainWindow::exportJson()
{
    if (!m_session) {
        QMessageBox::information(this, tr("No project"), tr("Create or open a project before exporting."));
        return;
    }

    persistProjectSummary(false);

    const QString defaultPath = m_session->exportsDir().absoluteFilePath(QStringLiteral("project_summary.json"));
    const QString targetPath = QFileDialog::getSaveFileName(this,
                                                            tr("Export project summary"),
                                                            defaultPath,
                                                            tr("JSON (*.json)"));
    if (targetPath.isEmpty()) {
        return;
    }

    QString error;
    if (writeProjectSummary(targetPath, &error)) {
        if (m_logView) {
            m_logView->append(tr("Exported project summary to %1").arg(QDir::toNativeSeparators(targetPath)));
        }
    } else {
        QMessageBox::warning(this,
                             tr("Export failed"),
                             error.isEmpty() ? tr("Unable to write the summary file.") : error);
    }
}

void MainWindow::showParameters()
{
    if (!m_lastOutput.success) {
        QMessageBox::information(this, tr("No data"), tr("Run calibration before showing parameters."));
        return;
    }

    ParameterDialog dialog(m_lastOutput, this);
    dialog.exec();
}

void MainWindow::showEvaluationDialog()
{
    if (!m_lastOutput.success) {
        QMessageBox::information(this, tr("No data"), tr("Run calibration before evaluating images."));
        return;
    }

    BoardSpec spec;
    spec.smallDiameterMm = 5.0;
    spec.centerSpacingMm = 25.0;

    if (m_evaluationDialog) {
        m_evaluationDialog->close();
        m_evaluationDialog->deleteLater();
        m_evaluationDialog.clear();
    }

    m_evaluationDialog = new ImageEvaluationDialog(m_lastOutput, spec, this);
    m_evaluationDialog->show();
    m_evaluationDialog->raise();
    m_evaluationDialog->activateWindow();
}

void MainWindow::showAuthorInfo()
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("About Calibration Studio"));
    dialog.setModal(true);
    dialog.setMinimumSize(420, 320);

    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(28, 24, 28, 20);
    layout->setSpacing(18);

    auto *title = new QLabel(tr("Calibration Studio"), &dialog);
    QFont titleFont = title->font();
    titleFont.setPointSizeF(titleFont.pointSizeF() + 6.0);
    titleFont.setBold(true);
    title->setFont(titleFont);
    layout->addWidget(title);

    auto *subtitle = new QLabel(tr("高质量标定与可视化的桌面工具"), &dialog);
    QFont subtitleFont = subtitle->font();
    subtitleFont.setPointSizeF(subtitleFont.pointSizeF() + 1.0);
    subtitle->setFont(subtitleFont);
    subtitle->setStyleSheet(QStringLiteral("color: #6c7fb5;"));
    layout->addWidget(subtitle);

    auto *separator = new QFrame(&dialog);
    separator->setFrameShape(QFrame::HLine);
    separator->setFrameShadow(QFrame::Sunken);
    separator->setStyleSheet(QStringLiteral("color: rgba(120, 135, 170, 120);"));
    layout->addWidget(separator);

    auto *description = new QLabel(&dialog);
    description->setWordWrap(true);
    description->setText(tr("Calibration Studio 将检测、求解与分析流程集成到一个现代化界面中，帮助团队快速获得可靠的内参并生成高质量可视化。"));
    layout->addWidget(description);

    auto *features = new QLabel(&dialog);
    features->setTextFormat(Qt::RichText);
    features->setWordWrap(true);
    features->setText(tr("<ul>"
                         "<li>⟡ 自动化的标定流程与参数导出</li>"
                         "<li>⟡ 交互式残差与热力图分析</li>"
                         "<li>⟡ 为论文排版优化的可视化输出</li>"
                         "</ul>"));
    layout->addWidget(features);

    auto *contactHeader = new QLabel(tr("作者"), &dialog);
    QFont contactFont = contactHeader->font();
    contactFont.setBold(true);
    contactHeader->setFont(contactFont);
    layout->addWidget(contactHeader);

    auto *contact = new QLabel(&dialog);
    contact->setTextFormat(Qt::RichText);
    contact->setOpenExternalLinks(true);
    contact->setWordWrap(true);
    contact->setText(tr("<p><strong>Jiaming Zhang</strong><br>"
                        "Email: <a href=\"mailto:zhan2374@msu.edu\">zhan2374@msu.edu</a><br>"
                        "Website: <a href=\"https://charmingzh.github.io\">https://charmingzh.github.io</a></p>"));
    layout->addWidget(contact);

    auto *license = new QLabel(tr("© 2024 Calibration Studio. All rights reserved."), &dialog);
    QFont licenseFont = license->font();
    licenseFont.setPointSizeF(licenseFont.pointSizeF() - 1.0);
    license->setFont(licenseFont);
    license->setStyleSheet(QStringLiteral("color: #8a93aa;"));
    layout->addWidget(license);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Close"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);

    dialog.exec();
}

void MainWindow::handleProgress(int processed, int total)
{
    if (total <= 0) {
        return;
    }
    const int value = static_cast<int>(static_cast<double>(processed) / total * 100.0);
    m_progressBar->setValue(value);
}

void MainWindow::handleStatus(const QString &message)
{
    m_logView->append(message);
}

void MainWindow::handleFinished(const CalibrationOutput &output)
{
    m_running = false;
    m_lastOutput = output;
    materializeDebugArtifacts(m_lastOutput);
    if (m_evaluationDialog) {
        m_evaluationDialog->close();
        m_evaluationDialog->deleteLater();
        m_evaluationDialog.clear();
    }
    refreshState(false);

    if (m_session) {
        auto captureState = m_session->stageState(ProjectSession::ProjectStage::CalibrationCapture);
        if (captureState.status != ProjectSession::StageStatus::Completed) {
            captureState.status = ProjectSession::StageStatus::Completed;
            m_session->updateStageState(ProjectSession::ProjectStage::CalibrationCapture, captureState);
        }

        const auto metadata = m_session->metadata();
        if (metadata.dataSource == ProjectSession::DataSource::LocalDataset || !m_session->tuningSnapshots().isEmpty()) {
            auto tuningState = m_session->stageState(ProjectSession::ProjectStage::CameraTuning);
            if (tuningState.status != ProjectSession::StageStatus::Completed) {
                tuningState.status = ProjectSession::StageStatus::Completed;
                m_session->updateStageState(ProjectSession::ProjectStage::CameraTuning, tuningState);
            }
        }
    }

    updateSummaryPanel(output);
    populateDetectionTree(output);
    updateDerivedCoverageFromOutput(output);
    refreshCapturePlanFromSession();
    showHeatmaps(output);
    ensurePoseView();
    if (m_poseView) {
        m_poseView->setDetections(output.allDetections);
    }
    if (m_stageNavigator && m_stageOverviewItem) {
        m_stageNavigator->setCurrentItem(m_stageOverviewItem);
    }
    m_logView->append(tr("Calibration complete."));
    const QString figureDir = QDir(m_outputDir).absoluteFilePath(QStringLiteral("paper_figures"));
    m_logView->append(tr("Paper-ready figures saved to %1").arg(QDir::toNativeSeparators(figureDir)));
    if (m_logView) {
        for (const QString &line : output.detectionDiagnostics) {
            m_logView->append(QStringLiteral("  · %1").arg(line));
        }
        if (!output.removalDiagnostics.isEmpty()) {
            m_logView->append(QStringLiteral("  · %1").arg(tr("鲁棒过滤统计：")));
            for (const QString &line : output.removalDiagnostics) {
                m_logView->append(QStringLiteral("    - %1").arg(line));
            }
        }
    }
    reconcileStageStates();
    persistCalibrationSnapshot(m_lastOutput);
    persistProjectSummary(true);
    updateStageNavigator();
}

void MainWindow::handleFailed(const QString &reason, const CalibrationOutput &details)
{
    m_running = false;
    refreshState(false);
    m_lastOutput = details;

    QString trimmedReason = reason.trimmed();
    if (trimmedReason.isEmpty()) {
        trimmedReason = tr("Calibration failed.");
    }

    QStringList diagnosticLines;
    if (!details.failureStage.isEmpty()) {
        diagnosticLines << tr("阶段：%1").arg(details.failureStage);
    }
    QStringList detailList = details.failureDetails;
    if (detailList.isEmpty()) {
        detailList = details.detectionDiagnostics;
    }
    if (!detailList.isEmpty()) {
        diagnosticLines.append(detailList);
    }

    QMessageBox box(this);
    box.setIcon(QMessageBox::Critical);
    box.setWindowTitle(tr("Calibration failed"));
    box.setText(trimmedReason);
    if (!diagnosticLines.isEmpty()) {
        box.setInformativeText(diagnosticLines.join(QStringLiteral("\n")));
    }
    box.setStandardButtons(QMessageBox::Ok);
    box.exec();

    if (m_logView) {
        m_logView->append(QStringLiteral("Failed: %1").arg(trimmedReason));
        for (const QString &line : diagnosticLines) {
            m_logView->append(QStringLiteral("  · %1").arg(line));
        }
    }

    if (m_captureFeedbackBox) {
        auto setLabel = [](QLabel *label, const QString &text) {
            if (label) {
                label->setText(text);
            }
        };
        QString summary = tr("标定失败：%1").arg(trimmedReason);
        if (!details.failureStage.isEmpty()) {
            summary += tr("（阶段：%1）").arg(details.failureStage);
        }
        setLabel(m_captureFeedbackSummary, summary);

        if (!detailList.isEmpty()) {
            QString html = QStringLiteral("<ul><li>%1</li></ul>")
                               .arg(detailList.join(QStringLiteral("</li><li>")));
            setLabel(m_captureFeedbackPose, html);
        } else {
            setLabel(m_captureFeedbackPose, QString());
        }
        setLabel(m_captureFeedbackActions, QString());
        m_captureFeedbackBox->setVisible(true);
    }

    persistProjectSummary(false);
    updateStageNavigator();
}

void MainWindow::refreshState(bool running)
{
    m_running = running;
    const bool allowLocalActions = !running && m_activeSource == ProjectSession::DataSource::LocalDataset;
    if (m_actionImportImages) {
        m_actionImportImages->setEnabled(allowLocalActions);
    }
    if (m_importButton) {
        m_importButton->setEnabled(allowLocalActions);
    }
    if (m_actionExport) {
        m_actionExport->setEnabled(!running && m_lastOutput.success);
    }
    if (m_actionShowParameters) {
        m_actionShowParameters->setEnabled(!running && m_lastOutput.success);
    }
    if (m_actionEvaluate) {
        m_actionEvaluate->setEnabled(!running && m_lastOutput.success);
    }
    m_actionReset->setEnabled(!running);
    updateRunAvailability();
}

void MainWindow::updateSummaryPanel(const CalibrationOutput &output)
{
    auto setLabelText = [](QLabel *label, const QString &text) {
        if (label) {
            label->setText(text);
        }
    };

    const int total = static_cast<int>(output.allDetections.size());
    const int kept = static_cast<int>(output.keptDetections.size());
    const int removed = static_cast<int>(output.removedDetections.size());

    setLabelText(m_metricTotalImages, QString::number(total));
    setLabelText(m_metricKeptImages, QString::number(kept));
    setLabelText(m_metricRemovedImages, QString::number(removed));

    auto formatNumber = [](double value, int precision = 3) {
        return QString::number(value, 'f', precision);
    };

    setLabelText(m_metricRms, formatNumber(output.metrics.rms));
    setLabelText(m_metricMeanPx, formatNumber(output.metrics.meanErrorPx));
    setLabelText(m_metricMedianPx, formatNumber(output.metrics.medianErrorPx));
    setLabelText(m_metricP95Px, formatNumber(output.metrics.p95ErrorPx));
    setLabelText(m_metricMaxPx, formatNumber(output.metrics.maxErrorPx));

    auto formatVec = [](const cv::Vec3d &vec, int precision) {
        return QStringLiteral("(%1, %2, %3)")
            .arg(vec[0], 0, 'f', precision)
            .arg(vec[1], 0, 'f', precision)
            .arg(vec[2], 0, 'f', precision);
    };

    setLabelText(m_metricMeanResidualMm, formatVec(output.metrics.meanResidualMm, 3));
    setLabelText(m_metricMeanResidualPercent, formatVec(output.metrics.meanResidualPercent, 3));
}

void MainWindow::populateDetectionTree(const CalibrationOutput &output)
{
    m_detectionTree->clear();
    auto addItem = [this](const DetectionResult &rec, bool kept) {
        auto *item = new QTreeWidgetItem(m_detectionTree);
        item->setText(0, QString::fromStdString(rec.name));
        item->setText(1, QString::number(rec.meanErrorPx(), 'f', 3));
        item->setText(2, QString::number(rec.maxErrorPx(), 'f', 3));
        item->setText(3, QString::number(std::abs(rec.meanResidualCameraMm[0]), 'f', 3));
        item->setText(4, QString::number(std::abs(rec.meanResidualCameraMm[1]), 'f', 3));
        item->setText(5, QString::number(std::abs(rec.meanResidualCameraMm[2]), 'f', 3));
        const QString status = kept ? tr("Kept")
                                    : tr("Removed (iter %1)").arg(rec.iterationRemoved);
        item->setText(6, status);
        item->setForeground(6, QBrush(kept ? QColor(102, 187, 106) : QColor(244, 143, 177)));
        item->setData(0, Qt::UserRole, QString::fromStdString(rec.name));
        return item;
    };

    QTreeWidgetItem *firstItem = nullptr;
    for (const auto &rec : output.keptDetections) {
        auto *item = addItem(rec, true);
        if (!firstItem) {
            firstItem = item;
        }
    }
    for (const auto &rec : output.removedDetections) {
        auto *item = addItem(rec, false);
        if (!firstItem) {
            firstItem = item;
        }
    }

    if (firstItem) {
        m_detectionTree->setCurrentItem(firstItem);
    } else {
        if (m_detectionPreview) {
            m_detectionPreview->clear();
        }
        updateDetectionDetailPanel(nullptr);
        if (m_poseView) {
            m_poseView->setActiveDetection(nullptr);
        }
    }

    updateCaptureFeedback(output);
}

void MainWindow::updateCaptureFeedback(const CalibrationOutput &output)
{
    if (!m_captureFeedbackBox) {
        return;
    }

    auto setLabel = [](QLabel *label, const QString &text) {
        if (label) {
            label->setText(text);
        }
    };

    if (!m_session) {
        setLabel(m_captureFeedbackSummary, tr("No project session loaded."));
        setLabel(m_captureFeedbackPose, QString());
        setLabel(m_captureFeedbackActions, QString());
        m_captureFeedbackBox->setVisible(true);
        return;
    }

    const QVector<ProjectSession::CaptureShot> shots = aggregateCoverageShots();
    const bool datasetMode = (m_session->metadata().dataSource == ProjectSession::DataSource::LocalDataset);

    if (shots.isEmpty()) {
        if (datasetMode) {
            setLabel(m_captureFeedbackSummary, tr("尚未生成九宫格统计。运行一次标定后即可根据检测结果自动映射覆盖。"));
        } else {
            setLabel(m_captureFeedbackSummary, tr("尚未记录阶段二样本。请在相机页启用九宫格采集并拍摄。"));
        }
        setLabel(m_captureFeedbackPose, QString());
        setLabel(m_captureFeedbackActions, QString());
        m_captureFeedbackBox->setVisible(true);
        return;
    }

    const bool hasDetections = !output.allDetections.empty();
    QSet<QString> keptNames;
    QSet<QString> removedNames;
    if (hasDetections) {
        for (const auto &rec : output.keptDetections) {
            keptNames.insert(QString::fromStdString(rec.name));
        }
        for (const auto &rec : output.removedDetections) {
            removedNames.insert(QString::fromStdString(rec.name));
        }
    }

    struct PoseStats {
        int captured {0};
        int kept {0};
        int removed {0};
    };

    std::array<PoseStats, std::size(kPoseDescriptors)> poseStats {};
    std::array<std::array<int, kCaptureGridCols>, kCaptureGridRows> capturedCells {};
    std::array<std::array<int, kCaptureGridCols>, kCaptureGridRows> keptCells {};
    int unmatchedShots = 0;

    auto resolveBaseName = [this](const ProjectSession::CaptureShot &shot) {
        QString baseName;
        if (!shot.relativePath.isEmpty()) {
            QFileInfo info(shot.relativePath);
            if (!info.isAbsolute() && m_session && !m_session->rootPath().isEmpty()) {
                info = QFileInfo(QDir(m_session->rootPath()).filePath(shot.relativePath));
            }
            baseName = info.completeBaseName();
            if (baseName.isEmpty()) {
                baseName = QFileInfo(info.fileName()).completeBaseName();
            }
        }
        if (baseName.isEmpty()) {
            baseName = shot.id.toString(QUuid::WithoutBraces);
        }
        return baseName;
    };

    int totalCaptured = 0;
    int totalKept = 0;
    int totalRemoved = 0;

    for (const auto &shot : shots) {
        const QString baseName = resolveBaseName(shot);
        const int poseIdx = capturePoseIndex(shot.pose);
        if (poseIdx < 0 || poseIdx >= static_cast<int>(poseStats.size())) {
            continue;
        }
        PoseStats &pose = poseStats[poseIdx];
        ++pose.captured;
        ++totalCaptured;

        const int row = std::clamp(shot.gridRow, 0, kCaptureGridRows - 1);
        const int col = std::clamp(shot.gridCol, 0, kCaptureGridCols - 1);
        capturedCells[row][col]++;

        if (hasDetections && keptNames.contains(baseName)) {
            ++pose.kept;
            ++totalKept;
            keptCells[row][col]++;
        } else if (hasDetections && removedNames.contains(baseName)) {
            ++pose.removed;
            ++totalRemoved;
        } else {
            ++unmatchedShots;
        }
    }

    QString summary;
    if (hasDetections) {
        if (datasetMode) {
            summary = tr("覆盖映射：共 %1 张（保留 %2，剔除 %3")
                          .arg(totalCaptured)
                          .arg(totalKept)
                          .arg(totalRemoved);
        } else {
            summary = tr("阶段二采集：共 %1 张（保留 %2，剔除 %3")
                          .arg(totalCaptured)
                          .arg(totalKept)
                          .arg(totalRemoved);
        }
        if (unmatchedShots > 0) {
            summary += tr("，待处理 %1").arg(unmatchedShots);
        }
        summary += QLatin1Char(')');
    } else {
        if (datasetMode) {
            summary = tr("覆盖映射：已记录 %1 张样本，等待标定输出以更新覆盖统计。")
                          .arg(totalCaptured);
        } else {
            summary = tr("阶段二采集：已记录 %1 张，尚未运行标定或尚无结果。")
                          .arg(totalCaptured);
        }
        if (unmatchedShots > 0) {
            summary += tr(" 其中 %1 张尚未参与标定。")
                           .arg(unmatchedShots);
        }
    }
    setLabel(m_captureFeedbackSummary, summary);

    QStringList poseItems;
    for (int i = 0; i < static_cast<int>(poseStats.size()); ++i) {
        const auto &stats = poseStats[i];
        const QString poseName = tr(kPoseDescriptors[i].display);
        QString line = tr("%1：采集 %2")
                           .arg(poseName)
                           .arg(stats.captured);
        if (hasDetections) {
            line += tr("，保留 %1，剔除 %2")
                        .arg(stats.kept)
                        .arg(stats.removed);
        }
        poseItems << line;
    }
    if (!poseItems.isEmpty()) {
        setLabel(m_captureFeedbackPose, QStringLiteral("<ul><li>%1</li></ul>")
                                               .arg(poseItems.join(QStringLiteral("</li><li>"))));
    } else {
        setLabel(m_captureFeedbackPose, QString());
    }

    QStringList actionBullets;
    QStringList missingCells;
    QStringList advisoryCells;
    if (hasDetections) {
        for (int r = 0; r < kCaptureGridRows; ++r) {
            for (int c = 0; c < kCaptureGridCols; ++c) {
                const int keptCount = keptCells[r][c];
                const QString cellName = tr("第%1行 · 第%2列").arg(r + 1).arg(c + 1);
                if (keptCount < kCaptureMinimumPerCell) {
                    missingCells << tr("%1 (%2/%3)")
                                      .arg(cellName)
                                      .arg(keptCount)
                                      .arg(kCaptureMinimumPerCell);
                } else if (keptCount < kCaptureTargetPerCell) {
                    advisoryCells << tr("%1 (%2/%3)")
                                       .arg(cellName)
                                       .arg(keptCount)
                                       .arg(kCaptureTargetPerCell);
                }
            }
        }
    }

    QStringList missingPoses;
    if (hasDetections) {
        for (int i = 0; i < static_cast<int>(poseStats.size()); ++i) {
            if (poseStats[i].captured > 0 && poseStats[i].kept == 0) {
                missingPoses << tr(kPoseDescriptors[i].display);
            }
        }
    }

    if (!missingCells.isEmpty()) {
        actionBullets << tr("补拍九宫格（低于最低覆盖）：%1").arg(missingCells.join(QStringLiteral("，")));
    }
    if (!advisoryCells.isEmpty()) {
        actionBullets << tr("补足建议覆盖：%1").arg(advisoryCells.join(QStringLiteral("，")));
    }
    if (!missingPoses.isEmpty()) {
        actionBullets << tr("补齐姿态：%1").arg(missingPoses.join(QStringLiteral("，")));
    }
    if (hasDetections && totalRemoved > 0) {
        actionBullets << tr("有 %1 张样本被剔除，可检查残差并考虑补拍。")
                              .arg(totalRemoved);
    }
    if (unmatchedShots > 0) {
        actionBullets << tr("%1 张样本尚未参与当前标定，可继续运行或补拍后重试。")
                              .arg(unmatchedShots);
    }

    if (actionBullets.isEmpty()) {
        setLabel(m_captureFeedbackActions, tr("覆盖良好，无需补拍。"));
    } else {
        setLabel(m_captureFeedbackActions, QStringLiteral("<ul><li>%1</li></ul>")
                                                   .arg(actionBullets.join(QStringLiteral("</li><li>"))));
    }

    m_captureFeedbackBox->setVisible(true);
    recomputeCellQuality();
}

void MainWindow::recomputeCellQuality()
{
    for (auto &row : m_cellQuality) {
        for (auto &cell : row) {
            cell.clear();
        }
    }

    struct ShotReference {
        const ProjectSession::CaptureShot *shot {nullptr};
        QString key;
        bool derived {false};
        bool matched {false};
    };

    const QVector<ProjectSession::CaptureShot> shots = aggregateCoverageShots();
    QVector<ShotReference> shotRefs;
    shotRefs.reserve(shots.size());
    for (const auto &shot : shots) {
        ShotReference ref;
        ref.shot = &shot;
        ref.derived = shot.metadata.value(QStringLiteral("coverage_source")).toString().compare(QStringLiteral("detection"), Qt::CaseInsensitive) == 0;
        ref.key = sampleKeyFromShot(shot, m_session);
        shotRefs.append(ref);
    }

    auto recordDetection = [&](const DetectionResult &det, bool kept) {
        if (det.name.empty()) {
            return;
        }
        const auto inferred = inferGridCellFromDetection(det);
        const int row = std::clamp(inferred.first, 0, kCaptureGridRows - 1);
        const int col = std::clamp(inferred.second, 0, kCaptureGridCols - 1);
        auto &cell = m_cellQuality[row][col];

        CellSampleInfo info;
        info.displayName = QString::fromStdString(det.name);
        info.key = sampleKeyFromDetection(det);
        info.kept = kept;
        info.meanErrorPx = det.meanErrorPx();
        info.maxErrorPx = det.maxErrorPx();
        const double residualMagnitude = std::sqrt(det.meanResidualCameraMm[0] * det.meanResidualCameraMm[0] +
                                                   det.meanResidualCameraMm[1] * det.meanResidualCameraMm[1] +
                                                   det.meanResidualCameraMm[2] * det.meanResidualCameraMm[2]);
        info.residualMm = residualMagnitude;

        if (kept) {
            cell.kept++;
        } else {
            cell.removed++;
        }
        cell.sumMeanErrorPx += info.meanErrorPx;
        cell.maxMeanErrorPx = std::max(cell.maxMeanErrorPx, info.meanErrorPx);
        cell.sumResidualMm += info.residualMm;
        cell.maxResidualMm = std::max(cell.maxResidualMm, info.residualMm);
        cell.samples.append(info);

        if (!info.key.isEmpty()) {
            for (auto &ref : shotRefs) {
                if (ref.matched || ref.derived || !ref.shot) {
                    continue;
                }
                if (ref.key == info.key &&
                    ref.shot->gridRow == row &&
                    ref.shot->gridCol == col) {
                    ref.matched = true;
                    break;
                }
            }
        }
    };

    for (const auto &det : m_lastOutput.keptDetections) {
        recordDetection(det, true);
    }
    for (const auto &det : m_lastOutput.removedDetections) {
        recordDetection(det, false);
    }

    for (auto &row : m_cellQuality) {
        for (auto &cell : row) {
            auto &samples = cell.samples;
            std::sort(samples.begin(), samples.end(), [](const CellSampleInfo &a, const CellSampleInfo &b) {
                if (a.kept != b.kept) {
                    return a.kept && !b.kept;
                }
                if (!qFuzzyCompare(a.meanErrorPx + 1.0, b.meanErrorPx + 1.0)) {
                    return a.meanErrorPx < b.meanErrorPx;
                }
                return a.displayName.localeAwareCompare(b.displayName) < 0;
            });
        }
    }

    for (const auto &ref : shotRefs) {
        if (ref.derived || ref.matched || !ref.shot) {
            continue;
        }
        const int row = std::clamp(ref.shot->gridRow, 0, kCaptureGridRows - 1);
        const int col = std::clamp(ref.shot->gridCol, 0, kCaptureGridCols - 1);
        auto &cell = m_cellQuality[row][col];
        ++cell.pending;
        cell.pendingSamples.append(sampleDisplayNameFromShot(*ref.shot, m_session));
    }

    bool anySamples = false;
    bool selectedHasSamples = false;
    for (int r = 0; r < kCaptureGridRows; ++r) {
        for (int c = 0; c < kCaptureGridCols; ++c) {
            const auto &cell = m_cellQuality[r][c];
            if (!cell.samples.isEmpty() || !cell.pendingSamples.isEmpty()) {
                anySamples = true;
                if (r == m_captureSelectedRow && c == m_captureSelectedCol) {
                    selectedHasSamples = true;
                }
            }
        }
    }

    if (m_capturePlanDetailButton) {
        m_capturePlanDetailButton->setVisible(anySamples);
        m_capturePlanDetailButton->setEnabled(selectedHasSamples);
        if (anySamples) {
            if (!selectedHasSamples) {
                m_capturePlanDetailButton->setToolTip(tr("请先选择有样本的格位。"));
            } else {
                m_capturePlanDetailButton->setToolTip(QString());
            }
        } else {
            m_capturePlanDetailButton->setToolTip(QString());
        }
    }

    updateCaptureSelectionSummary();
}

void MainWindow::showCaptureCellDetails(int row, int col)
{
    if (row < 0 || row >= kCaptureGridRows || col < 0 || col >= kCaptureGridCols) {
        return;
    }
    const auto &cell = m_cellQuality[row][col];
    if (cell.samples.isEmpty() && cell.pendingSamples.isEmpty()) {
        QMessageBox::information(this,
                                 tr("无样本"),
                                 tr("%1 尚未记录样本。")
                                     .arg(captureCellDisplayName(row, col)));
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(tr("%1 · 样本详情").arg(captureCellDisplayName(row, col)));
    dialog.setModal(true);
    dialog.resize(620, 420);

    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(18, 16, 18, 16);
    layout->setSpacing(12);

    QStringList summaryParts;
    summaryParts << tr("保留 %1").arg(cell.kept)
                 << tr("剔除 %1").arg(cell.removed);
    if (!qFuzzyIsNull(cell.sumMeanErrorPx) && cell.total() > 0) {
        summaryParts << tr("平均误差 %.3f px").arg(cell.averageMeanErrorPx());
    }
    if (!qFuzzyIsNull(cell.sumResidualMm) && cell.total() > 0) {
        summaryParts << tr("平均残差 %.3f mm").arg(cell.averageResidualMm());
    }
    if (!cell.pendingSamples.isEmpty()) {
        summaryParts << tr("待标定 %1").arg(cell.pendingSamples.size());
    }

    auto *summaryLabel = new QLabel(summaryParts.join(QStringLiteral(" · ")), &dialog);
    summaryLabel->setWordWrap(true);
    summaryLabel->setStyleSheet(QStringLiteral("font-weight:600;color:#d8e2f1;"));
    layout->addWidget(summaryLabel);

    if (!cell.samples.isEmpty()) {
        auto *table = new QTableWidget(cell.samples.size(), 5, &dialog);
        table->setHorizontalHeaderLabels({tr("样本"), tr("状态"), tr("平均误差(px)"), tr("最大误差(px)"), tr("平均残差(mm)")});
        table->horizontalHeader()->setStretchLastSection(true);
        table->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        table->verticalHeader()->setVisible(false);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->setSelectionMode(QAbstractItemView::NoSelection);
        table->setAlternatingRowColors(true);

        for (int i = 0; i < cell.samples.size(); ++i) {
            const auto &sample = cell.samples.at(i);
            auto *nameItem = new QTableWidgetItem(sample.displayName);
            nameItem->setData(Qt::UserRole, sample.key);
            table->setItem(i, 0, nameItem);

            auto *statusItem = new QTableWidgetItem(sample.kept ? tr("保留") : tr("剔除"));
            statusItem->setForeground(sample.kept ? QColor(125, 224, 178) : QColor(244, 143, 177));
            table->setItem(i, 1, statusItem);

            table->setItem(i, 2, new QTableWidgetItem(QString::number(sample.meanErrorPx, 'f', 3)));
            table->setItem(i, 3, new QTableWidgetItem(QString::number(sample.maxErrorPx, 'f', 3)));
            table->setItem(i, 4, new QTableWidgetItem(QString::number(sample.residualMm, 'f', 3)));
        }

        table->resizeColumnsToContents();
        layout->addWidget(table);
    }

    if (!cell.pendingSamples.isEmpty()) {
        auto *pendingLabel = new QLabel(&dialog);
        pendingLabel->setWordWrap(true);
        pendingLabel->setText(tr("待标定样本：%1")
                                  .arg(cell.pendingSamples.join(QStringLiteral("，"))));
        layout->addWidget(pendingLabel);
    }

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    buttons->button(QDialogButtonBox::Close)->setText(tr("关闭"));
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    layout->addWidget(buttons);

    dialog.exec();
}

void MainWindow::regenerateHeatmaps(CalibrationOutput &output)
{
    if (!output.success || output.imageSize.width <= 0 || output.imageSize.height <= 0) {
        return;
    }

    const auto &sourceDetections = !output.keptDetections.empty() ? output.keptDetections : output.allDetections;
    if (sourceDetections.empty()) {
        return;
    }

    HeatmapGenerator generator;
    HeatmapBundle refreshed;
    refreshed.boardCoverage = generator.buildBoardCoverage(sourceDetections,
                                                          output.imageSize,
                                                          &refreshed.boardCoverageMin,
                                                          &refreshed.boardCoverageMax,
                                                          &refreshed.boardCoverageScalar);
    refreshed.pixelError = generator.buildPixelErrorHeatmap(sourceDetections,
                                                           output.imageSize,
                                                           &refreshed.pixelErrorMin,
                                                           &refreshed.pixelErrorMax,
                                                           &refreshed.pixelErrorScalar);
    refreshed.boardError = generator.buildBoardErrorHeatmap(sourceDetections,
                                                           output.imageSize,
                                                           &refreshed.boardErrorMin,
                                                           &refreshed.boardErrorMax,
                                                           &refreshed.boardErrorScalar);
    refreshed.residualScatter = generator.buildResidualScatter(sourceDetections,
                                                               &refreshed.residualScatterMax);

    if (!output.cameraMatrix.empty() && !output.distCoeffs.empty()) {
        refreshed.distortionMap = generator.buildDistortionHeatmap(output.cameraMatrix,
                                                                   output.distCoeffs,
                                                                   output.imageSize,
                                                                   &refreshed.distortionMin,
                                                                   &refreshed.distortionMax,
                                                                   &refreshed.distortionGrid,
                                                                   &refreshed.distortionScalar,
                                                                   &refreshed.distortionVectors);
    }

    output.heatmaps = std::move(refreshed);
}

bool MainWindow::reloadHeatmapsFromDisk(const QString &outputDir, HeatmapBundle *bundle) const
{
    if (!bundle) {
        return false;
    }
    if (outputDir.isEmpty()) {
        return false;
    }

    QDir dir(outputDir);
    if (!dir.exists()) {
        return false;
    }

    bool anyLoaded = false;
    auto load = [&](const QString &fileName, cv::Mat *target) {
        if (!target || !target->empty()) {
            return false;
        }
        const QString path = dir.absoluteFilePath(fileName);
        if (!QFileInfo::exists(path)) {
            return false;
        }
        cv::Mat image = cv::imread(path.toStdString(), cv::IMREAD_UNCHANGED);
        if (image.empty()) {
            return false;
        }
        *target = image;
        return true;
    };

    anyLoaded |= load(QStringLiteral("board_coverage_heatmap.png"), &bundle->boardCoverage);
    anyLoaded |= load(QStringLiteral("reprojection_error_heatmap_pixels.png"), &bundle->pixelError);
    anyLoaded |= load(QStringLiteral("reprojection_error_heatmap_board.png"), &bundle->boardError);
    anyLoaded |= load(QStringLiteral("distortion_heatmap.png"), &bundle->distortionMap);

    return anyLoaded;
}

void MainWindow::showHeatmaps(const CalibrationOutput &output)
{
    if (m_heatmapBoard) {
        if (!output.heatmaps.boardCoverage.empty()) {
            m_heatmapBoard->setHeatmap(cvMatToQImage(output.heatmaps.boardCoverage),
                                       output.heatmaps.boardCoverageMin,
                                       output.heatmaps.boardCoverageMax,
                                       tr("Coverage ratio"));
        } else {
            m_heatmapBoard->clear();
        }
    }

    if (m_heatmapPixel) {
        if (!output.heatmaps.pixelError.empty()) {
            m_heatmapPixel->setHeatmap(cvMatToQImage(output.heatmaps.pixelError),
                                       output.heatmaps.pixelErrorMin,
                                       output.heatmaps.pixelErrorMax,
                                       tr("Reprojection error"));
        } else {
            m_heatmapPixel->clear();
        }
    }

    if (m_distortionMap) {
        if (!output.heatmaps.distortionMap.empty()) {
            m_distortionMap->setHeatmap(cvMatToQImage(output.heatmaps.distortionMap),
                                        output.heatmaps.distortionMin,
                                        output.heatmaps.distortionMax,
                                        tr("Δ distortion"));
            QVector<QPolygonF> warpedLines;
            warpedLines.reserve(static_cast<int>(output.heatmaps.distortionGrid.size()));
            for (const auto &line : output.heatmaps.distortionGrid) {
                if (line.size() < 2) {
                    continue;
                }
                QPolygonF poly;
                poly.reserve(static_cast<int>(line.size()));
                for (const auto &pt : line) {
                    if (!std::isfinite(pt.x) || !std::isfinite(pt.y)) {
                        continue;
                    }
                    poly.append(QPointF(pt.x, pt.y));
                }
                if (poly.size() >= 2) {
                    warpedLines.append(poly);
                }
            }
            m_distortionMap->setWarpedGridLines(warpedLines);
        } else {
            m_distortionMap->clear();
        }
    }

    if (m_scatterView) {
        std::vector<ResidualScatterView::Sample> samples;
        samples.reserve(4096);
        float maxMagnitudePx = 0.0f;
        float maxMagnitudeMm = 0.0f;

        auto accumulateResiduals = [&](const std::vector<DetectionResult> &detections) {
            for (const auto &det : detections) {
                if (!det.success || det.residualVectors.empty()) {
                    continue;
                }
                const size_t limit = std::min(det.residualVectors.size(), det.residualCameraMm.size());
                for (size_t i = 0; i < limit; ++i) {
                    const auto &vec = det.residualVectors[i];
                    const auto &mm = det.residualCameraMm[i];
                    ResidualScatterView::Sample sample;
                    sample.deltaPx = QPointF(vec.x, vec.y);
                    sample.magnitudePx = static_cast<float>(std::hypot(vec.x, vec.y));
                    const double mmMag = std::sqrt(mm[0] * mm[0] + mm[1] * mm[1] + mm[2] * mm[2]);
                    sample.magnitudeMm = static_cast<float>(mmMag);
                    maxMagnitudePx = std::max(maxMagnitudePx, sample.magnitudePx);
                    maxMagnitudeMm = std::max(maxMagnitudeMm, sample.magnitudeMm);
                    samples.push_back(sample);
                }
            }
        };

        accumulateResiduals(output.keptDetections);
        accumulateResiduals(output.removedDetections);

        if (samples.empty()) {
            m_scatterView->clear();
        } else {
            maxMagnitudePx = std::max(maxMagnitudePx, 0.001f);
            maxMagnitudeMm = std::max(maxMagnitudeMm, 0.001f);
            m_scatterView->setSamples(std::move(samples), maxMagnitudePx, maxMagnitudeMm);
        }
    }
}

void MainWindow::handleDetectionSelectionChanged()
{
    if (!m_detectionTree) {
        return;
    }
    auto *item = m_detectionTree->currentItem();
    const DetectionResult *result = nullptr;
    if (item) {
        const QString name = item->data(0, Qt::UserRole).toString();
        showDetectionPreview(name);
        result = findDetection(name);
        if (m_stageNavigator && m_stageCaptureItem) {
            m_stageNavigator->setCurrentItem(m_stageCaptureItem);
        }
    } else {
        showDetectionPreview(QString());
    }
    updateDetectionDetailPanel(result);
    ensurePoseView();
    if (m_poseView) {
        m_poseView->setActiveDetection(result);
    }
}

void MainWindow::showDetectionPreview(const QString &name)
{
    if (!m_detectionPreview) {
        return;
    }
    if (name.isEmpty()) {
        m_detectionPreview->clear();
        return;
    }
    if (const auto *result = findDetection(name)) {
        m_detectionPreview->setDetection(*result);
    } else {
        m_detectionPreview->clear();
    }
}

namespace {
struct ComponentStats {
    cv::Vec3d meanAbs{0.0, 0.0, 0.0};
    cv::Vec3d rms{0.0, 0.0, 0.0};
    cv::Vec3d maxAbs{0.0, 0.0, 0.0};
    int count {0};
};

ComponentStats computeComponentStats(const std::vector<cv::Vec3d> &values)
{
    ComponentStats stats;
    if (values.empty()) {
        return stats;
    }
    cv::Vec3d sumAbs{0.0, 0.0, 0.0};
    cv::Vec3d sumSquares{0.0, 0.0, 0.0};
    cv::Vec3d maxAbs{0.0, 0.0, 0.0};
    for (const auto &v : values) {
        for (int axis = 0; axis < 3; ++axis) {
            const double absVal = std::abs(v[axis]);
            sumAbs[axis] += absVal;
            sumSquares[axis] += v[axis] * v[axis];
            maxAbs[axis] = std::max(maxAbs[axis], absVal);
        }
    }
    const double invCount = 1.0 / static_cast<double>(values.size());
    stats.count = static_cast<int>(values.size());
    stats.meanAbs = sumAbs * invCount;
    stats.rms = cv::Vec3d(std::sqrt(sumSquares[0] * invCount),
                          std::sqrt(sumSquares[1] * invCount),
                          std::sqrt(sumSquares[2] * invCount));
    stats.maxAbs = maxAbs;
    return stats;
}

QString formatVec3(const cv::Vec3d &vec, int precision = 3)
{
    return QStringLiteral("(%1, %2, %3)")
        .arg(vec[0], 0, 'f', precision)
        .arg(vec[1], 0, 'f', precision)
        .arg(vec[2], 0, 'f', precision);
}
} // namespace

void MainWindow::updateDetectionDetailPanel(const DetectionResult *result)
{
    if (!m_detectionMetaLabel || !m_detectionResidualMmLabel || !m_detectionResidualPercentLabel) {
        return;
    }

    if (!result) {
        m_detectionMetaLabel->setText(tr("Select an image on the left to inspect residuals."));
        m_detectionResidualMmLabel->clear();
        m_detectionResidualPercentLabel->clear();
        return;
    }

    const int pointCount = static_cast<int>(result->imagePoints.size());
    const ComponentStats mmStats = computeComponentStats(result->residualCameraMm);
    const ComponentStats pctStats = computeComponentStats(result->residualCameraPercent);
    const double maxResidualPx = result->residualsPx.empty()
                                     ? 0.0
                                     : *std::max_element(result->residualsPx.begin(), result->residualsPx.end());

    QString meta = tr("<b>%1</b>")
                       .arg(QString::fromStdString(result->name));
    meta += tr("<br/>Resolution: %1 × %2 px | Points: %3")
                .arg(result->resolution.width)
                .arg(result->resolution.height)
                .arg(pointCount);
    meta += tr("<br/>Translation [mm]: %1")
                .arg(formatVec3(result->translationMm, 2));
    meta += tr("<br/>Rotation [deg]: %1")
                .arg(formatVec3(result->rotationDeg, 2));
    meta += tr("<br/>Mean reprojection error: %1 px | Max: %2 px")
                .arg(result->meanErrorPx(), 0, 'f', 3)
                .arg(maxResidualPx, 0, 'f', 3);

    if (result->iterationRemoved > 0) {
        meta += tr("<br/><span style=\"color:#f06292;\">Removed in iteration %1</span>")
                    .arg(result->iterationRemoved);
    }
    m_detectionMetaLabel->setText(meta);

    QString mmText;
    if (mmStats.count > 0) {
        mmText += tr("Mean |ΔX,Y,Z| [mm]: %1")
                      .arg(formatVec3(mmStats.meanAbs));
        mmText += tr("<br/>RMS |ΔX,Y,Z| [mm]: %1")
                      .arg(formatVec3(mmStats.rms));
        mmText += tr("<br/>Max |ΔX,Y,Z| [mm]: %1")
                      .arg(formatVec3(mmStats.maxAbs));
    } else {
        mmText = tr("No camera-space residuals.");
    }
    m_detectionResidualMmLabel->setText(mmText);

    QString pctText;
    if (pctStats.count > 0) {
        pctText += tr("Mean |ΔX,Y,Z| [%]: %1")
                       .arg(formatVec3(pctStats.meanAbs));
        pctText += tr("<br/>RMS |ΔX,Y,Z| [%]: %1")
                       .arg(formatVec3(pctStats.rms));
        pctText += tr("<br/>Max |ΔX,Y,Z| [%]: %1")
                       .arg(formatVec3(pctStats.maxAbs));
    } else {
        pctText = tr("No percent residuals.");
    }
    m_detectionResidualPercentLabel->setText(pctText);
}

const DetectionResult *MainWindow::findDetection(const QString &name) const
{
    const std::string needle = name.toStdString();
    for (const auto &rec : m_lastOutput.allDetections) {
        if (rec.name == needle) {
            return &rec;
        }
    }
    for (const auto &rec : m_lastOutput.keptDetections) {
        if (rec.name == needle) {
            return &rec;
        }
    }
    for (const auto &rec : m_lastOutput.removedDetections) {
        if (rec.name == needle) {
            return &rec;
        }
    }
    return nullptr;
}

void MainWindow::appendLog(QtMsgType type, const QString &message)
{
    if (!m_logView) {
        return;
    }

    static const QRegularExpression headerPattern(
        QStringLiteral("^(\\d{4}-\\d{2}-\\d{2} \\d{2}:\\d{2}:\\d{2}\\.\\d{3}) \\[([^\\]]+)\\]\\s*(.*)$"));
    static const QRegularExpression stagePattern(
        QStringLiteral("^\\[(OK|FAIL|SUCCESS|PROGRESS|STEP|TASK)\\]\\s*(.*)$"),
        QRegularExpression::CaseInsensitiveOption);

    auto toHtml = [](const QString &plain) {
        return Qt::convertFromPlainText(plain);
    };

    auto normalize = [](QString text) {
        text = text.trimmed();
        text.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
        return text;
    };

    struct Visual {
        QString label;
        QString badgeColor;
        QString badgeBackground;
        QString bodyColor;
    };

    auto levelVisual = [&](const QString &token, QtMsgType msgType) -> Visual {
        const QString upper = token.toUpper();
        if (upper == QStringLiteral("WARNING") || upper == QStringLiteral("WARN")) {
            return {QStringLiteral("WARNING"), QStringLiteral("#FB8C00"), QStringLiteral("rgba(251,140,0,0.18)"), QStringLiteral("#FFE0B2")};
        }
        if (upper == QStringLiteral("ERROR") || upper == QStringLiteral("ERR") || msgType == QtCriticalMsg || msgType == QtFatalMsg) {
            return {QStringLiteral("ERROR"), QStringLiteral("#EF5350"), QStringLiteral("rgba(239,83,80,0.20)"), QStringLiteral("#FFCDD2")};
        }
        if (upper == QStringLiteral("DEBUG")) {
            return {QStringLiteral("DEBUG"), QStringLiteral("#90A4AE"), QStringLiteral("rgba(144,164,174,0.20)"), QStringLiteral("#ECEFF1")};
        }
        return {QStringLiteral("INFO"), QStringLiteral("#42A5F5"), QStringLiteral("rgba(66,165,245,0.18)"), QStringLiteral("#E3F2FD")};
    };

    struct StageVisual {
        bool valid {false};
        QString label;
        QString badgeColor;
        QString badgeBackground;
    };

    auto stageVisualFor = [](const QString &token) -> StageVisual {
        const QString key = token.toUpper();
        if (key == QStringLiteral("OK") || key == QStringLiteral("SUCCESS")) {
            return {true, QStringLiteral("SUCCESS"), QStringLiteral("#66BB6A"), QStringLiteral("rgba(102,187,106,0.22)")};
        }
        if (key == QStringLiteral("FAIL") || key == QStringLiteral("FAILED")) {
            return {true, QStringLiteral("FAILED"), QStringLiteral("#EF5350"), QStringLiteral("rgba(239,83,80,0.22)")};
        }
        if (key == QStringLiteral("PROGRESS")) {
            return {true, QStringLiteral("PROGRESS"), QStringLiteral("#AB47BC"), QStringLiteral("rgba(171,71,188,0.22)")};
        }
        if (key == QStringLiteral("STEP")) {
            return {true, QStringLiteral("STEP"), QStringLiteral("#29B6F6"), QStringLiteral("rgba(41,182,246,0.20)")};
        }
        if (key == QStringLiteral("TASK")) {
            return {true, QStringLiteral("TASK"), QStringLiteral("#7E57C2"), QStringLiteral("rgba(126,87,194,0.20)")};
        }
        return {};
    };

    auto badge = [](const QString &fg, const QString &bg, const QString &text) {
        return QStringLiteral("<span style=\"color:%1;background:%2;border-radius:6px;padding:1px 7px;font-size:11px;font-weight:600;letter-spacing:0.4px;\">%3</span>")
            .arg(fg, bg, Qt::convertFromPlainText(text));
    };

    QString timestamp;
    QString levelToken;
    QString body = message;

    if (const QRegularExpressionMatch match = headerPattern.match(message); match.hasMatch()) {
        timestamp = match.captured(1);
        levelToken = match.captured(2).trimmed();
        body = match.captured(3);
    }

    if (levelToken.isEmpty()) {
        switch (type) {
        case QtWarningMsg:
            levelToken = QStringLiteral("WARNING");
            break;
        case QtCriticalMsg:
        case QtFatalMsg:
            levelToken = QStringLiteral("ERROR");
            break;
        case QtDebugMsg:
            levelToken = QStringLiteral("DEBUG");
            break;
        case QtInfoMsg:
        default:
            levelToken = QStringLiteral("INFO");
            break;
        }
    }

    const Visual level = levelVisual(levelToken, type);
    const QString levelBadge = badge(level.badgeColor, level.badgeBackground, level.label);

    QString stageKey;
    StageVisual stage;
    if (const QRegularExpressionMatch stageMatch = stagePattern.match(body); stageMatch.hasMatch()) {
        stageKey = stageMatch.captured(1).toUpper();
        body = stageMatch.captured(2);
        stage = stageVisualFor(stageKey);
    }

    QString stageBadge;
    if (stage.valid) {
        stageBadge = badge(stage.badgeColor, stage.badgeBackground, stage.label);
    }

    body = body.trimmed();
    const QString normalizedBody = normalize(body);
    const QString flattenedBody = normalizedBody.isEmpty() ? QStringLiteral(" ") : normalizedBody;
    const QString bodyHtml = toHtml(flattenedBody);

    QString timestampHtml;
    if (!timestamp.isEmpty()) {
        timestampHtml = QStringLiteral("<span style=\"color:#8EA4D2;font-family:'JetBrains Mono','Consolas','Menlo',monospace;font-size:11px;\">%1</span>")
            .arg(toHtml(timestamp));
    }

    QStringList fragments;
    if (!timestampHtml.isEmpty()) {
        fragments << timestampHtml;
    }
    fragments << levelBadge;
    if (!stageBadge.isEmpty()) {
        fragments << stageBadge;
    }
    fragments << QStringLiteral("<span style=\"color:%1;\">%2</span>").arg(level.bodyColor, bodyHtml);

    const QString baseHtml = fragments.join(QStringLiteral(" "));
    const QString repeatKey = levelToken.toUpper() + QLatin1Char('\x1F') +
        stageKey + QLatin1Char('\x1F') + normalizedBody.toCaseFolded();

    auto scrollToBottom = [this]() {
        if (m_logView) {
            if (auto *sb = m_logView->verticalScrollBar()) {
                sb->setValue(sb->maximum());
            }
        }
    };

    if (!m_lastLogKey.isEmpty() && repeatKey == m_lastLogKey) {
        ++m_lastLogRepeat;
        if (auto *doc = m_logView->document(); doc && doc->blockCount() > 0) {
            QTextCursor cursor(doc->lastBlock());
            cursor.select(QTextCursor::BlockUnderCursor);
            const QString repeatedHtml = m_lastLogHtml +
                QStringLiteral(" <span style=\"color:#80CBC4;font-size:11px;\">(×%1)</span>").arg(m_lastLogRepeat);
            cursor.insertHtml(repeatedHtml);
        }
        scrollToBottom();
        return;
    }

    m_lastLogKey = repeatKey;
    m_lastLogHtml = baseHtml;
    m_lastLogRepeat = 1;
    if (auto *doc = m_logView->document()) {
        QTextCursor cursor(doc);
        cursor.movePosition(QTextCursor::End);
        QTextBlockFormat fmt;
        fmt.setTopMargin(0.0);
        fmt.setBottomMargin(0.0);
        if (!doc->isEmpty()) {
            cursor.insertBlock(fmt);
        } else {
            cursor.setBlockFormat(fmt);
        }
        cursor.insertHtml(baseHtml);
    }
    scrollToBottom();
}

void MainWindow::ensurePoseView()
{
    if (!m_poseTab || !m_poseStack) {
        return;
    }
    if (!m_poseView) {
        m_poseView = new Pose3DView(m_poseTab);
        m_poseView->setMinimumHeight(320);
        m_poseStack->addWidget(m_poseView);
    }
    if (m_poseStack && m_poseView) {
        m_poseStack->setCurrentWidget(m_poseView);
    }
}

} // namespace mycalib
