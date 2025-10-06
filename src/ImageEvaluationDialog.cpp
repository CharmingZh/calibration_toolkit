#include "ImageEvaluationDialog.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QDirIterator>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QGraphicsTextItem>
#include <QGraphicsView>
#include <QGraphicsItem>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QMimeData>
#include <QFrame>
#include <QScrollBar>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QResizeEvent>
#include <QSplitter>
#include <QStyle>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtConcurrent>

#include <opencv2/calib3d.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <numeric>
#include <unordered_set>

#include "BoardDetector.h"
#include "Logger.h"

namespace mycalib {

class SyncedImageView;

class ViewSyncController {
public:
    void addView(SyncedImageView *view);
    void removeView(SyncedImageView *view);
    void requestZoom(SyncedImageView *origin,
                     double zoom,
                     const QPointF &focusRatio);
    void requestCenter(SyncedImageView *origin, const QPointF &centerRatio);
    void refresh();
    [[nodiscard]] double zoom() const { return m_zoom; }
    [[nodiscard]] QPointF centerRatio() const { return m_centerRatio; }

private:
    double m_zoom {1.0};
    QPointF m_centerRatio {0.5, 0.5};
    bool m_block {false};
    std::vector<SyncedImageView *> m_views;
};

class SyncedImageView : public QGraphicsView {
public:
    explicit SyncedImageView(ViewSyncController *controller,
                             QWidget *parent = nullptr);
    ~SyncedImageView() override;

