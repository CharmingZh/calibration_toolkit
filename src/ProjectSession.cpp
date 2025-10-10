#include "ProjectSession.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QUuid>
#include <QtGlobal>

#include <algorithm>

#include "Logger.h"

namespace mycalib {

namespace {

static constexpr auto kSessionFileName = "session.json";

static QString defaultProjectName()
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    return QStringLiteral("MyCalib-%1").arg(now.toString(QStringLiteral("yyyyMMdd-hhmmss")));
}

static QString makeProjectId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

static QString ensureExtension(const QFileInfo &info, const QString &fallback)
{
    const QString ext = info.suffix().toLower();
    return ext.isEmpty() ? fallback : ext;
}

static QString makeDestFileName(const QString &prefix, const QString &extension)
{
    return QStringLiteral("%1_%2.%3")
        .arg(prefix,
             QUuid::createUuid().toString(QUuid::WithoutBraces),
             extension);
}

static QString formatDateTime(const QDateTime &value)
{
    if (!value.isValid()) {
        return {};
    }
    return value.toUTC().toString(Qt::ISODateWithMs);
}

static QDateTime parseDateTime(const QString &value)
{
    if (value.isEmpty()) {
        return {};
    }
    QDateTime dt = QDateTime::fromString(value, Qt::ISODateWithMs);
    if (!dt.isValid()) {
        dt = QDateTime::fromString(value, Qt::ISODate);
    }
    if (dt.isValid()) {
        dt = dt.toUTC();
    }
    return dt;
}

static bool copyAsset(const QString &sourcePath, const QString &targetPath)
{
    if (sourcePath.isEmpty()) {
        return false;
    }

    const QFileInfo sourceInfo(sourcePath);
    if (!sourceInfo.exists() || !sourceInfo.isFile()) {
        Logger::warning(QStringLiteral("Source asset does not exist: %1").arg(sourcePath));
        return false;
    }

    const QFileInfo targetInfo(targetPath);
    QDir().mkpath(targetInfo.absolutePath());

    if (QFile::exists(targetInfo.absoluteFilePath())) {
        QFile existing(targetInfo.absoluteFilePath());
        if (!existing.remove()) {
            Logger::warning(QStringLiteral("Failed to replace existing file: %1").arg(targetPath));
            return false;
        }
    }

    if (!QFile::copy(sourceInfo.absoluteFilePath(), targetInfo.absoluteFilePath())) {
        Logger::warning(QStringLiteral("Failed to copy %1 → %2").arg(sourcePath, targetPath));
        return false;
    }

    return true;
}

static QJsonObject stageToJson(const ProjectSession::StageState &stage)
{
    QJsonObject obj;
    obj.insert(QStringLiteral("status"), ProjectSession::toString(stage.status));
    if (stage.startedAt.isValid()) {
        obj.insert(QStringLiteral("started_at"), formatDateTime(stage.startedAt));
    }
    if (stage.completedAt.isValid()) {
        obj.insert(QStringLiteral("completed_at"), formatDateTime(stage.completedAt));
    }
    if (!stage.notes.isEmpty()) {
        obj.insert(QStringLiteral("notes"), stage.notes);
    }
    return obj;
}

static ProjectSession::StageState stageFromJson(const QJsonObject &obj)
{
    ProjectSession::StageState stage;

    if (obj.contains(QStringLiteral("status"))) {
        stage.status = ProjectSession::stageStatusFromString(obj.value(QStringLiteral("status")).toString());
    } else if (obj.contains(QStringLiteral("completed"))) {
        const bool completed = obj.value(QStringLiteral("completed")).toBool(false);
        stage.status = completed ? ProjectSession::StageStatus::Completed : ProjectSession::StageStatus::NotStarted;
    }

    stage.startedAt = parseDateTime(obj.value(QStringLiteral("started_at")).toString());
    stage.completedAt = parseDateTime(obj.value(QStringLiteral("completed_at")).toString());
    stage.notes = obj.value(QStringLiteral("notes")).toString();

    return stage;
}

static QJsonObject tuningSnapshotToJson(const ProjectSession::TuningSnapshot &snapshot)
{
    QJsonObject obj;
    obj.insert(QStringLiteral("id"), snapshot.id.toString(QUuid::WithoutBraces));
    obj.insert(QStringLiteral("captured_at"), formatDateTime(snapshot.capturedAt));
    obj.insert(QStringLiteral("relative_path"), snapshot.relativePath);
    if (!snapshot.metrics.isEmpty()) {
        obj.insert(QStringLiteral("metrics"), QJsonObject::fromVariantMap(snapshot.metrics));
    }
    return obj;
}

static ProjectSession::TuningSnapshot tuningSnapshotFromJson(const QJsonObject &obj)
{
    ProjectSession::TuningSnapshot snapshot;
    snapshot.id = QUuid::fromString(obj.value(QStringLiteral("id")).toString());
    if (snapshot.id.isNull()) {
        snapshot.id = QUuid::createUuid();
    }
    snapshot.capturedAt = parseDateTime(obj.value(QStringLiteral("captured_at")).toString());
    snapshot.relativePath = obj.value(QStringLiteral("relative_path")).toString();
    snapshot.metrics = obj.value(QStringLiteral("metrics")).toObject().toVariantMap();
    return snapshot;
}

static QJsonObject captureShotToJson(const ProjectSession::CaptureShot &shot)
{
    QJsonObject obj;
    obj.insert(QStringLiteral("id"), shot.id.toString(QUuid::WithoutBraces));
    obj.insert(QStringLiteral("captured_at"), formatDateTime(shot.capturedAt));
    obj.insert(QStringLiteral("grid_row"), shot.gridRow);
    obj.insert(QStringLiteral("grid_col"), shot.gridCol);
    obj.insert(QStringLiteral("pose"), ProjectSession::toString(shot.pose));
    obj.insert(QStringLiteral("relative_path"), shot.relativePath);
    obj.insert(QStringLiteral("accepted"), shot.accepted);
    if (!shot.rejectionReason.isEmpty()) {
        obj.insert(QStringLiteral("rejection_reason"), shot.rejectionReason);
    }
    if (!shot.metadata.isEmpty()) {
        obj.insert(QStringLiteral("metadata"), QJsonObject::fromVariantMap(shot.metadata));
    }
    return obj;
}

static ProjectSession::CaptureShot captureShotFromJson(const QJsonObject &obj)
{
    ProjectSession::CaptureShot shot;
    shot.id = QUuid::fromString(obj.value(QStringLiteral("id")).toString());
    if (shot.id.isNull()) {
        shot.id = QUuid::createUuid();
    }
    shot.capturedAt = parseDateTime(obj.value(QStringLiteral("captured_at")).toString());
    shot.gridRow = obj.value(QStringLiteral("grid_row")).toInt();
    shot.gridCol = obj.value(QStringLiteral("grid_col")).toInt();
    shot.pose = ProjectSession::capturePoseFromString(obj.value(QStringLiteral("pose")).toString());
    shot.relativePath = obj.value(QStringLiteral("relative_path")).toString();
    shot.accepted = obj.value(QStringLiteral("accepted")).toBool(false);
    shot.rejectionReason = obj.value(QStringLiteral("rejection_reason")).toString();
    shot.metadata = obj.value(QStringLiteral("metadata")).toObject().toVariantMap();
    return shot;
}

static QJsonObject laserFrameToJson(const ProjectSession::LaserFrame &frame)
{
    QJsonObject obj;
    obj.insert(QStringLiteral("id"), frame.id.toString(QUuid::WithoutBraces));
    obj.insert(QStringLiteral("captured_at"), formatDateTime(frame.capturedAt));
    obj.insert(QStringLiteral("relative_path"), frame.relativePath);
    if (!frame.annotations.isEmpty()) {
        obj.insert(QStringLiteral("annotations"), QJsonObject::fromVariantMap(frame.annotations));
    }
    return obj;
}

static ProjectSession::LaserFrame laserFrameFromJson(const QJsonObject &obj)
{
    ProjectSession::LaserFrame frame;
    frame.id = QUuid::fromString(obj.value(QStringLiteral("id")).toString());
    if (frame.id.isNull()) {
        frame.id = QUuid::createUuid();
    }
    frame.capturedAt = parseDateTime(obj.value(QStringLiteral("captured_at")).toString());
    frame.relativePath = obj.value(QStringLiteral("relative_path")).toString();
    frame.annotations = obj.value(QStringLiteral("annotations")).toObject().toVariantMap();
    return frame;
}

static QJsonObject vectorToJson(const QVector3D &vector)
{
    QJsonObject obj;
    obj.insert(QStringLiteral("x"), vector.x());
    obj.insert(QStringLiteral("y"), vector.y());
    obj.insert(QStringLiteral("z"), vector.z());
    return obj;
}

static QVector3D vectorFromJson(const QJsonObject &obj, const QVector3D &fallback)
{
    QVector3D result = fallback;
    if (obj.contains(QStringLiteral("x"))) {
        result.setX(static_cast<float>(obj.value(QStringLiteral("x")).toDouble(result.x())));
    }
    if (obj.contains(QStringLiteral("y"))) {
        result.setY(static_cast<float>(obj.value(QStringLiteral("y")).toDouble(result.y())));
    }
    if (obj.contains(QStringLiteral("z"))) {
        result.setZ(static_cast<float>(obj.value(QStringLiteral("z")).toDouble(result.z())));
    }
    return result;
}

static QJsonObject laserPlaneToJson(const ProjectSession::LaserPlaneEstimate &plane)
{
    QJsonObject obj;
    obj.insert(QStringLiteral("solved"), plane.solved);
    obj.insert(QStringLiteral("distance"), plane.distance);
    obj.insert(QStringLiteral("normal"), vectorToJson(plane.normal));
    if (!plane.extra.isEmpty()) {
        obj.insert(QStringLiteral("extra"), QJsonObject::fromVariantMap(plane.extra));
    }
    return obj;
}

static ProjectSession::LaserPlaneEstimate laserPlaneFromJson(const QJsonObject &obj)
{
    ProjectSession::LaserPlaneEstimate plane;
    plane.solved = obj.value(QStringLiteral("solved")).toBool(false);
    plane.distance = obj.value(QStringLiteral("distance")).toDouble(0.0);
    if (obj.value(QStringLiteral("normal")).isObject()) {
        plane.normal = vectorFromJson(obj.value(QStringLiteral("normal")).toObject(), QVector3D(0.0f, 0.0f, 1.0f));
    }
    plane.extra = obj.value(QStringLiteral("extra")).toObject().toVariantMap();
    return plane;
}

} // namespace

