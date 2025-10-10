#pragma once

#include <array>
#include <memory>

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QMainWindow>
#include <QPointer>
#include <QString>
#include <QVector>

#include "CalibrationEngine.h"
#include "ProjectSession.h"

#ifndef MYCALIB_HAVE_CONNECTED_CAMERA
#define MYCALIB_HAVE_CONNECTED_CAMERA 0
#endif

class QAction;
class QLabel;
class QComboBox;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QSplitter;
class QTextEdit;
class QButtonGroup;
class QCheckBox;
class QToolButton;
class QGroupBox;
class QFrame;
class QTreeWidget;
class QTreeWidgetItem;
class QDoubleSpinBox;
class QListWidget;
class QListWidgetItem;
class QScrollArea;
class QStackedLayout;
class QStackedWidget;
class QTabWidget;
class QFileSystemWatcher;
class QToolBar;
class QTabWidget;
class QVBoxLayout;
class QTableWidget;
class QDialog;

class CameraWindow;

namespace mycalib {

class HeatmapView;
class ResidualScatterView;
class Pose3DView;
class DetectionPreviewWidget;
class ImageEvaluationDialog;
struct DetectionResult;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(ProjectSession *session, QWidget *parent = nullptr);
    ~MainWindow() override;

    static constexpr int kCaptureGridRows = 3;
    static constexpr int kCaptureGridCols = 3;
    static constexpr int kCaptureTargetPerCell = 5;
    static constexpr int kCaptureMinimumPerCell = 3;

private Q_SLOTS:
    void runCalibration();
    void resetUi();
    void exportJson();
    void showParameters();
    void showEvaluationDialog();
    void showAuthorInfo();
    void importLocalImages();
    void openInputLocation();
    void openOutputLocation();
    void handleStageCapturePrimaryAction();
    void showCaptureCoverageDialog();
    void showTuningTimeline();
    void handleModeChanged(int index);
    void importLaserFrames();
    void openLaserCaptureFolder();
    void openLaserOutputFolder();
    void markLaserStageCompleted();

#if MYCALIB_HAVE_CONNECTED_CAMERA
    void connectCamera();
    void disconnectCamera();
    void startCameraStream();
    void stopCameraStream();
    void captureFromCamera();
    void refreshCameraDevices();
    void handleCameraConnectionChanged(bool connected, const QString &id, const QString &model);
    void handleCameraStreamingChanged(bool streaming);
#else
    void connectCamera();
    void disconnectCamera();
    void startCameraStream();
    void stopCameraStream();
    void captureFromCamera();
    void refreshCameraDevices();
    void handleCameraConnectionChanged(bool connected, const QString &id, const QString &model);
    void handleCameraStreamingChanged(bool streaming);
#endif

    void handleProgress(int processed, int total);
    void handleStatus(const QString &message);
    void handleFinished(const CalibrationOutput &output);
    void handleFailed(const QString &reason, const CalibrationOutput &details);
    void handleDetectionSelectionChanged();
    void handleInputDirectoryChanged(const QString &path);
    void handleTuningItemActivated(QTreeWidgetItem *item, int column);
    void markCameraTuningCompleted();
    void openTuningFolder();
    void showTuningControlsPopup();
    void showFeaturePanelPopup();

private:
    void setupUi();
    void setupActions();
    void setupTuningSection();
    void applyTheme();
    void updateSummaryPanel(const CalibrationOutput &output);
    void populateDetectionTree(const CalibrationOutput &output);
    void updateCaptureFeedback(const CalibrationOutput &output);
    void showHeatmaps(const CalibrationOutput &output);
    void regenerateHeatmaps(CalibrationOutput &output);
    bool reloadHeatmapsFromDisk(const QString &outputDir, HeatmapBundle *bundle) const;
    void showDetectionPreview(const QString &name);
    void updateDetectionDetailPanel(const DetectionResult *result);
    const DetectionResult *findDetection(const QString &name) const;
    void refreshState(bool running);
    void appendLog(QtMsgType type, const QString &message);
    void ensurePoseView();
    void refreshModeUi();
    void openDirectory(const QString &path);
    void updateStageNavigator();
    void updateStageOneCard();
    void updateStageTwoCard();
    void refreshCaptureTabs();
    void reconcileStageStates();
    void setStatusChip(QLabel *chip, ProjectSession::StageStatus status, const QString &labelText);
    QString formatStageTimestamp(const QDateTime &dt) const;
    QString stageStateLongSummary(const ProjectSession::StageState &state) const;
    void syncInputWatcher();
    void refreshTuningPanel();
    void updateTuningStageUi();
    void ensureCameraTuningStageStarted();
    void ensureCalibrationStageStarted();
    void ensureLaserStageStarted();
    void persistProjectSummary(bool announce = false);
    QTreeWidgetItem *appendTuningSnapshotRow(const ProjectSession::TuningSnapshot &snapshot);
    QString absoluteSessionPath(const QString &relative) const;
    QString relativeSessionPath(const QString &absolute) const;