    void setPixmap(const QPixmap &pixmap);
    void clear();
    void showMessage(const QString &text);
    void applyZoomFromController(double zoom, const QPointF &focusRatio, bool fromSelf);
    void applyCenterFromController(const QPointF &ratio, bool fromSelf);
    void detachController();

protected:
    void wheelEvent(QWheelEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void initializeScene();
    void updateTextPosition();
    void updateSceneRect();
    void connectScrollbars();
    void handleScrollChanged();
    [[nodiscard]] bool hasPixmap() const;
    [[nodiscard]] QPointF scenePointFromRatio(const QPointF &ratio) const;
    [[nodiscard]] QPointF ratioFromScenePoint(const QPointF &scenePoint) const;
    [[nodiscard]] QPointF currentCenterRatio() const;
    void centerOnRatio(const QPointF &ratio);

    ViewSyncController *m_controller {nullptr};
    QGraphicsScene *m_scene {nullptr};
    QGraphicsPixmapItem *m_pixmapItem {nullptr};
    QGraphicsTextItem *m_textItem {nullptr};
    double m_zoom {1.0};
    bool m_blockScroll {false};
};

void ViewSyncController::addView(SyncedImageView *view)
{
    if (!view) {
        return;
    }
    if (std::find(m_views.begin(), m_views.end(), view) != m_views.end()) {
        return;
    }
    m_views.push_back(view);
    view->applyZoomFromController(m_zoom, m_centerRatio, false);
    view->applyCenterFromController(m_centerRatio, false);
}

void ViewSyncController::removeView(SyncedImageView *view)
{
    auto it = std::remove(m_views.begin(), m_views.end(), view);
    if (it != m_views.end()) {
        m_views.erase(it, m_views.end());
    }
}

void ViewSyncController::requestZoom(SyncedImageView *origin,
                                     double zoom,
                                     const QPointF &focusRatio)
{
    if (m_block) {
        return;
    }
    const double clampedZoom = std::clamp(zoom, 0.15, 12.0);
    QPointF ratio = focusRatio;
    ratio.setX(std::clamp(ratio.x(), 0.0, 1.0));
    ratio.setY(std::clamp(ratio.y(), 0.0, 1.0));
    m_zoom = clampedZoom;
    m_centerRatio = ratio;
    m_block = true;
    for (SyncedImageView *view : m_views) {
        if (!view) {
            continue;
        }
        view->applyZoomFromController(m_zoom, m_centerRatio, view == origin);
    }
    m_block = false;
}

void ViewSyncController::requestCenter(SyncedImageView *origin, const QPointF &centerRatio)
{
    if (m_block) {
        return;
    }
    QPointF ratio = centerRatio;
    ratio.setX(std::clamp(ratio.x(), 0.0, 1.0));
    ratio.setY(std::clamp(ratio.y(), 0.0, 1.0));
    m_centerRatio = ratio;
    m_block = true;
    for (SyncedImageView *view : m_views) {
        if (!view || view == origin) {
            continue;
        }
        view->applyCenterFromController(m_centerRatio, false);
    }
    m_block = false;
}

void ViewSyncController::refresh()
{
    if (m_block) {
        return;
    }
    m_block = true;
    for (SyncedImageView *view : m_views) {
        if (!view) {
            continue;
        }
        view->applyZoomFromController(m_zoom, m_centerRatio, false);
        view->applyCenterFromController(m_centerRatio, false);
    }
    m_block = false;
}

SyncedImageView::SyncedImageView(ViewSyncController *controller, QWidget *parent)
    : QGraphicsView(parent)
    , m_controller(controller)
{
    initializeScene();
    setDragMode(QGraphicsView::ScrollHandDrag);
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    setResizeAnchor(QGraphicsView::AnchorViewCenter);
    setFrameShape(QFrame::NoFrame);
    setBackgroundBrush(QColor(8, 10, 18));
    setRenderHint(QPainter::Antialiasing, false);
    setRenderHint(QPainter::SmoothPixmapTransform, true);
    connectScrollbars();
    if (m_controller) {
        m_controller->addView(this);
    }
}

SyncedImageView::~SyncedImageView()
{
    if (m_controller) {
        m_controller->removeView(this);
    }
}

void SyncedImageView::initializeScene()
{
    if (!m_scene) {
        m_scene = new QGraphicsScene(this);
        setScene(m_scene);
    }
    if (!m_pixmapItem) {
        m_pixmapItem = m_scene->addPixmap(QPixmap());
        m_pixmapItem->setTransformationMode(Qt::SmoothTransformation);
    }
    if (!m_textItem) {
        m_textItem = m_scene->addText(QString());
        m_textItem->setDefaultTextColor(QColor(180, 194, 220));
        m_textItem->setVisible(false);
        m_textItem->setZValue(10.0);
        m_textItem->setFlag(QGraphicsItem::ItemIgnoresTransformations, true);
    }
    updateSceneRect();
}

void SyncedImageView::setPixmap(const QPixmap &pixmap)
{
    if (!m_pixmapItem) {
        return;
    }
    m_pixmapItem->setPixmap(pixmap);
    updateSceneRect();
    if (m_textItem) {
        m_textItem->setVisible(false);
    }
    if (m_controller) {
        m_controller->refresh();
    }
}

void SyncedImageView::clear()
{
    if (m_pixmapItem) {
        m_pixmapItem->setPixmap(QPixmap());
    }
    updateSceneRect();
    if (m_textItem) {
        m_textItem->setVisible(false);
    }
}

void SyncedImageView::showMessage(const QString &text)
{
    clear();
    if (m_textItem) {
        m_textItem->setPlainText(text);
        m_textItem->setVisible(true);
        updateTextPosition();
    }
}

void SyncedImageView::applyZoomFromController(double zoom, const QPointF &focusRatio, bool fromSelf)
{
    Q_UNUSED(fromSelf);
    m_zoom = zoom;
    if (!hasPixmap()) {
        updateTextPosition();
        return;
    }
    const QPointF focus = scenePointFromRatio(focusRatio);
    m_blockScroll = true;
    resetTransform();
    scale(m_zoom, m_zoom);
    centerOn(focus);
    m_blockScroll = false;
    updateTextPosition();
}

void SyncedImageView::applyCenterFromController(const QPointF &ratio, bool fromSelf)
{
    Q_UNUSED(fromSelf);
    if (!hasPixmap()) {
        updateTextPosition();
        return;
    }
    m_blockScroll = true;
    centerOnRatio(ratio);
    m_blockScroll = false;
    updateTextPosition();
}

void SyncedImageView::detachController()
{
    if (m_controller) {
        m_controller->removeView(this);
        m_controller = nullptr;
    }
}

void SyncedImageView::wheelEvent(QWheelEvent *event)
{
    if (!m_controller || !hasPixmap()) {
        QGraphicsView::wheelEvent(event);
        return;
    }
    if (event->angleDelta().y() == 0) {
        event->ignore();
        return;
    }
    const double factor = std::pow(1.25, static_cast<double>(event->angleDelta().y()) / 120.0);
    const double newZoom = m_zoom * factor;
    const QPointF scenePoint = mapToScene(event->position().toPoint());
    const QPointF ratio = ratioFromScenePoint(scenePoint);
    m_controller->requestZoom(this, newZoom, ratio);
    event->accept();
}

void SyncedImageView::resizeEvent(QResizeEvent *event)
{
    QGraphicsView::resizeEvent(event);
    updateTextPosition();
    if (m_controller && hasPixmap()) {
        m_blockScroll = true;
        centerOnRatio(m_controller->centerRatio());
        m_blockScroll = false;
    }
}

void SyncedImageView::updateTextPosition()
{
    if (!m_textItem || !m_textItem->isVisible()) {
        return;
    }
    const QRect viewportRect = viewport()->rect();
    const QPointF sceneCenter = mapToScene(viewportRect.center());
    const QRectF textRect = m_textItem->boundingRect();
    const QPointF pos(sceneCenter.x() - textRect.width() * 0.5,
                      sceneCenter.y() - textRect.height() * 0.5);
    m_textItem->setPos(pos);
}

void SyncedImageView::updateSceneRect()
{
    if (!m_scene) {
        return;
    }
    QRectF bounds;
    if (m_pixmapItem && !m_pixmapItem->pixmap().isNull()) {
        bounds = m_pixmapItem->boundingRect();
    } else {
        bounds = QRectF(QPointF(0.0, 0.0), QSizeF(1.0, 1.0));
    }
    m_scene->setSceneRect(bounds);
}

void SyncedImageView::connectScrollbars()
{
    auto connectBar = [this](QScrollBar *bar) {
        if (!bar) {
            return;
        }
        QObject::connect(bar, &QScrollBar::valueChanged, this, [this]() {
            handleScrollChanged();
        });
    };
    connectBar(horizontalScrollBar());
    connectBar(verticalScrollBar());
}

void SyncedImageView::handleScrollChanged()
{
    if (m_blockScroll || !m_controller || !hasPixmap()) {
        return;
    }
    m_controller->requestCenter(this, currentCenterRatio());
}

bool SyncedImageView::hasPixmap() const
{
    return m_pixmapItem && !m_pixmapItem->pixmap().isNull();
}

QPointF SyncedImageView::scenePointFromRatio(const QPointF &ratio) const
{
    if (!hasPixmap()) {
        return QPointF();
    }
    QRectF rect = m_pixmapItem->boundingRect();
    const double x = rect.left() + ratio.x() * rect.width();
    const double y = rect.top() + ratio.y() * rect.height();
    return QPointF(x, y);
}

QPointF SyncedImageView::ratioFromScenePoint(const QPointF &scenePoint) const
{
    if (!hasPixmap()) {
        return QPointF(0.5, 0.5);
    }
    QRectF rect = m_pixmapItem->boundingRect();
    const double width = std::max(rect.width(), 1.0);
    const double height = std::max(rect.height(), 1.0);
    const double rx = (scenePoint.x() - rect.left()) / width;
    const double ry = (scenePoint.y() - rect.top()) / height;
    return QPointF(std::clamp(rx, 0.0, 1.0), std::clamp(ry, 0.0, 1.0));
}

QPointF SyncedImageView::currentCenterRatio() const
{
    const QPoint viewportCenter = viewport()->rect().center();
    const QPointF sceneCenter = mapToScene(viewportCenter);
    return ratioFromScenePoint(sceneCenter);
}

void SyncedImageView::centerOnRatio(const QPointF &ratio)
{
    if (!hasPixmap()) {
        return;
    }
    const QPointF target = scenePointFromRatio(ratio);
    centerOn(target);
}

namespace {

bool isImageFile(const QString &path)
{
    static const QSet<QString> kExtensions = {
        QStringLiteral(".jpg"), QStringLiteral(".jpeg"), QStringLiteral(".png"),
        QStringLiteral(".bmp"), QStringLiteral(".tif"), QStringLiteral(".tiff"),
        QStringLiteral(".webp"), QStringLiteral(".gif")
    };
    const QString suffix = QFileInfo(path).suffix().toLower();
    return kExtensions.contains(QStringLiteral(".") + suffix);
}

QString canonicalPath(const QString &path)
{
    QFileInfo info(path);
    return info.canonicalFilePath().isEmpty() ? info.absoluteFilePath() : info.canonicalFilePath();
}

void removeDebugArtifacts(const DetectionResult &result)
{
    if (result.debugDirectory.empty()) {
        return;
    }
    std::error_code ec;
    std::filesystem::remove_all(result.debugDirectory, ec);
}

float computeUndistortedRadius(const cv::Point2f &center,
                               float radius,
                               const cv::Mat &cameraMatrix,
                               const cv::Mat &distCoeffs)
{
    if (radius <= 0.0f || cameraMatrix.empty() || distCoeffs.empty()) {
        return radius;
    }
    std::vector<cv::Point2f> sample = {
        center,
        center + cv::Point2f(radius, 0.0f),
        center - cv::Point2f(radius, 0.0f),
        center + cv::Point2f(0.0f, radius),
        center - cv::Point2f(0.0f, radius)
    };
    std::vector<cv::Point2f> mapped;
    cv::undistortPoints(sample, mapped, cameraMatrix, distCoeffs, cv::noArray(), cameraMatrix);
    if (mapped.size() < 2) {
        return radius;
    }
    const cv::Point2f c = mapped.front();
    double total = 0.0;
    for (size_t i = 1; i < mapped.size(); ++i) {
        total += cv::norm(mapped[i] - c);
    }
    const double count = static_cast<double>(mapped.size() - 1);
    return count > 0.0 ? static_cast<float>(total / count) : radius;
}

} // namespace

ImageEvaluationDialog::ImageEvaluationDialog(const CalibrationOutput &calibration,
                                             const BoardSpec &boardSpec,
                                             QWidget *parent)
    : QDialog(parent)
    , m_calibration(calibration)
    , m_boardSpec(boardSpec)
    , m_cameraMatrix(calibration.cameraMatrix.empty() ? cv::Mat() : calibration.cameraMatrix.clone())
    , m_distCoeffs(calibration.distCoeffs.empty() ? cv::Mat() : calibration.distCoeffs.clone())
{
    setWindowTitle(tr("Image Evaluation"));
    setModal(false);
    resize(1280, 780);
    setMinimumSize(1100, 700);
    setAcceptDrops(true);

    setupUi();
    updateControlsState();
}

ImageEvaluationDialog::~ImageEvaluationDialog()
{
    if (m_originalView) {
        m_originalView->detachController();
    }
    if (m_undistortedView) {
        m_undistortedView->detachController();
    }
    delete m_compareSync;
    m_compareSync = nullptr;
}

void ImageEvaluationDialog::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
    } else {
        QDialog::dragEnterEvent(event);
    }
}

