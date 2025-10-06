#include "Logger.h"

#include <QDateTime>
#include <QDebug>
#include <utility>

namespace mycalib {

namespace {

QString levelToken(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg:
        return QStringLiteral("DEBUG");
    case QtWarningMsg:
        return QStringLiteral("WARNING");
    case QtCriticalMsg:
    case QtFatalMsg:
        return QStringLiteral("ERROR");
    case QtInfoMsg:
    default:
        return QStringLiteral("INFO");
    }
}

QString prefix(QtMsgType type)
{
    return QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz") +
           QStringLiteral(" [") + levelToken(type) + QStringLiteral("] ");
}

} // namespace

Logger::Sink Logger::s_sink;
std::mutex Logger::s_mutex;

void Logger::info(const QString &message)
{
    constexpr QtMsgType type = QtInfoMsg;
    const QString text = prefix(type) + message;
    qInfo().noquote() << text;
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_sink) {
        s_sink(type, text);
    }
}

void Logger::warning(const QString &message)
{
    constexpr QtMsgType type = QtWarningMsg;
    const QString text = prefix(type) + message;
    qWarning().noquote() << text;
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_sink) {
        s_sink(type, text);
    }
}

void Logger::error(const QString &message)
{
    constexpr QtMsgType type = QtCriticalMsg;
    const QString text = prefix(type) + message;
    qCritical().noquote() << text;
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_sink) {
        s_sink(type, text);
    }
}

void Logger::setSink(Sink sink)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    s_sink = std::move(sink);
}

} // namespace mycalib