    void bindSessionSignals();
    void updateWindowTitle();
    bool eventFilter(QObject *watched, QEvent *event) override;

    QString defaultInputDirectory() const;
    QString defaultOutputDirectory() const;

    void setupCapturePlanner();
    void ensureCapturePlannerMounted();
    void configureCapturePlannerForMode(ProjectSession::DataSource source);
    void refreshCapturePlanFromSession();
    void refreshCapturePlanUi();
    void refreshCaptureGridButtons();
    void updateCaptureSelectionSummary();
    void refreshCapturePoseButtons();
    void updateCaptureOverlay();
    void recomputeCellQuality();
    void showCaptureCellDetails(int row, int col);
    void handleCapturePlanToggled(bool enabled);
    void handleCaptureGridSelection(int id);
    void handleCapturePoseSelection(int id);
    QString capturePoseDisplayName(ProjectSession::CapturePose pose) const;
    QString capturePoseHint(ProjectSession::CapturePose pose) const;
    QString captureCellDisplayName(int row, int col) const;
    QVector<QVector<int>> captureTotalsMatrix() const;
    int captureTotalShots() const;
    bool isCapturePlanActive() const;
    QString debugArtifactRoot() const;
    void materializeDebugArtifacts(CalibrationOutput &output);
    void updateDerivedCoverageFromOutput(const CalibrationOutput &output);
    QVector<ProjectSession::CaptureShot> aggregateCoverageShots() const;
    void updateCameraSnapshotTarget();
    void handleCameraSnapshotCaptured(const QString &filePath);
    void syncCameraActions();
    void updateModeExplainer();
    void updateInputSummary();
    void updateRunAvailability();
    void updateLaserStageUi();
    int countImageFiles(const QString &directory) const;
    bool hasInputImages() const;
    QJsonObject stageStateToJson(const ProjectSession::StageState &state) const;
    QJsonArray calibrationShotsToJson(const QVector<ProjectSession::CaptureShot> &shots) const;
    QJsonArray laserFramesToJson(const QVector<ProjectSession::LaserFrame> &frames) const;
    QJsonObject laserPlaneToJson(const ProjectSession::LaserPlaneEstimate &plane) const;
    QJsonObject calibrationOutputToJson(const CalibrationOutput &output) const;
    QJsonObject buildProjectSummaryJson() const;
    bool writeProjectSummary(const QString &filePath, QString *errorMessage = nullptr) const;
    void persistCalibrationSnapshot(const CalibrationOutput &output) const;
    void restoreCalibrationSnapshot();
    bool loadCalibrationSnapshot(const QString &filePath, CalibrationOutput *output, QString *outputDir) const;

    QString m_inputDir;
    QString m_outputDir;

    ProjectSession *m_session {nullptr};
    ProjectSession::DataSource m_activeSource {ProjectSession::DataSource::LocalDataset};

    QAction *m_actionImportImages {nullptr};
    QAction *m_toolbarPrimarySeparator {nullptr};
    QAction *m_actionRun {nullptr};
    QAction *m_actionExport {nullptr};
    QAction *m_actionShowParameters {nullptr};
    QAction *m_actionEvaluate {nullptr};
    QAction *m_actionReset {nullptr};
    QAction *m_actionAbout {nullptr};

