#include "camera/focus/FocusAssistWindow.h"

#include "camera/focus/FocusEvaluator.h"
#include "camera/ImageView.h"
#include "camera/Utils.h"
#include "camera/VimbaController.h"

#include <QAbstractItemView>
#include <QAction>
#include <QComboBox>
#include <QDateTime>
#include <QFont>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QSignalBlocker>
#include <QSize>
#include <QSizePolicy>
#include <QStatusBar>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextBrowser>
#include <QToolBar>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {
QLabel* makeMetricLabel(QWidget* parent = nullptr) {
    auto* label = new QLabel(QStringLiteral("--"), parent);
    label->setObjectName(QStringLiteral("MetricValue"));
    label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    QFont f = label->font();
    f.setPointSizeF(f.pointSizeF() + 1.0);
    f.setBold(true);
    label->setFont(f);
    return label;
}
}

FocusAssistWindow::FocusAssistWindow(QWidget* parent)
    : QMainWindow(parent)
    , m_controller(new VimbaController(this)) {
    buildUi();
    connectControllerSignals();
    refreshCameraList();
    updateActionStates();
    pushGuidance({tr("步骤 1：通过工具栏连接相机，并选择目标设备。"),
                  tr("步骤 2：开始取流，在中央图像上框选需要评估的 ROI。"),
                  tr("步骤 3：缓慢调节焦距环，观察实时评分和历史记录，寻找峰值。"),
                  tr("步骤 4：根据亮度提示调节光圈，兼顾景深与曝光。"),
                  tr("可点击“标记最佳”冻结当前得分作为参考，完成后记得锁紧镜头。")});
}

