#include "ImageLoader.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <stdexcept>

#include <QImage>
#include <QImageReader>
#include <QString>

#include <opencv2/imgcodecs.hpp>

namespace fs = std::filesystem;

namespace mycalib {

namespace {
constexpr const char *kExtensions[] = {".png", ".jpg", ".jpeg", ".bmp", ".tif", ".tiff", ".dng", ".DNG"};

bool hasSupportedExtension(const fs::path &path)
{
    const std::string ext = path.extension().string();
    for (const char *candidate : kExtensions) {
        if (ext == candidate) {
            return true;
        }
    }
    return false;
}

bool isRawDng(const fs::path &path)
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".dng";
}

cv::Mat loadWithQtReader(const std::string &path)
{
    QImageReader reader(QString::fromStdString(path));
    reader.setAutoTransform(true);
    QImage qimage = reader.read();
    if (qimage.isNull()) {
        throw std::runtime_error("Failed to read image: " + path + ", error: " + reader.errorString().toStdString());
    }
    if (qimage.format() != QImage::Format_Grayscale8) {
        qimage = qimage.convertToFormat(QImage::Format_Grayscale8);
    }
    if (qimage.isNull()) {
        throw std::runtime_error("Failed to convert image to grayscale: " + path);
    }
    cv::Mat converted(qimage.height(), qimage.width(), CV_8UC1, const_cast<uchar *>(qimage.bits()), qimage.bytesPerLine());
    return converted.clone();
}

} // namespace

std::vector<std::string> ImageLoader::gatherImageFiles(const std::string &directory) const
{
    std::vector<std::string> files;
    fs::path dirPath(directory);
    if (!fs::exists(dirPath) || !fs::is_directory(dirPath)) {
        throw std::runtime_error("Directory does not exist: " + directory);
    }

    for (const auto &entry : fs::directory_iterator(dirPath)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        if (hasSupportedExtension(entry.path())) {
            files.emplace_back(entry.path().string());
        }
    }

    std::sort(files.begin(), files.end());
    return files;
}

cv::Mat ImageLoader::loadImage(const std::string &path) const
{
    fs::path filePath(path);
    if (isRawDng(filePath)) {
        return loadWithQtReader(path);
    }

    cv::Mat image = cv::imread(path, cv::IMREAD_GRAYSCALE);
    if (!image.empty()) {
        return image;
    }

    return loadWithQtReader(path);
}

} // namespace mycalib
