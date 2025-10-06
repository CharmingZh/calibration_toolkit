#pragma once

#include <vector>

#include <QImage>
#include <QPixmap>
#include <QWidget>

#include "DetectionResult.h"

class QLabel;
class QComboBox;
class QScrollArea;
class QToolButton;
class QFrame;
class QVBoxLayout;

namespace mycalib {

class DetectionPreviewWidget : public QWidget {
    Q_OBJECT

public:
    explicit DetectionPreviewWidget(QWidget *parent = nullptr);

    void clear();
    void setDetection(const DetectionResult &result);

protected:
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private Q_SLOTS:
    void handleStageChanged(int index);
    void handleZoomIn();
    void handleZoomOut();
    void handleFitToWindowToggled(bool checked);
    void handleResetZoom();

private:
    struct ViewItem {
        QString title;
        QImage image;
    };

    void rebuildStageList();
    void updateImage();
    static QImage matToQImage(const cv::Mat &mat);
    void setInfoText(const DetectionResult &result);
    void applyScale(double factor);
    void resetZoom(bool forceFit = false);
    void updateZoomUi();

    QLabel *m_titleLabel {nullptr};
    QLabel *m_infoLabel {nullptr};
    QComboBox *m_stageCombo {nullptr};
    QLabel *m_imageLabel {nullptr};
    QScrollArea *m_scrollArea {nullptr};
    QFrame *m_innerFrame {nullptr};
    QVBoxLayout *m_innerLayout {nullptr};
    QToolButton *m_zoomInButton {nullptr};
    QToolButton *m_zoomOutButton {nullptr};
    QToolButton *m_fitButton {nullptr};
    QToolButton *m_resetZoomButton {nullptr};
    QLabel *m_zoomLabel {nullptr};

    std::vector<ViewItem> m_views;
    int m_currentIndex {-1};
    QPixmap m_originalPixmap;
    double m_scaleFactor {1.0};
    bool m_fitToWindow {true};
};

} // namespace mycalib
