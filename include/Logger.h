#pragma once

#include <QtGlobal>
#include <QString>
#include <functional>
#include <mutex>

namespace mycalib {

class Logger {
public:
    static void info(const QString &message);
    static void warning(const QString &message);
    static void error(const QString &message);

    using Sink = std::function<void(QtMsgType, const QString &)>;
    static void setSink(Sink sink);

private:
    static Sink s_sink;
    static std::mutex s_mutex;
};

} // namespace mycalib
