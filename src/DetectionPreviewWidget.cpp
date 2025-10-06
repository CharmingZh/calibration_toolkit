#include "DetectionPreviewWidget.h"

#include <algorithm>
#include <cmath>

#include <QComboBox>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPalette>
#include <QPainter>
#include <QResizeEvent>
#include <QSizePolicy>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStringList>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace mycalib {

namespace {

cv::Mat normalizeTo8U(const cv::Mat &input)
{
    if (input.empty()) {
        return {};
    }
    if (input.depth() == CV_8U) {
        return input;
    }
    cv::Mat normalized;
    double minVal = 0.0;
    double maxVal = 0.0;
    cv::minMaxLoc(input, &minVal, &maxVal);
    if (!std::isfinite(minVal) || !std::isfinite(maxVal) || std::abs(maxVal - minVal) < 1e-6) {
        normalized = cv::Mat::zeros(input.size(), CV_8U);
    } else {
        const double scale = 255.0 / (maxVal - minVal);
        const double shift = -minVal * scale;
        input.convertTo(normalized, CV_8U, scale, shift);
    }
    return normalized;
}

} // namespace

DetectionPreviewWidget::DetectionPreviewWidget(QWidget *parent)
    : QWidget(parent)
{
    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(12, 12, 12, 12);
    rootLayout->setSpacing(8);

    m_titleLabel = new QLabel(tr("Select an image on the left to inspect detection details"), this);
    QFont titleFont = m_titleLabel->font();
    titleFont.setPointSizeF(titleFont.pointSizeF() + 1.5);
    titleFont.setBold(true);
    m_titleLabel->setFont(titleFont);
    m_titleLabel->setStyleSheet(QStringLiteral("color: #E3F2FD;"));
    rootLayout->addWidget(m_titleLabel);

    auto *controlFrame = new QFrame(this);
    controlFrame->setFrameShape(QFrame::StyledPanel);
    controlFrame->setStyleSheet(QStringLiteral("QFrame { background-color: rgba(28,33,48,0.8); border-radius: 8px; }"));
    auto *controlLayout = new QHBoxLayout(controlFrame);
    controlLayout->setContentsMargins(12, 8, 12, 8);
    controlLayout->setSpacing(8);

    m_stageCombo = new QComboBox(controlFrame);
    m_stageCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    controlLayout->addWidget(new QLabel(tr("View"), controlFrame));
    controlLayout->addWidget(m_stageCombo, 1);

    controlLayout->addStretch(1);

    m_zoomLabel = new QLabel(tr("Fit"), controlFrame);
    m_zoomLabel->setAlignment(Qt::AlignCenter);
    m_zoomLabel->setMinimumWidth(64);
    m_zoomLabel->setStyleSheet(QStringLiteral("color: #90CAF9;"));
    controlLayout->addWidget(m_zoomLabel);

    m_zoomOutButton = new QToolButton(controlFrame);
    m_zoomOutButton->setText(QStringLiteral("−"));
    m_zoomOutButton->setToolTip(tr("Zoom out (Ctrl + mouse wheel)"));
    controlLayout->addWidget(m_zoomOutButton);

    m_zoomInButton = new QToolButton(controlFrame);
    m_zoomInButton->setText(QStringLiteral("＋"));
    m_zoomInButton->setToolTip(tr("Zoom in (Ctrl + mouse wheel)"));
    controlLayout->addWidget(m_zoomInButton);

    m_fitButton = new QToolButton(controlFrame);
    m_fitButton->setText(tr("Fit"));
    m_fitButton->setCheckable(true);
    m_fitButton->setChecked(true);
    m_fitButton->setToolTip(tr("Scale the image to fit the viewport"));
    controlLayout->addWidget(m_fitButton);

    m_resetZoomButton = new QToolButton(controlFrame);
    m_resetZoomButton->setText(tr("100%"));
    m_resetZoomButton->setToolTip(tr("Restore 100% zoom"));
    controlLayout->addWidget(m_resetZoomButton);

    controlFrame->setLayout(controlLayout);
    rootLayout->addWidget(controlFrame);

    m_scrollArea = new QScrollArea(this);
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setFrameShape(QFrame::NoFrame);
    m_scrollArea->setBackgroundRole(QPalette::Base);

    m_imageLabel = new QLabel(m_scrollArea);
    m_imageLabel->setAlignment(Qt::AlignCenter);
    m_imageLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    m_imageLabel->setScaledContents(false);

    m_innerFrame = new QFrame(this);
    m_innerFrame->setFrameShape(QFrame::StyledPanel);
    m_innerFrame->setStyleSheet(QStringLiteral("QFrame { background-color: rgba(18,22,34,0.85); border-radius: 12px; }"));
    m_innerLayout = new QVBoxLayout(m_innerFrame);
    m_innerLayout->setContentsMargins(16, 16, 16, 16);
    m_innerLayout->addWidget(m_imageLabel);
    m_scrollArea->setWidget(m_innerFrame);
    rootLayout->addWidget(m_scrollArea, 1);

    m_infoLabel = new QLabel(this);
    m_infoLabel->setWordWrap(true);
    m_infoLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_infoLabel->setStyleSheet(QStringLiteral("color: #C5CAE9;"));
    rootLayout->addWidget(m_infoLabel);

    connect(m_stageCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &DetectionPreviewWidget::handleStageChanged);
    connect(m_zoomInButton, &QToolButton::clicked, this, &DetectionPreviewWidget::handleZoomIn);
    connect(m_zoomOutButton, &QToolButton::clicked, this, &DetectionPreviewWidget::handleZoomOut);
    connect(m_fitButton, &QToolButton::toggled, this, &DetectionPreviewWidget::handleFitToWindowToggled);
    connect(m_resetZoomButton, &QToolButton::clicked, this, &DetectionPreviewWidget::handleResetZoom);

    m_scrollArea->viewport()->installEventFilter(this);
    updateZoomUi();
}

