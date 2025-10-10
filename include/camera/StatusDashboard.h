#pragma once

#include <QFrame>
#include <QMap>
#include <QString>

class QLabel;

class StatusDashboard : public QFrame {
    Q_OBJECT
public:
    explicit StatusDashboard(QWidget* parent = nullptr);

    void setMetrics(const QMap<QString, QString>& metrics);

private:
    QLabel* label(const QString& key) const;
    void ensureLabels();

    QMap<QString, QLabel*> m_labels;
};
