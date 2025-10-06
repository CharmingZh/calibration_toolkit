#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

namespace mycalib {

struct BoardSpec {
    double smallDiameterMm {5.0};
    double centerSpacingMm {25.0};

    [[nodiscard]] std::string description() const;
    [[nodiscard]] std::size_t expectedCircleCount() const;
    [[nodiscard]] std::vector<cv::Point3f> buildObjectPoints(int count) const;
};

inline std::string BoardSpec::description() const
{
    return "7x6 asymmetric circles (center missing) -- d=" + std::to_string(smallDiameterMm) +
           "mm, spacing=" + std::to_string(centerSpacingMm) + "mm";
}

inline std::size_t BoardSpec::expectedCircleCount() const
{
    return 41U; // 7x6 grid with one missing centre location
}

inline std::vector<cv::Point3f> BoardSpec::buildObjectPoints(int count) const
{
    std::vector<cv::Point3f> coords;
    coords.reserve(count);

    for (int row = 6; row >= 0; --row) {
        for (int col = 5; col >= 0; --col) {
            if (row == 3 && col == 3) {
                continue; // central circle is missing on the physical board
            }
            const float x = static_cast<float>(col * centerSpacingMm);
            const float y = static_cast<float>(row * centerSpacingMm);
            coords.emplace_back(x, y, 0.0f);
            if (static_cast<int>(coords.size()) >= count) {
                return coords;
            }
        }
    }

    return coords;
}

} // namespace mycalib
