#include "camera/Utils.h"

#include <cmath>
#include <cstring>

QString Utils::bytesHumanReadable(double bps) {
    static const char* units[] = {"B/s", "KB/s", "MB/s", "GB/s"};
    int idx = 0;
    while (bps > 1024.0 && idx < 3) {
        bps /= 1024.0;
        ++idx;
    }
    return QString::asprintf("%.1f %s", bps, units[idx]);
}

QImage Utils::makeImageFromMono8(const uint8_t* data, int w, int h, int stride) {
    if (!data || w <= 0 || h <= 0) {
        return {};
    }

    if (stride == 0) {
        stride = w;
    }

    QImage img(w, h, QImage::Format_Grayscale8);
    if (stride == w) {
        std::memcpy(img.bits(), data, static_cast<size_t>(w) * static_cast<size_t>(h));
    } else {
        for (int y = 0; y < h; ++y) {
            std::memcpy(img.scanLine(y), data + static_cast<size_t>(y) * stride, w);
        }
    }
    return img;
}