    QComboBox *m_modeCombo {nullptr};
    QLineEdit *m_inputPathEdit {nullptr};
    QLineEdit *m_outputPathEdit {nullptr};
    QPushButton *m_importButton {nullptr};
    QPushButton *m_openInputButton {nullptr};
    QPushButton *m_openOutputButton {nullptr};
    QLabel *m_boardInfoLabel {nullptr};
    QLabel *m_modeChip {nullptr};
    QLabel *m_modeHeadline {nullptr};
    QLabel *m_modeDescription {nullptr};
    QLabel *m_inputStatusLabel {nullptr};
    QProgressBar *m_progressBar {nullptr};
    QTextEdit *m_logView {nullptr};
    QTreeWidget *m_detectionTree {nullptr};
    QGroupBox *m_stageTuningBox {nullptr};
    QLabel *m_stageTuningStatusChip {nullptr};
    QLabel *m_stageTuningSummaryLabel {nullptr};
    QLabel *m_stageTuningNotesLabel {nullptr};
    QPushButton *m_stageTuningReviewButton {nullptr};
    QPushButton *m_stageTuningFolderButton {nullptr};
    QPushButton *m_stageTuningCompleteButton {nullptr};
    QGroupBox *m_stageCaptureBox {nullptr};
    QLabel *m_stageCaptureStatusChip {nullptr};
    QLabel *m_stageCaptureSummaryLabel {nullptr};
    QLabel *m_stageCaptureTodoLabel {nullptr};
    QLabel *m_stageCaptureModeHint {nullptr};
    QProgressBar *m_stageCaptureProgress {nullptr};
    QPushButton *m_stageCapturePrimaryButton {nullptr};
    QPushButton *m_stageCaptureSecondaryButton {nullptr};
    QGroupBox *m_captureFeedbackBox {nullptr};
    QLabel *m_captureFeedbackSummary {nullptr};
    QLabel *m_captureFeedbackPose {nullptr};
    QLabel *m_captureFeedbackActions {nullptr};
    QGroupBox *m_laserStageBox {nullptr};
    QLabel *m_laserStageStatusChip {nullptr};
    QLabel *m_laserStageStatusLabel {nullptr};
    QLabel *m_laserStageFrameLabel {nullptr};
    QLabel *m_laserStageHintLabel {nullptr};
    QPushButton *m_importLaserButton {nullptr};
    QPushButton *m_openLaserCaptureButton {nullptr};
    QPushButton *m_openLaserOutputButton {nullptr};
    QPushButton *m_markLaserCompletedButton {nullptr};
    HeatmapView *m_heatmapBoard {nullptr};
    HeatmapView *m_heatmapPixel {nullptr};
    ResidualScatterView *m_scatterView {nullptr};
    HeatmapView *m_distortionMap {nullptr};
    QLabel *m_detectionMetaLabel {nullptr};
    QLabel *m_detectionResidualMmLabel {nullptr};
    QLabel *m_detectionResidualPercentLabel {nullptr};
    QSplitter *m_stageSplitter {nullptr};
    QListWidget *m_stageNavigator {nullptr};
    QStackedWidget *m_stageStack {nullptr};
    QListWidgetItem *m_stageOverviewItem {nullptr};
    QListWidgetItem *m_stageTuningItem {nullptr};
    QListWidgetItem *m_stageCaptureItem {nullptr};
    QListWidgetItem *m_stageAnalysisItem {nullptr};
    QScrollArea *m_stageOverviewScroll {nullptr};
    QWidget *m_stageOverviewPage {nullptr};
    QScrollArea *m_stageTuningScroll {nullptr};
    QWidget *m_stageTuningPage {nullptr};
    QWidget *m_tuningMainColumn {nullptr};
    QVBoxLayout *m_tuningMainLayout {nullptr};
    QWidget *m_stageCapturePage {nullptr};
    QWidget *m_stageAnalysisPage {nullptr};
    QTabWidget *m_analysisTabs {nullptr};
    QTabWidget *m_captureTabs {nullptr};
    QWidget *m_datasetPage {nullptr};
    QFrame *m_capturePlannerWidget {nullptr};
    QWidget *m_capturePlannerHost {nullptr};
    QVBoxLayout *m_capturePlannerHostLayout {nullptr};
    QCheckBox *m_capturePlanToggle {nullptr};
    QButtonGroup *m_captureGridGroup {nullptr};
    QButtonGroup *m_capturePoseGroup {nullptr};
    QWidget *m_capturePoseContainer {nullptr};
    QLabel *m_capturePlanTitleLabel {nullptr};
    QLabel *m_capturePlanHintLabel {nullptr};
    QLabel *m_capturePlanSelectionLabel {nullptr};
    QLabel *m_capturePlanPoseLabel {nullptr};
    QLabel *m_capturePlanSummary {nullptr};
    QPushButton *m_capturePlanDetailButton {nullptr};
    struct CaptureCellState {
        int total {0};
        std::array<int, 5> poseCounts {{0, 0, 0, 0, 0}};
    };
    std::array<std::array<CaptureCellState, 3>, 3> m_capturePlanState {};
    int m_captureSelectedRow {0};
    int m_captureSelectedCol {0};
    ProjectSession::CapturePose m_captureSelectedPose {ProjectSession::CapturePose::Flat};
    QVector<ProjectSession::CaptureShot> m_datasetDerivedShots;
    struct CellSampleInfo {
        QString displayName;
        QString key;
        bool kept {false};
        double meanErrorPx {0.0};
        double maxErrorPx {0.0};
        double residualMm {0.0};
    };
    struct CellQuality {
        int kept {0};
        int removed {0};
        int pending {0};
        double sumMeanErrorPx {0.0};
        double maxMeanErrorPx {0.0};
        double sumResidualMm {0.0};
        double maxResidualMm {0.0};
        QVector<CellSampleInfo> samples;
        QVector<QString> pendingSamples;