void FocusAssistWindow::buildUi() {
    setWindowTitle(tr("Focus & Aperture Assistant"));
    resize(1360, 840);

    auto* toolbar = addToolBar(tr("FocusControl"));
    toolbar->setObjectName(QStringLiteral("FocusToolbar"));
    toolbar->setMovable(false);
    toolbar->setFloatable(false);
    toolbar->setAllowedAreas(Qt::TopToolBarArea);
    toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toolbar->setIconSize(QSize(20, 20));

    m_actConnect = toolbar->addAction(QIcon(QStringLiteral(":/icons/camera_open.svg")), tr("连接"));
    connect(m_actConnect, &QAction::triggered, this, &FocusAssistWindow::onConnectCamera);

    m_actDisconnect = toolbar->addAction(QIcon(QStringLiteral(":/icons/camera_close.svg")), tr("断开"));
    connect(m_actDisconnect, &QAction::triggered, this, &FocusAssistWindow::onDisconnectCamera);

    toolbar->addSeparator();

    m_actStart = toolbar->addAction(QIcon(QStringLiteral(":/icons/stream_start.svg")), tr("开始取流"));
    m_actStart->setCheckable(true);
    connect(m_actStart, &QAction::triggered, this, &FocusAssistWindow::onStartStream);

    m_actStop = toolbar->addAction(QIcon(QStringLiteral(":/icons/stream_stop.svg")), tr("停止取流"));
    m_actStop->setCheckable(true);
    connect(m_actStop, &QAction::triggered, this, &FocusAssistWindow::onStopStream);

    toolbar->addSeparator();

    m_actMarkBest = toolbar->addAction(QIcon(QStringLiteral(":/icons/snapshot.svg")), tr("标记最佳"));
    connect(m_actMarkBest, &QAction::triggered, this, &FocusAssistWindow::onMarkBest);

    m_actResetBaseline = toolbar->addAction(QIcon(QStringLiteral(":/icons/refresh.svg")), tr("重置基线"));
    connect(m_actResetBaseline, &QAction::triggered, this, &FocusAssistWindow::onResetBaseline);

    toolbar->addSeparator();

    m_cameraCombo = new QComboBox(toolbar);
    m_cameraCombo->setEnabled(false);
    m_cameraCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_cameraCombo->addItem(tr("未检测到相机"));
    connect(m_cameraCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, &FocusAssistWindow::onSelectCamera);
    toolbar->addWidget(m_cameraCombo);

    auto* spacer = new QWidget(toolbar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar->addWidget(spacer);

    auto* central = new QWidget(this);
    auto* mainLayout = new QHBoxLayout(central);
    mainLayout->setContentsMargins(24, 20, 24, 20);
    mainLayout->setSpacing(18);

    auto* leftColumn = new QVBoxLayout();
    leftColumn->setSpacing(12);

    m_view = new ImageView(central);
    m_view->setMinimumSize(640, 480);
    connect(m_view, &ImageView::roiChanged, this, &FocusAssistWindow::onRoiUpdated);
    leftColumn->addWidget(m_view, 1);

    auto* roiHint = new QLabel(tr("在图像上拖拽来选择评估区域，未选择时默认使用全图"), central);
    roiHint->setWordWrap(true);
    leftColumn->addWidget(roiHint);

    mainLayout->addLayout(leftColumn, 3);

    auto* rightColumn = new QVBoxLayout();
    rightColumn->setSpacing(12);

    auto* metricsBox = new QGroupBox(tr("实时指标"), central);
    auto* metricsLayout = new QGridLayout(metricsBox);
    metricsLayout->setHorizontalSpacing(14);
    metricsLayout->setVerticalSpacing(8);

    int row = 0;
    auto addMetric = [&](const QString& labelText, QLabel*& valueLabel, const QString& unit = QString()) {
        auto* nameLabel = new QLabel(labelText, metricsBox);
        metricsLayout->addWidget(nameLabel, row, 0);
        valueLabel = makeMetricLabel(metricsBox);
        metricsLayout->addWidget(valueLabel, row, 1);
        auto* unitLabel = new QLabel(unit, metricsBox);
        unitLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        metricsLayout->addWidget(unitLabel, row, 2);
        ++row;
    };

    addMetric(tr("Laplacian Var"), m_metricLap);
    addMetric(tr("Tenengrad"), m_metricTen);
    addMetric(tr("高频能量"), m_metricHighFreq, tr("%"));
    addMetric(tr("方向均衡"), m_metricUniformity, tr("%"));
    addMetric(tr("对比度 σ"), m_metricContrast);
    addMetric(tr("平均灰度"), m_metricMean);
    addMetric(tr("高光占比"), m_metricHighlight, tr("%"));
    addMetric(tr("暗部占比"), m_metricShadow, tr("%"));

    auto* scoreLabel = new QLabel(tr("综合评分"), metricsBox);
    metricsLayout->addWidget(scoreLabel, row, 0);
    m_metricComposite = makeMetricLabel(metricsBox);
    metricsLayout->addWidget(m_metricComposite, row, 1);
    m_scoreProgress = new QProgressBar(metricsBox);
    m_scoreProgress->setRange(0, 120);
    m_scoreProgress->setFormat(QStringLiteral("%p%"));
    metricsLayout->addWidget(m_scoreProgress, row, 2);

    rightColumn->addWidget(metricsBox);

    m_historyTable = new QTableWidget(0, 8, central);
    m_historyTable->setHorizontalHeaderLabels({tr("时间"), tr("评分"), tr("Laplacian"), tr("Tenengrad"), tr("高频能量"), tr("方向均衡"), tr("对比度"), tr("均值")});
    m_historyTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_historyTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_historyTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_historyTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_historyTable->setMinimumHeight(220);
    rightColumn->addWidget(m_historyTable, 1);

    m_guidanceBox = new QTextBrowser(central);
    m_guidanceBox->setMinimumHeight(180);
    rightColumn->addWidget(m_guidanceBox);

    mainLayout->addLayout(rightColumn, 2);

    setCentralWidget(central);

    statusBar()->setSizeGripEnabled(false);
    m_connectionLabel = new QLabel(tr("离线"), this);
    m_statusLabel = new QLabel(tr("FPS: --  |  带宽: --"), this);
    statusBar()->addWidget(m_connectionLabel);
    statusBar()->addPermanentWidget(m_statusLabel, 1);
}

void FocusAssistWindow::connectControllerSignals() {
    connect(m_controller, &VimbaController::frameReady, this, &FocusAssistWindow::onFrame);
    connect(m_controller, &VimbaController::statsUpdated, this, &FocusAssistWindow::onStats);
    connect(m_controller, &VimbaController::cameraOpened, this, &FocusAssistWindow::onCameraOpened);
    connect(m_controller, &VimbaController::cameraClosed, this, &FocusAssistWindow::onCameraClosed);
    connect(m_controller, &VimbaController::errorOccured, this, [this](const QString& msg) {
        flashStatus(msg, 4000);
    });
}

void FocusAssistWindow::onCameraOpened(const QString& id, const QString& model) {
    const QString descriptor = model.isEmpty() ? id : QStringLiteral("%1 · %2").arg(model, id);
    m_connectionLabel->setText(tr("在线 · %1").arg(descriptor));
    flashStatus(tr("已连接 %1").arg(descriptor), 2500);
    m_streaming = false;
    updateActionStates();
}

void FocusAssistWindow::onCameraClosed() {
    m_connectionLabel->setText(tr("离线"));
    m_streaming = false;
    updateActionStates();
}

void FocusAssistWindow::onFrame(const QImage& image) {
    m_view->setImage(image);
    const QRect roi = currentEvaluationRoi(image);
    const double previous = m_prevComposite;
    FocusEvaluator::Metrics metrics = m_evaluator.evaluate(image, roi);
    applyMetrics(metrics);
    m_prevComposite = metrics.valid ? metrics.compositeScore : previous;
}

void FocusAssistWindow::onStats(double fps, double bps) {
    const QString stats = tr("⚡ %1 FPS  ·  %2")
        .arg(QString::number(fps, 'f', 1))
        .arg(Utils::bytesHumanReadable(bps));
    m_statusLabel->setText(stats);
}

void FocusAssistWindow::onSelectCamera(int) {
    updateActionStates();
}

void FocusAssistWindow::onConnectCamera() {
    if (!m_controller) {
        return;
    }
    QString targetId;
    if (m_cameraCombo && m_cameraCombo->isEnabled() && m_cameraCombo->count() > 0) {
        targetId = m_cameraCombo->currentText();
    }
    if (targetId.isEmpty() || targetId == tr("未检测到相机")) {
        flashStatus(tr("没有可用的相机"), 2500);
        return;
    }
    if (!m_controller->open(targetId)) {
        flashStatus(tr("连接相机失败"), 3000);
        return;
    }
    updateActionStates();
}

void FocusAssistWindow::onDisconnectCamera() {
    if (!m_controller) {
        return;
    }
    m_controller->close();
    m_streaming = false;
    updateActionStates();
}

void FocusAssistWindow::onStartStream() {
    if (!m_controller || !m_controller->camera()) {
        flashStatus(tr("请先连接相机"), 3000);
        return;
    }
    if (!m_controller->start()) {
        flashStatus(tr("取流启动失败"), 3000);
        m_actStart->setChecked(false);
        return;
    }
    m_streaming = true;
    flashStatus(tr("取流已开始"), 2200);
    updateActionStates();
}

void FocusAssistWindow::onStopStream() {
    if (!m_controller) {
        return;
    }
    m_controller->stop();
    if (m_streaming) {
        flashStatus(tr("取流已停止"), 2200);
    }
    m_streaming = false;
    updateActionStates();
}

void FocusAssistWindow::onMarkBest() {
    if (!m_lastMetrics.valid) {
        flashStatus(tr("当前没有可用的评分"), 2200);
        return;
    }
    m_bestMetrics = m_lastMetrics;
    m_bestComposite = m_lastMetrics.compositeScore;
    m_hasBest = true;
    flashStatus(tr("已将当前评分 %1 设为参考基线")
                    .arg(QString::number(m_bestComposite, 'f', 1)),
                2600);
}

void FocusAssistWindow::onResetBaseline() {
    m_hasBest = false;
    m_bestComposite = 0.0;
    m_history.clear();
    updateHistoryTable();
    m_scoreProgress->setValue(0);
    pushGuidance({tr("评分基线已重置，请重新开始调焦流程。")});
    flashStatus(tr("已重置历史记录"), 2000);
}

void FocusAssistWindow::onRoiUpdated(const QRect& roi) {
    m_selectedRoi = roi;
    flashStatus(tr("ROI 已更新：%1 × %2")
                    .arg(roi.width())
                    .arg(roi.height()),
                1600);
}

void FocusAssistWindow::updateActionStates() {
    const bool hasController = m_controller && m_controller->camera();
    m_actConnect->setEnabled(!hasController);
    m_actDisconnect->setEnabled(hasController);
    m_actStart->setEnabled(hasController && !m_streaming);
    m_actStop->setEnabled(hasController && m_streaming);
    m_actStart->setChecked(m_streaming);
    m_actStop->setChecked(hasController && m_streaming);
    m_actMarkBest->setEnabled(m_lastMetrics.valid);
    m_actResetBaseline->setEnabled(m_hasBest || !m_history.empty());
    if (m_cameraCombo) {
        m_cameraCombo->setEnabled(!hasController);
    }
}

void FocusAssistWindow::refreshCameraList() {
    if (!m_cameraCombo) {
        return;
    }
    const QString previous = m_cameraCombo->currentText();
    const QStringList cams = m_controller ? m_controller->listCameras() : QStringList{};

    QSignalBlocker blocker(m_cameraCombo);
    m_cameraCombo->clear();

    if (cams.isEmpty()) {
        m_cameraCombo->addItem(tr("未检测到相机"));
        m_cameraCombo->setEnabled(false);
    } else {
        m_cameraCombo->addItems(cams);
        m_cameraCombo->setEnabled(true);
        const int idx = m_cameraCombo->findText(previous, Qt::MatchFixedString);
        if (idx >= 0) {
            m_cameraCombo->setCurrentIndex(idx);
        }
    }
}

void FocusAssistWindow::applyMetrics(const FocusEvaluator::Metrics& metrics) {
    m_lastMetrics = metrics;
    if (!metrics.valid) {
        return;
    }

    m_metricLap->setText(QString::number(metrics.laplacianVariance, 'f', 2));
    m_metricTen->setText(QString::number(metrics.tenengrad, 'f', 2));
    m_metricHighFreq->setText(QString::number(metrics.highFrequencyRatio * 100.0, 'f', 1));
    m_metricUniformity->setText(QString::number(metrics.gradientUniformity * 100.0, 'f', 1));
    m_metricContrast->setText(QString::number(metrics.contrast, 'f', 2));
    m_metricMean->setText(QString::number(metrics.meanIntensity, 'f', 1));
    m_metricHighlight->setText(QString::number(metrics.highlightRatio, 'f', 2));
    m_metricShadow->setText(QString::number(metrics.shadowRatio, 'f', 2));
    m_metricComposite->setText(QString::number(metrics.compositeScore, 'f', 1));

    if (!m_hasBest || metrics.compositeScore > m_bestComposite) {
        m_bestComposite = metrics.compositeScore;
        m_bestMetrics = metrics;
        m_hasBest = true;
    }

    const double reference = (std::max)(m_bestComposite, 1.0);
    const double relative = std::clamp(metrics.compositeScore / reference * 100.0, 0.0, 120.0);
    m_scoreProgress->setValue(static_cast<int>(std::round(relative)));

    appendHistory(metrics);
    updateActionStates();

    pushGuidance(buildGuidance(metrics, m_prevComposite));
}

void FocusAssistWindow::appendHistory(const FocusEvaluator::Metrics& metrics) {
    HistoryEntry entry;
    entry.timestamp = QDateTime::currentDateTime();
    entry.metrics = metrics;
    m_history.push_front(entry);
    while (static_cast<int>(m_history.size()) > m_historyLimit) {
        m_history.pop_back();
    }
    updateHistoryTable();
    m_actResetBaseline->setEnabled(true);
}

void FocusAssistWindow::updateHistoryTable() {
    m_historyTable->setRowCount(static_cast<int>(m_history.size()));
    for (int row = 0; row < static_cast<int>(m_history.size()); ++row) {
        const auto& entry = m_history[row];
        const auto timeStr = entry.timestamp.toString(QStringLiteral("HH:mm:ss"));
        const auto addItem = [&](int column, const QString& text) {
            auto* item = new QTableWidgetItem(text);
            item->setTextAlignment(Qt::AlignCenter);
            m_historyTable->setItem(row, column, item);
        };
        addItem(0, timeStr);
        addItem(1, QString::number(entry.metrics.compositeScore, 'f', 1));
        addItem(2, QString::number(entry.metrics.laplacianVariance, 'f', 2));
        addItem(3, QString::number(entry.metrics.tenengrad, 'f', 2));
        addItem(4, QString::number(entry.metrics.highFrequencyRatio * 100.0, 'f', 1));
        addItem(5, QString::number(entry.metrics.gradientUniformity * 100.0, 'f', 1));
        addItem(6, QString::number(entry.metrics.contrast, 'f', 2));
        addItem(7, QString::number(entry.metrics.meanIntensity, 'f', 1));
    }
    m_historyTable->scrollToTop();
}

QStringList FocusAssistWindow::buildGuidance(const FocusEvaluator::Metrics& metrics, double previousComposite) const {
    QStringList lines;
    const double relative = (m_hasBest && m_bestComposite > 0.0)
                                ? metrics.compositeScore / m_bestComposite * 100.0
                                : 100.0;
    QString clarityLine = tr("当前清晰度评分：%1（占历史最佳 %2%）");
    clarityLine = clarityLine.arg(QString::number(metrics.compositeScore, 'f', 1))
                             .arg(QString::number(relative, 'f', 1));
    lines << clarityLine;

    if (metrics.highlightRatio > 5.0) {
        QString highlightLine = tr("高光占比 %1% 偏高，建议收小光圈或缩短曝光时间。");
        highlightLine = highlightLine.arg(QString::number(metrics.highlightRatio, 'f', 1));
        lines << highlightLine;
    } else if (metrics.shadowRatio > 12.0) {
        QString shadowLine = tr("暗部占比 %1% 偏高，可适当开大光圈或延长曝光。");
        shadowLine = shadowLine.arg(QString::number(metrics.shadowRatio, 'f', 1));
        lines << shadowLine;
    } else {
        lines << tr("亮度分布稳定，可继续专注于焦距微调。");
    }

    const double highFreqPct = metrics.highFrequencyRatio * 100.0;
    if (highFreqPct < 18.0) {
        lines << tr("ROI 高频能量 %.1f%% 偏低，尝试让 ROI 覆盖实心圆的清晰边缘，加大对焦步幅。")
                       .arg(highFreqPct, 0, 'f', 1);
    } else if (highFreqPct > 32.0) {
        lines << tr("高频细节充分（%.1f%%），边缘锐度表现优秀。")
                       .arg(highFreqPct, 0, 'f', 1);
    }

    if (metrics.gradientUniformity < 0.35) {
        lines << tr("梯度方向偏单一，可调整 ROI 让其包含圆靶的完整轮廓段或多枚圆心。");
    } else if (metrics.gradientUniformity > 0.65) {
        lines << tr("方向均衡度 %.1f%% 良好，说明圆靶边缘覆盖充分，可继续微调焦距。")
                       .arg(metrics.gradientUniformity * 100.0, 0, 'f', 1);
    }

    if (previousComposite > 0.0) {
        const double delta = metrics.compositeScore - previousComposite;
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

void FocusAssistWindow::pushGuidance(const QStringList& lines) {
    m_guidanceBox->setPlainText(lines.join(QStringLiteral("\n\n")));
}

QRect FocusAssistWindow::currentEvaluationRoi(const QImage& source) const {
    QRect roi = m_selectedRoi;
    if (roi.isNull() || roi.width() < 8 || roi.height() < 8) {
        roi = QRect(QPoint(0, 0), source.size());
    }
    roi = roi.intersected(QRect(QPoint(0, 0), source.size()));
    return roi;
}

void FocusAssistWindow::flashStatus(const QString& message, int timeoutMs) {
    if (auto* bar = statusBar()) {
        bar->showMessage(message, timeoutMs);
    }
}