ProjectSession::ProjectSession(QObject *parent)
    : QObject(parent)
{
}

bool ProjectSession::initializeNew(const QString &rootDirectory,
                                   const QString &projectName,
                                   DataSource source,
                                   QString *errorMessage)
{
    m_rootPath = QDir::cleanPath(rootDirectory);
    if (m_rootPath.isEmpty()) {
        if (errorMessage) {
            *errorMessage = tr("Invalid project directory");
        }
        return false;
    }

    const QFileInfo rootInfo(m_rootPath);
    if (rootInfo.exists()) {
        if (!rootInfo.isDir()) {
            if (errorMessage) {
                *errorMessage = tr("Project path %1 is not a directory.").arg(m_rootPath);
            }
            return false;
        }

        QDir existing(m_rootPath);
        if (existing.exists(QString::fromUtf8(kSessionFileName))) {
            if (errorMessage) {
                *errorMessage = tr("Project already exists at %1").arg(m_rootPath);
            }
            return false;
        }

        const QStringList entries = existing.entryList(QDir::NoDotAndDotDot | QDir::AllEntries);
        if (!entries.isEmpty()) {
            if (errorMessage) {
                *errorMessage = tr("Project directory %1 is not empty.").arg(m_rootPath);
            }
            return false;
        }
    } else {
        QDir parent = rootInfo.dir();
        if (!parent.exists()) {
            if (!QDir().mkpath(parent.absolutePath())) {
                if (errorMessage) {
                    *errorMessage = tr("Failed to create project parent directory: %1")
                                        .arg(parent.absolutePath());
                }
                return false;
            }
        }
    }

    if (!QDir().mkpath(m_rootPath)) {
        if (errorMessage) {
            *errorMessage = tr("Failed to create project directory: %1").arg(m_rootPath);
        }
        return false;
    }

    const QString trimmedName = projectName.trimmed();
    m_metadata.projectName = trimmedName.isEmpty() ? defaultProjectName() : trimmedName;
    m_metadata.projectId = makeProjectId();
    m_metadata.dataSource = source;
    m_metadata.createdAt = QDateTime::currentDateTimeUtc();
    m_metadata.lastOpenedAt = m_metadata.createdAt;
    m_metadata.cameraVendor.clear();
    m_metadata.cameraModel.clear();
    m_metadata.cameraTuning = {};
    m_metadata.calibrationCapture = {};
    m_metadata.laserCalibration = {};
    m_metadata.tuningSnapshots.clear();
    m_metadata.calibrationShots.clear();
    m_metadata.laserFrames.clear();
    m_metadata.laserPlane = {};

    ensureScaffold();
    return save(errorMessage);
}

