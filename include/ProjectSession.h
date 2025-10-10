#pragma once

#include <QDateTime>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>
#include <QUuid>
#include <QVector3D>

namespace mycalib {

class ProjectSession : public QObject {
    Q_OBJECT

public:
    enum class DataSource {
        LocalDataset,
        ConnectedCamera
    };

    enum class StageStatus {
        NotStarted,
        InProgress,
        Completed
    };

    enum class ProjectStage {
        CameraTuning,
        CalibrationCapture,
        LaserCalibration
    };

    enum class CapturePose {
        Flat,
        TiltUp,
        TiltDown,
        TiltLeft,
        TiltRight
    };

    struct StageState {
        StageStatus status {StageStatus::NotStarted};
        QDateTime startedAt;
        QDateTime completedAt;
        QString notes;
    };

    struct TuningSnapshot {
        QUuid id;
        QDateTime capturedAt;
        QString relativePath;
        QVariantMap metrics;
    };

    struct CaptureShot {
        QUuid id;
        QDateTime capturedAt;
        int gridRow {0};
        int gridCol {0};
        CapturePose pose {CapturePose::Flat};
        QString relativePath;
        QVariantMap metadata;
        bool accepted {false};
        QString rejectionReason;
    };

    struct LaserFrame {
        QUuid id;
        QDateTime capturedAt;
        QString relativePath;
        QVariantMap annotations;
    };

    struct LaserPlaneEstimate {
        bool solved {false};
        QVector3D normal {0.0f, 0.0f, 1.0f};
        double distance {0.0};
        QVariantMap extra;
    };

    struct Metadata {
        QString projectName;
        QString projectId;
        QDateTime createdAt;
        QDateTime lastOpenedAt;
        DataSource dataSource {DataSource::LocalDataset};
        QString cameraVendor;
        QString cameraModel;
        StageState cameraTuning;
        StageState calibrationCapture;
        StageState laserCalibration;
        QVector<TuningSnapshot> tuningSnapshots;
        QVector<CaptureShot> calibrationShots;
        QVector<LaserFrame> laserFrames;
        LaserPlaneEstimate laserPlane;
    };

    explicit ProjectSession(QObject *parent = nullptr);

    bool initializeNew(const QString &rootDirectory,
                       const QString &projectName,
                       DataSource source,
                       QString *errorMessage = nullptr);

    bool loadExisting(const QString &rootDirectory,
                      QString *errorMessage = nullptr);

    bool save(QString *errorMessage = nullptr);

    const Metadata &metadata() const { return m_metadata; }
    Metadata &metadata() { return m_metadata; }

    QString rootPath() const { return m_rootPath; }
    QString sessionFilePath() const;

    QDir capturesRoot() const;
    QDir tuningCaptureDir() const;
    QDir calibrationCaptureDir() const;
    QDir liveCacheDir() const;
    QDir laserCaptureDir() const;
    QDir calibrationOutputDir() const;
    QDir laserOutputDir() const;
    QDir logsDir() const;
    QDir reportsDir() const;
    QDir exportsDir() const;
    QDir configDir() const;

    QString relativePath(const QString &absolutePath) const;

    bool setDataSource(DataSource source, QString *errorMessage = nullptr);

    StageState stageState(ProjectStage stage) const;
    void updateStageState(ProjectStage stage, const StageState &state, bool touchTimestamps = true);

    QVector<TuningSnapshot> tuningSnapshots() const;
    TuningSnapshot recordTuningSnapshot(const QString &absolutePath,
                                        const QVariantMap &metrics = {});

    QVector<CaptureShot> calibrationShots() const;
    CaptureShot addCalibrationShot(int gridRow,
                                   int gridCol,
                                   CapturePose pose,
                                   const QString &absolutePath,
                                   const QVariantMap &metadata = {});
    bool markCalibrationShotAccepted(const QUuid &id, bool accepted, const QString &reason = {});
    bool updateCalibrationShotMetadata(const QUuid &id, const QVariantMap &metadata);

    QVector<LaserFrame> laserFrames() const;
    LaserFrame recordLaserFrame(const QString &absolutePath,
                                const QVariantMap &annotations = {});
    void updateLaserPlane(const LaserPlaneEstimate &estimate);
    LaserPlaneEstimate laserPlane() const;

    static QString stageKeyCamera();
    static QString stageKeyCalibration();
    static QString stageKeyLaser();

    static QString toString(DataSource source);
    static QString toString(StageStatus status);
    static QString toString(ProjectStage stage);
    static QString toString(CapturePose pose);

    static DataSource dataSourceFromString(const QString &value,
                                           DataSource fallback = DataSource::LocalDataset);
    static StageStatus stageStatusFromString(const QString &value,
                                             StageStatus fallback = StageStatus::NotStarted);
    static CapturePose capturePoseFromString(const QString &value,
                                             CapturePose fallback = CapturePose::Flat);

Q_SIGNALS:
    void metadataChanged();
    void dataSourceChanged(DataSource source);

private:
    QString m_rootPath;
    Metadata m_metadata;

    StageState &mutableStage(ProjectStage stage);
    const StageState &stageConst(ProjectStage stage) const;

    CaptureShot makeShotRecord(int gridRow,
                               int gridCol,
                               CapturePose pose,
                               const QString &absolutePath,
                               const QVariantMap &metadata) const;
    TuningSnapshot makeTuningSnapshot(const QString &absolutePath,
                                      const QVariantMap &metrics) const;
    LaserFrame makeLaserFrame(const QString &absolutePath,
                              const QVariantMap &annotations) const;

    void ensureScaffold() const;
    QJsonObject toJson() const;
    void fromJson(const QJsonObject &obj);
};

QString toString(ProjectSession::DataSource source);
ProjectSession::DataSource dataSourceFromString(const QString &value,
                                                ProjectSession::DataSource fallback);

QString toString(ProjectSession::StageStatus status);
ProjectSession::StageStatus stageStatusFromString(const QString &value,
                                                  ProjectSession::StageStatus fallback);

QString toString(ProjectSession::CapturePose pose);
ProjectSession::CapturePose capturePoseFromString(const QString &value,
                                                  ProjectSession::CapturePose fallback);

} // namespace mycalib
