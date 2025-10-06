#pragma once

#include <array>
#include <string>
#include <vector>

#include <QColor>
#include <QPoint>
#include <QQuaternion>
#include <QVector3D>
#include <QWidget>

#include "DetectionResult.h"

namespace mycalib {

class Pose3DView : public QWidget {
    Q_OBJECT

public:
    explicit Pose3DView(QWidget *parent = nullptr);
    ~Pose3DView() override;

    void setDetections(const std::vector<DetectionResult> &detections);
    void setActiveDetection(const DetectionResult *detection);
    void clear();

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    struct PoseSample {
        std::string name;
        bool success {false};
        bool removed {false};
        QQuaternion rotation;
        QVector3D translation;
        QVector3D center;
    };

    struct CameraBasis {
        QVector3D position;
        QVector3D forward;
        QVector3D right;
        QVector3D up;
    };

    void updateBoardGeometryFromPoints(const std::vector<cv::Point3f> &objectPoints);
    void resetBoardGeometry();
    void updateSceneStatistics();
    void resetCameraView();
    CameraBasis cameraBasis() const;
    QPointF projectPoint(const QVector3D &worldPoint, const CameraBasis &basis, bool &visible) const;
    static QQuaternion quaternionFromRotation(const cv::Matx33d &rotation);

    std::vector<PoseSample> m_poseSamples;
    std::string m_activeName;
    float m_boardWidth {150.0f};
    float m_boardHeight {125.0f};
    float m_boardThickness {6.0f};
    QVector3D m_boardCenterOffset {75.0f, 62.5f, 0.0f};
    bool m_boardGeometryValid {false};
    QVector3D m_sceneCenter {0.0f, 0.0f, 0.0f};
    float m_sceneRadius {220.0f};
    float m_cameraYaw {-34.0f};
    float m_cameraPitch {26.0f};
    float m_cameraDistance {600.0f};
    QPoint m_lastMousePos;
    bool m_dragging {false};
};

} // namespace mycalib