void DetectionPreviewWidget::clear()
{
    m_views.clear();
    m_stageCombo->clear();
    m_titleLabel->setText(tr("Select an image on the left to inspect detection details"));
    m_infoLabel->clear();
    m_imageLabel->clear();
    m_currentIndex = -1;
    m_originalPixmap = QPixmap();
    m_scaleFactor = 1.0;
    m_fitToWindow = true;
    if (m_fitButton) {
        QSignalBlocker blocker(m_fitButton);
        m_fitButton->setChecked(true);
    }
    updateZoomUi();
}

void DetectionPreviewWidget::setDetection(const DetectionResult &result)
{
    m_views.clear();
    m_currentIndex = -1;

    for (const auto &view : result.debugImages) {
        if (view.filePath.empty()) {
            continue;
        }
        cv::Mat decoded = cv::imread(view.filePath, cv::IMREAD_UNCHANGED);
        if (decoded.empty()) {
            continue;
        }
        QImage converted = matToQImage(decoded);
        if (!converted.isNull()) {
            ViewItem item;
            item.title = QString::fromStdString(view.label);
            item.image = converted;
            m_views.push_back(std::move(item));
        }
    }

    if (m_views.empty()) {
        // create a fallback placeholder with the resolution text
        const cv::Size res = result.resolution;
        const int width = res.width > 0 ? res.width : 800;
        const int height = res.height > 0 ? res.height : 600;
        QImage placeholder(std::max(width / 4, 320), std::max(height / 4, 240), QImage::Format_RGB32);
        placeholder.fill(QColor(24, 28, 40));
        QPainter painter(&placeholder);
        painter.setPen(QColor(144, 164, 238));
        painter.setRenderHint(QPainter::Antialiasing);
        QFont font = painter.font();
        font.setPointSize(12);
        painter.setFont(font);
    const QString text = tr("No visualization available\n%1")
                 .arg(result.success ? tr("Detection succeeded") : tr("Detection failed: %1").arg(QString::fromStdString(result.message)));
        painter.drawText(placeholder.rect(), Qt::AlignCenter, text);
        painter.end();

    ViewItem fallback;
    fallback.title = tr("Placeholder");
        fallback.image = placeholder;
        m_views.push_back(fallback);
    }

    rebuildStageList();
    if (!m_views.empty()) {
        int defaultIndex = 0;
        const int count = m_stageCombo->count();
        for (int i = 0; i < count; ++i) {
            const QString text = m_stageCombo->itemText(i).toLower();
            const bool matchesEnglish = text.contains(QStringLiteral("numbered")) && text.contains(QStringLiteral("grid"));
            const bool matchesChinese = text.contains(QStringLiteral("编号")) && text.contains(QStringLiteral("网格"));
            if (matchesEnglish || matchesChinese) {
                defaultIndex = i;
                break;
            }
        }
        m_stageCombo->setCurrentIndex(defaultIndex);
    }
    resetZoom(true);

    m_titleLabel->setText(QStringLiteral("%1 — %2")
                              .arg(QString::fromStdString(result.name))
                              .arg(result.success ? tr("Detection succeeded") : tr("Detection failed")));
    setInfoText(result);
}

void DetectionPreviewWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updateImage();
}

