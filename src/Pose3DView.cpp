#include "Pose3DView.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include <QLinearGradient>
#include <QMatrix3x3>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPainterPath>
#include <QResizeEvent>
#include <QEvent>
#include <QWheelEvent>
#include <QtMath>

namespace mycalib {
namespace {

constexpr float kMinCameraDistance = 80.0f;
constexpr float kMaxCameraDistance = 5000.0f;
constexpr float kDefaultSceneRadius = 220.0f;

QColor heatColorForIndex(int index)
{
    static const QColor palette[] = {
        QColor(100, 181, 246),
        QColor(129, 199, 132),
        QColor(255, 202, 40),
        QColor(244, 143, 177),
        QColor(206, 147, 216),
        QColor(255, 171, 145),
    };
    return palette[index % (sizeof(palette) / sizeof(QColor))];
}

} // namespace

Pose3DView::Pose3DView(QWidget *parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAutoFillBackground(false);
    setMouseTracking(true);
    resetCameraView();
    setCursor(Qt::ArrowCursor);
}

Pose3DView::~Pose3DView() = default;

void Pose3DView::setDetections(const std::vector<DetectionResult> &detections)
{
    m_poseSamples.clear();
    m_poseSamples.reserve(detections.size());

    bool geometryUpdated = false;
    for (const auto &det : detections) {
        if (!geometryUpdated && det.success && !det.objectPoints.empty()) {
            updateBoardGeometryFromPoints(det.objectPoints);
            geometryUpdated = m_boardGeometryValid;
        }

        PoseSample sample;
        sample.name = det.name;
        sample.success = det.success;
        sample.removed = det.iterationRemoved > 0;
        sample.rotation = quaternionFromRotation(det.rotationMatrix);
        sample.translation = QVector3D(static_cast<float>(det.translationMm[0]),
                                       static_cast<float>(det.translationMm[1]),
                                       static_cast<float>(det.translationMm[2]));
        sample.center = sample.translation + sample.rotation.rotatedVector(m_boardCenterOffset);
        m_poseSamples.push_back(std::move(sample));
    }

    if (!geometryUpdated) {
        resetBoardGeometry();
    }

    updateSceneStatistics();
    resetCameraView();
    setCursor(m_poseSamples.empty() ? Qt::ArrowCursor : Qt::OpenHandCursor);
    update();
}

void Pose3DView::setActiveDetection(const DetectionResult *detection)
{
    m_activeName = detection ? detection->name : std::string();
    update();
}

void Pose3DView::clear()
{
    m_poseSamples.clear();
    m_activeName.clear();
    resetBoardGeometry();
    updateSceneStatistics();
    resetCameraView();
    setCursor(Qt::ArrowCursor);
    update();
}

void Pose3DView::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QRectF bounds = rect();
    if (bounds.width() <= 0.0 || bounds.height() <= 0.0) {
        return;
    }

    QLinearGradient background(bounds.topLeft(), bounds.bottomRight());
    background.setColorAt(0.0, QColor(24, 29, 45));
    background.setColorAt(1.0, QColor(8, 10, 18));
    painter.fillRect(bounds, background);

    const QRectF frameRect = bounds.adjusted(6.0, 6.0, -6.0, -6.0);
    QPainterPath framePath;
    framePath.addRoundedRect(frameRect, 26.0, 26.0);

    QLinearGradient cardGradient(frameRect.topLeft(), frameRect.bottomRight());
    cardGradient.setColorAt(0.0, QColor(36, 44, 66, 245));
    cardGradient.setColorAt(1.0, QColor(16, 20, 33, 250));
    painter.setPen(Qt::NoPen);
    painter.setBrush(cardGradient);
    painter.drawPath(framePath);

    painter.setPen(QPen(QColor(82, 96, 138, 150), 1.15));
    painter.drawPath(framePath);

    painter.save();
    painter.setClipPath(framePath);

