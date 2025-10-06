#pragma once

#include <QDialog>
#include <QList>
#include <QSet>
#include <QVector>
#include <QString>

#include <vector>

#include <opencv2/core.hpp>

#include "BoardSpec.h"
#include "CalibrationEngine.h"
#include "ResidualScatterView.h"

class QListWidget;
class QListWidgetItem;
class QPushButton;
class QLabel;
class QTabWidget;
class QCheckBox;
class QScrollArea;
class QTableWidget;
class QProgressBar;
class QSplitter;

namespace mycalib {

class SyncedImageView;
class ViewSyncController;

class ImageEvaluationDialog : public QDialog {
    Q_OBJECT

public:
    ImageEvaluationDialog(const CalibrationOutput &calibration,
                          const BoardSpec &boardSpec,
                          QWidget *parent = nullptr);
    ~ImageEvaluationDialog() override;

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private Q_SLOTS:
    void handleAddImages();
    void handleAddFolder();
    void handleClear();
    void handleSelectionChanged();
    void handleToggleChanged();

private:
    struct EvaluationMetrics {
        double meanPx {0.0};
        double rmsPx {0.0};
        double medianPx {0.0};
        double p95Px {0.0};
        double maxPx {0.0};
        double meanMm {0.0};
        double rmsMm {0.0};
        double maxMm {0.0};
        int sampleCount {0};
        double sumPx {0.0};
        double sumSqPx {0.0};
    };

    struct EvaluationResult {
        QString filePath;
        QString displayName;
        bool success {false};
        QString message;
        cv::Size resolution {0, 0};

        cv::Mat originalBgr;
        cv::Mat undistortedBgr;
        cv::Mat whiteRegionMask;
        cv::Mat whiteRegionMaskUndistorted;

        std::vector<cv::Point2f> imagePoints;
        std::vector<cv::Point2f> undistortedPoints;
        std::vector<cv::Vec2i> logicalIndices;
        std::vector<cv::Point3f> objectPoints;
    std::vector<cv::Point2f> bigCirclePoints;
    std::vector<cv::Point2f> bigCirclePointsUndistorted;
    std::vector<float> circleRadii;
    std::vector<float> circleRadiiUndistorted;
    std::vector<float> bigCircleRadii;
    std::vector<float> bigCircleRadiiUndistorted;

        bool poseValid {false};
    cv::Matx33d rotation {cv::Matx33d::eye()};
    cv::Vec3d rotationVector {0.0, 0.0, 0.0};
    cv::Vec3d translation {0.0, 0.0, 0.0};

        EvaluationMetrics calibrated;
        EvaluationMetrics uncorrected;
        std::vector<double> residualsPx;
        std::vector<double> residualsMm;
        std::vector<double> residualsPxNoCorrection;
        std::vector<ResidualScatterView::Sample> scatterSamples;
    };

    void setupUi();
    QWidget *createLeftPanel();
    QWidget *createAnnotatedTab();
    QWidget *createComparisonTab();
    QWidget *createScatterTab();
    QWidget *createMetricsTab();
    void setupTabs();

    void enqueuePaths(const QStringList &paths);
    EvaluationResult evaluateImage(const QString &path) const;
    void applyResult(int index, EvaluationResult &&result);

    void refreshViews();
    void updateAnnotatedView(const EvaluationResult &result);
    void updateComparisonView(const EvaluationResult &result);
    void updateScatterView(const EvaluationResult &result);
    void updateMetricsTable(const EvaluationResult &result);
    void updateListItem(int index);
    void updateSummaryBanner();
    void updateControlsState();

    static EvaluationMetrics computeMetrics(const std::vector<double> &residualsPx,
                                            const std::vector<double> &residualsMm);
    static double percentile(const std::vector<double> &values, double q);
    cv::Mat renderAnnotated(const EvaluationResult &result,
                            bool showGrid,
                            bool showMask) const;
    cv::Mat renderUndistortedAnnotated(const EvaluationResult &result,
                                       bool showGrid,
                                       bool showMask) const;
    void drawAxes(cv::Mat &canvas,
                  const EvaluationResult &result,
                  bool undistorted) const;
    static void drawGrid(cv::Mat &canvas,
                         const EvaluationResult &result,
                         const BoardSpec &spec,
                         bool useUndistortedPoints);
    static cv::Mat blendMask(const cv::Mat &base,
                             const cv::Mat &mask,
                             const cv::Mat &boardMask,
                             double opacity);
    static cv::Mat buildBoardMask(const EvaluationResult &result,
                                  const cv::Size &size,
                                  bool useUndistortedPoints);
    static QImage toQImage(const cv::Mat &mat);
    static QString formatNumber(double value, int precision = 3);

    CalibrationOutput m_calibration;
    BoardSpec m_boardSpec;
    cv::Mat m_cameraMatrix;
    cv::Mat m_distCoeffs;

    QListWidget *m_itemList {nullptr};
    QPushButton *m_btnAddImages {nullptr};
    QPushButton *m_btnAddFolder {nullptr};
    QPushButton *m_btnClear {nullptr};
    QLabel *m_hintLabel {nullptr};
    QLabel *m_summaryLabel {nullptr};

    QCheckBox *m_toggleGrid {nullptr};
    QCheckBox *m_toggleMask {nullptr};
    QLabel *m_annotatedLabel {nullptr};
    QScrollArea *m_annotatedScroll {nullptr};
    SyncedImageView *m_originalView {nullptr};
    SyncedImageView *m_undistortedView {nullptr};
    ViewSyncController *m_compareSync {nullptr};
    ResidualScatterView *m_scatterView {nullptr};
    QTableWidget *m_metricsTable {nullptr};
    QProgressBar *m_progress {nullptr};
    QTabWidget *m_contentTabs {nullptr};

    QVector<EvaluationResult> m_results;
    QSet<QString> m_loadedPaths;
    int m_pendingJobs {0};
};

} // namespace mycalib
