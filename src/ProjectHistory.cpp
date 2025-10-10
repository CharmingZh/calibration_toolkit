#include "ProjectHistory.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <algorithm>

namespace mycalib {

namespace {

constexpr int kMaxHistoryEntries = 8;

QString historyFilePath()
{
    QString configRoot = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (configRoot.isEmpty()) {
        configRoot = QDir::homePath();
    }

    QString organization = QCoreApplication::organizationName();
    if (organization.isEmpty()) {
        organization = QStringLiteral("CalibLab");
    }
    QString application = QCoreApplication::applicationName();
    if (application.isEmpty()) {
        application = QStringLiteral("MyCalib");
    }

    QDir base(configRoot);
    const QString relative = organization + QLatin1Char('/') + application;
    base.mkpath(relative);
    return QDir::cleanPath(base.filePath(relative + QLatin1String("/recent_projects.json")));
}

QString normalizePath(const QString &path)
{
    if (path.isEmpty()) {
        return {};
    }
    return QDir::cleanPath(QDir::fromNativeSeparators(path));
}

QDateTime parseTimestamp(const QString &value)
{
    if (value.isEmpty()) {
        return {};
    }
    QDateTime dt = QDateTime::fromString(value, Qt::ISODateWithMs);
    if (!dt.isValid()) {
        dt = QDateTime::fromString(value, Qt::ISODate);
    }
    if (dt.isValid()) {
        dt = dt.toUTC();
    }
    return dt;
}

} // namespace

QVector<ProjectHistoryEntry> loadProjectHistory()
{
    QVector<ProjectHistoryEntry> entries;

    const QString filePath = historyFilePath();
    QFile file(filePath);
    if (!file.exists()) {
        return entries;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        return entries;
    }

    QJsonParseError error {};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isArray()) {
        return entries;
    }

    const QJsonArray array = doc.array();
    entries.reserve(array.size());
    for (const QJsonValue &value : array) {
        if (!value.isObject()) {
            continue;
        }
        const QJsonObject obj = value.toObject();
        ProjectHistoryEntry entry;
        entry.path = normalizePath(obj.value(QStringLiteral("path")).toString());
        entry.name = obj.value(QStringLiteral("name")).toString();
        entry.lastOpened = parseTimestamp(obj.value(QStringLiteral("last_opened")).toString());
        if (entry.path.isEmpty()) {
            continue;
        }
        entries.append(entry);
    }

    std::sort(entries.begin(), entries.end(), [](const ProjectHistoryEntry &a, const ProjectHistoryEntry &b) {
        return a.lastOpened > b.lastOpened;
    });

    if (entries.size() > kMaxHistoryEntries) {
        entries.resize(kMaxHistoryEntries);
    }

    return entries;
}

void recordProjectHistoryEntry(const QString &path, const QString &projectName)
{
    QString normalized = normalizePath(path);
    if (normalized.isEmpty()) {
        return;
    }

    QVector<ProjectHistoryEntry> entries = loadProjectHistory();
    entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const ProjectHistoryEntry &entry) {
                        return entry.path.compare(normalized, Qt::CaseInsensitive) == 0;
                    }),
                  entries.end());

    ProjectHistoryEntry current;
    current.path = normalized;
    current.name = projectName;
    current.lastOpened = QDateTime::currentDateTimeUtc();
    entries.prepend(current);

    if (entries.size() > kMaxHistoryEntries) {
        entries.resize(kMaxHistoryEntries);
    }

    QJsonArray array;
    for (const ProjectHistoryEntry &entry : entries) {
        QJsonObject obj;
        obj.insert(QStringLiteral("path"), entry.path);
        if (!entry.name.isEmpty()) {
            obj.insert(QStringLiteral("name"), entry.name);
        }
        if (entry.lastOpened.isValid()) {
            obj.insert(QStringLiteral("last_opened"), entry.lastOpened.toUTC().toString(Qt::ISODateWithMs));
        }
        array.append(obj);
    }

    const QString filePath = historyFilePath();
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return;
    }
    QJsonDocument doc(array);
    file.write(doc.toJson(QJsonDocument::Indented));
}

} // namespace mycalib