    QRadialGradient halo(frameRect.center(), std::max(frameRect.width(), frameRect.height()) * 0.58);
    halo.setColorAt(0.0, QColor(255, 255, 255, 20));
    halo.setColorAt(1.0, QColor(0, 0, 0, 0));
    painter.fillRect(frameRect, halo);

    QLinearGradient topSheen(frameRect.topLeft(), frameRect.topRight());
    topSheen.setColorAt(0.0, QColor(120, 168, 255, 26));
    topSheen.setColorAt(0.5, QColor(180, 210, 255, 12));
    topSheen.setColorAt(1.0, QColor(120, 168, 255, 26));
    QRectF sheenRect = frameRect.adjusted(28.0, 22.0, -28.0, -frameRect.height() * 0.72);
    painter.fillRect(sheenRect, topSheen);

    const CameraBasis basis = cameraBasis();
    auto projectVisible = [&](const QVector3D &point, QPointF &out) -> bool {
        bool visible = false;
        out = projectPoint(point, basis, visible);
        return visible;
    };

    auto drawSegment = [&](const QVector3D &a, const QVector3D &b, const QColor &color, qreal width) {
        QPointF p1, p2;
        if (!projectVisible(a, p1) || !projectVisible(b, p2)) {
            return;
        }
        QColor glow = color;
        glow.setAlphaF(0.28);
        painter.setPen(QPen(glow, width * 2.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawLine(p1, p2);
        painter.setPen(QPen(color, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawLine(p1, p2);
    };

    QFont axisFont("Times New Roman", 12, QFont::Bold);
    auto drawAxis = [&](const QVector3D &direction, const QColor &color, const QString &label) {
        const QVector3D endPoint = m_sceneCenter + direction;
        QPointF start, end;
        if (!projectVisible(m_sceneCenter, start) || !projectVisible(endPoint, end)) {
            return;
        }
        QColor glow = color;
        glow.setAlphaF(0.22);
        painter.setPen(QPen(glow, 7.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawLine(start, end);
        painter.setPen(QPen(color, 2.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawLine(start, end);

        QVector3D dirNorm = direction.normalized();
        QVector3D side = QVector3D::crossProduct(dirNorm, basis.up).normalized();
        if (!std::isfinite(side.lengthSquared()) || side.lengthSquared() < 1e-4f) {
            side = QVector3D::crossProduct(dirNorm, basis.right).normalized();
        }
        QVector3D upAux = QVector3D::crossProduct(dirNorm, side).normalized();
        const float arrowSize = direction.length() * 0.08f;
        const QVector3D tip = endPoint;
        const QVector3D baseCenter = tip - dirNorm * arrowSize;
        QPointF tipP, leftP, rightP;
        if (projectVisible(tip, tipP) &&
            projectVisible(baseCenter + (side * arrowSize * 0.6f + upAux * arrowSize * 0.15f), leftP) &&
            projectVisible(baseCenter - (side * arrowSize * 0.6f - upAux * arrowSize * 0.15f), rightP)) {
            QPolygonF arrow;
            arrow << tipP << leftP << rightP;
            painter.setBrush(color);
            painter.setPen(Qt::NoPen);
            painter.drawPolygon(arrow);
        }

        painter.setPen(color.darker(140));
        painter.setFont(axisFont);
        painter.drawText(end + QPointF(8.0, -6.0), label);
    };

    // Ground grid beneath scene
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, false);
    const float gridExtent = std::max(m_sceneRadius * 2.4f, 220.0f);
    const float groundY = m_sceneCenter.y() - std::max(40.0f, m_sceneRadius * 0.35f);
    painter.setPen(QPen(QColor(88, 104, 148, 95), 1.0));
    for (int i = -8; i <= 8; ++i) {
        const float t = static_cast<float>(i) / 8.0f;
        QVector3D a = m_sceneCenter + QVector3D(-gridExtent, groundY, t * gridExtent);
        QVector3D b = m_sceneCenter + QVector3D(gridExtent, groundY, t * gridExtent);
        QVector3D c = m_sceneCenter + QVector3D(t * gridExtent, groundY, -gridExtent);
        QVector3D d = m_sceneCenter + QVector3D(t * gridExtent, groundY, gridExtent);
    drawSegment(a, b, QColor(88, 108, 162, 82), 1.0);
    drawSegment(c, d, QColor(88, 108, 162, 82), 1.0);
    }
    painter.restore();

    // Coordinate axes with arrowheads
    const float axisLength = std::max(m_sceneRadius * 1.1f, 120.0f);
    drawAxis(QVector3D(axisLength, 0.0f, 0.0f), QColor(239, 83, 80), tr("X_c"));
    drawAxis(QVector3D(0.0f, axisLength, 0.0f), QColor(102, 187, 106), tr("Y_c"));
    drawAxis(QVector3D(0.0f, 0.0f, axisLength), QColor(66, 165, 245), tr("Z_c"));

    // Camera frustum at origin (camera frame equals world frame)
    QPointF cameraOrigin2D;
    if (projectVisible(QVector3D(0.0f, 0.0f, 0.0f), cameraOrigin2D)) {
        const float depth = axisLength * 0.45f;
        const float halfSize = axisLength * 0.18f;
        std::array<QPointF, 4> frustumBase;
        std::array<QVector3D, 4> corners3D = {
            QVector3D(-halfSize, -halfSize, depth),
            QVector3D(halfSize, -halfSize, depth),
            QVector3D(halfSize, halfSize, depth),
            QVector3D(-halfSize, halfSize, depth)
        };
        bool allVisible = true;
        for (size_t i = 0; i < corners3D.size(); ++i) {
            QPointF projected;
            if (!projectVisible(corners3D[i], projected)) {
                allVisible = false;
                break;
            }
            frustumBase[i] = projected;
        }
        if (allVisible) {
            painter.setPen(QPen(QColor(70, 80, 120), 1.2));
            for (const auto &corner : frustumBase) {
                painter.drawLine(cameraOrigin2D, corner);
            }
            for (size_t i = 0; i < frustumBase.size(); ++i) {
                painter.drawLine(frustumBase[i], frustumBase[(i + 1) % frustumBase.size()]);
            }
            painter.setBrush(QColor(90, 100, 140, 160));
            painter.setPen(Qt::NoPen);
            QPolygonF aperture;
            for (const auto &corner : frustumBase) {
                aperture << corner;
            }
            painter.drawPolygon(aperture);

            painter.setPen(QColor(40, 50, 90));
            painter.setFont(QFont("Times New Roman", 12));
            painter.drawText(cameraOrigin2D + QPointF(10.0, -10.0), tr("Camera"));
        }
    }

    const float halfX = m_boardWidth * 0.5f;
    const float halfY = m_boardHeight * 0.5f;
    const float halfZ = m_boardThickness * 0.5f;
    const std::array<QVector3D, 8> localCorners = {
        QVector3D(-halfX, -halfY, -halfZ),
        QVector3D( halfX, -halfY, -halfZ),
        QVector3D(-halfX,  halfY, -halfZ),
        QVector3D( halfX,  halfY, -halfZ),
        QVector3D(-halfX, -halfY,  halfZ),
        QVector3D( halfX, -halfY,  halfZ),
        QVector3D(-halfX,  halfY,  halfZ),
        QVector3D( halfX,  halfY,  halfZ)
    };
    static const int edges[12][2] = {
        {0, 1}, {1, 3}, {3, 2}, {2, 0},
        {4, 5}, {5, 7}, {7, 6}, {6, 4},
        {0, 4}, {1, 5}, {2, 6}, {3, 7}
    };
    static const std::array<int, 4> kTopFace = {4, 5, 7, 6};
    static const std::array<int, 4> kBottomFace = {0, 1, 3, 2};

    int colorIndex = 0;
    for (const auto &sample : m_poseSamples) {
        if (!sample.success) {
            continue;
        }
        std::array<QVector3D, 8> worldCorners {};
        for (size_t i = 0; i < localCorners.size(); ++i) {
            worldCorners[i] = sample.center + sample.rotation.rotatedVector(localCorners[i]);
        }

        const QColor baseColor = sample.removed ? QColor(244, 143, 177) : heatColorForIndex(colorIndex++);
        const bool active = !m_activeName.empty() && sample.name == m_activeName;
        const QColor wireColor = active ? QColor(255, 213, 79) : baseColor;
        const qreal penWidth = active ? 3.0 : 1.6;
        auto drawFacePolygon = [&](const std::array<int, 4> &indices, qreal alphaFactor) {
            QPolygonF poly;
            poly.reserve(indices.size());
            for (int idx : indices) {
                QPointF projected;
                if (!projectVisible(worldCorners[static_cast<size_t>(idx)], projected)) {
                    return;
                }
                poly.append(projected);
            }
            QColor fill = wireColor;
            fill.setAlphaF(alphaFactor);
            painter.setPen(Qt::NoPen);
            painter.setBrush(fill);
            painter.drawPolygon(poly);
        };

        drawFacePolygon(kBottomFace, active ? 0.16 : 0.08);
        drawFacePolygon(kTopFace, active ? 0.25 : 0.12);

        for (const auto &edge : edges) {
            const QVector3D &a = worldCorners[static_cast<size_t>(edge[0])];
            const QVector3D &b = worldCorners[static_cast<size_t>(edge[1])];
            drawSegment(a, b, wireColor, penWidth);
        }

        QPointF centerPoint;
        if (projectVisible(sample.center, centerPoint)) {
            QRadialGradient glow(centerPoint, active ? 22.0 : 16.0);
            QColor glowColor = wireColor;
            glowColor.setAlpha(active ? 170 : 110);
            glow.setColorAt(0.0, glowColor);
            glowColor.setAlpha(0);
            glow.setColorAt(1.0, glowColor);
            painter.setPen(Qt::NoPen);
            painter.setBrush(glow);
            painter.drawEllipse(centerPoint, glow.radius(), glow.radius());

            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(wireColor, active ? 2.0 : 1.2));
            painter.drawEllipse(centerPoint, active ? 6.0 : 4.0, active ? 6.0 : 4.0);
        }
    }

    bool centerVisible = false;
    const QPointF sceneCenterPoint = projectPoint(m_sceneCenter, basis, centerVisible);
    if (centerVisible) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(200, 200, 200, 120));
        painter.drawEllipse(sceneCenterPoint, 2.5, 2.5);
    }

    painter.restore();

    painter.setPen(QColor(178, 192, 222, 160));
    QFont hintFont = painter.font();
    hintFont.setPointSizeF(hintFont.pointSizeF() - 0.2f);
    painter.setFont(hintFont);
    painter.drawText(frameRect.adjusted(28.0, frameRect.height() - 44.0, -28.0, -20.0),
                     Qt::AlignRight | Qt::AlignVCenter,
                     tr("Drag with left mouse to rotate · scroll to zoom · double-click to reset"));
}

void Pose3DView::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    update();
}

void Pose3DView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_dragging = true;
        m_lastMousePos = event->pos();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void Pose3DView::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragging) {
        const QPoint delta = event->pos() - m_lastMousePos;
        m_lastMousePos = event->pos();
        m_cameraYaw -= static_cast<float>(delta.x()) * 0.4f;
        m_cameraPitch -= static_cast<float>(delta.y()) * 0.3f;
        m_cameraPitch = std::clamp(m_cameraPitch, -80.0f, 80.0f);
        update();
        event->accept();
        return;
    }
    if (!m_poseSamples.empty() && cursor().shape() != Qt::OpenHandCursor) {
        setCursor(Qt::OpenHandCursor);
    }
    QWidget::mouseMoveEvent(event);
}

void Pose3DView::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_dragging) {
        m_dragging = false;
        setCursor(m_poseSamples.empty() ? Qt::ArrowCursor : Qt::OpenHandCursor);
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void Pose3DView::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        resetCameraView();
        setCursor(m_poseSamples.empty() ? Qt::ArrowCursor : Qt::OpenHandCursor);
        update();
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void Pose3DView::leaveEvent(QEvent *event)
{
    if (m_dragging) {
        m_dragging = false;
    }
    setCursor(Qt::ArrowCursor);
    QWidget::leaveEvent(event);
}

void Pose3DView::wheelEvent(QWheelEvent *event)
{
    if (event->angleDelta().y() == 0) {
        QWidget::wheelEvent(event);
        return;
    }
    const float factor = 1.0f - static_cast<float>(event->angleDelta().y()) / 960.0f;
    const float minDist = std::max(kMinCameraDistance, m_sceneRadius * 0.6f);
    const float maxDist = std::min(kMaxCameraDistance, m_sceneRadius * 8.0f);
    m_cameraDistance = std::clamp(m_cameraDistance * factor, minDist, maxDist);
    update();
    event->accept();
}

void Pose3DView::updateBoardGeometryFromPoints(const std::vector<cv::Point3f> &objectPoints)
{
    if (objectPoints.empty()) {
        return;
    }

    cv::Point3f minPt(std::numeric_limits<float>::max(),
                      std::numeric_limits<float>::max(),
                      std::numeric_limits<float>::max());
    cv::Point3f maxPt(-std::numeric_limits<float>::max(),
                      -std::numeric_limits<float>::max(),
                      -std::numeric_limits<float>::max());
    for (const auto &pt : objectPoints) {
        minPt.x = std::min(minPt.x, pt.x);
        minPt.y = std::min(minPt.y, pt.y);
        minPt.z = std::min(minPt.z, pt.z);
        maxPt.x = std::max(maxPt.x, pt.x);
        maxPt.y = std::max(maxPt.y, pt.y);
        maxPt.z = std::max(maxPt.z, pt.z);
    }

    m_boardWidth = std::max(10.0f, maxPt.x - minPt.x);
    m_boardHeight = std::max(10.0f, maxPt.y - minPt.y);
    const float depth = std::max(1.0f, maxPt.z - minPt.z);
    const float inferredThickness = std::max(depth, 0.025f * std::max(m_boardWidth, m_boardHeight));
    m_boardThickness = std::clamp(inferredThickness, 3.0f, 20.0f);
    m_boardCenterOffset = QVector3D((minPt.x + maxPt.x) * 0.5f,
                                    (minPt.y + maxPt.y) * 0.5f,
                                    (minPt.z + maxPt.z) * 0.5f);
    m_boardGeometryValid = true;
}

void Pose3DView::resetBoardGeometry()
{
    m_boardWidth = 150.0f;
    m_boardHeight = 125.0f;
    m_boardThickness = 6.0f;
    m_boardCenterOffset = QVector3D(m_boardWidth * 0.5f, m_boardHeight * 0.5f, 0.0f);
    m_boardGeometryValid = false;
}

void Pose3DView::updateSceneStatistics()
{
    if (m_poseSamples.empty()) {
        m_sceneCenter = QVector3D(0.0f, 0.0f, 0.0f);
        m_sceneRadius = kDefaultSceneRadius;
        m_cameraDistance = std::max(m_sceneRadius * 2.5f, 260.0f);
        return;
    }

    QVector3D sumCenters(0.0f, 0.0f, 0.0f);
    int count = 0;
    for (const auto &sample : m_poseSamples) {
        if (!sample.success) {
            continue;
        }
        sumCenters += sample.center;
        ++count;
    }

    if (count == 0) {
        m_sceneCenter = QVector3D(0.0f, 0.0f, 0.0f);
        m_sceneRadius = kDefaultSceneRadius;
        m_cameraDistance = std::max(m_sceneRadius * 2.5f, 260.0f);
        return;
    }

    m_sceneCenter = sumCenters / static_cast<float>(count);

    const float halfDiagonal = 0.5f * std::sqrt(m_boardWidth * m_boardWidth +
                                                m_boardHeight * m_boardHeight +
                                                m_boardThickness * m_boardThickness);
    float maxDistance = halfDiagonal;
    for (const auto &sample : m_poseSamples) {
        if (!sample.success) {
            continue;
        }
        const float distance = (sample.center - m_sceneCenter).length();
        maxDistance = std::max(maxDistance, distance + halfDiagonal);
    }

    m_sceneRadius = std::clamp(maxDistance, kDefaultSceneRadius * 0.2f, kMaxCameraDistance * 0.2f);
    const float preferred = std::max(m_sceneRadius * 2.4f, 220.0f);
    m_cameraDistance = std::clamp(preferred,
                                  std::max(kMinCameraDistance, m_sceneRadius * 0.6f),
                                  std::min(kMaxCameraDistance, m_sceneRadius * 8.0f));
}

void Pose3DView::resetCameraView()
{
    m_cameraYaw = 90.0f;
    m_cameraPitch = 65.0f;
    const float minDist = std::max(kMinCameraDistance, m_sceneRadius * 0.85f);
    const float maxDist = std::min(kMaxCameraDistance, m_sceneRadius * 6.0f);
    m_cameraDistance = std::clamp(m_sceneRadius * 2.6f, minDist, maxDist);
}

Pose3DView::CameraBasis Pose3DView::cameraBasis() const
{
    CameraBasis basis;
    const float yawRad = qDegreesToRadians(m_cameraYaw);
    const float pitchRad = qDegreesToRadians(m_cameraPitch);
    const float cosPitch = std::cos(pitchRad);

    const QVector3D toCamera(std::cos(yawRad) * cosPitch,
                              std::sin(pitchRad),
                              std::sin(yawRad) * cosPitch);

    basis.position = m_sceneCenter + toCamera.normalized() * m_cameraDistance;
    basis.forward = (m_sceneCenter - basis.position).normalized();

    QVector3D upHint(0.0f, 1.0f, 0.0f);
    if (std::abs(QVector3D::dotProduct(basis.forward, upHint)) > 0.94f) {
        upHint = QVector3D(0.0f, 0.0f, 1.0f);
    }
    basis.right = QVector3D::crossProduct(basis.forward, upHint).normalized();
    basis.up = QVector3D::crossProduct(basis.right, basis.forward).normalized();
    return basis;
}

QPointF Pose3DView::projectPoint(const QVector3D &worldPoint,
                                 const CameraBasis &basis,
                                 bool &visible) const
{
    const QVector3D relative = worldPoint - basis.position;
    const float cx = QVector3D::dotProduct(relative, basis.right);
    const float cy = QVector3D::dotProduct(relative, basis.up);
    const float cz = QVector3D::dotProduct(relative, basis.forward);

    const float epsilon = 1e-2f;
    if (cz <= epsilon) {
        visible = false;
        return {};
    }

    const float aspect = width() > 0 ? static_cast<float>(width()) / static_cast<float>(height()) : 1.0f;
    const float fov = 45.0f;
    const float tanHalfFov = std::tan(qDegreesToRadians(fov * 0.5f));

    const float ndcX = (cx / (cz * tanHalfFov)) / aspect;
    const float ndcY = (cy / (cz * tanHalfFov));

    const float halfW = static_cast<float>(width()) * 0.5f;
    const float halfH = static_cast<float>(height()) * 0.5f;

    visible = true;
    return QPointF(halfW + ndcX * halfW, halfH - ndcY * halfH);
}

QQuaternion Pose3DView::quaternionFromRotation(const cv::Matx33d &rotation)
{
    if (std::abs(cv::determinant(rotation)) < 1e-6) {
        return QQuaternion();
    }

    QMatrix3x3 matrix;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            matrix(r, c) = static_cast<float>(rotation(r, c));
        }
    }
    return QQuaternion::fromRotationMatrix(matrix).normalized();
}

} // namespace mycalib
