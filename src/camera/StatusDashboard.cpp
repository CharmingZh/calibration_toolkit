#include "camera/StatusDashboard.h"

#include <QGridLayout>
#include <QLabel>
#include <QPalette>
#include <QStyle>

namespace {
QString labelText(const QString& title, const QString& value) {
    return QStringLiteral("<span style='color:#7f8baa;font-size:11px;'>%1</span><br/><span style='color:#e4ecf8;font-size:15px;font-weight:600;'>%2</span>")
        .arg(title, value.isEmpty() ? QStringLiteral("--") : value);
}
}

StatusDashboard::StatusDashboard(QWidget* parent)
    : QFrame(parent) {
    setObjectName(QStringLiteral("StatusDashboard"));
    setFrameShape(QFrame::NoFrame);
    setAutoFillBackground(false);

    auto* layout = new QGridLayout(this);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setHorizontalSpacing(18);
    layout->setVerticalSpacing(12);

    const struct Field {
        QString key;
        QString title;
        int row;
        int column;
    } fields[] = {
        {QStringLiteral("connection"), QStringLiteral("连接状态"), 0, 0},
        {QStringLiteral("camera"), QStringLiteral("相机信息"), 0, 1},
        {QStringLiteral("frameRate"), QStringLiteral("实时帧率"), 0, 2},
        {QStringLiteral("bandwidth"), QStringLiteral("数据吞吐"), 0, 3},
        {QStringLiteral("acqFrameRate"), QStringLiteral("采集帧率上限"), 1, 0},
        {QStringLiteral("acqFrameRateEnable"), QStringLiteral("采集帧率控制"), 1, 1},
        {QStringLiteral("exposure"), QStringLiteral("曝光时间"), 1, 2},
        {QStringLiteral("resolution"), QStringLiteral("当前分辨率"), 1, 3},
        {QStringLiteral("pixelFormat"), QStringLiteral("像素格式"), 2, 0},
        {QStringLiteral("stream"), QStringLiteral("链路带宽配置"), 2, 1}
    };

    for (const auto& field : fields) {
        auto* valueLabel = new QLabel(this);
    valueLabel->setText(labelText(field.title, QString()));
    valueLabel->setProperty("title", field.title);
        valueLabel->setTextFormat(Qt::RichText);
        valueLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        valueLabel->setObjectName(QStringLiteral("DashboardValue"));
        layout->addWidget(valueLabel, field.row, field.column);
        m_labels.insert(field.key, valueLabel);
    }

    setStyleSheet(QStringLiteral(
        "#StatusDashboard {"
        "  background: rgba(13, 18, 28, 0.92);"
        "  border-radius: 18px;"
        "  border: 1px solid rgba(88, 172, 255, 0.15);"
        "}"
        "#DashboardValue {"
        "  min-width: 150px;"
        "}"
    ));
}

void StatusDashboard::setMetrics(const QMap<QString, QString>& metrics) {
    ensureLabels();
    for (auto it = m_labels.cbegin(); it != m_labels.cend(); ++it) {
        const QString key = it.key();
        QLabel* labelPtr = it.value();
        const QString title = labelPtr->property("title").toString();
        const QString value = metrics.value(key, QStringLiteral("--"));
        labelPtr->setText(labelText(title, value));
    }
}

void StatusDashboard::ensureLabels() {
    if (!m_labels.isEmpty()) {
        return;
    }
    // This should not happen because constructor populates labels, but keep safe guard.
    auto* layout = qobject_cast<QGridLayout*>(this->layout());
    if (!layout) {
        return;
    }
    const int count = layout->count();
    for (int i = 0; i < count; ++i) {
        if (auto* labelPtr = qobject_cast<QLabel*>(layout->itemAt(i)->widget())) {
            m_labels.insert(QStringLiteral("field_%1").arg(i), labelPtr);
        }
    }
}
