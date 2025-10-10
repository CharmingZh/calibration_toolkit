#include "camera/focus/FocusSummaryPanel.h"

#include <QAbstractItemView>
#include <QGridLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextBrowser>
#include <QVBoxLayout>
#include <QSizePolicy>

#include <algorithm>
#include <cmath>

namespace {
QLabel* makeMetricLabel(QWidget* parent) {
    auto* label = new QLabel(QStringLiteral("--"), parent);
    label->setObjectName(QStringLiteral("FocusMetric"));
    label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    QFont f = label->font();
    f.setPointSizeF(f.pointSizeF() + 3.0);
    f.setBold(true);
    label->setFont(f);
    return label;
}
}

FocusSummaryPanel::FocusSummaryPanel(QWidget* parent)
    : QFrame(parent) {
    setObjectName(QStringLiteral("FocusSummaryPanel"));
    setFrameShape(QFrame::NoFrame);
    setAutoFillBackground(false);

    buildUi();
    resetPanel();
}

void FocusSummaryPanel::buildUi() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(18, 18, 18, 18);
    root->setSpacing(14);

    auto* header = new QLabel(tr("对焦助手"), this);
    QFont headerFont = header->font();
    headerFont.setPointSizeF(headerFont.pointSizeF() + 4.0);
    headerFont.setBold(true);
    header->setFont(headerFont);
    header->setStyleSheet(QStringLiteral("color:#f2f6ff;"));
    root->addWidget(header);

    m_roiSummary = new QLabel(tr("ROI：全图"), this);
    m_roiSummary->setStyleSheet(QStringLiteral("color:#9aaacc;font-size:12.5px;"));
    m_roiSummary->setWordWrap(true);
    root->addWidget(m_roiSummary);

    auto* metricsGrid = new QGridLayout();
    metricsGrid->setContentsMargins(0, 0, 0, 0);
    metricsGrid->setHorizontalSpacing(18);
    metricsGrid->setVerticalSpacing(16);
    const int columnCount = 3;
    for (int i = 0; i < columnCount; ++i) {
        metricsGrid->setColumnStretch(i, 1);
    }

    int metricIndex = 0;
    auto addMetricBlock = [&](const QString& labelText, QLabel*& valueLabel, const QString& unitText = QString()) {
        auto* block = new QWidget(this);
        auto* blockLayout = new QVBoxLayout(block);
        blockLayout->setContentsMargins(0, 0, 0, 0);
        blockLayout->setSpacing(6);

        auto* name = new QLabel(labelText, block);
        name->setStyleSheet(QStringLiteral("color:#8ea2c0;font-size:13px;font-weight:600;"));
        blockLayout->addWidget(name);

        auto* valueRow = new QHBoxLayout();
        valueRow->setContentsMargins(0, 0, 0, 0);
        valueRow->setSpacing(6);
        valueLabel = makeMetricLabel(block);
        valueRow->addWidget(valueLabel);
        if (!unitText.isEmpty()) {
            auto* unit = new QLabel(unitText, block);
            unit->setStyleSheet(QStringLiteral("color:#6d7fa6;font-size:12px;font-weight:600;"));
            unit->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            valueRow->addWidget(unit);
        }
        valueRow->addStretch(1);
        blockLayout->addLayout(valueRow);

        const int row = metricIndex / columnCount;
        const int column = metricIndex % columnCount;
        metricsGrid->addWidget(block, row, column);
        ++metricIndex;
    };

    addMetricBlock(tr("Laplacian Var"), m_metricLap);
    addMetricBlock(tr("Tenengrad / Sobel"), m_metricTen);
    addMetricBlock(tr("高频能量"), m_metricHighFreq, tr("%"));
    addMetricBlock(tr("方向均衡"), m_metricUniformity, tr("%"));
    addMetricBlock(tr("对比度 σ"), m_metricContrast);
    addMetricBlock(tr("平均灰度"), m_metricMean);
    addMetricBlock(tr("高光占比"), m_metricHighlight, tr("%"));
    addMetricBlock(tr("暗部占比"), m_metricShadow, tr("%"));

    root->addLayout(metricsGrid);

    auto* scoreCard = new QFrame(this);
    scoreCard->setObjectName(QStringLiteral("FocusScoreCard"));
    auto* scoreLayout = new QVBoxLayout(scoreCard);
    scoreLayout->setContentsMargins(16, 14, 16, 16);
    scoreLayout->setSpacing(12);

    auto* scoreHeaderRow = new QHBoxLayout();
    scoreHeaderRow->setContentsMargins(0, 0, 0, 0);
    scoreHeaderRow->setSpacing(8);
    auto* scoreLabel = new QLabel(tr("综合评分"), scoreCard);
    scoreLabel->setStyleSheet(QStringLiteral("color:#d5e6ff;font-size:14px;font-weight:600;"));
    scoreHeaderRow->addWidget(scoreLabel);
    scoreHeaderRow->addStretch(1);
    m_scorePercent = new QLabel(QStringLiteral("--%"), scoreCard);
    m_scorePercent->setObjectName(QStringLiteral("FocusScorePercent"));
    m_scorePercent->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_scorePercent->setStyleSheet(QStringLiteral("color:#9dc4ff;font-size:13px;font-weight:600;"));
    scoreHeaderRow->addWidget(m_scorePercent);
    scoreLayout->addLayout(scoreHeaderRow);

    auto* scoreValueRow = new QHBoxLayout();
    scoreValueRow->setContentsMargins(0, 0, 0, 0);
    scoreValueRow->setSpacing(12);
    m_metricComposite = makeMetricLabel(scoreCard);
    scoreValueRow->addWidget(m_metricComposite, 0, Qt::AlignLeft);
    scoreValueRow->addStretch(1);
    scoreLayout->addLayout(scoreValueRow);

    m_scoreProgress = new QProgressBar(scoreCard);
    m_scoreProgress->setRange(0, 120);
    m_scoreProgress->setTextVisible(false);
    m_scoreProgress->setFixedHeight(26);
    m_scoreProgress->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    scoreLayout->addWidget(m_scoreProgress);

    root->addWidget(scoreCard);

    auto* buttonRow = new QHBoxLayout();
    buttonRow->setSpacing(10);
    m_markBestButton = new QPushButton(tr("标记最佳"), this);
    m_resetBaselineButton = new QPushButton(tr("重置基线"), this);
    buttonRow->addWidget(m_markBestButton);
    buttonRow->addWidget(m_resetBaselineButton);
    buttonRow->addStretch(1);
    root->addLayout(buttonRow);

    connect(m_markBestButton, &QPushButton::clicked, this, &FocusSummaryPanel::handleMarkBest);
    connect(m_resetBaselineButton, &QPushButton::clicked, this, &FocusSummaryPanel::handleResetBaseline);

    m_historyTable = new QTableWidget(0, 6, this);
    m_historyTable->setHorizontalHeaderLabels({tr("时间"), tr("评分"), tr("Laplacian"), tr("Sobel"), tr("高频"), tr("对比度")});
    m_historyTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_historyTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_historyTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_historyTable->setMinimumHeight(160);
    root->addWidget(m_historyTable, 1);

    m_guidanceBox = new QTextBrowser(this);
    m_guidanceBox->setMinimumHeight(150);
    m_guidanceBox->setStyleSheet(QStringLiteral("color:#c8d9f3;font-size:13px;line-height:1.6;"));
    m_guidanceBox->setPlaceholderText(tr("实时建议将在此处显示"));
    root->addWidget(m_guidanceBox);

    setStyleSheet(QStringLiteral(
        "#FocusSummaryPanel {"
        "  background: rgba(13, 18, 28, 0.92);"
        "  border: 1px solid rgba(88, 172, 255, 0.14);"
        "  border-radius: 16px;"
        "}"
        "#FocusScoreCard {"
        "  background: rgba(20, 30, 48, 0.85);"
        "  border: 1px solid rgba(88, 172, 255, 0.18);"
        "  border-radius: 14px;"
        "}"
        "#FocusMetric {"
        "  color: #f0f6ff;"
        "}"
    "QProgressBar {"
    "  background: rgba(12, 20, 34, 0.6);"
    "  color: #e6f1ff;"
    "  border: 1px solid rgba(88, 172, 255, 0.25);"
    "  border-radius: 9px;"
    "  font-size: 12px;"
    "}"
    "QProgressBar::chunk {"
    "  background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #52e5ff, stop:1 #5c7dff);"
    "  border-radius: 8px;"
    "}"
    "QPushButton {"
    "  background: rgba(80, 143, 255, 0.28);"
    "  color: #f2f7ff;"
    "  border: 1px solid rgba(120, 196, 255, 0.45);"
    "  border-radius: 12px;"
    "  padding: 8px 14px;"
    "  font-size: 13px;"
    "}"
    "QPushButton:disabled {"
    "  color: rgba(210, 224, 250, 0.55);"
    "  border-color: rgba(120, 196, 255, 0.25);"
    "  background: rgba(40, 68, 110, 0.4);"
    "}"
    ));
}