bool ProjectSession::loadExisting(const QString &rootDirectory, QString *errorMessage)
{
    m_rootPath = QDir::cleanPath(rootDirectory);
    const QFileInfo info(QDir(m_rootPath).filePath(kSessionFileName));
    if (!info.exists() || !info.isFile()) {
        if (errorMessage) {
            *errorMessage = tr("Project session file not found in %1").arg(m_rootPath);
        }
        return false;
    }

    QFile file(info.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorMessage) {
            *errorMessage = tr("Failed to open session file: %1").arg(file.errorString());
        }
        return false;
    }

    QJsonParseError parseError {};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        if (errorMessage) {
            *errorMessage = tr("Invalid session JSON: %1").arg(parseError.errorString());
        }
        return false;
    }

    if (!doc.isObject()) {
        if (errorMessage) {
            *errorMessage = tr("Session file is not a JSON object");
        }
        return false;
    }

    fromJson(doc.object());
    ensureScaffold();
    m_metadata.lastOpenedAt = QDateTime::currentDateTimeUtc();
    return save(errorMessage);
}

bool ProjectSession::save(QString *errorMessage)
{
    if (m_rootPath.isEmpty()) {
        if (errorMessage) {
            *errorMessage = tr("Project root is empty");
        }
        return false;
    }

    ensureScaffold();

    const QString sessionPath = sessionFilePath();
    QFile file(sessionPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorMessage) {
            *errorMessage = tr("Failed to write session file: %1").arg(file.errorString());
        }
        return false;
    }

    const QJsonDocument doc(toJson());
    file.write(doc.toJson(QJsonDocument::Indented));
    file.close();

    Q_EMIT metadataChanged();
    return true;
}

