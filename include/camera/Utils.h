#pragma once
#include <QString>
#include <QImage>
#include <cstdint>


namespace Utils
{
QString bytesHumanReadable(double bytesPerSec);
QImage makeImageFromMono8(const uint8_t* data, int w, int h, int stride=0);
}
