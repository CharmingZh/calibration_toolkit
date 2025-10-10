#pragma once

#include <QDateTime>
#include <QString>
#include <QVector>

namespace mycalib {

struct ProjectHistoryEntry {
    QString path;
    QString name;
    QDateTime lastOpened;
};

QVector<ProjectHistoryEntry> loadProjectHistory();
void recordProjectHistoryEntry(const QString &path, const QString &projectName);

} // namespace mycalib