QString ProjectSession::sessionFilePath() const
{
    if (m_rootPath.isEmpty()) {
        return {};
    }
    QDir root(m_rootPath);
    return root.filePath(kSessionFileName);
}

QDir ProjectSession::capturesRoot() const
{
    if (m_rootPath.isEmpty()) {
        return {};
    }
    QDir root(m_rootPath);
    root.mkpath(QStringLiteral("captures"));
    return QDir(root.filePath(QStringLiteral("captures")));
}

QDir ProjectSession::calibrationCaptureDir() const
{
    if (m_rootPath.isEmpty()) {
        return {};
    }
    QDir captures = capturesRoot();
    captures.mkpath(QStringLiteral("calibration"));
    return QDir(captures.filePath(QStringLiteral("calibration")));
}

QDir ProjectSession::tuningCaptureDir() const
{
    if (m_rootPath.isEmpty()) {
        return {};
    }
    QDir captures = capturesRoot();
    captures.mkpath(QStringLiteral("tuning"));
    return QDir(captures.filePath(QStringLiteral("tuning")));
}

QDir ProjectSession::liveCacheDir() const
{
    if (m_rootPath.isEmpty()) {
        return {};
    }
    QDir captures = capturesRoot();
    captures.mkpath(QStringLiteral("live"));
    return QDir(captures.filePath(QStringLiteral("live")));
}

QDir ProjectSession::laserCaptureDir() const
{
    if (m_rootPath.isEmpty()) {
        return {};
    }
    QDir captures = capturesRoot();
    captures.mkpath(QStringLiteral("laser"));
    return QDir(captures.filePath(QStringLiteral("laser")));
}

QDir ProjectSession::calibrationOutputDir() const
{
    if (m_rootPath.isEmpty()) {
        return {};
    }
    QDir root(m_rootPath);
    root.mkpath(QStringLiteral("calibration"));
    return QDir(root.filePath(QStringLiteral("calibration")));
}

QDir ProjectSession::laserOutputDir() const
{
    if (m_rootPath.isEmpty()) {
        return {};
    }
    QDir root(m_rootPath);
    root.mkpath(QStringLiteral("laser"));
    return QDir(root.filePath(QStringLiteral("laser")));
}

QDir ProjectSession::logsDir() const
{
    if (m_rootPath.isEmpty()) {
        return {};
    }
    QDir root(m_rootPath);
    root.mkpath(QStringLiteral("logs"));
    return QDir(root.filePath(QStringLiteral("logs")));
}

QDir ProjectSession::reportsDir() const
{
    if (m_rootPath.isEmpty()) {
        return {};
    }
    QDir root(m_rootPath);
    root.mkpath(QStringLiteral("reports"));
    return QDir(root.filePath(QStringLiteral("reports")));
}

QDir ProjectSession::exportsDir() const
{
    if (m_rootPath.isEmpty()) {
        return {};
    }
    QDir root(m_rootPath);
    root.mkpath(QStringLiteral("exports"));
    return QDir(root.filePath(QStringLiteral("exports")));
}

QDir ProjectSession::configDir() const
{
    if (m_rootPath.isEmpty()) {
        return {};
    }
    QDir root(m_rootPath);
    root.mkpath(QStringLiteral("config"));
    return QDir(root.filePath(QStringLiteral("config")));
}

QString ProjectSession::relativePath(const QString &absolutePath) const
{
    if (m_rootPath.isEmpty()) {
        return QFileInfo(absolutePath).absoluteFilePath();
    }
    QDir root(m_rootPath);
    return root.relativeFilePath(QFileInfo(absolutePath).absoluteFilePath());
}

bool ProjectSession::setDataSource(DataSource source, QString *errorMessage)
{
    if (m_metadata.dataSource == source) {
        return true;
    }

    m_metadata.dataSource = source;
    if (!save(errorMessage)) {
        return false;
    }
    Q_EMIT dataSourceChanged(source);
    return true;
}

ProjectSession::StageState ProjectSession::stageState(ProjectStage stage) const
{
    return stageConst(stage);
}