void FocusSummaryPanel::setMetrics(const FocusEvaluator::Metrics& metrics) {
    if (!metrics.valid) {
        updateButtons();
        return;
    }
    applyMetrics(metrics);
}

void FocusSummaryPanel::applyMetrics(const FocusEvaluator::Metrics& metrics) {
    auto setLabelValue = [](QLabel* label, double value, int precision = 2, double scale = 1.0) {
        if (!label) {
            return;
        }
        const QString text = QString::number(value * scale, 'f', precision);
        label->setText(text);
    };

    setLabelValue(m_metricLap, metrics.laplacianVariance, 1);
    setLabelValue(m_metricTen, metrics.tenengrad, 1);
    setLabelValue(m_metricHighFreq, metrics.highFrequencyRatio, 1, 100.0);
    setLabelValue(m_metricUniformity, metrics.gradientUniformity, 1, 100.0);
    setLabelValue(m_metricContrast, metrics.contrast);
    setLabelValue(m_metricMean, metrics.meanIntensity, 1);
    setLabelValue(m_metricHighlight, metrics.highlightRatio, 2);
    setLabelValue(m_metricShadow, metrics.shadowRatio, 2);
    setLabelValue(m_metricComposite, metrics.compositeScore, 1);

    if (!m_hasBaseline || metrics.compositeScore > m_bestComposite) {
        m_bestComposite = metrics.compositeScore;
        m_bestMetrics = metrics;
        m_hasBaseline = true;
    }

    const double reference = std::max(m_bestComposite, 1.0);
    const double relative = std::clamp(metrics.compositeScore / reference * 100.0, 0.0, 120.0);
    if (m_scoreProgress) {
        m_scoreProgress->setValue(static_cast<int>(std::round(relative)));
    }
    if (m_scorePercent) {
        m_scorePercent->setText(QStringLiteral("%1%")
            .arg(QString::number(relative, 'f', 1)));
    }

    pushHistory(metrics);
    updateGuidance(metrics, m_previousComposite);
    m_previousComposite = metrics.compositeScore;
    m_lastMetrics = metrics;

    updateButtons();
}