void ImageEvaluationDialog::dropEvent(QDropEvent *event)
{
    QStringList paths;
    for (const QUrl &url : event->mimeData()->urls()) {
        if (!url.isLocalFile()) {
            continue;
        }
        const QString localPath = url.toLocalFile();
        QFileInfo info(localPath);
        if (info.isDir()) {
            QDirIterator it(localPath, QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                const QString candidate = it.next();
                if (isImageFile(candidate)) {
                    paths << candidate;
                }
            }
        } else if (isImageFile(localPath)) {
            paths << localPath;
        }
    }
    if (!paths.isEmpty()) {
        enqueuePaths(paths);
    }
    event->acceptProposedAction();
}

void ImageEvaluationDialog::handleAddImages()
{
    const QStringList files = QFileDialog::getOpenFileNames(this,
                                                            tr("Select images for evaluation"),
                                                            QString(),
                                                            tr("Images (*.png *.jpg *.jpeg *.bmp *.tif *.tiff *.webp *.gif)"));
    if (!files.isEmpty()) {
        enqueuePaths(files);
    }
}

void ImageEvaluationDialog::handleAddFolder()
{
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Select folder"));
    if (dir.isEmpty()) {
        return;
    }
    QStringList files;
    QDirIterator it(dir, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString candidate = it.next();
        if (isImageFile(candidate)) {
            files << candidate;
        }
    }
    if (files.isEmpty()) {
        QMessageBox::information(this, tr("No images"), tr("The selected folder does not contain supported image files."));
        return;
    }
    enqueuePaths(files);
}

void ImageEvaluationDialog::handleClear()
{
    if (m_pendingJobs > 0) {
        QMessageBox::information(this, tr("Processing"), tr("Please wait for current evaluations to finish."));
        return;
    }
    m_results.clear();
    m_loadedPaths.clear();
    if (m_itemList) {
        m_itemList->clear();
    }
    if (m_scatterView) {
        m_scatterView->clear();
    }
    if (m_metricsTable) {
        for (int row = 0; row < m_metricsTable->rowCount(); ++row) {
            auto ensureTextItem = [&](int column) {
                QTableWidgetItem *item = m_metricsTable->item(row, column);
                if (!item) {
                    item = new QTableWidgetItem(QStringLiteral("--"));
                    m_metricsTable->setItem(row, column, item);
                } else {
                    item->setText(QStringLiteral("--"));
                }
            };
            ensureTextItem(1);
            ensureTextItem(2);
        }
    }
    if (m_annotatedLabel) {
        m_annotatedLabel->setPixmap(QPixmap());
        m_annotatedLabel->setText(tr("拖放图像或使用上方按钮开始评估"));
    }
    if (m_originalView) {
        m_originalView->showMessage(tr("拖放图像或使用上方按钮开始评估"));
    }
    if (m_undistortedView) {
        m_undistortedView->showMessage(QString());
    }
    updateSummaryBanner();
    updateControlsState();
}

void ImageEvaluationDialog::handleSelectionChanged()
{
    refreshViews();
}

void ImageEvaluationDialog::handleToggleChanged()
{
    refreshViews();
}

QWidget *ImageEvaluationDialog::createLeftPanel()
{
    auto *panel = new QWidget(this);
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);

    auto *buttonRow = new QHBoxLayout();
    buttonRow->setSpacing(8);

    m_btnAddImages = new QPushButton(tr("添加图像"), panel);
    m_btnAddImages->setIcon(QIcon(QStringLiteral(":/icons/folder.svg")));
    buttonRow->addWidget(m_btnAddImages);

    m_btnAddFolder = new QPushButton(tr("添加文件夹"), panel);
    buttonRow->addWidget(m_btnAddFolder);

    m_btnClear = new QPushButton(tr("清空"), panel);
    buttonRow->addWidget(m_btnClear);

    layout->addLayout(buttonRow);

    m_progress = new QProgressBar(panel);
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    m_progress->setVisible(false);
    layout->addWidget(m_progress);

    m_hintLabel = new QLabel(tr("可拖放图像或目录到此窗口进行批量评估"), panel);
    m_hintLabel->setWordWrap(true);
    m_hintLabel->setStyleSheet(QStringLiteral("color: #7a88a8;"));
    layout->addWidget(m_hintLabel);

    m_summaryLabel = new QLabel(tr("尚未评估任何图像"), panel);
    m_summaryLabel->setWordWrap(true);
    QFont summaryFont = m_summaryLabel->font();
    summaryFont.setBold(true);
    m_summaryLabel->setFont(summaryFont);
    layout->addWidget(m_summaryLabel);

    m_itemList = new QListWidget(panel);
    m_itemList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_itemList->setAlternatingRowColors(true);
    layout->addWidget(m_itemList, 1);

    layout->addStretch(0);

    connect(m_btnAddImages, &QPushButton::clicked, this, &ImageEvaluationDialog::handleAddImages);
    connect(m_btnAddFolder, &QPushButton::clicked, this, &ImageEvaluationDialog::handleAddFolder);
    connect(m_btnClear, &QPushButton::clicked, this, &ImageEvaluationDialog::handleClear);
    connect(m_itemList, &QListWidget::currentRowChanged, this, &ImageEvaluationDialog::handleSelectionChanged);

    return panel;
}

QWidget *ImageEvaluationDialog::createAnnotatedTab()
{
    auto *tab = new QWidget(m_contentTabs);
    auto *layout = new QVBoxLayout(tab);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    auto *toggleRow = new QHBoxLayout();
    toggleRow->setSpacing(12);
    m_toggleGrid = new QCheckBox(tr("叠加编号网格"), tab);
    m_toggleGrid->setChecked(true);
    toggleRow->addWidget(m_toggleGrid);
    m_toggleMask = new QCheckBox(tr("叠加白域掩膜"), tab);
    m_toggleMask->setChecked(true);
    toggleRow->addWidget(m_toggleMask);
    toggleRow->addStretch(1);
    layout->addLayout(toggleRow);

    m_annotatedScroll = new QScrollArea(tab);
    m_annotatedScroll->setWidgetResizable(true);
    m_annotatedLabel = new QLabel(tr("拖放图像或使用上方按钮开始评估"), m_annotatedScroll);
    m_annotatedLabel->setAlignment(Qt::AlignCenter);
    m_annotatedLabel->setWordWrap(true);
    m_annotatedScroll->setWidget(m_annotatedLabel);
    layout->addWidget(m_annotatedScroll, 1);

    connect(m_toggleGrid, &QCheckBox::toggled, this, &ImageEvaluationDialog::handleToggleChanged);
    connect(m_toggleMask, &QCheckBox::toggled, this, &ImageEvaluationDialog::handleToggleChanged);

    return tab;
}