bool DetectionPreviewWidget::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_scrollArea->viewport()) {
        if (event->type() == QEvent::Resize) {
            if (m_fitToWindow) {
                updateImage();
            }
        } else if (event->type() == QEvent::Wheel) {
            auto *wheelEvent = static_cast<QWheelEvent *>(event);
            if (wheelEvent->modifiers().testFlag(Qt::ControlModifier)) {
                const double oldScale = m_scaleFactor;
                if (wheelEvent->angleDelta().y() > 0) {
                    applyScale(oldScale * 1.2);
                } else {
                    applyScale(oldScale / 1.2);
                }

                if (!m_fitToWindow) {
                    if (auto *hBar = m_scrollArea->horizontalScrollBar()) {
                        const double ratio = m_scaleFactor / std::max(oldScale, 1e-6);
                        const double posX = wheelEvent->position().x();
                        hBar->setValue(static_cast<int>(ratio * (hBar->value() + posX) - posX));
                    }
                    if (auto *vBar = m_scrollArea->verticalScrollBar()) {
                        const double ratio = m_scaleFactor / std::max(oldScale, 1e-6);
                        const double posY = wheelEvent->position().y();
                        vBar->setValue(static_cast<int>(ratio * (vBar->value() + posY) - posY));
                    }
                }
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

void DetectionPreviewWidget::handleStageChanged(int index)
{
    if (index < 0 || index >= static_cast<int>(m_views.size())) {
        m_currentIndex = -1;
        m_imageLabel->clear();
        m_originalPixmap = QPixmap();
        updateZoomUi();
        return;
    }
    m_currentIndex = index;
    m_originalPixmap = QPixmap::fromImage(m_views[static_cast<size_t>(m_currentIndex)].image);
    updateImage();
    updateZoomUi();
}

void DetectionPreviewWidget::rebuildStageList()
{
    m_stageCombo->blockSignals(true);
    m_stageCombo->clear();
    for (const auto &view : m_views) {
        m_stageCombo->addItem(view.title);
    }
    m_stageCombo->blockSignals(false);
    if (m_stageCombo->count() == 0) {
    m_stageCombo->addItem(tr("No view"));
    }
}

void DetectionPreviewWidget::updateImage()
{
    if (m_currentIndex < 0 || m_currentIndex >= static_cast<int>(m_views.size())) {
        m_imageLabel->clear();
        return;
    }
    if (m_originalPixmap.isNull()) {
        const QImage &img = m_views[static_cast<size_t>(m_currentIndex)].image;
        if (img.isNull()) {
            m_imageLabel->clear();
            return;
        }
        m_originalPixmap = QPixmap::fromImage(img);
    }
    if (m_originalPixmap.isNull()) {
        m_imageLabel->clear();
        return;
    }

    if (m_fitToWindow) {
        if (!m_scrollArea->widgetResizable()) {
            m_scrollArea->setWidgetResizable(true);
        }
        m_imageLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
        m_imageLabel->setMinimumSize(QSize(0, 0));
        m_imageLabel->setMaximumSize(QSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX));
        m_innerFrame->setMinimumSize(QSize(0, 0));
        m_innerFrame->setMaximumSize(QSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX));
        const QSize available = m_scrollArea->viewport()->size();
        if (!available.isEmpty()) {
            QPixmap scaled = m_originalPixmap.scaled(available, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            m_imageLabel->setPixmap(scaled);
        } else {
            m_imageLabel->setPixmap(m_originalPixmap);
        }
    } else {
        if (m_scrollArea->widgetResizable()) {
            m_scrollArea->setWidgetResizable(false);
        }
        const QSizeF baseSize = m_originalPixmap.size();
        QSize target = (baseSize * m_scaleFactor).toSize();
        target.setWidth(std::max(1, target.width()));
        target.setHeight(std::max(1, target.height()));
        QPixmap scaled = m_originalPixmap.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        m_imageLabel->setPixmap(scaled);
        m_imageLabel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        m_imageLabel->setFixedSize(scaled.size());
    m_imageLabel->setMaximumSize(scaled.size());
        const auto margins = m_innerLayout->contentsMargins();
        const QSize frameSize = scaled.size() + QSize(margins.left() + margins.right(), margins.top() + margins.bottom());
        m_innerFrame->setMinimumSize(frameSize);
        m_innerFrame->setMaximumSize(frameSize);
        m_innerFrame->resize(frameSize);
    }
}

void DetectionPreviewWidget::applyScale(double factor)
{
    if (m_currentIndex < 0 || m_views.empty()) {
        return;
    }
    factor = std::clamp(factor, 0.1, 6.0);
    if (!m_fitToWindow && std::abs(factor - m_scaleFactor) < 1e-3) {
        return;
    }
    m_scaleFactor = factor;
    m_fitToWindow = false;
    if (m_fitButton) {
        QSignalBlocker blocker(m_fitButton);
        m_fitButton->setChecked(false);
    }
    updateImage();
    updateZoomUi();
}

void DetectionPreviewWidget::resetZoom(bool forceFit)
{
    m_scaleFactor = 1.0;
    m_fitToWindow = forceFit;
    if (m_fitButton) {
        QSignalBlocker blocker(m_fitButton);
        m_fitButton->setChecked(m_fitToWindow);
    }
    if (!m_views.empty()) {
        updateImage();
    }
    updateZoomUi();
}

void DetectionPreviewWidget::updateZoomUi()
{
    if (m_zoomLabel) {
        if (m_currentIndex < 0 || m_originalPixmap.isNull()) {
            m_zoomLabel->setText(tr("--"));
        } else if (m_fitToWindow) {
            m_zoomLabel->setText(tr("Fit"));
        } else {
            const int percent = static_cast<int>(std::round(m_scaleFactor * 100.0));
            m_zoomLabel->setText(QStringLiteral("%1%%").arg(percent));
        }
    }

    const bool enabled = (m_currentIndex >= 0) && !m_originalPixmap.isNull();
    if (m_zoomInButton) {
        m_zoomInButton->setEnabled(enabled);
    }
    if (m_zoomOutButton) {
        m_zoomOutButton->setEnabled(enabled);
    }
    if (m_fitButton) {
        m_fitButton->setEnabled(enabled);
    }
    if (m_resetZoomButton) {
        m_resetZoomButton->setEnabled(enabled);
    }
}

void DetectionPreviewWidget::handleZoomIn()
{
    applyScale(m_scaleFactor * 1.2);
}

void DetectionPreviewWidget::handleZoomOut()
{
    applyScale(m_scaleFactor / 1.2);
}

void DetectionPreviewWidget::handleFitToWindowToggled(bool checked)
{
    if (m_views.empty()) {
        return;
    }
    m_fitToWindow = checked;
    if (checked) {
        m_scaleFactor = 1.0;
    }
    updateImage();
    updateZoomUi();
}

void DetectionPreviewWidget::handleResetZoom()
{
    resetZoom(false);
}

QImage DetectionPreviewWidget::matToQImage(const cv::Mat &mat)
{
    if (mat.empty()) {
        return {};
    }
    cv::Mat base = normalizeTo8U(mat);
    if (base.channels() == 1) {
        return QImage(base.data, base.cols, base.rows, base.step, QImage::Format_Grayscale8).copy();
    }
    cv::Mat converted;
    switch (base.channels()) {
    case 3:
        cv::cvtColor(base, converted, cv::COLOR_BGR2RGB);
        return QImage(converted.data, converted.cols, converted.rows, converted.step, QImage::Format_RGB888).copy();
    case 4:
        cv::cvtColor(base, converted, cv::COLOR_BGRA2RGBA);
        return QImage(converted.data, converted.cols, converted.rows, converted.step, QImage::Format_RGBA8888).copy();
    default:
        cv::cvtColor(base, converted, cv::COLOR_BGR2RGB);
        return QImage(converted.data, converted.cols, converted.rows, converted.step, QImage::Format_RGB888).copy();
    }
}

void DetectionPreviewWidget::setInfoText(const DetectionResult &result)
{
    QStringList lines;
    lines << tr("Status: %1").arg(result.success ? tr("Success") : tr("Failure"));
    if (!result.message.empty()) {
    lines << tr("Notes: %1").arg(QString::fromStdString(result.message));
    }
    if (result.resolution.width > 0 && result.resolution.height > 0) {
    lines << tr("Resolution: %1 × %2").arg(result.resolution.width).arg(result.resolution.height);
    }
    if (result.elapsed.count() > 0) {
    lines << tr("Elapsed: %1 ms").arg(result.elapsed.count());
    }
    if (!result.imagePoints.empty()) {
    lines << tr("Small circles: %1").arg(result.imagePoints.size());
    }
    if (result.bigCircleCount > 0) {
    lines << tr("Large circles: %1").arg(result.bigCircleCount);
    }
    if (!result.residualsPx.empty()) {
    lines << tr("Mean reprojection: %1 px | Max: %2 px")
                     .arg(result.meanErrorPx(), 0, 'f', 3)
                     .arg(result.maxErrorPx(), 0, 'f', 3);
    }
    m_infoLabel->setText(lines.join(QStringLiteral("\n")));
}

} // namespace mycalib