void ProjectSession::updateStageState(ProjectStage stage, const StageState &state, bool touchTimestamps)
{
    StageState &stored = mutableStage(stage);
    StageState updated = stored;

    updated.status = state.status;
    updated.notes = state.notes;

    if (state.startedAt.isValid()) {
        updated.startedAt = state.startedAt;
    } else if (state.status == StageStatus::NotStarted && touchTimestamps) {
        updated.startedAt = {};
    }

    if (state.completedAt.isValid()) {
        updated.completedAt = state.completedAt;
    } else if (state.status != StageStatus::Completed && touchTimestamps) {
        updated.completedAt = {};
    }

    if (touchTimestamps) {
        const QDateTime now = QDateTime::currentDateTimeUtc();
        switch (updated.status) {
        case StageStatus::NotStarted:
            updated.startedAt = {};
            updated.completedAt = {};
            break;
        case StageStatus::InProgress:
            if (!updated.startedAt.isValid()) {
                updated.startedAt = now;
            }
            updated.completedAt = {};
            break;
        case StageStatus::Completed:
            if (!updated.startedAt.isValid()) {
                updated.startedAt = now;
            }
            if (!updated.completedAt.isValid()) {
                updated.completedAt = now;
            }
            break;
        }
    }

    if (stored.status == updated.status && stored.startedAt == updated.startedAt &&
        stored.completedAt == updated.completedAt && stored.notes == updated.notes) {
        return;
    }

    StageState previous = stored;
    stored = updated;

    QString error;
    if (!save(&error)) {
        stored = previous;
        Logger::error(error.isEmpty() ? QStringLiteral("Failed to persist stage state update") : error);
    }
}

QVector<ProjectSession::TuningSnapshot> ProjectSession::tuningSnapshots() const
{
    return m_metadata.tuningSnapshots;
}

ProjectSession::TuningSnapshot ProjectSession::recordTuningSnapshot(const QString &absolutePath,
                                                                    const QVariantMap &metrics)
{
    if (m_rootPath.isEmpty()) {
        Logger::error(QStringLiteral("Cannot record snapshot without a project root"));
        return {};
    }

    ensureScaffold();

    const TuningSnapshot snapshot = makeTuningSnapshot(absolutePath, metrics);
    m_metadata.tuningSnapshots.append(snapshot);

    QString error;
    if (!save(&error)) {
        m_metadata.tuningSnapshots.removeLast();
        Logger::error(error.isEmpty() ? QStringLiteral("Failed to save session after recording tuning snapshot")
                                      : error);
        return {};
    }

    return snapshot;
}

QVector<ProjectSession::CaptureShot> ProjectSession::calibrationShots() const
{
    return m_metadata.calibrationShots;
}

ProjectSession::CaptureShot ProjectSession::addCalibrationShot(int gridRow,
                                                               int gridCol,
                                                               CapturePose pose,
                                                               const QString &absolutePath,
                                                               const QVariantMap &metadata)
{
    if (m_rootPath.isEmpty()) {
        Logger::error(QStringLiteral("Cannot add calibration shot without a project root"));
        return {};
    }

    ensureScaffold();

    const CaptureShot shot = makeShotRecord(gridRow, gridCol, pose, absolutePath, metadata);
    m_metadata.calibrationShots.append(shot);

    QString error;
    if (!save(&error)) {
        m_metadata.calibrationShots.removeLast();
        Logger::error(error.isEmpty() ? QStringLiteral("Failed to save session after adding calibration shot")
                                      : error);
        return {};
    }

    return shot;
}

bool ProjectSession::markCalibrationShotAccepted(const QUuid &id, bool accepted, const QString &reason)
{
    auto it = std::find_if(m_metadata.calibrationShots.begin(),
                           m_metadata.calibrationShots.end(),
                           [&id](const CaptureShot &shot) { return shot.id == id; });
    if (it == m_metadata.calibrationShots.end()) {
        return false;
    }

    CaptureShot previous = *it;
    it->accepted = accepted;
    it->rejectionReason = accepted ? QString() : reason;

    QString error;
    if (!save(&error)) {
        *it = previous;
        Logger::error(error.isEmpty() ? QStringLiteral("Failed to persist calibration shot update") : error);
        return false;
    }

    return true;
}

bool ProjectSession::updateCalibrationShotMetadata(const QUuid &id, const QVariantMap &metadata)
{
    auto it = std::find_if(m_metadata.calibrationShots.begin(),
                           m_metadata.calibrationShots.end(),
                           [&id](const CaptureShot &shot) { return shot.id == id; });
    if (it == m_metadata.calibrationShots.end()) {
        return false;
    }

    CaptureShot previous = *it;
    it->metadata = metadata;

    QString error;
    if (!save(&error)) {
        *it = previous;
        Logger::error(error.isEmpty() ? QStringLiteral("Failed to persist calibration metadata update") : error);
        return false;
    }

    return true;
}

QVector<ProjectSession::LaserFrame> ProjectSession::laserFrames() const
{
    return m_metadata.laserFrames;
}

ProjectSession::LaserFrame ProjectSession::recordLaserFrame(const QString &absolutePath,
                                                            const QVariantMap &annotations)
{
    if (m_rootPath.isEmpty()) {
        Logger::error(QStringLiteral("Cannot record laser frame without a project root"));
        return {};
    }

    ensureScaffold();

    const LaserFrame frame = makeLaserFrame(absolutePath, annotations);
    m_metadata.laserFrames.append(frame);

    QString error;
    if (!save(&error)) {
        m_metadata.laserFrames.removeLast();
        Logger::error(error.isEmpty() ? QStringLiteral("Failed to save session after recording laser frame")
                                      : error);
        return {};
    }

    return frame;
}