QWidget *ImageEvaluationDialog::createComparisonTab()
{
    auto *tab = new QWidget(m_contentTabs);
    auto *layout = new QVBoxLayout(tab);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    if (!m_compareSync) {
        m_compareSync = new ViewSyncController();
    }

    auto *splitter = new QSplitter(Qt::Horizontal, tab);

    m_originalView = new SyncedImageView(m_compareSync, splitter);
    m_originalView->showMessage(tr("拖放图像或使用上方按钮开始评估"));
    splitter->addWidget(m_originalView);

    m_undistortedView = new SyncedImageView(m_compareSync, splitter);
    m_undistortedView->showMessage(QString());
    splitter->addWidget(m_undistortedView);

    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);

    layout->addWidget(splitter, 1);

    return tab;
}

QWidget *ImageEvaluationDialog::createScatterTab()
{
    auto *tab = new QWidget(m_contentTabs);
    auto *layout = new QVBoxLayout(tab);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    m_scatterView = new ResidualScatterView(tab);
    layout->addWidget(m_scatterView, 1);

    return tab;
}

QWidget *ImageEvaluationDialog::createMetricsTab()
{
    auto *tab = new QWidget(m_contentTabs);
    auto *layout = new QVBoxLayout(tab);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    m_metricsTable = new QTableWidget(8, 3, tab);
    QStringList headers;
    headers << tr("指标") << tr("校正后") << tr("未校正");
    m_metricsTable->setHorizontalHeaderLabels(headers);
    m_metricsTable->verticalHeader()->setVisible(false);
    m_metricsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_metricsTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_metricsTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_metricsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_metricsTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_metricsTable->setFocusPolicy(Qt::NoFocus);
    m_metricsTable->setAlternatingRowColors(true);

    const QString labels[8] = {
        tr("平均 (px)"),
        tr("均方根 (px)"),
        tr("中位数 (px)"),
        tr("P95 (px)"),
        tr("最大 (px)"),
        tr("平均 (mm)"),
        tr("均方根 (mm)"),
        tr("最大 (mm)")
    };
    for (int row = 0; row < 8; ++row) {
        m_metricsTable->setItem(row, 0, new QTableWidgetItem(labels[row]));
        m_metricsTable->setItem(row, 1, new QTableWidgetItem(QStringLiteral("--")));
        m_metricsTable->setItem(row, 2, new QTableWidgetItem(QStringLiteral("--")));
    }

    layout->addWidget(m_metricsTable, 1);

    return tab;
}

void ImageEvaluationDialog::setupTabs()
{
    if (!m_contentTabs) {
        return;
    }
    m_contentTabs->clear();
    m_contentTabs->addTab(createAnnotatedTab(), tr("注释视图"));
    m_contentTabs->addTab(createComparisonTab(), tr("畸变对比"));
    m_contentTabs->addTab(createScatterTab(), tr("残差散点"));
    m_contentTabs->addTab(createMetricsTab(), tr("指标对比"));
}

void ImageEvaluationDialog::setupUi()
{
    auto *mainLayout = new QHBoxLayout(this);
    mainLayout->setContentsMargins(18, 18, 18, 18);
    mainLayout->setSpacing(12);

    auto *mainSplitter = new QSplitter(Qt::Horizontal, this);
    mainSplitter->setChildrenCollapsible(false);
    mainSplitter->addWidget(createLeftPanel());

    m_contentTabs = new QTabWidget(mainSplitter);
    m_contentTabs->setTabPosition(QTabWidget::North);
    setupTabs();
    mainSplitter->addWidget(m_contentTabs);
    mainSplitter->setStretchFactor(0, 1);
    mainSplitter->setStretchFactor(1, 3);

    mainLayout->addWidget(mainSplitter, 1);
}

void ImageEvaluationDialog::enqueuePaths(const QStringList &paths)
{
    QStringList unique;
    unique.reserve(paths.size());
    for (const QString &path : paths) {
        const QString canon = canonicalPath(path);
        if (canon.isEmpty() || m_loadedPaths.contains(canon)) {
            continue;
        }
        unique << canon;
    }
    if (unique.isEmpty()) {
        return;
    }

    std::sort(unique.begin(), unique.end());

    for (const QString &path : unique) {
        EvaluationResult placeholder;
        placeholder.filePath = path;
        placeholder.displayName = QFileInfo(path).fileName();
        m_results.push_back(std::move(placeholder));
        const int index = m_results.size() - 1;

        auto *item = new QListWidgetItem(tr("处理中 · %1").arg(QFileInfo(path).fileName()));
        item->setData(Qt::UserRole, index);
        if (m_itemList) {
            m_itemList->addItem(item);
            if (m_itemList->currentRow() < 0) {
                m_itemList->setCurrentItem(item);
            }
        }

        m_loadedPaths.insert(path);
        m_pendingJobs++;
        if (m_progress) {
            m_progress->setVisible(true);
            m_progress->setRange(0, 0);
        }

        auto future = QtConcurrent::run([this, path]() { return evaluateImage(path); });
        auto *watcher = new QFutureWatcher<EvaluationResult>(this);
        connect(watcher, &QFutureWatcher<EvaluationResult>::finished, this, [this, watcher, index]() {
            applyResult(index, watcher->result());
            watcher->deleteLater();
            m_pendingJobs = std::max(0, m_pendingJobs - 1);
            if (m_progress && m_pendingJobs == 0) {
                m_progress->setVisible(false);
            }
            updateControlsState();
            updateSummaryBanner();
            if (m_pendingJobs == 0) {
                refreshViews();
            }
        });
        watcher->setFuture(future);
    }

    updateControlsState();
}