void FocusSummaryPanel::setRoiInfo(const QSize& frameSize, const QRect& roi) {
    m_lastFrameSize = frameSize;
    m_lastRoi = roi;

    QString text;
    if (roi.isNull() || roi.width() < 8 || roi.height() < 8) {
        text = tr("ROI：全图（%1 × %2）")
                   .arg(frameSize.width())
                   .arg(frameSize.height());
    } else {
        text = tr("ROI：(%1, %2) · %3 × %4 / 帧尺寸 %5 × %6")
                   .arg(roi.x())
                   .arg(roi.y())
                   .arg(roi.width())
                   .arg(roi.height())
                   .arg(frameSize.width())
                   .arg(frameSize.height());
    }
    if (m_roiSummary) {
        m_roiSummary->setText(text);
    }
}

void FocusSummaryPanel::pushHistory(const FocusEvaluator::Metrics& metrics) {
    HistoryEntry entry;
    entry.timestamp = QDateTime::currentDateTime();
    entry.metrics = metrics;
    m_history.push_front(entry);
    while (static_cast<int>(m_history.size()) > m_historyLimit) {
        m_history.pop_back();
    }
    refreshHistoryTable();
}

void FocusSummaryPanel::refreshHistoryTable() {
    if (!m_historyTable) {
        return;
    }
    m_historyTable->setRowCount(static_cast<int>(m_history.size()));
    int row = 0;
    for (const auto& entry : m_history) {
        auto addItem = [&](int column, const QString& value) {
            auto* item = new QTableWidgetItem(value);
            item->setTextAlignment(Qt::AlignCenter);
            m_historyTable->setItem(row, column, item);
        };

        addItem(0, entry.timestamp.toLocalTime().toString(QStringLiteral("HH:mm:ss")));
        addItem(1, QString::number(entry.metrics.compositeScore, 'f', 1));
    addItem(2, QString::number(entry.metrics.laplacianVariance, 'f', 1));
    addItem(3, QString::number(entry.metrics.tenengrad, 'f', 1));
        addItem(4, QString::number(entry.metrics.highFrequencyRatio * 100.0, 'f', 1));
        addItem(5, QString::number(entry.metrics.contrast, 'f', 2));
        ++row;
    }
    if (row > 0) {
        m_historyTable->scrollToTop();
    }
}

