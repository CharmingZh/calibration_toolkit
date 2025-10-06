#pragma once

#include <string>
#include <vector>

#include <opencv2/core.hpp>

namespace mycalib {

class ImageLoader {
public:
    ImageLoader() = default;

    [[nodiscard]] std::vector<std::string> gatherImageFiles(const std::string &directory) const;

    [[nodiscard]] cv::Mat loadImage(const std::string &path) const;
};

} // namespace mycalib