ImageEvaluationDialog::EvaluationResult ImageEvaluationDialog::evaluateImage(const QString &path) const
{
    EvaluationResult result;
    result.filePath = path;
    result.displayName = QFileInfo(path).fileName();

    cv::Mat original = cv::imread(path.toStdString(), cv::IMREAD_COLOR);
    if (original.empty()) {
        result.message = tr("无法读取图像文件");
        return result;
    }

    if (m_cameraMatrix.empty()) {
        result.message = tr("当前会话缺少相机内参，无法评估");
        return result;
    }

    result.originalBgr = original;
    result.resolution = original.size();

    BoardDetector detector;
    DetectionResult detection = detector.detect(original, m_boardSpec, path.toStdString());
    removeDebugArtifacts(detection);

    if (!detection.success) {
        result.message = QString::fromStdString(detection.message);
        return result;
    }

    result.imagePoints = detection.imagePoints;
    result.objectPoints = detection.objectPoints;
    result.logicalIndices = detection.logicalIndices;
    result.bigCirclePoints = detection.bigCirclePoints;
    result.circleRadii = detection.circleRadiiPx;
    result.bigCircleRadii = detection.bigCircleRadiiPx;

    if (!detection.whiteRegionMask.empty()) {
        if (detection.whiteRegionMask.size() != original.size()) {
            cv::resize(detection.whiteRegionMask, result.whiteRegionMask, original.size(), 0, 0, cv::INTER_NEAREST);
        } else {
            result.whiteRegionMask = detection.whiteRegionMask.clone();
        }
    }

    if (!m_cameraMatrix.empty()) {
        cv::undistort(original, result.undistortedBgr, m_cameraMatrix, m_distCoeffs);
        if (!result.imagePoints.empty()) {
            cv::undistortPoints(result.imagePoints, result.undistortedPoints, m_cameraMatrix, m_distCoeffs, cv::noArray(), m_cameraMatrix);
        }
        if (!result.bigCirclePoints.empty()) {
            if (!m_distCoeffs.empty()) {
                std::vector<cv::Point2f> undistBig;
                cv::undistortPoints(result.bigCirclePoints, undistBig, m_cameraMatrix, m_distCoeffs, cv::noArray(), m_cameraMatrix);
                result.bigCirclePointsUndistorted = std::move(undistBig);
            } else {
                result.bigCirclePointsUndistorted = result.bigCirclePoints;
            }
        } else {
            result.bigCirclePointsUndistorted.clear();
        }
        if (!result.circleRadii.empty()) {
            result.circleRadiiUndistorted.resize(result.circleRadii.size());
            for (size_t i = 0; i < result.circleRadii.size(); ++i) {
                const cv::Point2f center = (i < result.imagePoints.size())
                                               ? result.imagePoints[static_cast<size_t>(i)]
                                               : cv::Point2f();
                result.circleRadiiUndistorted[i] = computeUndistortedRadius(center,
                                                                           result.circleRadii[i],
                                                                           m_cameraMatrix,
                                                                           m_distCoeffs);
            }
        } else {
            result.circleRadiiUndistorted.clear();
        }
        if (!result.bigCircleRadii.empty()) {
            result.bigCircleRadiiUndistorted.resize(result.bigCircleRadii.size());
            for (size_t i = 0; i < result.bigCircleRadii.size(); ++i) {
                const cv::Point2f center = (i < result.bigCirclePoints.size())
                                               ? result.bigCirclePoints[static_cast<size_t>(i)]
                                               : cv::Point2f();
                result.bigCircleRadiiUndistorted[i] = computeUndistortedRadius(center,
                                                                               result.bigCircleRadii[i],
                                                                               m_cameraMatrix,
                                                                               m_distCoeffs);
            }
        } else {
            result.bigCircleRadiiUndistorted.clear();
        }
        if (!result.whiteRegionMask.empty()) {
            cv::Mat map1, map2;
            cv::initUndistortRectifyMap(m_cameraMatrix, m_distCoeffs, cv::Mat(), m_cameraMatrix,
                                        original.size(), CV_32FC1, map1, map2);
            cv::remap(result.whiteRegionMask, result.whiteRegionMaskUndistorted, map1, map2, cv::INTER_NEAREST);
        }
    }

    if (result.imagePoints.size() < 4 || result.objectPoints.size() != result.imagePoints.size()) {
        result.message = tr("检测到的角点数量不足");
        return result;
    }

    cv::Mat rvec, tvec;
    bool pnpOk = cv::solvePnP(result.objectPoints, result.imagePoints, m_cameraMatrix, m_distCoeffs, rvec, tvec);
    if (!pnpOk) {
        result.message = tr("位姿求解失败");
        return result;
    }

    cv::Mat projected;
    cv::projectPoints(result.objectPoints, rvec, tvec, m_cameraMatrix, m_distCoeffs, projected);

    cv::Mat zeroDist = m_distCoeffs.empty() ? cv::Mat() : cv::Mat::zeros(m_distCoeffs.size(), m_distCoeffs.type());
    cv::Mat projectedNoCorr;
    cv::projectPoints(result.objectPoints, rvec, tvec, m_cameraMatrix, zeroDist, projectedNoCorr);

    cv::Matx33d R;
    cv::Rodrigues(rvec, R);
    cv::Vec3d tvecVec(tvec.at<double>(0, 0), tvec.at<double>(1, 0), tvec.at<double>(2, 0));

    result.poseValid = true;
    result.rotation = R;
    result.rotationVector = cv::Vec3d(rvec.at<double>(0, 0), rvec.at<double>(1, 0), rvec.at<double>(2, 0));
    result.translation = tvecVec;

    result.residualsPx.reserve(result.imagePoints.size());
    result.residualsMm.reserve(result.imagePoints.size());
    result.residualsPxNoCorrection.reserve(result.imagePoints.size());
    std::vector<double> residualsMmNoCorrection;
    residualsMmNoCorrection.reserve(result.imagePoints.size());
    result.scatterSamples.reserve(result.imagePoints.size());

    cv::Mat camera = m_cameraMatrix.empty() ? cv::Mat::eye(3, 3, CV_64F) : m_cameraMatrix;
    const double fx = camera.at<double>(0, 0);
    const double fy = camera.at<double>(1, 1);

    std::vector<double> depths;
    depths.reserve(result.objectPoints.size());

    for (size_t i = 0; i < result.objectPoints.size(); ++i) {
        const cv::Point3f &obj = result.objectPoints[i];
        const cv::Point2f &obs = result.imagePoints[i];
        const cv::Point2f repro = projected.at<cv::Point2f>(static_cast<int>(i), 0);
        const cv::Point2f reproNoCorr = projectedNoCorr.at<cv::Point2f>(static_cast<int>(i), 0);

        cv::Vec3d camPoint = R * cv::Vec3d(obj.x, obj.y, obj.z) + tvecVec;
        const double Z = std::max(1e-6, camPoint[2]);
        depths.push_back(Z);

        const double dx = static_cast<double>(obs.x - repro.x);
        const double dy = static_cast<double>(obs.y - repro.y);
        const double magPx = std::hypot(dx, dy);
        const double mmX = dx * (Z / fx);
        const double mmY = dy * (Z / fy);
        const double magMm = std::hypot(mmX, mmY);

        result.residualsPx.push_back(magPx);
        result.residualsMm.push_back(magMm);

        ResidualScatterView::Sample sample;
        sample.deltaPx = QPointF(dx, dy);
        sample.magnitudePx = static_cast<float>(magPx);
        sample.magnitudeMm = static_cast<float>(magMm);
        result.scatterSamples.push_back(sample);

        const double dxNo = static_cast<double>(obs.x - reproNoCorr.x);
        const double dyNo = static_cast<double>(obs.y - reproNoCorr.y);
        const double magPxNo = std::hypot(dxNo, dyNo);
        const double mmXNo = dxNo * (Z / fx);
        const double mmYNo = dyNo * (Z / fy);
        const double magMmNo = std::hypot(mmXNo, mmYNo);

        result.residualsPxNoCorrection.push_back(magPxNo);
        residualsMmNoCorrection.push_back(magMmNo);
    }

    result.calibrated = computeMetrics(result.residualsPx, result.residualsMm);
    result.uncorrected = computeMetrics(result.residualsPxNoCorrection, residualsMmNoCorrection);
    result.success = true;
    result.message = tr("评估成功");
    return result;
}

void ImageEvaluationDialog::applyResult(int index, EvaluationResult &&result)
{
    if (index < 0 || index >= m_results.size()) {
        return;
    }
    m_results[index] = std::move(result);
    updateListItem(index);
    if (m_itemList && m_itemList->currentRow() == index) {
        refreshViews();
    }
}

