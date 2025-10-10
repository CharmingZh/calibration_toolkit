#pragma once

#include <QMainWindow>
#include <QDateTime>
#include <QImage>
#include <QRect>
#include <QStringList>
#include <deque>

#include "camera/focus/FocusEvaluator.h"

class QAction;
class QComboBox;
class QLabel;
class QProgressBar;
class QTableWidget;
class QTextBrowser;
class ImageView;
class VimbaController;

class FocusAssistWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit FocusAssistWindow(QWidget* parent = nullptr);
    ~FocusAssistWindow() override = default;

private Q_SLOTS:
    void onCameraOpened(const QString& id, const QString& model);
    void onCameraClosed();
    void onFrame(const QImage& image);
    void onStats(double fps, double bps);
    void onSelectCamera(int index);
    void onConnectCamera();
    void onDisconnectCamera();
    void onStartStream();
    void onStopStream();
    void onMarkBest();
    void onResetBaseline();
    void onRoiUpdated(const QRect& roi);

private:
    void buildUi();
    void connectControllerSignals();
    void updateActionStates();
    void refreshCameraList();
    void applyMetrics(const FocusEvaluator::Metrics& metrics);
    void appendHistory(const FocusEvaluator::Metrics& metrics);
    void updateHistoryTable();
    QStringList buildGuidance(const FocusEvaluator::Metrics& metrics, double previousComposite) const;
    void pushGuidance(const QStringList& lines);
    QRect currentEvaluationRoi(const QImage& source) const;
    void flashStatus(const QString& message, int timeoutMs = 2500);

    struct HistoryEntry {
        QDateTime timestamp;
        FocusEvaluator::Metrics metrics;
    };

    VimbaController* m_controller{nullptr};
    FocusEvaluator m_evaluator;
    ImageView* m_view{nullptr};
    QComboBox* m_cameraCombo{nullptr};
    QAction* m_actConnect{nullptr};
    QAction* m_actDisconnect{nullptr};
    QAction* m_actStart{nullptr};
    QAction* m_actStop{nullptr};
    QAction* m_actMarkBest{nullptr};
    QAction* m_actResetBaseline{nullptr};
    QLabel* m_connectionLabel{nullptr};
    QLabel* m_statusLabel{nullptr};
    QLabel* m_metricLap{nullptr};
    QLabel* m_metricTen{nullptr};
    QLabel* m_metricHighFreq{nullptr};
    QLabel* m_metricUniformity{nullptr};
    QLabel* m_metricContrast{nullptr};
    QLabel* m_metricMean{nullptr};
    QLabel* m_metricHighlight{nullptr};
    QLabel* m_metricShadow{nullptr};
    QLabel* m_metricComposite{nullptr};
    QProgressBar* m_scoreProgress{nullptr};
    QTableWidget* m_historyTable{nullptr};
    QTextBrowser* m_guidanceBox{nullptr};

    bool m_streaming{false};
    bool m_hasBest{false};
    QRect m_selectedRoi;
    FocusEvaluator::Metrics m_lastMetrics{};
    FocusEvaluator::Metrics m_bestMetrics{};
    double m_bestComposite{0.0};
    double m_prevComposite{0.0};
    std::deque<HistoryEntry> m_history;
    const int m_historyLimit{40};
};