        void clear()
        {
            kept = removed = pending = 0;
            sumMeanErrorPx = maxMeanErrorPx = sumResidualMm = maxResidualMm = 0.0;
            samples.clear();
            pendingSamples.clear();
        }

        int total() const { return kept + removed; }
        double averageMeanErrorPx() const { return total() > 0 ? sumMeanErrorPx / total() : 0.0; }
        double averageResidualMm() const { return total() > 0 ? sumResidualMm / total() : 0.0; }
    };
    QString captureCellStyle(const CellQuality &quality,
                             int totalShots,
                             bool meetsMinimum,
                             bool meetsRecommended) const;
    std::array<std::array<CellQuality, 3>, 3> m_cellQuality;
#if MYCALIB_HAVE_CONNECTED_CAMERA
    CameraWindow *m_cameraWindow {nullptr};
    QWidget *m_tuningControlsPanel {nullptr};
    QToolButton *m_tuningConnectButton {nullptr};
    QToolButton *m_tuningDisconnectButton {nullptr};
    QToolButton *m_tuningStartButton {nullptr};
    QToolButton *m_tuningStopButton {nullptr};
    QToolButton *m_tuningSnapshotButton {nullptr};
    QToolButton *m_tuningRefreshButton {nullptr};
    QLabel *m_tuningCameraStateChip {nullptr};
    QLabel *m_tuningStreamStateChip {nullptr};
    QLabel *m_tuningDeviceLabel {nullptr};
    QComboBox *m_tuningDeviceCombo {nullptr};
    QPushButton *m_tuningControlPopupButton {nullptr};
    QPushButton *m_tuningFeaturePopupButton {nullptr};
    QPointer<QDialog> m_tuningControlPopup;
    QPointer<QDialog> m_featurePopup;
    QWidget *m_featurePanelOriginalParent {nullptr};
    int m_featurePanelOriginalIndex {-1};
#endif
    QWidget *m_poseTab {nullptr};
    QStackedLayout *m_poseStack {nullptr};
    QLabel *m_posePlaceholder {nullptr};
    Pose3DView *m_poseView {nullptr};
    DetectionPreviewWidget *m_detectionPreview {nullptr};
    QToolBar *m_toolBar {nullptr};

    QLabel *m_metricTotalImages {nullptr};
    QLabel *m_metricKeptImages {nullptr};
    QLabel *m_metricRemovedImages {nullptr};
    QLabel *m_metricRms {nullptr};
    QLabel *m_metricMeanPx {nullptr};
    QLabel *m_metricMedianPx {nullptr};
    QLabel *m_metricP95Px {nullptr};
    QLabel *m_metricMaxPx {nullptr};
    QLabel *m_metricMeanResidualMm {nullptr};
    QLabel *m_metricMeanResidualPercent {nullptr};

    QPointer<CalibrationEngine> m_engine;
    QPointer<ImageEvaluationDialog> m_evaluationDialog;
    CalibrationOutput m_lastOutput;
    bool m_lastOutputFromSnapshot {false};
    bool m_running {false};
    QString m_lastLogKey;
    QString m_lastLogHtml;
    int m_lastLogRepeat {0};
    int m_lastSortColumn {0};
    Qt::SortOrder m_lastSortOrder {Qt::AscendingOrder};
    QFileSystemWatcher *m_inputWatcher {nullptr};
    int m_lastInputImageCount {0};

#if MYCALIB_HAVE_CONNECTED_CAMERA
    QAction *m_toolbarCameraSeparator {nullptr};
    QAction *m_actionCameraConnect {nullptr};
    QAction *m_actionCameraDisconnect {nullptr};
    QAction *m_actionCameraStart {nullptr};
    QAction *m_actionCameraStop {nullptr};
    QAction *m_actionCameraSnapshot {nullptr};
    QAction *m_actionCameraRefresh {nullptr};
    QLabel *m_cameraStatusChip {nullptr};
    bool m_cameraConnected {false};
    bool m_cameraStreaming {false};
    QString m_cameraDescriptor;
    QString m_cameraSnapshotPrefix;
    int m_cameraSnapshotSequence {0};
#endif
    QTreeWidget *m_tuningTimeline {nullptr};
    QLabel *m_tuningStageLabel {nullptr};
    QLabel *m_tuningCountLabel {nullptr};
    QPushButton *m_markTuningDoneButton {nullptr};
    QGroupBox *m_tuningTimelineBox {nullptr};
};

} // namespace mycalib