void ImageEvaluationDialog::refreshViews()
{
    if (!m_itemList || m_results.isEmpty()) {
        return;
    }
    const int row = m_itemList->currentRow();
    if (row < 0 || row >= m_results.size()) {
        return;
    }
    const EvaluationResult &res = m_results[row];
    updateAnnotatedView(res);
    updateComparisonView(res);
    updateScatterView(res);
    updateMetricsTable(res);
}

cv::Mat ImageEvaluationDialog::blendMask(const cv::Mat &base,
                                         const cv::Mat &mask,
                                         const cv::Mat &boardMask,
                                         double opacity)
{
    if (base.empty() || mask.empty()) {
        return base.clone();
    }

    cv::Mat overlay = base.clone();

    cv::Mat mask8u;
    if (mask.type() == CV_8U) {
        mask8u = mask;
    } else {
        double scale = (mask.depth() == CV_32F || mask.depth() == CV_64F) ? 255.0 : 1.0;
        mask.convertTo(mask8u, CV_8U, scale);
    }
    cv::threshold(mask8u, mask8u, 1, 255, cv::THRESH_BINARY);

    const cv::Scalar whiteRegionColor(180, 210, 255);
    overlay.setTo(whiteRegionColor, mask8u);

    if (!boardMask.empty()) {
        cv::Mat boardMask8u;
        if (boardMask.type() == CV_8U) {
            boardMask8u = boardMask;
        } else {
            boardMask.convertTo(boardMask8u, CV_8U);
        }
        cv::threshold(boardMask8u, boardMask8u, 1, 255, cv::THRESH_BINARY);
        const cv::Scalar boardColor(40, 120, 255);
        overlay.setTo(boardColor, boardMask8u);
    }

    cv::Mat blended;
    cv::addWeighted(overlay, opacity, base, 1.0 - opacity, 0.0, blended);
    return blended;
}

cv::Mat ImageEvaluationDialog::buildBoardMask(const EvaluationResult &result,
                                              const cv::Size &size,
                                              bool useUndistortedPoints)
{
    cv::Mat mask = cv::Mat::zeros(size, CV_8UC1);
    const auto &points = useUndistortedPoints && !result.undistortedPoints.empty()
                             ? result.undistortedPoints
                             : result.imagePoints;
    if (points.size() < 3) {
        return mask;
    }

    std::vector<cv::Point2f> hull;
    cv::convexHull(points, hull);
    if (hull.size() < 3) {
        return mask;
    }

    std::vector<cv::Point> polygon;
    polygon.reserve(hull.size());
    for (const cv::Point2f &pt : hull) {
        polygon.emplace_back(static_cast<int>(std::round(pt.x)),
                             static_cast<int>(std::round(pt.y)));
    }
    std::vector<std::vector<cv::Point>> polys(1, polygon);
    cv::fillPoly(mask, polys, cv::Scalar(255));
    return mask;
}

void ImageEvaluationDialog::drawAxes(cv::Mat &canvas,
                                     const EvaluationResult &result,
                                     bool undistorted) const
{
    if (!result.poseValid || canvas.empty() || m_cameraMatrix.empty()) {
        return;
    }

    const double axisLenMm = std::max(3.0 * m_boardSpec.centerSpacingMm, 100.0);
    std::vector<cv::Point3f> axes = {
        {0.f, 0.f, 0.f},
        {static_cast<float>(axisLenMm), 0.f, 0.f},
        {0.f, static_cast<float>(axisLenMm), 0.f},
        {0.f, 0.f, static_cast<float>(axisLenMm)}
    };

    cv::Mat distCoeffs;
    if (undistorted && !m_distCoeffs.empty()) {
        distCoeffs = cv::Mat::zeros(m_distCoeffs.size(), m_distCoeffs.type());
    } else {
        distCoeffs = m_distCoeffs;
    }

    std::vector<cv::Point2f> projected;
    cv::projectPoints(axes,
                      result.rotationVector,
                      result.translation,
                      m_cameraMatrix,
                      distCoeffs,
                      projected);

    if (projected.size() != axes.size()) {
        return;
    }

    const cv::Point origin(projected[0]);
    const cv::Point xAxis(projected[1]);
    const cv::Point yAxis(projected[2]);
    const cv::Point zAxis(projected[3]);

    auto drawArrow = [&](const cv::Point &to, const cv::Scalar &color, const char *label) {
        cv::arrowedLine(canvas, origin, to, color, 2, cv::LINE_AA, 0, 0.12);
        cv::putText(canvas,
                    label,
                    to + cv::Point(4, -4),
                    cv::FONT_HERSHEY_SIMPLEX,
                    0.5,
                    color,
                    2,
                    cv::LINE_AA);
    };

    drawArrow(xAxis, cv::Scalar(60, 60, 220), "X");
    drawArrow(yAxis, cv::Scalar(80, 200, 80), "Y");
    drawArrow(zAxis, cv::Scalar(220, 70, 70), "Z");
}

void ImageEvaluationDialog::drawGrid(cv::Mat &canvas,
                                     const EvaluationResult &result,
                                     const BoardSpec &spec,
                                     bool useUndistortedPoints)
{
    static const std::array<cv::Scalar, 7> rowColors = {
        cv::Scalar(255, 206, 86),
        cv::Scalar(129, 212, 250),
        cv::Scalar(186, 104, 200),
        cv::Scalar(255, 167, 112),
        cv::Scalar(144, 238, 144),
        cv::Scalar(173, 190, 255),
        cv::Scalar(255, 221, 153)
    };

    const auto &points = useUndistortedPoints && !result.undistortedPoints.empty()
                             ? result.undistortedPoints
                             : result.imagePoints;
    const std::vector<float> &radii = useUndistortedPoints && !result.circleRadiiUndistorted.empty()
                                          ? result.circleRadiiUndistorted
                                          : result.circleRadii;
    const std::vector<cv::Point2f> &largeCenters = useUndistortedPoints && !result.bigCirclePointsUndistorted.empty()
                                                       ? result.bigCirclePointsUndistorted
                                                       : result.bigCirclePoints;
    const std::vector<float> &largeRadii = useUndistortedPoints && !result.bigCircleRadiiUndistorted.empty()
                                                ? result.bigCircleRadiiUndistorted
                                                : result.bigCircleRadii;
    if (points.empty()) {
        return;
    }

    auto brighten = [](const cv::Scalar &color, double delta) {
        return cv::Scalar(std::min(255.0, color[0] + delta),
                          std::min(255.0, color[1] + delta),
                          std::min(255.0, color[2] + delta));
    };

    for (size_t i = 0; i < points.size(); ++i) {
        const cv::Point2f &pt = points[i];
        if (!std::isfinite(pt.x) || !std::isfinite(pt.y)) {
            continue;
        }
        cv::Point center(cvRound(pt.x), cvRound(pt.y));
        int rowIdx = 0;
        QString labelText;
        if (i < result.logicalIndices.size()) {
            const cv::Vec2i logical = result.logicalIndices[i];
            rowIdx = std::clamp(logical[0], 0, static_cast<int>(rowColors.size() - 1));
            labelText = QStringLiteral("%1:%2").arg(logical[0]).arg(logical[1]);
        } else {
            rowIdx = static_cast<int>(i % rowColors.size());
            if (!result.objectPoints.empty()) {
                const double spacing = std::max(spec.centerSpacingMm, 1e-3);
                const double x = result.objectPoints[i].x;
                const double y = result.objectPoints[i].y;
                labelText = QStringLiteral("%1,%2").arg(static_cast<int>(std::round(y / spacing)))
                                .arg(static_cast<int>(std::round(x / spacing)));
            } else {
                labelText = QString::number(static_cast<int>(i + 1));
            }
        }
        const cv::Scalar color = rowColors[static_cast<size_t>(rowIdx)];
        const double radius = (i < radii.size()) ? radii[i] : 0.0;
        const int outlineRadius = radius > 0.5 ? static_cast<int>(std::round(radius)) : 12;
        const cv::Scalar outlineColor = brighten(color, 60.0);
        cv::circle(canvas, center, outlineRadius, outlineColor, 2, cv::LINE_AA);
        const int fillRadius = radius > 0.5 ? std::max(3, static_cast<int>(std::round(std::max(radius * 0.18, 3.0)))) : 5;
        cv::circle(canvas, center, fillRadius, color, -1, cv::LINE_AA);

        const std::string text = labelText.toStdString();
        cv::putText(canvas, text, center + cv::Point(10, -10), cv::FONT_HERSHEY_SIMPLEX, 0.45,
                    cv::Scalar(12, 16, 26), 3, cv::LINE_AA);
        cv::putText(canvas, text, center + cv::Point(10, -10), cv::FONT_HERSHEY_SIMPLEX, 0.45,
                    cv::Scalar(245, 250, 255), 1, cv::LINE_AA);
    }

    if (!largeCenters.empty()) {
        const cv::Scalar bigColor(40, 90, 240);
        const cv::Scalar bigFill(230, 240, 255);
        for (size_t i = 0; i < largeCenters.size(); ++i) {
            const cv::Point2f &pt = largeCenters[i];
            if (!std::isfinite(pt.x) || !std::isfinite(pt.y)) {
                continue;
            }
            const cv::Point center(cvRound(pt.x), cvRound(pt.y));
            const double radius = (i < largeRadii.size()) ? largeRadii[i] : 0.0;
            const int outlineRadius = radius > 0.5 ? static_cast<int>(std::round(radius)) : 18;
            cv::circle(canvas, center, outlineRadius, bigColor, 3, cv::LINE_AA);
            const int fillRadius = std::max(6, outlineRadius / 5);
            cv::circle(canvas, center, fillRadius, bigFill, -1, cv::LINE_AA);
        }
    }
}

