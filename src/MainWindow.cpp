#include "MainWindow.h"

#include <QAction>
#include <QAbstractItemView>
#include <QApplication>
#include <QDateTime>
#include <QDockWidget>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QBrush>
#include <QFileDialog>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QSizePolicy>
#include <QSplitter>
#include <QTabWidget>
#include <QStackedLayout>
#include <QTextEdit>
#include <QTextCursor>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QVector>
#include <QPolygonF>
#include <QStringList>
#include <QFile>
#include <QDir>
#include <QMetaObject>
#include <QWidget>
#include <QRegularExpression>
#include <QScrollBar>
#include <QtCore/Qt>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <initializer_list>
#include <numeric>
#include <tuple>
#include <unordered_set>

#include <opencv2/imgproc.hpp>

#include "DetectionPreviewWidget.h"
#include "ImageEvaluationDialog.h"
#include "HeatmapView.h"
#include "Logger.h"
#include "Pose3DView.h"
#include "ResidualScatterView.h"
#include "ParameterDialog.h"

namespace mycalib {

namespace {

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

    for (const auto &file : files) {
        std::error_code ec;
        std::filesystem::remove(file, ec);
    }

    for (const auto &dir : directories) {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_engine(new CalibrationEngine(this))
{
    setupUi();
    setupActions();
    applyTheme();

    const QString defaultInputDir = QStringLiteral("/Users/charminzh/VSCodeProjects/Calib/data/raw/calibration/calib_25");
    if (QDir(defaultInputDir).exists()) {
        m_inputDir = defaultInputDir;
        if (m_inputPathEdit) {
            m_inputPathEdit->setText(defaultInputDir);
        }
        if (m_logView) {
            m_logView->append(QStringLiteral("Selected input: %1").arg(defaultInputDir));
        }
    }

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

    auto *inputLabel = new QLabel(tr("Image Directory"), pathsWidget);
    m_inputPathEdit = new QLineEdit(pathsWidget);
    m_inputPathEdit->setPlaceholderText(tr("Select folder with calibration images"));

    auto *outputLabel = new QLabel(tr("Output Directory"), pathsWidget);
    m_outputPathEdit = new QLineEdit(pathsWidget);
    const QString candidate = QDir::homePath() + QStringLiteral("/outputs");
    const QString resolvedDefault = CalibrationEngine::resolveOutputDirectory(candidate);
    m_outputDir = resolvedDefault;
    m_outputPathEdit->setText(resolvedDefault);

    auto *browseInputBtn = new QPushButton(tr("Browse"), pathsWidget);
    browseInputBtn->setProperty("accent", true);
    connect(browseInputBtn, &QPushButton::clicked, this, &MainWindow::chooseInputDirectory);

    auto *browseOutputBtn = new QPushButton(tr("Browse"), pathsWidget);
    connect(browseOutputBtn, &QPushButton::clicked, this, &MainWindow::chooseOutputDirectory);

    m_diameterSpin = new QDoubleSpinBox(pathsWidget);
    m_diameterSpin->setSuffix(" mm");
    m_diameterSpin->setRange(0.1, 100.0);
    m_diameterSpin->setValue(5.0);

    m_spacingSpin = new QDoubleSpinBox(pathsWidget);
    m_spacingSpin->setSuffix(" mm");
    m_spacingSpin->setRange(1.0, 200.0);
    m_spacingSpin->setValue(25.0);

    pathsLayout->addWidget(inputLabel, 0, 0);
    pathsLayout->addWidget(m_inputPathEdit, 0, 1, 1, 3);
    pathsLayout->addWidget(browseInputBtn, 0, 4);
    pathsLayout->addWidget(outputLabel, 1, 0);
    pathsLayout->addWidget(m_outputPathEdit, 1, 1, 1, 3);
    pathsLayout->addWidget(browseOutputBtn, 1, 4);

    pathsLayout->addWidget(new QLabel(tr("Circle Ø"), pathsWidget), 2, 0);
    pathsLayout->addWidget(m_diameterSpin, 2, 1);
    pathsLayout->addWidget(new QLabel(tr("Spacing"), pathsWidget), 2, 2);
    pathsLayout->addWidget(m_spacingSpin, 2, 3);

    mainLayout->addWidget(pathsWidget);

    m_mainTabs = new QTabWidget(central);
    m_mainTabs->setDocumentMode(true);
    m_mainTabs->setTabPosition(QTabWidget::North);

    m_overviewPage = new QWidget(m_mainTabs);
    auto *overviewLayout = new QVBoxLayout(m_overviewPage);
    overviewLayout->setContentsMargins(20, 20, 20, 20);
    overviewLayout->setSpacing(12);
    auto createMetricCard = [this](const QString &title,
                                   QLabel *&valueLabel,
                                   const QString &subtitle) {
        auto *frame = new QFrame(m_overviewPage);
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

    auto *logTitle = new QLabel(tr("Activity log"), m_overviewPage);
    QFont logFont = logTitle->font();
    logFont.setBold(true);
    logTitle->setFont(logFont);
    overviewLayout->addWidget(logTitle);

    m_logView = new QTextEdit(m_overviewPage);
    m_logView->setReadOnly(true);
    m_logView->setMinimumHeight(160);
    if (m_logView->document()) {
        m_logView->document()->setMaximumBlockCount(2000);
    }
    overviewLayout->addWidget(m_logView, 1);
    overviewLayout->addStretch(1);
    m_mainTabs->addTab(m_overviewPage, tr("Overview"));

    m_imagePage = new QWidget(m_mainTabs);
    auto *imageLayout = new QVBoxLayout(m_imagePage);
    imageLayout->setContentsMargins(0, 0, 0, 0);
    imageLayout->setSpacing(0);

    auto *imageSplitter = new QSplitter(Qt::Horizontal, m_imagePage);
    imageSplitter->setHandleWidth(6);

    auto *imageLeftPane = new QWidget(imageSplitter);
    auto *leftLayout = new QVBoxLayout(imageLeftPane);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(8);

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

    imageLayout->addWidget(imageSplitter);
    m_mainTabs->addTab(m_imagePage, tr("Image details"));

    m_posePage = new QWidget(m_mainTabs);
    m_poseStack = new QStackedLayout(m_posePage);
    m_poseStack->setContentsMargins(0, 0, 0, 0);
    m_posePlaceholder = new QLabel(tr("The 3D interaction view becomes available after calibration"), m_posePage);
    m_posePlaceholder->setAlignment(Qt::AlignCenter);
    m_posePlaceholder->setWordWrap(true);
    m_posePlaceholder->setMargin(24);
    m_poseStack->addWidget(m_posePlaceholder);
    m_mainTabs->addTab(m_posePage, tr("3D inspection"));

    mainLayout->addWidget(m_mainTabs, 1);

    if (m_posePlaceholder) {
        m_posePlaceholder->setText(tr("The calibrated board pose will appear here once processing is done."));
    }

    m_progressBar = new QProgressBar(central);
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    mainLayout->addWidget(m_progressBar);

    setCentralWidget(central);

    auto createHeatmapDock = [this](const QString &title, HeatmapView *&view, QDockWidget *&dock) {
        dock = new QDockWidget(title, this);
        dock->setObjectName(title.toLower().replace(' ', '_'));
        dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
        view = new HeatmapView(dock);
        view->setTitle(title);
        dock->setWidget(view);
        addDockWidget(Qt::RightDockWidgetArea, dock);
    };

    createHeatmapDock(tr("Coverage"), m_heatmapBoard, m_dockCoverage);
    if (m_heatmapBoard) {
        m_heatmapBoard->setLegendUnit(tr("ratio"));
        m_heatmapBoard->setLegendTickCount(5);
        m_heatmapBoard->setLegendPrecision(2);
    }

    createHeatmapDock(tr("Pixel Error"), m_heatmapPixel, m_dockPixel);
    if (m_heatmapPixel) {
        m_heatmapPixel->setLegendUnit(tr("px"));
        m_heatmapPixel->setLegendTickCount(5);
        m_heatmapPixel->setLegendPrecision(2);
    }

    m_dockScatter = new QDockWidget(tr("Residual Scatter"), this);
    m_dockScatter->setObjectName(QStringLiteral("residual_scatter"));
    m_dockScatter->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    m_scatterView = new ResidualScatterView(m_dockScatter);
    m_dockScatter->setWidget(m_scatterView);
    addDockWidget(Qt::RightDockWidgetArea, m_dockScatter);

    createHeatmapDock(tr("Distortion"), m_distortionMap, m_dockDistortion);
    if (m_distortionMap) {
        m_distortionMap->setLegendUnit(tr("px"));
        m_distortionMap->setLegendTickCount(5);
        m_distortionMap->setLegendPrecision(2);
        m_distortionMap->setGridOverlayEnabled(true);
    }

    tabifyDockWidget(m_dockCoverage, m_dockPixel);
    tabifyDockWidget(m_dockPixel, m_dockScatter);
    tabifyDockWidget(m_dockScatter, m_dockDistortion);
    m_dockCoverage->raise();
}

void MainWindow::setupActions()
{
    m_actionOpenDir = new QAction(QIcon(":/icons/folder.svg"), tr("Open folder"), this);
    connect(m_actionOpenDir, &QAction::triggered, this, &MainWindow::chooseInputDirectory);

    m_actionSetOutput = new QAction(QIcon(":/icons/export.svg"), tr("Set output"), this);
    connect(m_actionSetOutput, &QAction::triggered, this, &MainWindow::chooseOutputDirectory);

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
        m_toolBar->addAction(m_actionOpenDir);
        m_toolBar->addAction(m_actionSetOutput);
        m_toolBar->addSeparator();
        m_toolBar->addAction(m_actionRun);
        m_toolBar->addAction(m_actionExport);
        m_toolBar->addAction(m_actionShowParameters);
        m_toolBar->addAction(m_actionEvaluate);
        m_toolBar->addSeparator();
        m_toolBar->addAction(m_actionReset);

        QWidget *spacer = new QWidget(m_toolBar);
        spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        m_toolBar->addWidget(spacer);
        m_toolBar->addAction(m_actionAbout);
    }
}

void MainWindow::applyTheme()
{
    setWindowTitle(tr("Calibration Studio"));
    qApp->setWindowIcon(QIcon(QStringLiteral(":/icons/app.png")));
}

void MainWindow::chooseInputDirectory()
{
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Select image directory"), m_inputDir);
    if (!dir.isEmpty()) {
        m_inputDir = dir;
        m_inputPathEdit->setText(dir);
        m_logView->append(QStringLiteral("Selected input: %1").arg(dir));
    }
}

void MainWindow::chooseOutputDirectory()
{
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Select output directory"), m_outputDir);
    if (!dir.isEmpty()) {
        const QString resolved = CalibrationEngine::resolveOutputDirectory(dir);
        m_outputDir = resolved;
        m_outputPathEdit->setText(resolved);
        m_logView->append(QStringLiteral("Selected output: %1").arg(resolved));
    }
}

void MainWindow::runCalibration()
{
    if (m_running) {
        return;
    }
    if (m_inputPathEdit->text().isEmpty()) {
        QMessageBox::warning(this, tr("Missing input"), tr("Please choose an image directory first."));
        return;
    }

    CalibrationEngine::Settings settings;
    settings.boardSpec.smallDiameterMm = m_diameterSpin->value();
    settings.boardSpec.centerSpacingMm = m_spacingSpin->value();
    // board layout (7x6 with centre gap) follows the Python reference implementation

    resetUi();

    refreshState(true);
    m_progressBar->setValue(0);
    m_logView->append(QStringLiteral("[%1] Starting calibration ...")
                           .arg(QDateTime::currentDateTime().toString("hh:mm:ss")));

    const QString resolvedOutput = CalibrationEngine::resolveOutputDirectory(m_outputPathEdit->text());
    m_outputDir = resolvedOutput;
    m_outputPathEdit->setText(resolvedOutput);

    m_engine->run(m_inputPathEdit->text(), settings, resolvedOutput);
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
    if (m_mainTabs && m_overviewPage) {
        m_mainTabs->setCurrentWidget(m_overviewPage);
    }
    m_lastOutput = CalibrationOutput{};
    if (m_actionShowParameters) {
        m_actionShowParameters->setEnabled(false);
    }
}

void MainWindow::exportJson()
{
    if (!m_lastOutput.success) {
        QMessageBox::information(this, tr("No data"), tr("Run calibration before exporting."));
        return;
    }

    const QString targetPath = QFileDialog::getSaveFileName(this, tr("Export calibration JSON"),
                                                            m_outputDir + "/calibration_report.json",
                                                            tr("JSON (*.json)"));
    if (targetPath.isEmpty()) {
        return;
    }

    QFile source(m_outputDir + "/calibration_report.json");
    if (source.exists()) {
        source.copy(targetPath);
        m_logView->append(QStringLiteral("Exported report to %1").arg(targetPath));
    } else {
        QMessageBox::warning(this, tr("Missing report"), tr("No report file found in output directory."));
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
    if (m_diameterSpin) {
        spec.smallDiameterMm = m_diameterSpin->value();
    }
    if (m_spacingSpin) {
        spec.centerSpacingMm = m_spacingSpin->value();
    }

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
    if (m_evaluationDialog) {
        m_evaluationDialog->close();
        m_evaluationDialog->deleteLater();
        m_evaluationDialog.clear();
    }
    refreshState(false);
    updateSummaryPanel(output);
    populateDetectionTree(output);
    showHeatmaps(output);
    ensurePoseView();
    if (m_poseView) {
        m_poseView->setDetections(output.allDetections);
    }
    if (m_mainTabs && m_overviewPage) {
        m_mainTabs->setCurrentWidget(m_overviewPage);
    }
    m_logView->append(tr("Calibration complete."));
    const QString figureDir = QDir(m_outputDir).absoluteFilePath(QStringLiteral("paper_figures"));
    m_logView->append(tr("Paper-ready figures saved to %1").arg(QDir::toNativeSeparators(figureDir)));
}

void MainWindow::handleFailed(const QString &reason)
{
    m_running = false;
    refreshState(false);
    QMessageBox::critical(this, tr("Calibration failed"), reason);
    m_logView->append(QStringLiteral("Failed: %1").arg(reason));
}

void MainWindow::refreshState(bool running)
{
    m_running = running;
    m_actionRun->setEnabled(!running);
    m_actionOpenDir->setEnabled(!running);
    m_actionSetOutput->setEnabled(!running);
    m_actionExport->setEnabled(!running && m_lastOutput.success);
    if (m_actionShowParameters) {
        m_actionShowParameters->setEnabled(!running && m_lastOutput.success);
    }
    if (m_actionEvaluate) {
        m_actionEvaluate->setEnabled(!running && m_lastOutput.success);
    }
    m_actionReset->setEnabled(!running);
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
        if (m_mainTabs && m_imagePage) {
            m_mainTabs->setCurrentWidget(m_imagePage);
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
    const QString bodyHtml = toHtml(body.isEmpty() ? QStringLiteral(" ") : body);

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
    if (!m_posePage || !m_poseStack) {
        return;
    }
    if (!m_poseView) {
        m_poseView = new Pose3DView(m_posePage);
        m_poseView->setMinimumHeight(320);
        m_poseStack->addWidget(m_poseView);
    }
    if (m_poseStack && m_poseView) {
        m_poseStack->setCurrentWidget(m_poseView);
    }
}

} // namespace mycalib