void ProjectSession::updateLaserPlane(const LaserPlaneEstimate &estimate)
{
    LaserPlaneEstimate previous = m_metadata.laserPlane;
    m_metadata.laserPlane = estimate;

    QString error;
    if (!save(&error)) {
        m_metadata.laserPlane = previous;
        Logger::error(error.isEmpty() ? QStringLiteral("Failed to persist laser plane update") : error);
    }
}

ProjectSession::LaserPlaneEstimate ProjectSession::laserPlane() const
{
    return m_metadata.laserPlane;
}

QString ProjectSession::stageKeyCamera()
{
    return QStringLiteral("camera_tuning");
}

QString ProjectSession::stageKeyCalibration()
{
    return QStringLiteral("calibration_capture");
}

QString ProjectSession::stageKeyLaser()
{
    return QStringLiteral("laser_calibration");
}

QString ProjectSession::toString(DataSource source)
{
    switch (source) {
    case DataSource::LocalDataset:
        return QStringLiteral("local_dataset");
    case DataSource::ConnectedCamera:
        return QStringLiteral("connected_camera");
    }
    return QStringLiteral("local_dataset");
}

QString ProjectSession::toString(StageStatus status)
{
    switch (status) {
    case StageStatus::NotStarted:
        return QStringLiteral("not_started");
    case StageStatus::InProgress:
        return QStringLiteral("in_progress");
    case StageStatus::Completed:
        return QStringLiteral("completed");
    }
    return QStringLiteral("not_started");
}

QString ProjectSession::toString(ProjectStage stage)
{
    switch (stage) {
    case ProjectStage::CameraTuning:
        return stageKeyCamera();
    case ProjectStage::CalibrationCapture:
        return stageKeyCalibration();
    case ProjectStage::LaserCalibration:
        return stageKeyLaser();
    }
    return stageKeyCamera();
}

QString ProjectSession::toString(CapturePose pose)
{
    switch (pose) {
    case CapturePose::Flat:
        return QStringLiteral("flat");
    case CapturePose::TiltUp:
        return QStringLiteral("tilt_up");
    case CapturePose::TiltDown:
        return QStringLiteral("tilt_down");
    case CapturePose::TiltLeft:
        return QStringLiteral("tilt_left");
    case CapturePose::TiltRight:
        return QStringLiteral("tilt_right");
    }
    return QStringLiteral("flat");
}

ProjectSession::DataSource ProjectSession::dataSourceFromString(const QString &value,
                                                                DataSource fallback)
{
    const QString normalized = value.trimmed().toLower();
    if (normalized == QStringLiteral("connected_camera") || normalized == QStringLiteral("camera")) {
        return DataSource::ConnectedCamera;
    }
    if (normalized == QStringLiteral("local_dataset") || normalized == QStringLiteral("local")) {
        return DataSource::LocalDataset;
    }
    return fallback;
}

ProjectSession::StageStatus ProjectSession::stageStatusFromString(const QString &value,
                                                                  StageStatus fallback)
{
    QString normalized = value.trimmed().toLower();
    normalized.replace(QLatin1Char('-'), QLatin1Char('_'));
    normalized.replace(QLatin1Char(' '), QLatin1Char('_'));
    if (normalized == QStringLiteral("in_progress")) {
        return StageStatus::InProgress;
    }
    if (normalized == QStringLiteral("completed") || normalized == QStringLiteral("done")) {
        return StageStatus::Completed;
    }
    if (normalized == QStringLiteral("not_started") || normalized == QStringLiteral("pending")) {
        return StageStatus::NotStarted;
    }
    return fallback;
}

ProjectSession::CapturePose ProjectSession::capturePoseFromString(const QString &value,
                                                                  CapturePose fallback)
{
    QString normalized = value.trimmed().toLower();
    normalized.replace(QLatin1Char('-'), QLatin1Char('_'));
    normalized.replace(QLatin1Char(' '), QLatin1Char('_'));
    if (normalized == QStringLiteral("flat")) {
        return CapturePose::Flat;
    }
    if (normalized == QStringLiteral("tilt_up") || normalized == QStringLiteral("up")) {
        return CapturePose::TiltUp;
    }
    if (normalized == QStringLiteral("tilt_down") || normalized == QStringLiteral("down")) {
        return CapturePose::TiltDown;
    }
    if (normalized == QStringLiteral("tilt_left") || normalized == QStringLiteral("left")) {
        return CapturePose::TiltLeft;
    }
    if (normalized == QStringLiteral("tilt_right") || normalized == QStringLiteral("right")) {
        return CapturePose::TiltRight;
    }
    return fallback;
}

ProjectSession::StageState &ProjectSession::mutableStage(ProjectStage stage)
{
    switch (stage) {
    case ProjectStage::CameraTuning:
        return m_metadata.cameraTuning;
    case ProjectStage::CalibrationCapture:
        return m_metadata.calibrationCapture;
    case ProjectStage::LaserCalibration:
        return m_metadata.laserCalibration;
    }
    Q_ASSERT_X(false, Q_FUNC_INFO, "Unhandled project stage");
    return m_metadata.cameraTuning;
}