cv::Mat ImageEvaluationDialog::renderAnnotated(const EvaluationResult &result,
                                               bool showGrid,
                                               bool showMask) const
{
    if (result.originalBgr.empty()) {
        return {};
    }
    cv::Mat annotated = result.originalBgr.clone();
    if (showMask && !result.whiteRegionMask.empty()) {
        const cv::Mat boardMask = buildBoardMask(result, annotated.size(), false);
        annotated = blendMask(annotated, result.whiteRegionMask, boardMask, 0.6);
    }
    if (showGrid) {
        drawGrid(annotated, result, m_boardSpec, false);
    }
    drawAxes(annotated, result, false);
    return annotated;
}

cv::Mat ImageEvaluationDialog::renderUndistortedAnnotated(const EvaluationResult &result,
                                                          bool showGrid,
                                                          bool showMask) const
{
    if (result.undistortedBgr.empty()) {
        return {};
    }
    cv::Mat annotated = result.undistortedBgr.clone();
    if (showMask && !result.whiteRegionMaskUndistorted.empty()) {
        const cv::Mat boardMask = buildBoardMask(result, annotated.size(), true);
        annotated = blendMask(annotated, result.whiteRegionMaskUndistorted, boardMask, 0.5);
    }
    if (showGrid) {
        drawGrid(annotated, result, m_boardSpec, true);
    }
    drawAxes(annotated, result, true);
    return annotated;
}

void ImageEvaluationDialog::updateAnnotatedView(const EvaluationResult &result)
{
    if (!m_annotatedLabel) {
        return;
    }
    if (!result.success) {
        m_annotatedLabel->setText(result.message.isEmpty() ? tr("评估失败") : result.message);
        m_annotatedLabel->setPixmap(QPixmap());
        return;
    }
    cv::Mat annotated = renderAnnotated(result,
                                        m_toggleGrid && m_toggleGrid->isChecked(),
                                        m_toggleMask && m_toggleMask->isChecked());
    if (annotated.empty()) {
        m_annotatedLabel->setText(tr("无法生成注释视图"));
        m_annotatedLabel->setPixmap(QPixmap());
        return;
    }
    QImage image = toQImage(annotated);
    m_annotatedLabel->setPixmap(QPixmap::fromImage(image));
    m_annotatedLabel->setAlignment(Qt::AlignCenter);
    m_annotatedLabel->setMinimumSize(image.size());
}

void ImageEvaluationDialog::updateComparisonView(const EvaluationResult &result)
{
    if (!m_originalView || !m_undistortedView) {
        return;
    }
    if (!result.success) {
        const QString message = result.message.isEmpty() ? tr("评估失败") : result.message;
        m_originalView->showMessage(message);
        m_undistortedView->showMessage(QString());
        return;
    }

    cv::Mat original = renderAnnotated(result,
                                       m_toggleGrid && m_toggleGrid->isChecked(),
                                       m_toggleMask && m_toggleMask->isChecked());
    if (original.empty()) {
        original = result.originalBgr;
    }
    if (!original.empty()) {
        QImage origImage = toQImage(original);
        m_originalView->setPixmap(QPixmap::fromImage(origImage));
    } else {
        m_originalView->showMessage(tr("无法生成原始图像"));
    }

    cv::Mat undistorted = renderUndistortedAnnotated(result,
                                                     m_toggleGrid && m_toggleGrid->isChecked(),
                                                     m_toggleMask && m_toggleMask->isChecked());
    if (undistorted.empty() && !result.undistortedBgr.empty()) {
        undistorted = result.undistortedBgr;
    }
    if (!undistorted.empty()) {
        QImage undistImage = toQImage(undistorted);
        m_undistortedView->setPixmap(QPixmap::fromImage(undistImage));
    } else {
        m_undistortedView->showMessage(tr("未生成去畸变图"));
    }
}

void ImageEvaluationDialog::updateScatterView(const EvaluationResult &result)
{
    if (!m_scatterView) {
        return;
    }
    if (!result.success || result.scatterSamples.empty()) {
        m_scatterView->clear();
        return;
    }
    const float maxPx = static_cast<float>(std::max(result.calibrated.maxPx, 1e-3));
    const float maxMm = static_cast<float>(std::max(result.calibrated.maxMm, 1e-3));
    m_scatterView->setSamples(result.scatterSamples, maxPx, maxMm);
}

QString ImageEvaluationDialog::formatNumber(double value, int precision)
{
    if (!std::isfinite(value)) {
        return QStringLiteral("--");
    }
    return QString::number(value, 'f', precision);
}