void FocusSummaryPanel::updateGuidance(const FocusEvaluator::Metrics& metrics, double previousScore) {
    if (!m_guidanceBox) {
        return;
    }
    const QStringList lines = buildGuidanceLines(metrics, previousScore);
    m_guidanceBox->setPlainText(lines.join(QStringLiteral("\n\n")));
}

QStringList FocusSummaryPanel::buildGuidanceLines(const FocusEvaluator::Metrics& metrics, double previousScore) const {
    QStringList lines;
    const double relative = (m_hasBaseline && m_bestComposite > 0.0)
                                ? metrics.compositeScore / m_bestComposite * 100.0
                                : 100.0;
    lines << tr("当前清晰度评分：%1（占历史最佳 %2%）")
                 .arg(QString::number(metrics.compositeScore, 'f', 1))
                 .arg(QString::number(relative, 'f', 1));

    if (metrics.highlightRatio > 5.0) {
        lines << tr("高光占比 %1% 偏高，建议收小光圈或缩短曝光时间。")
                      .arg(QString::number(metrics.highlightRatio, 'f', 1));
    } else if (metrics.shadowRatio > 12.0) {
        lines << tr("暗部占比 %1% 偏高，可适当开大光圈或延长曝光。")
                      .arg(QString::number(metrics.shadowRatio, 'f', 1));
    } else {
        lines << tr("亮度分布稳定，可继续专注于焦距微调。");
    }

    const double highFreqPct = metrics.highFrequencyRatio * 100.0;
    if (highFreqPct < 18.0) {
        lines << tr("ROI 高频能量 %1% 偏低，尝试让 ROI 覆盖实心圆的清晰边缘，加大对焦步幅。")
                      .arg(QString::number(highFreqPct, 'f', 1));
    } else if (highFreqPct > 32.0) {
        lines << tr("高频细节充足（%1%），边缘锐度表现优秀。")
                      .arg(QString::number(highFreqPct, 'f', 1));
    }

    if (metrics.gradientUniformity < 0.35) {
        lines << tr("梯度方向偏单一，可调整 ROI 让其包含圆靶的完整轮廓段或多枚圆心。");
    } else if (metrics.gradientUniformity > 0.65) {
        lines << tr("方向均衡度 %1% 良好，说明圆靶边缘覆盖充分，可继续微调焦距。")
                      .arg(QString::number(metrics.gradientUniformity * 100.0, 'f', 1));
    }

    if (previousScore > 0.0) {
        const double delta = metrics.compositeScore - previousScore;
        if (delta > 2.5) {
            lines << tr("清晰度正在提升，保持当前调焦方向。");
        } else if (delta < -2.5) {
            lines << tr("清晰度下降，尝试反向微调焦距或重新定位 ROI。");
        } else {
            lines << tr("清晰度变化平稳，可尝试轻微调整光圈以优化景深。");
        }
    }

    if (metrics.compositeScore >= m_bestComposite - 1.0) {
        lines << tr("已经非常接近最佳状态，请锁紧焦距并记录当前镜头参数。");
    } else {
        lines << tr("建议记录多组高分图像，确认峰值后再锁定焦距。");
    }

    return lines;
}