const ProjectSession::StageState &ProjectSession::stageConst(ProjectStage stage) const
{
    switch (stage) {
    case ProjectStage::CameraTuning:
        return m_metadata.cameraTuning;
    case ProjectStage::CalibrationCapture:
        return m_metadata.calibrationCapture;
    case ProjectStage::LaserCalibration:
        return m_metadata.laserCalibration;
    }
    Q_ASSERT_X(false, Q_FUNC_INFO, "Unhandled project stage");
    return m_metadata.cameraTuning;
}

ProjectSession::CaptureShot ProjectSession::makeShotRecord(int gridRow,
                                                           int gridCol,
                                                           CapturePose pose,
                                                           const QString &absolutePath,
                                                           const QVariantMap &metadata) const
{
    CaptureShot shot;
    shot.id = QUuid::createUuid();
    shot.capturedAt = QDateTime::currentDateTimeUtc();
    shot.gridRow = gridRow;
    shot.gridCol = gridCol;
    shot.pose = pose;
    shot.metadata = metadata;
    shot.accepted = false;

    QString chosenPath = absolutePath;

    if (!m_rootPath.isEmpty()) {
        const QFileInfo sourceInfo(absolutePath);
        const QString extension = ensureExtension(sourceInfo, QStringLiteral("png"));
        const QString fileName = makeDestFileName(QStringLiteral("calibration"), extension);
        const QString targetPath = calibrationCaptureDir().filePath(fileName);
        if (copyAsset(sourceInfo.absoluteFilePath(), targetPath)) {
            chosenPath = targetPath;
        }
    }

    shot.relativePath = relativePath(chosenPath);
    return shot;
}

ProjectSession::TuningSnapshot ProjectSession::makeTuningSnapshot(const QString &absolutePath,
                                                                  const QVariantMap &metrics) const
{
    TuningSnapshot snapshot;
    snapshot.id = QUuid::createUuid();
    snapshot.capturedAt = QDateTime::currentDateTimeUtc();
    snapshot.metrics = metrics;

    QString chosenPath = absolutePath;

    if (!m_rootPath.isEmpty()) {
        const QFileInfo sourceInfo(absolutePath);
        const QString extension = ensureExtension(sourceInfo, QStringLiteral("png"));
        const QString fileName = makeDestFileName(QStringLiteral("tuning"), extension);
        const QString targetPath = tuningCaptureDir().filePath(fileName);
        if (copyAsset(sourceInfo.absoluteFilePath(), targetPath)) {
            chosenPath = targetPath;
        }
    }

    snapshot.relativePath = relativePath(chosenPath);
    return snapshot;
}

ProjectSession::LaserFrame ProjectSession::makeLaserFrame(const QString &absolutePath,
                                                          const QVariantMap &annotations) const
{
    LaserFrame frame;
    frame.id = QUuid::createUuid();
    frame.capturedAt = QDateTime::currentDateTimeUtc();
    frame.annotations = annotations;

    QString chosenPath = absolutePath;

    if (!m_rootPath.isEmpty()) {
        const QFileInfo sourceInfo(absolutePath);
        const QString extension = ensureExtension(sourceInfo, QStringLiteral("png"));
        const QString fileName = makeDestFileName(QStringLiteral("laser"), extension);
        const QString targetPath = laserCaptureDir().filePath(fileName);
        if (copyAsset(sourceInfo.absoluteFilePath(), targetPath)) {
            chosenPath = targetPath;
        }
    }

    frame.relativePath = relativePath(chosenPath);
    return frame;
}

void ProjectSession::ensureScaffold() const
{
    if (m_rootPath.isEmpty()) {
        return;
    }
    QDir root(m_rootPath);
    const QStringList required {
        QStringLiteral("captures"),
        QStringLiteral("captures/tuning"),
        QStringLiteral("captures/calibration"),
        QStringLiteral("captures/live"),
        QStringLiteral("captures/laser"),
        QStringLiteral("calibration"),
        QStringLiteral("laser"),
        QStringLiteral("logs"),
        QStringLiteral("exports"),
        QStringLiteral("reports"),
        QStringLiteral("config")
    };
    for (const QString &entry : required) {
        root.mkpath(entry);
    }
}