void ImageEvaluationDialog::updateMetricsTable(const EvaluationResult &result)
{
    if (!m_metricsTable) {
        return;
    }
    if (!result.success) {
        for (int row = 0; row < 8; ++row) {
            m_metricsTable->item(row, 1)->setText(QStringLiteral("--"));
            m_metricsTable->item(row, 2)->setText(QStringLiteral("--"));
        }
        return;
    }

    const EvaluationMetrics &a = result.calibrated;
    const EvaluationMetrics &b = result.uncorrected;

    const double calibratedValues[8] = {
        a.meanPx,
        a.rmsPx,
        a.medianPx,
        a.p95Px,
        a.maxPx,
        a.meanMm,
        a.rmsMm,
        a.maxMm
    };
    const double uncorrectedValues[8] = {
        b.meanPx,
        b.rmsPx,
        b.medianPx,
        b.p95Px,
        b.maxPx,
        b.meanMm,
        b.rmsMm,
        b.maxMm
    };

    for (int row = 0; row < 8; ++row) {
        m_metricsTable->item(row, 1)->setText(formatNumber(calibratedValues[row]));
        m_metricsTable->item(row, 2)->setText(formatNumber(uncorrectedValues[row]));
    }
}

void ImageEvaluationDialog::updateListItem(int index)
{
    if (!m_itemList || index < 0 || index >= m_itemList->count()) {
        return;
    }
    QListWidgetItem *item = m_itemList->item(index);
    const EvaluationResult &res = m_results[index];
    if (!res.success) {
        item->setText(tr("❌ %1 — %2").arg(res.displayName, res.message));
        item->setForeground(QColor(200, 100, 100));
        return;
    }

    QString summary = tr("mean %.3f px | max %.3f px")
                          .arg(res.calibrated.meanPx, 0, 'f', 3)
                          .arg(res.calibrated.maxPx, 0, 'f', 3);
    QString delta;
    if (res.uncorrected.meanPx > 1e-6) {
        const double improvement = (res.uncorrected.meanPx - res.calibrated.meanPx) / res.uncorrected.meanPx * 100.0;
        delta = tr("改善 %.1f%%", "improvement percentage").arg(improvement, 0, 'f', 1);
    }
    item->setText(QStringLiteral("✅ %1\n%2%3")
                      .arg(res.displayName)
                      .arg(summary)
                      .arg(delta.isEmpty() ? QString() : QStringLiteral(" · %1").arg(delta)));
    item->setForeground(QBrush());
}

void ImageEvaluationDialog::updateSummaryBanner()
{
    if (!m_summaryLabel) {
        return;
    }
    int successCount = 0;
    double totalSum = 0.0;
    double totalSumSq = 0.0;
    int totalSamples = 0;
    double totalSumNo = 0.0;
    int totalSamplesNo = 0;
    std::vector<double> allResiduals;
    allResiduals.reserve(512);

    for (const auto &res : m_results) {
        if (!res.success) {
            continue;
        }
        successCount++;
        totalSum += res.calibrated.sumPx;
        totalSumSq += res.calibrated.sumSqPx;
        totalSamples += res.calibrated.sampleCount;
        totalSumNo += res.uncorrected.sumPx;
        totalSamplesNo += res.uncorrected.sampleCount;
        allResiduals.insert(allResiduals.end(), res.residualsPx.begin(), res.residualsPx.end());
    }

    if (successCount == 0 || totalSamples == 0) {
        m_summaryLabel->setText(tr("尚未评估任何图像"));
        return;
    }

    const double mean = totalSum / totalSamples;
    const double rms = std::sqrt(std::max(0.0, totalSumSq / totalSamples));
    const double meanNo = totalSamplesNo > 0 ? totalSumNo / totalSamplesNo : 0.0;
    const double p95 = percentile(allResiduals, 0.95);

    double improvement = 0.0;
    if (meanNo > 1e-6) {
        improvement = (meanNo - mean) / meanNo * 100.0;
    }

    m_summaryLabel->setText(tr("已评估 %1 张图像 · 校正均值 %2 px · RMS %3 px · P95 %4 px · 相比未校正提升 %5%")
                                .arg(successCount)
                                .arg(formatNumber(mean))
                                .arg(formatNumber(rms))
                                .arg(formatNumber(p95))
                                .arg(formatNumber(improvement, 1)));
}

void ImageEvaluationDialog::updateControlsState()
{
    const bool busy = m_pendingJobs > 0;
    if (m_btnClear) {
        m_btnClear->setEnabled(!busy && !m_results.isEmpty());
    }
    if (m_btnAddImages) {
        m_btnAddImages->setEnabled(true);
    }
    if (m_btnAddFolder) {
        m_btnAddFolder->setEnabled(true);
    }
}

ImageEvaluationDialog::EvaluationMetrics ImageEvaluationDialog::computeMetrics(const std::vector<double> &residualsPx,
                                                                               const std::vector<double> &residualsMm)
{
    EvaluationMetrics metrics;
    if (residualsPx.empty()) {
        return metrics;
    }
    const int count = static_cast<int>(residualsPx.size());
    metrics.sampleCount = count;
    metrics.sumPx = std::accumulate(residualsPx.begin(), residualsPx.end(), 0.0);
    metrics.sumSqPx = std::accumulate(residualsPx.begin(), residualsPx.end(), 0.0,
                                      [](double acc, double v) { return acc + v * v; });
    metrics.meanPx = metrics.sumPx / count;
    metrics.rmsPx = std::sqrt(std::max(0.0, metrics.sumSqPx / count));
    metrics.maxPx = *std::max_element(residualsPx.begin(), residualsPx.end());
    metrics.medianPx = percentile(residualsPx, 0.5);
    metrics.p95Px = percentile(residualsPx, 0.95);

    if (!residualsMm.empty()) {
        metrics.meanMm = std::accumulate(residualsMm.begin(), residualsMm.end(), 0.0) / residualsMm.size();
        double sumSqMm = std::accumulate(residualsMm.begin(), residualsMm.end(), 0.0,
                                         [](double acc, double v) { return acc + v * v; });
        metrics.rmsMm = std::sqrt(std::max(0.0, sumSqMm / residualsMm.size()));
        metrics.maxMm = *std::max_element(residualsMm.begin(), residualsMm.end());
    }

    return metrics;
}

double ImageEvaluationDialog::percentile(const std::vector<double> &values, double q)
{
    if (values.empty()) {
        return 0.0;
    }
    q = std::clamp(q, 0.0, 1.0);
    std::vector<double> sorted(values.begin(), values.end());
    std::sort(sorted.begin(), sorted.end());
    const double pos = q * (sorted.size() - 1);
    const size_t idx = static_cast<size_t>(std::floor(pos));
    const size_t idx2 = static_cast<size_t>(std::ceil(pos));
    if (idx == idx2) {
        return sorted[idx];
    }
    const double weight = pos - idx;
    return sorted[idx] * (1.0 - weight) + sorted[idx2] * weight;
}

QImage ImageEvaluationDialog::toQImage(const cv::Mat &mat)
{
    if (mat.empty()) {
        return {};
    }
    cv::Mat converted;
    if (mat.type() == CV_8UC3) {
        cv::cvtColor(mat, converted, cv::COLOR_BGR2RGB);
        return QImage(converted.data, converted.cols, converted.rows, converted.step, QImage::Format_RGB888).copy();
    }
    if (mat.type() == CV_8UC1) {
        return QImage(mat.data, mat.cols, mat.rows, mat.step, QImage::Format_Grayscale8).copy();
    }
    mat.convertTo(converted, CV_8UC3);
    cv::cvtColor(converted, converted, cv::COLOR_BGR2RGB);
    return QImage(converted.data, converted.cols, converted.rows, converted.step, QImage::Format_RGB888).copy();
}

} // namespace mycalib
