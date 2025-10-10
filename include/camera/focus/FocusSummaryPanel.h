#pragma once

#include <QDateTime>
#include <QFrame>
#include <QVariantMap>
#include <QVector>
#include <deque>

#include "camera/focus/FocusEvaluator.h"

class QLabel;
class QProgressBar;
class QPushButton;
class QTableWidget;
class QTextBrowser;

class FocusSummaryPanel : public QFrame {
    Q_OBJECT
public:
    explicit FocusSummaryPanel(QWidget* parent = nullptr);

    void setMetrics(const FocusEvaluator::Metrics& metrics);
    void setRoiInfo(const QSize& frameSize, const QRect& roi);
    void resetPanel();

    FocusEvaluator::Metrics lastMetrics() const { return m_lastMetrics; }
    FocusEvaluator::Metrics bestMetrics() const { return m_bestMetrics; }
    bool hasBaseline() const { return m_hasBaseline; }
    double bestCompositeScore() const { return m_bestComposite; }

    QVariantMap exportMetrics() const;

private Q_SLOTS:
    void handleMarkBest();
    void handleResetBaseline();

private:
    struct HistoryEntry {
        QDateTime timestamp;
        FocusEvaluator::Metrics metrics;
    };

    void buildUi();
    void applyMetrics(const FocusEvaluator::Metrics& metrics);
    void pushHistory(const FocusEvaluator::Metrics& metrics);
    void refreshHistoryTable();
    void updateButtons();
    void updateGuidance(const FocusEvaluator::Metrics& metrics, double previousScore);
    QStringList buildGuidanceLines(const FocusEvaluator::Metrics& metrics, double previousScore) const;
    QString formatPercent(double value) const;

    QLabel* m_metricLap{nullptr};
    QLabel* m_metricTen{nullptr};
    QLabel* m_metricHighFreq{nullptr};
    QLabel* m_metricUniformity{nullptr};
    QLabel* m_metricContrast{nullptr};
    QLabel* m_metricMean{nullptr};
    QLabel* m_metricHighlight{nullptr};
    QLabel* m_metricShadow{nullptr};
    QLabel* m_metricComposite{nullptr};
    QLabel* m_scorePercent{nullptr};
    QLabel* m_roiSummary{nullptr};
    QProgressBar* m_scoreProgress{nullptr};
    QPushButton* m_markBestButton{nullptr};
    QPushButton* m_resetBaselineButton{nullptr};
    QTableWidget* m_historyTable{nullptr};
    QTextBrowser* m_guidanceBox{nullptr};

    FocusEvaluator::Metrics m_lastMetrics{};
    FocusEvaluator::Metrics m_bestMetrics{};
    double m_bestComposite{0.0};
    double m_previousComposite{0.0};
    bool m_hasBaseline{false};
    std::deque<HistoryEntry> m_history;
    const int m_historyLimit{40};
    QSize m_lastFrameSize;
    QRect m_lastRoi;
};