QJsonObject ProjectSession::toJson() const
{
    QJsonObject obj;
    obj.insert(QStringLiteral("project_name"), m_metadata.projectName);
    obj.insert(QStringLiteral("project_id"), m_metadata.projectId);
    obj.insert(QStringLiteral("created_at"), formatDateTime(m_metadata.createdAt));
    obj.insert(QStringLiteral("last_opened_at"), formatDateTime(m_metadata.lastOpenedAt));
    obj.insert(QStringLiteral("data_source"), toString(m_metadata.dataSource));
    if (!m_metadata.cameraVendor.isEmpty()) {
        obj.insert(QStringLiteral("camera_vendor"), m_metadata.cameraVendor);
    }
    if (!m_metadata.cameraModel.isEmpty()) {
        obj.insert(QStringLiteral("camera_model"), m_metadata.cameraModel);
    }
    obj.insert(stageKeyCamera(), stageToJson(m_metadata.cameraTuning));
    obj.insert(stageKeyCalibration(), stageToJson(m_metadata.calibrationCapture));
    obj.insert(stageKeyLaser(), stageToJson(m_metadata.laserCalibration));
    QJsonArray tuningArray;
    for (const TuningSnapshot &snapshot : m_metadata.tuningSnapshots) {
        tuningArray.append(tuningSnapshotToJson(snapshot));
    }
    obj.insert(QStringLiteral("tuning_snapshots"), tuningArray);

    QJsonArray shotsArray;
    for (const CaptureShot &shot : m_metadata.calibrationShots) {
        shotsArray.append(captureShotToJson(shot));
    }
    obj.insert(QStringLiteral("calibration_shots"), shotsArray);

    QJsonArray laserArray;
    for (const LaserFrame &frame : m_metadata.laserFrames) {
        laserArray.append(laserFrameToJson(frame));
    }
    obj.insert(QStringLiteral("laser_frames"), laserArray);

    obj.insert(QStringLiteral("laser_plane"), laserPlaneToJson(m_metadata.laserPlane));
    return obj;
}

void ProjectSession::fromJson(const QJsonObject &obj)
{
    m_metadata.projectName = obj.value(QStringLiteral("project_name")).toString(defaultProjectName());
    m_metadata.projectId = obj.value(QStringLiteral("project_id")).toString(makeProjectId());
    m_metadata.createdAt = parseDateTime(obj.value(QStringLiteral("created_at")).toString());
    if (!m_metadata.createdAt.isValid()) {
        m_metadata.createdAt = QDateTime::currentDateTimeUtc();
    }
    m_metadata.lastOpenedAt = parseDateTime(obj.value(QStringLiteral("last_opened_at")).toString());
    if (!m_metadata.lastOpenedAt.isValid()) {
        m_metadata.lastOpenedAt = m_metadata.createdAt;
    }
    m_metadata.dataSource = dataSourceFromString(obj.value(QStringLiteral("data_source")).toString(), DataSource::LocalDataset);
    m_metadata.cameraVendor = obj.value(QStringLiteral("camera_vendor")).toString();
    m_metadata.cameraModel = obj.value(QStringLiteral("camera_model")).toString();
    m_metadata.cameraTuning = stageFromJson(obj.value(stageKeyCamera()).toObject());
    m_metadata.calibrationCapture = stageFromJson(obj.value(stageKeyCalibration()).toObject());
    m_metadata.laserCalibration = stageFromJson(obj.value(stageKeyLaser()).toObject());

    m_metadata.tuningSnapshots.clear();
    const QJsonArray tuningArray = obj.value(QStringLiteral("tuning_snapshots")).toArray();
    for (const QJsonValue &value : tuningArray) {
        if (value.isObject()) {
            m_metadata.tuningSnapshots.append(tuningSnapshotFromJson(value.toObject()));
        }
    }

    m_metadata.calibrationShots.clear();
    const QJsonArray shotsArray = obj.value(QStringLiteral("calibration_shots")).toArray();
    for (const QJsonValue &value : shotsArray) {
        if (value.isObject()) {
            m_metadata.calibrationShots.append(captureShotFromJson(value.toObject()));
        }
    }

    m_metadata.laserFrames.clear();
    const QJsonArray laserArray = obj.value(QStringLiteral("laser_frames")).toArray();
    for (const QJsonValue &value : laserArray) {
        if (value.isObject()) {
            m_metadata.laserFrames.append(laserFrameFromJson(value.toObject()));
        }
    }

    if (obj.value(QStringLiteral("laser_plane")).isObject()) {
        m_metadata.laserPlane = laserPlaneFromJson(obj.value(QStringLiteral("laser_plane")).toObject());
    } else {
        m_metadata.laserPlane = {};
    }
}

QString toString(ProjectSession::DataSource source)
{
    return ProjectSession::toString(source);
}

ProjectSession::DataSource dataSourceFromString(const QString &value,
                                                ProjectSession::DataSource fallback)
{
    return ProjectSession::dataSourceFromString(value, fallback);
}

QString toString(ProjectSession::StageStatus status)
{
    return ProjectSession::toString(status);
}

ProjectSession::StageStatus stageStatusFromString(const QString &value,
                                                  ProjectSession::StageStatus fallback)
{
    return ProjectSession::stageStatusFromString(value, fallback);
}

QString toString(ProjectSession::CapturePose pose)
{
    return ProjectSession::toString(pose);
}

ProjectSession::CapturePose capturePoseFromString(const QString &value,
                                                  ProjectSession::CapturePose fallback)
{
    return ProjectSession::capturePoseFromString(value, fallback);
}

} // namespace mycalib