void FocusSummaryPanel::handleMarkBest() {
    if (!m_lastMetrics.valid) {
        return;
    }
    m_bestMetrics = m_lastMetrics;
    m_bestComposite = m_lastMetrics.compositeScore;
    m_hasBaseline = true;
    updateButtons();
    if (m_guidanceBox) {
        m_guidanceBox->append(QStringLiteral("\n%1")
                                  .arg(tr("已将当前评分 %1 设为参考基线。")
                                           .arg(QString::number(m_bestComposite, 'f', 1))));
    }
}

void FocusSummaryPanel::handleResetBaseline() {
    m_hasBaseline = false;
    m_bestComposite = 0.0;
    m_bestMetrics = FocusEvaluator::Metrics{};
    m_history.clear();
    m_previousComposite = 0.0;
    refreshHistoryTable();
    updateButtons();
    if (m_guidanceBox) {
        m_guidanceBox->setPlainText(tr("评分基线已重置，请重新开始调焦流程。"));
    }
}

void FocusSummaryPanel::updateButtons() {
    if (m_markBestButton) {
        m_markBestButton->setEnabled(m_lastMetrics.valid);
    }
    if (m_resetBaselineButton) {
        m_resetBaselineButton->setEnabled(m_hasBaseline || !m_history.empty());
    }
}

void FocusSummaryPanel::resetPanel() {
    m_lastMetrics = FocusEvaluator::Metrics{};
    m_bestMetrics = FocusEvaluator::Metrics{};
    m_bestComposite = 0.0;
    m_previousComposite = 0.0;
    m_hasBaseline = false;
    m_history.clear();
    m_lastFrameSize = {};
    m_lastRoi = {};

    auto resetLabel = [](QLabel* label) {
        if (label) {
            label->setText(QStringLiteral("--"));
        }
    };

    resetLabel(m_metricLap);
    resetLabel(m_metricTen);
    resetLabel(m_metricHighFreq);
    resetLabel(m_metricUniformity);
    resetLabel(m_metricContrast);
    resetLabel(m_metricMean);
    resetLabel(m_metricHighlight);
    resetLabel(m_metricShadow);
    resetLabel(m_metricComposite);

    if (m_scoreProgress) {
        m_scoreProgress->setValue(0);
    }
    if (m_scorePercent) {
        m_scorePercent->setText(QStringLiteral("--%"));
    }
    if (m_roiSummary) {
        m_roiSummary->setText(tr("ROI：全图"));
    }
    if (m_guidanceBox) {
        m_guidanceBox->clear();
    }
    refreshHistoryTable();
    updateButtons();
}

QVariantMap FocusSummaryPanel::exportMetrics() const {
    QVariantMap map;
    if (!m_lastMetrics.valid) {
        return map;
    }
    map.insert(QStringLiteral("focusComposite"), m_lastMetrics.compositeScore);
    map.insert(QStringLiteral("focusLaplacianVariance"), m_lastMetrics.laplacianVariance);
    map.insert(QStringLiteral("focusTenengrad"), m_lastMetrics.tenengrad);
    map.insert(QStringLiteral("focusHighFrequency"), m_lastMetrics.highFrequencyRatio);
    map.insert(QStringLiteral("focusGradientUniformity"), m_lastMetrics.gradientUniformity);
    map.insert(QStringLiteral("focusContrast"), m_lastMetrics.contrast);
    map.insert(QStringLiteral("focusMean"), m_lastMetrics.meanIntensity);
    map.insert(QStringLiteral("focusHighlights"), m_lastMetrics.highlightRatio);
    map.insert(QStringLiteral("focusShadows"), m_lastMetrics.shadowRatio);
    map.insert(QStringLiteral("focusBaseline"), m_bestComposite);
    map.insert(QStringLiteral("focusHasBaseline"), m_hasBaseline);
    if (!m_lastRoi.isNull()) {
        QVariantMap roiMap;
        roiMap.insert(QStringLiteral("x"), m_lastRoi.x());
        roiMap.insert(QStringLiteral("y"), m_lastRoi.y());
        roiMap.insert(QStringLiteral("width"), m_lastRoi.width());
        roiMap.insert(QStringLiteral("height"), m_lastRoi.height());
        roiMap.insert(QStringLiteral("frameWidth"), m_lastFrameSize.width());
        roiMap.insert(QStringLiteral("frameHeight"), m_lastFrameSize.height());
        map.insert(QStringLiteral("focusRoi"), roiMap);
    }
    return map;
}
