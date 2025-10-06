#pragma once

#include <memory>

#include <QMainWindow>
#include <QPointer>
#include <QtGlobal>

#include "CalibrationEngine.h"

class QAction;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QSplitter;
class QTabWidget;
class QTextEdit;
class QTreeWidget;
class QTreeWidgetItem;
class QDoubleSpinBox;
class QSpinBox;
class QToolBar;
class QDockWidget;
class QStackedLayout;

namespace mycalib {

class HeatmapView;
class DetectionPreviewWidget;
class Pose3DView;
class ResidualScatterView;
class ImageEvaluationDialog;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private Q_SLOTS:
    void chooseInputDirectory();
    void chooseOutputDirectory();
    void runCalibration();
    void resetUi();
    void exportJson();
    void showParameters();
    void showEvaluationDialog();
    void showAuthorInfo();

    void handleProgress(int processed, int total);
    void handleStatus(const QString &message);
    void handleFinished(const CalibrationOutput &output);
    void handleFailed(const QString &reason);
    void handleDetectionSelectionChanged();

private:
    void setupUi();
    void setupActions();
    void applyTheme();
    void updateSummaryPanel(const CalibrationOutput &output);
    void populateDetectionTree(const CalibrationOutput &output);
    void showHeatmaps(const CalibrationOutput &output);
    void showDetectionPreview(const QString &name);
    void updateDetectionDetailPanel(const DetectionResult *result);
    const DetectionResult *findDetection(const QString &name) const;
    void refreshState(bool running);
    void appendLog(QtMsgType type, const QString &message);
    void ensurePoseView();

    QString m_inputDir;
    QString m_outputDir;

    QAction *m_actionOpenDir {nullptr};
    QAction *m_actionSetOutput {nullptr};
    QAction *m_actionRun {nullptr};
    QAction *m_actionExport {nullptr};
    QAction *m_actionShowParameters {nullptr};
    QAction *m_actionEvaluate {nullptr};
    QAction *m_actionReset {nullptr};
    QAction *m_actionAbout {nullptr};

    QLineEdit *m_inputPathEdit {nullptr};
    QLineEdit *m_outputPathEdit {nullptr};
    QProgressBar *m_progressBar {nullptr};
    QTextEdit *m_logView {nullptr};
    QTreeWidget *m_detectionTree {nullptr};
    HeatmapView *m_heatmapBoard {nullptr};
    HeatmapView *m_heatmapPixel {nullptr};
    ResidualScatterView *m_scatterView {nullptr};
    HeatmapView *m_distortionMap {nullptr};
    QDockWidget *m_dockCoverage {nullptr};
    QDockWidget *m_dockPixel {nullptr};
    QDockWidget *m_dockScatter {nullptr};
    QDockWidget *m_dockDistortion {nullptr};
    QLabel *m_detectionMetaLabel {nullptr};
    QLabel *m_detectionResidualMmLabel {nullptr};
    QLabel *m_detectionResidualPercentLabel {nullptr};
    QTabWidget *m_mainTabs {nullptr};
    QWidget *m_overviewPage {nullptr};
    QWidget *m_imagePage {nullptr};
    QWidget *m_posePage {nullptr};
    QStackedLayout *m_poseStack {nullptr};
    QLabel *m_posePlaceholder {nullptr};
    Pose3DView *m_poseView {nullptr};
    DetectionPreviewWidget *m_detectionPreview {nullptr};
    QDoubleSpinBox *m_diameterSpin {nullptr};
    QDoubleSpinBox *m_spacingSpin {nullptr};
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
    bool m_running {false};
    QString m_lastLogKey;
    QString m_lastLogHtml;
    int m_lastLogRepeat {0};
    int m_lastSortColumn {0};
    Qt::SortOrder m_lastSortOrder {Qt::AscendingOrder};
};

} // namespace mycalib
