#include "BoardDetector.h"
#include "Logger.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <numeric>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <utility>
#include <unordered_set>

#include <QDir>
#include <QString>
#include <QStringList>

#include <opencv2/core.hpp>
#include <opencv2/features2d.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace mycalib {

using Point2 = cv::Point2f;
using PointVec = std::vector<Point2>;

struct BlobCandidate {
    cv::KeyPoint keypoint;
    int index {0};
};

struct RefinedBlob {
    Point2 center {};
    double radius {0.0};
    double area {0.0};
    double score {0.0};
    int sourceIndex {-1};
};

struct PolarLine {
    double theta {0.0};
    double rho {0.0};
    cv::Vec4f segment {0, 0, 0, 0};
};

struct QuadCandidate {
    std::array<Point2, 4> corners;
    double score {0.0};
    double area {0.0};
};

namespace {

double bilinear_sample(const cv::Mat &gray, const Point2 &pt);

std::atomic<std::uint64_t> g_debugCounter {0};

std::string sanitize_filename(const std::string &input)
{
    std::string result;
    result.reserve(input.size());
    for (unsigned char ch : input) {
        if (std::isalnum(ch)) {
            result.push_back(static_cast<char>(ch));
        } else if (ch == '-' || ch == '_') {
            result.push_back(static_cast<char>(ch));
        } else {
            result.push_back('_');
        }
    }
    if (result.empty()) {
        result = "image";
    }
    return result;
}

struct AxisOrientation {
    Point2 origin {0.0F, 0.0F};
    cv::Vec2f xHat {1.0F, 0.0F};
    cv::Vec2f yHat {0.0F, 1.0F};
    bool valid {false};
};

struct KMeans1dResult {
    bool success {false};
    std::vector<int> labels;
    std::vector<float> centers;
};

AxisOrientation axes_from_big4(const std::vector<RefinedBlob> &big)
{
    AxisOrientation axes;
    if (big.size() < 4) {
        return axes;
    }

    std::array<cv::Point2f, 4> pts {};
    for (size_t i = 0; i < 4; ++i) {
        pts[i] = big[i].center;
    }

    std::array<double, 4> distSum {};
    for (int i = 0; i < 4; ++i) {
        double sum = 0.0;
        for (int j = 0; j < 4; ++j) {
            if (i == j) {
                continue;
            }
            sum += cv::norm(pts[i] - pts[j]);
        }
        distSum[static_cast<size_t>(i)] = sum;
    }

    int idxTl = static_cast<int>(std::distance(distSum.begin(), std::max_element(distSum.begin(), distSum.end())));
    std::array<int, 3> others {};
    int o = 0;
    for (int i = 0; i < 4; ++i) {
        if (i == idxTl) {
            continue;
        }
        others[static_cast<size_t>(o++)] = i;
    }

    auto angle_at = [&](int centre, int a, int b) {
        cv::Vec2f v1 = pts[a] - pts[centre];
        cv::Vec2f v2 = pts[b] - pts[centre];
    const float norm1 = std::max(static_cast<float>(cv::norm(v1)), 1e-6F);
    const float norm2 = std::max(static_cast<float>(cv::norm(v2)), 1e-6F);
        v1 /= norm1;
        v2 /= norm2;
        const float dot = std::clamp(v1[0] * v2[0] + v1[1] * v2[1], -1.0F, 1.0F);
        return std::abs(std::acos(dot) * 180.0 / CV_PI);
    };

    std::vector<std::pair<double, int>> angleCandidates;
    angleCandidates.reserve(3);
    for (int idx : others) {
        std::array<int, 2> rest {};
        int r = 0;
        for (int v : others) {
            if (v == idx) {
                continue;
            }
            rest[static_cast<size_t>(r++)] = v;
        }
        const double angle = angle_at(idx, rest[0], rest[1]);
        angleCandidates.emplace_back(std::abs(angle - 90.0), idx);
    }
    std::sort(angleCandidates.begin(), angleCandidates.end(), [](const auto &lhs, const auto &rhs) {
        if (std::abs(lhs.first - rhs.first) > 1e-6) {
            return lhs.first < rhs.first;
        }
        return lhs.second < rhs.second;
    });

    const int idxBr = angleCandidates.front().second;
    std::array<int, 2> remaining {};
    int rr = 0;
    for (int idx : others) {
        if (idx == idxBr) {
            continue;
        }
        remaining[static_cast<size_t>(rr++)] = idx;
    }

    const int idxTr = remaining[0];
    const int idxBl = remaining[1];

    const cv::Point2f br = pts[idxBr];
    const cv::Point2f tr = pts[idxTr];
    const cv::Point2f bl = pts[idxBl];

    cv::Vec2f xVec = tr - br;
    const float xNorm = std::max(static_cast<float>(cv::norm(xVec)), 1e-6F);
    cv::Vec2f xHat = xVec / xNorm;

    cv::Vec2f yVec = bl - br;
    const float projection = xHat[0] * yVec[0] + xHat[1] * yVec[1];
    cv::Vec2f yOrth = yVec - xHat * projection;
    const float yNorm = std::max(static_cast<float>(cv::norm(yOrth)), 1e-6F);
    cv::Vec2f yHat = yOrth / yNorm;

    const float z = xHat[0] * yHat[1] - xHat[1] * yHat[0];
    if (z < 0.0F) {
        std::swap(xHat, yHat);
    }

    axes.origin = br;
    axes.xHat = xHat;
    axes.yHat = yHat;
    axes.valid = true;
    return axes;
}

KMeans1dResult kmeans_1d(const std::vector<float> &values, int clusters)
{
    KMeans1dResult result;
    if (values.empty() || clusters <= 0) {
        return result;
    }

    cv::Mat samples(static_cast<int>(values.size()), 1, CV_32F);
    for (size_t i = 0; i < values.size(); ++i) {
        samples.at<float>(static_cast<int>(i), 0) = values[i];
    }

    cv::Mat labels;
    cv::Mat centers;
    const cv::TermCriteria criteria(cv::TermCriteria::EPS + cv::TermCriteria::MAX_ITER, 200, 1e-4);
    bool fallback = false;
    try {
        cv::kmeans(samples, clusters, labels, criteria, 10, cv::KMEANS_PP_CENTERS, centers);
        if (centers.rows != clusters) {
            fallback = true;
        }
    } catch (const cv::Exception &) {
        fallback = true;
    }

    auto run_lloyd = [&](std::vector<float> &centersVec) {
        const float minValue = *std::min_element(values.begin(), values.end());
        const float maxValue = *std::max_element(values.begin(), values.end());
        centersVec.resize(static_cast<size_t>(clusters));
        if (std::abs(maxValue - minValue) < 1e-6F) {
            std::fill(centersVec.begin(), centersVec.end(), minValue);
        } else {
            const float step = (maxValue - minValue) / static_cast<float>(clusters - 1);
            for (int k = 0; k < clusters; ++k) {
                centersVec[static_cast<size_t>(k)] = minValue + step * static_cast<float>(k);
            }
        }

        std::vector<int> assign(values.size(), 0);
        for (int iter = 0; iter < 32; ++iter) {
            double maxShift = 0.0;
            for (size_t i = 0; i < values.size(); ++i) {
                float bestDist = std::numeric_limits<float>::infinity();
                int bestIdx = 0;
                for (int k = 0; k < clusters; ++k) {
                    const float diff = values[i] - centersVec[static_cast<size_t>(k)];
                    const float dist = std::abs(diff);
                    if (dist < bestDist) {
                        bestDist = dist;
                        bestIdx = k;
                    }
                }
                assign[i] = bestIdx;
            }

            std::vector<float> newCenters = centersVec;
            for (int k = 0; k < clusters; ++k) {
                double sum = 0.0;
                int count = 0;
                for (size_t i = 0; i < values.size(); ++i) {
                    if (assign[i] == k) {
                        sum += values[i];
                        ++count;
                    }
                }
                if (count > 0) {
                    const float updated = static_cast<float>(sum / static_cast<double>(count));
                    maxShift = std::max(maxShift, static_cast<double>(std::abs(updated - centersVec[static_cast<size_t>(k)])));
                    newCenters[static_cast<size_t>(k)] = updated;
                }
            }
            centersVec = newCenters;
            if (maxShift < 1e-4) {
                break;
            }
        }

        result.labels = assign;
        result.centers = centersVec;
        result.success = true;
    };

    if (!fallback) {
        std::vector<float> centersVec(static_cast<size_t>(clusters));
        for (int r = 0; r < clusters; ++r) {
            centersVec[static_cast<size_t>(r)] = centers.at<float>(r, 0);
        }
        std::set<float> uniqueCenters;
        for (float c : centersVec) {
            uniqueCenters.insert(std::round(c * 1e4F) / 1e4F);
        }
        if (static_cast<int>(uniqueCenters.size()) < clusters) {
            fallback = true;
        } else {
            result.labels.resize(values.size());
            for (int i = 0; i < labels.rows; ++i) {
                result.labels[static_cast<size_t>(i)] = labels.at<int>(i, 0);
            }
            result.centers = std::move(centersVec);
            result.success = true;
        }
    }

    if (fallback) {
        std::vector<float> centersVec;
        run_lloyd(centersVec);
    }

    return result;
}

struct BlobSet {
    std::vector<BlobCandidate> raw;
    std::vector<std::optional<RefinedBlob>> refined;
};

struct NormalLine {
    double nx {0.0};
    double ny {0.0};
    double c {0.0};
    double theta {0.0};
    double rho {0.0};
    cv::Vec4f segment {0, 0, 0, 0};
};

struct WarpResult {
    cv::Mat image;
    cv::Mat homography;
    cv::Mat homographyInv;
};

struct NumberingResult {
    bool success {false};
    std::vector<Point2> orderedPoints;
    std::vector<cv::Vec2i> logicalIndices;
    std::string message;
    std::vector<int> sourceIndices;
};

QString matTypeToString(int type)
{
    const int depth = type & CV_MAT_DEPTH_MASK;
    const int channels = 1 + (type >> CV_CN_SHIFT);
    QString depthStr;
    switch (depth) {
    case CV_8U: depthStr = QStringLiteral("8U"); break;
    case CV_8S: depthStr = QStringLiteral("8S"); break;
    case CV_16U: depthStr = QStringLiteral("16U"); break;
    case CV_16S: depthStr = QStringLiteral("16S"); break;
    case CV_32S: depthStr = QStringLiteral("32S"); break;
    case CV_32F: depthStr = QStringLiteral("32F"); break;
    case CV_64F: depthStr = QStringLiteral("64F"); break;
    default: depthStr = QStringLiteral("Unknown"); break;
    }
    return QStringLiteral("CV_%1C%2").arg(depthStr).arg(channels);
}

DetectionConfig sanitize_config(const DetectionConfig &config)
{
    DetectionConfig cfg = config;

    const auto sanitizeSigma = [](double value, double fallback, const char *name) {
        if (!std::isfinite(value) || value <= 0.0) {
            Logger::warning(QStringLiteral("Detection config %1=%2 is invalid, falling back to %3")
                                .arg(QString::fromUtf8(name))
                                .arg(QString::number(value, 'g', 6))
                                .arg(QString::number(fallback, 'g', 6)));
            return fallback;
        }
        return value;
    };

    const auto ensurePositiveInt = [](int value, int fallback, const char *name) {
        if (value <= 0) {
            Logger::warning(QStringLiteral("Detection config %1=%2 is invalid, falling back to %3")
                                .arg(QString::fromUtf8(name))
                                .arg(value)
                                .arg(fallback));
            return fallback;
        }
        return value;
    };

    const auto ensurePositiveSize = [](cv::Size size, int fallback, const char *name) {
        if (size.width <= 0 || size.height <= 0) {
            Logger::warning(QStringLiteral("Detection config %1=%2x%3 is invalid, falling back to %4")
                                .arg(QString::fromUtf8(name))
                                .arg(size.width)
                                .arg(size.height)
                                .arg(fallback));
            return cv::Size(fallback, fallback);
        }
        return size;
    };

    cfg.houghGaussianSigma = sanitizeSigma(cfg.houghGaussianSigma, 1.0, "houghGaussianSigma");
    cfg.whiteGaussianSigma = sanitizeSigma(cfg.whiteGaussianSigma, 1.2, "whiteGaussianSigma");

    cfg.houghDilateKernel = ensurePositiveInt(cfg.houghDilateKernel, 3, "houghDilateKernel");
    cfg.houghDilateIterations = ensurePositiveInt(cfg.houghDilateIterations, 1, "houghDilateIterations");
    cfg.whiteMorphKernel = ensurePositiveInt(cfg.whiteMorphKernel, 11, "whiteMorphKernel");
    cfg.whiteMorphIterations = ensurePositiveInt(cfg.whiteMorphIterations, 1, "whiteMorphIterations");
    cfg.quadEdgeSamples = ensurePositiveInt(cfg.quadEdgeSamples, 48, "quadEdgeSamples");
    cfg.quadEdgeHalf = ensurePositiveInt(cfg.quadEdgeHalf, 6, "quadEdgeHalf");
    cfg.refineSegmentKsize = ensurePositiveInt(cfg.refineSegmentKsize, 3, "refineSegmentKsize");

    cfg.claheTileGrid = ensurePositiveSize(cfg.claheTileGrid, 8, "claheTileGrid");
    cfg.rectBlurKernel = ensurePositiveSize(cfg.rectBlurKernel, 3, "rectBlurKernel");
    cfg.refineOpenKernel = ensurePositiveSize(cfg.refineOpenKernel, 3, "refineOpenKernel");

    const auto makeOdd = [](int value) {
        if (value % 2 == 0) {
            ++value;
        }
        return value;
    };

    cfg.houghDilateKernel = makeOdd(cfg.houghDilateKernel);
    cfg.whiteMorphKernel = makeOdd(cfg.whiteMorphKernel);
    cfg.rectBlurKernel.width = makeOdd(cfg.rectBlurKernel.width);
    cfg.rectBlurKernel.height = makeOdd(cfg.rectBlurKernel.height);
    cfg.refineSegmentKsize = makeOdd(cfg.refineSegmentKsize);
    cfg.refineOpenKernel.width = makeOdd(cfg.refineOpenKernel.width);
    cfg.refineOpenKernel.height = makeOdd(cfg.refineOpenKernel.height);

    cfg.blobThresholdStep = std::max(cfg.blobThresholdStep, 1e-3);
    if (cfg.blobMaxThreshold <= cfg.blobMinThreshold) {
        Logger::warning(QStringLiteral("Detection config blobMaxThreshold=%1 ≤ blobMinThreshold=%2; adjusting to %3")
                            .arg(cfg.blobMaxThreshold)
                            .arg(cfg.blobMinThreshold)
                            .arg(cfg.blobMinThreshold + 1.0));
        cfg.blobMaxThreshold = cfg.blobMinThreshold + 1.0;
    }

    return cfg;
}

cv::Mat ensure_gray(const cv::Mat &input) {
    if (input.channels() == 1) {
        return input.clone();
    }
    cv::Mat gray;
    cv::cvtColor(input, gray, cv::COLOR_BGR2GRAY);
    return gray;
}

cv::Mat ensure_color_8u(const cv::Mat &input)
{
    if (input.empty()) {
        return {};
    }
    cv::Mat converted;
    if (input.depth() != CV_8U) {
        double minVal = 0.0;
        double maxVal = 0.0;
        cv::minMaxLoc(input, &minVal, &maxVal);
        if (!std::isfinite(minVal) || !std::isfinite(maxVal) || std::abs(maxVal - minVal) < 1e-6) {
            converted = cv::Mat::zeros(input.size(), CV_8U);
        } else {
            const double scale = 255.0 / (maxVal - minVal);
            const double shift = -minVal * scale;
            input.convertTo(converted, CV_8U, scale, shift);
        }
    } else {
        converted = input;
    }

    if (converted.channels() == 3) {
        return converted.clone();
    }
    if (converted.channels() == 4) {
        cv::Mat bgr;
        cv::cvtColor(converted, bgr, cv::COLOR_BGRA2BGR);
        return bgr;
    }
    if (converted.channels() == 1) {
        cv::Mat bgr;
        cv::cvtColor(converted, bgr, cv::COLOR_GRAY2BGR);
        return bgr;
    }

    cv::Mat reshaped;
    converted.convertTo(reshaped, CV_8UC3);
    return reshaped;
}

cv::Mat downscale_for_display(const cv::Mat &input, int maxDim = 1600)
{
    if (input.empty()) {
        return {};
    }
    const int largest = std::max(input.cols, input.rows);
    if (largest <= maxDim) {
        return input.clone();
    }
    const double scale = static_cast<double>(maxDim) / static_cast<double>(largest);
    cv::Mat resized;
    cv::resize(input, resized, cv::Size(), scale, scale, cv::INTER_AREA);
    return resized;
}

cv::Mat apply_clahe(const cv::Mat &input, const DetectionConfig &cfg) {
    auto clip = std::max(0.1, cfg.claheClipLimit);
    cv::Size grid(std::max(1, cfg.claheTileGrid.width), std::max(1, cfg.claheTileGrid.height));
    auto clahe = cv::createCLAHE(clip, grid);
    cv::Mat equalized;
    clahe->apply(input, equalized);
    return equalized;
}

cv::Mat preprocess_rect(const cv::Mat &rect, const DetectionConfig &cfg) {
    cv::Mat eq = apply_clahe(rect, cfg);
    int kx = std::max(1, cfg.rectBlurKernel.width);
    int ky = std::max(1, cfg.rectBlurKernel.height);
    if (kx % 2 == 0) {
        ++kx;
    }
    if (ky % 2 == 0) {
        ++ky;
    }
    cv::Mat blurred;
    cv::GaussianBlur(eq, blurred, cv::Size(kx, ky), 0.0);
    return blurred;
}

PointVec order_quad(const std::array<Point2, 4> &quad) {
    PointVec ordered(4);
    const auto sum = [](const Point2 &p) { return p.x + p.y; };
    const auto diff = [](const Point2 &p) { return p.x - p.y; };

    auto minSumIt = std::min_element(quad.begin(), quad.end(), [&](const Point2 &a, const Point2 &b) {
        return sum(a) < sum(b);
    });
    auto maxSumIt = std::max_element(quad.begin(), quad.end(), [&](const Point2 &a, const Point2 &b) {
        return sum(a) < sum(b);
    });
    auto minDiffIt = std::min_element(quad.begin(), quad.end(), [&](const Point2 &a, const Point2 &b) {
        return diff(a) < diff(b);
    });
    auto maxDiffIt = std::max_element(quad.begin(), quad.end(), [&](const Point2 &a, const Point2 &b) {
        return diff(a) < diff(b);
    });

    ordered[0] = *minSumIt;   // top-left
    ordered[2] = *maxSumIt;   // bottom-right
    ordered[1] = *maxDiffIt;  // top-right
    ordered[3] = *minDiffIt;  // bottom-left
    return ordered;
}

Point2 segment_direction(const cv::Vec4f &seg) {
    return {seg[2] - seg[0], seg[3] - seg[1]};
}

Point2 segment_center(const cv::Vec4f &seg) {
    return {(seg[0] + seg[2]) * 0.5F, (seg[1] + seg[3]) * 0.5F};
}

PolarLine to_polar(const cv::Vec4f &seg) {
    const auto dir = segment_direction(seg);
    const auto angle = std::atan2(dir.y, dir.x);
    const auto theta = std::fmod(angle + CV_PI, CV_PI);
    const auto center = segment_center(seg);
    const double rho = center.x * std::cos(theta) + center.y * std::sin(theta);
    PolarLine line;
    line.segment = seg;
    line.theta = theta;
    line.rho = rho;
    return line;
}

NormalLine segment_to_normal(const cv::Vec4f &segment) {
    const double x1 = segment[0];
    const double y1 = segment[1];
    const double x2 = segment[2];
    const double y2 = segment[3];
    const double vx = x2 - x1;
    const double vy = y2 - y1;
    const double L = std::hypot(vx, vy) + 1e-9;
    const double nx = -vy / L;
    const double ny = vx / L;
    const double c = -(nx * x1 + ny * y1);
    NormalLine line;
    line.nx = nx;
    line.ny = ny;
    line.c = c;
    line.theta = std::fmod(std::atan2(ny, nx) + CV_PI, CV_PI);
    line.rho = -c;
    line.segment = segment;
    return line;
}

double line_angle_deg(const NormalLine &line) {
    double deg = std::atan2(line.ny, line.nx) * 180.0 / CV_PI;
    deg = std::fmod(deg - 90.0 + 180.0, 180.0);
    if (deg < 0.0) {
        deg += 180.0;
    }
    return deg;
}

double deg_diff(double a, double b) {
    double d = std::fmod((a - b + 90.0), 180.0);
    if (d < 0.0) {
        d += 180.0;
    }
    return std::fabs(d - 90.0);
}

std::optional<Point2> intersect_lines(const NormalLine &a, const NormalLine &b) {
    const double det = a.nx * b.ny - b.nx * a.ny;
    if (std::abs(det) < 1e-8) {
        return std::nullopt;
    }
    const double rhs1 = -a.c;
    const double rhs2 = -b.c;
    const double x = (rhs1 * b.ny - rhs2 * a.ny) / det;
    const double y = (a.nx * rhs2 - b.nx * rhs1) / det;
    if (!std::isfinite(x) || !std::isfinite(y)) {
        return std::nullopt;
    }
    return Point2{static_cast<float>(x), static_cast<float>(y)};
}

double median_intensity(const cv::Mat &gray) {
    CV_Assert(gray.type() == CV_8UC1);
    const cv::Mat contiguous = gray.isContinuous() ? gray : gray.clone();
    const size_t total = contiguous.total();
    std::vector<uint8_t> buffer(total);
    std::memcpy(buffer.data(), contiguous.data, total);
    std::nth_element(buffer.begin(), buffer.begin() + buffer.size() / 2, buffer.end());
    return static_cast<double>(buffer[buffer.size() / 2]);
}

double edge_contrast(const cv::Mat &gray, const std::array<Point2, 4> &quad, const DetectionConfig &cfg) {
    PointVec ordered = order_quad(quad);
    const int edgeSamples = std::max(1, cfg.quadEdgeSamples);
    const double halfWidth = static_cast<double>(std::max(1, cfg.quadEdgeHalf));

    auto evaluate_edge = [&](const Point2 &a, const Point2 &b) -> std::optional<double> {
        Point2 vec = b - a;
        const double length = std::hypot(vec.x, vec.y);
        if (length < 1e-6) {
            return std::nullopt;
        }
        Point2 unit = vec * static_cast<float>(1.0 / length);
        Point2 normal {-unit.y, unit.x};

        double accum = 0.0;
        int valid = 0;
        for (int i = 0; i < edgeSamples; ++i) {
            const float t = (static_cast<float>(i) + 0.5F) / static_cast<float>(edgeSamples);
            Point2 point = a + unit * static_cast<float>(t * length);
            Point2 inner = point - normal * static_cast<float>(halfWidth);
            Point2 outer = point + normal * static_cast<float>(halfWidth);
            if (inner.x < 1.0F || inner.y < 1.0F || inner.x >= gray.cols - 2 || inner.y >= gray.rows - 2) {
                continue;
            }
            if (outer.x < 1.0F || outer.y < 1.0F || outer.x >= gray.cols - 2 || outer.y >= gray.rows - 2) {
                continue;
            }
            const double diff = std::abs(bilinear_sample(gray, inner) - bilinear_sample(gray, outer));
            accum += diff;
            ++valid;
        }
        if (valid == 0) {
            return std::nullopt;
        }
        return accum / static_cast<double>(valid);
    };

    std::vector<double> diffs;
    diffs.reserve(4);
    for (int i = 0; i < 4; ++i) {
        if (auto diff = evaluate_edge(ordered[i], ordered[(i + 1) % 4])) {
            diffs.push_back(*diff);
        }
    }

    if (diffs.empty()) {
        return 0.0;
    }

    double mean = std::accumulate(diffs.begin(), diffs.end(), 0.0) / static_cast<double>(diffs.size());
    return mean;
}

double quad_score(const cv::Mat &gray, const std::array<Point2, 4> &quad, const DetectionConfig &cfg) {
    const double contrast = edge_contrast(gray, quad, cfg);
    const auto ordered = order_quad(quad);
    double area = std::abs(cv::contourArea(ordered));
    const double totalArea = static_cast<double>(gray.rows * gray.cols);
    const double marginX = cfg.quadMargin * gray.cols;
    const double marginY = cfg.quadMargin * gray.rows;
    const double relaxedMarginX = marginX * 3.0 + 12.0;
    const double relaxedMarginY = marginY * 3.0 + 12.0;

    double penalty = 0.0;

    for (const auto &p : ordered) {
        double overflowX = 0.0;
        if (p.x < marginX) {
            overflowX = marginX - p.x;
        } else if (p.x > gray.cols - marginX) {
            overflowX = p.x - (gray.cols - marginX);
        }

        double overflowY = 0.0;
        if (p.y < marginY) {
            overflowY = marginY - p.y;
        } else if (p.y > gray.rows - marginY) {
            overflowY = p.y - (gray.rows - marginY);
        }

        if (overflowX > 0.0 || overflowY > 0.0) {
            if (overflowX > relaxedMarginX || overflowY > relaxedMarginY) {
                Logger::warning(QStringLiteral("quad_score: vertex outside margin (%1,%2) | margin=(%3,%4) | size=%5x%6")
                                    .arg(p.x, 0, 'f', 2)
                                    .arg(p.y, 0, 'f', 2)
                                    .arg(marginX, 0, 'f', 2)
                                    .arg(marginY, 0, 'f', 2)
                                    .arg(gray.cols)
                                    .arg(gray.rows));
                return -1e9;
            }
            const double normX = overflowX / std::max(relaxedMarginX, 1.0);
            const double normY = overflowY / std::max(relaxedMarginY, 1.0);
            penalty += (normX + normY) * 500.0;
        }
    }

    const double minArea = cfg.quadAreaMinRatio * totalArea;
    const double maxArea = cfg.quadAreaMaxRatio * totalArea;
    const double relaxedMinArea = minArea * 0.15;
    const double relaxedMaxArea = maxArea * 1.6;
    if (area < minArea) {
        if (area < relaxedMinArea) {
            Logger::warning(QStringLiteral("quad_score: area ratio=%1 below minimum=%2 (hard fail)")
                                .arg(area / totalArea, 0, 'f', 4)
                                .arg(cfg.quadAreaMinRatio, 0, 'f', 2));
            return -1e9;
        }
        const double span = std::max(minArea - relaxedMinArea, 1.0);
        penalty += ((minArea - area) / span) * 1200.0;
    } else if (area > maxArea) {
        if (area > relaxedMaxArea) {
            Logger::warning(QStringLiteral("quad_score: area ratio=%1 above maximum=%2 (hard fail)")
                                .arg(area / totalArea, 0, 'f', 4)
                                .arg(cfg.quadAreaMaxRatio, 0, 'f', 2));
            return -1e9;
        }
        const double span = std::max(relaxedMaxArea - maxArea, 1.0);
        penalty += ((area - maxArea) / span) * 1000.0;
    }

    const double widthTop = cv::norm(ordered[1] - ordered[0]);
    const double widthBottom = cv::norm(ordered[2] - ordered[3]);
    const double heightLeft = cv::norm(ordered[3] - ordered[0]);
    const double heightRight = cv::norm(ordered[2] - ordered[1]);

    const double meanWidth = 0.5 * (widthTop + widthBottom);
    const double meanHeight = 0.5 * (heightLeft + heightRight);
    const double ratio = meanWidth > meanHeight ? (meanWidth / std::max(1.0, meanHeight))
                                                : (meanHeight / std::max(1.0, meanWidth));
    const double relaxedAspectMin = cfg.quadAspectMin * 0.7;
    const double relaxedAspectMax = cfg.quadAspectMax * 1.5;
    if (ratio < cfg.quadAspectMin) {
        if (ratio < relaxedAspectMin) {
            Logger::warning(QStringLiteral("quad_score: aspect ratio=%1 below [%2,%3] (hard fail)")
                                .arg(ratio, 0, 'f', 3)
                                .arg(cfg.quadAspectMin, 0, 'f', 2)
                                .arg(cfg.quadAspectMax, 0, 'f', 2));
            return -1e9;
        }
        const double span = std::max(cfg.quadAspectMin - relaxedAspectMin, 1e-3);
        penalty += ((cfg.quadAspectMin - ratio) / span) * 600.0;
    } else if (ratio > cfg.quadAspectMax) {
        if (ratio > relaxedAspectMax) {
            Logger::warning(QStringLiteral("quad_score: aspect ratio=%1 above [%2,%3] (hard fail)")
                                .arg(ratio, 0, 'f', 3)
                                .arg(cfg.quadAspectMin, 0, 'f', 2)
                                .arg(cfg.quadAspectMax, 0, 'f', 2));
            return -1e9;
        }
        const double span = std::max(relaxedAspectMax - cfg.quadAspectMax, 1e-3);
        penalty += ((ratio - cfg.quadAspectMax) / span) * 600.0;
    }

    if (contrast < cfg.quadEdgeMinContrast) {
        const double relaxedContrast = cfg.quadEdgeMinContrast * 0.45;
        if (contrast < relaxedContrast) {
            Logger::warning(QStringLiteral("quad_score: edge contrast=%1 below threshold=%2 (hard fail)")
                                .arg(contrast, 0, 'f', 3)
                                .arg(cfg.quadEdgeMinContrast, 0, 'f', 2));
            return -1e9;
        }
        const double span = std::max(cfg.quadEdgeMinContrast - relaxedContrast, 1e-3);
        penalty += ((cfg.quadEdgeMinContrast - contrast) / span) * 800.0;
    }

    double score = contrast * 2000.0 + cfg.quadAreaBonus * std::sqrt(std::max(0.0, area));
    score -= penalty;
    if (score < -1e8) {
        score = -1e8;
    }
    return score;
}

std::optional<std::array<Point2, 4>> expand_quad(const std::array<Point2, 4> &quad, double scale, double offset) {
    Point2 center {0, 0};
    for (const auto &p : quad) {
        center += p;
    }
    center *= 0.25F;
    std::array<Point2, 4> expanded;
    for (int i = 0; i < 4; ++i) {
        Point2 dir = quad[i] - center;
        double len = std::hypot(dir.x, dir.y);
        if (len < 1e-6) {
            return std::nullopt;
        }
        dir /= static_cast<float>(len);
        double newLen = len * scale + offset;
        expanded[i] = center + dir * static_cast<float>(newLen);
    }
    return expanded;
}

WarpResult warp_quad(const cv::Mat &image, const std::array<Point2, 4> &quad, const DetectionConfig &cfg) {
    auto ordered = order_quad(quad);
    const double widthA = cv::norm(ordered[1] - ordered[0]);
    const double widthB = cv::norm(ordered[2] - ordered[3]);
    const double heightA = cv::norm(ordered[3] - ordered[0]);
    const double heightB = cv::norm(ordered[2] - ordered[1]);

    const double baseWidth = std::max(widthA, widthB);
    const double baseHeight = std::max(heightA, heightB);

    int dstW = static_cast<int>(std::round(std::max(baseWidth, static_cast<double>(cfg.warpMinDim))));
    int dstH = static_cast<int>(std::round(std::max(baseHeight, static_cast<double>(cfg.warpMinDim))));

    std::array<Point2, 4> dst {
        Point2{0, 0}, Point2{static_cast<float>(dstW - 1), 0},
        Point2{static_cast<float>(dstW - 1), static_cast<float>(dstH - 1)},
        Point2{0, static_cast<float>(dstH - 1)}
    };

    cv::Mat H = cv::getPerspectiveTransform(ordered, dst);

    cv::Mat warped;
    cv::warpPerspective(image, warped, H, cv::Size(dstW, dstH));

    const int shortDim = std::min(dstW, dstH);
    if (shortDim < cfg.warpMinShort && shortDim > 0) {
        const double scale = static_cast<double>(cfg.warpMinShort) / static_cast<double>(shortDim);
        cv::resize(warped, warped, cv::Size(), scale, scale, cv::INTER_CUBIC);
        cv::Mat scaleMat = (cv::Mat_<double>(3, 3) << scale, 0, 0, 0, scale, 0, 0, 0, 1);
        H = scaleMat * H;
    }

    WarpResult result;
    result.image = warped;
    result.homography = H;
    cv::Mat Hinv;
    if (cv::invert(H, Hinv, cv::DECOMP_LU) == 0) {
        result.homography.release();
        result.homographyInv.release();
        result.image.release();
        return result;
    }
    result.homographyInv = Hinv;
    return result;
}

std::vector<cv::Vec4f> detect_segments(const cv::Mat &edges, const DetectionConfig &cfg) {
    const double dim = static_cast<double>(std::min(edges.rows, edges.cols));
    const double votes = std::max(cfg.houghVotesRatio * dim, 10.0);
    const double minLineLength = cfg.houghMinLineRatio * dim;
    const double maxGap = cfg.houghMaxGapRatio * dim;

    std::vector<cv::Vec4i> lines;
    cv::HoughLinesP(edges, lines, 1.0, CV_PI / 180.0,
                    static_cast<int>(std::round(votes)),
                    static_cast<double>(minLineLength),
                    static_cast<double>(maxGap));

    std::vector<cv::Vec4f> result;
    result.reserve(lines.size());
    for (const auto &l : lines) {
        result.emplace_back(static_cast<float>(l[0]), static_cast<float>(l[1]),
                             static_cast<float>(l[2]), static_cast<float>(l[3]));
    }
    return result;
}

bool inside_bounds(const Point2 &p, int rows, int cols, double marginPixels) {
    return p.x >= marginPixels && p.x < cols - marginPixels && p.y >= marginPixels && p.y < rows - marginPixels;
}

double bilinear_sample(const cv::Mat &gray, const Point2 &pt) {
    const int cols = gray.cols;
    const int rows = gray.rows;
    if (cols <= 1 || rows <= 1) {
        return 0.0;
    }

    const float x = std::clamp(pt.x, 0.0f, static_cast<float>(cols - 1));
    const float y = std::clamp(pt.y, 0.0f, static_cast<float>(rows - 1));
    const int x0 = std::clamp(static_cast<int>(std::floor(x)), 0, cols - 2);
    const int y0 = std::clamp(static_cast<int>(std::floor(y)), 0, rows - 2);
    const int x1 = x0 + 1;
    const int y1 = y0 + 1;

    const float dx = x - static_cast<float>(x0);
    const float dy = y - static_cast<float>(y0);

    const float w00 = (1.0f - dx) * (1.0f - dy);
    const float w10 = dx * (1.0f - dy);
    const float w01 = (1.0f - dx) * dy;
    const float w11 = dx * dy;

    const auto v00 = gray.at<uint8_t>(y0, x0);
    const auto v10 = gray.at<uint8_t>(y0, x1);
    const auto v01 = gray.at<uint8_t>(y1, x0);
    const auto v11 = gray.at<uint8_t>(y1, x1);

    return w00 * v00 + w10 * v10 + w01 * v01 + w11 * v11;
}

std::vector<QuadCandidate> detect_quads_from_segments(const cv::Mat &gray, const std::vector<cv::Vec4f> &segments, const DetectionConfig &cfg) {
    if (segments.size() < 4) {
        return {};
    }
    const double maxDim = static_cast<double>(std::max(gray.rows, gray.cols));
    std::vector<NormalLine> normals;
    normals.reserve(segments.size());
    for (const auto &seg : segments) {
        normals.emplace_back(segment_to_normal(seg));
    }

    if (normals.size() < 4) {
        return {};
    }

    cv::Mat samples(static_cast<int>(normals.size()), 2, CV_32F);
    for (size_t i = 0; i < normals.size(); ++i) {
        const double angle = normals[i].theta * 2.0;
        samples.at<float>(static_cast<int>(i), 0) = static_cast<float>(std::cos(angle));
        samples.at<float>(static_cast<int>(i), 1) = static_cast<float>(std::sin(angle));
    }

    cv::Mat labels;
    cv::Mat centers;
    const cv::TermCriteria criteria(cv::TermCriteria::EPS + cv::TermCriteria::MAX_ITER,
                                    cfg.houghKmeansMaxIter, cfg.houghKmeansEps);
    cv::kmeans(samples, 2, labels, criteria, cfg.houghKmeansAttempts, cv::KMEANS_PP_CENTERS, centers);

    std::array<std::vector<NormalLine>, 2> groups;
    for (int i = 0; i < labels.rows; ++i) {
        int label = labels.at<int>(i, 0);
        if (label < 0 || label > 1) {
            label = 0;
        }
        groups[static_cast<size_t>(label)].push_back(normals[static_cast<size_t>(i)]);
    }

    const auto rhoThreshold = cfg.houghRhoNmsRatio * maxDim;
    const auto nms_lines = [&](std::vector<NormalLine> lines) {
        std::sort(lines.begin(), lines.end(), [](const NormalLine &lhs, const NormalLine &rhs) {
            return lhs.rho < rhs.rho;
        });
        std::vector<NormalLine> kept;
        for (const auto &line : lines) {
            bool ok = true;
            for (const auto &prev : kept) {
                if (std::abs(line.rho - prev.rho) <= rhoThreshold) {
                    ok = false;
                    break;
                }
            }
            if (ok) {
                kept.push_back(line);
            }
        }
        return kept;
    };

    for (auto &group : groups) {
        group = nms_lines(group);
    }

    if (groups[0].size() < 2 || groups[1].size() < 2) {
        return {};
    }

    std::vector<QuadCandidate> candidates;
    const auto try_pairs = [&](const std::vector<NormalLine> &g0, const std::vector<NormalLine> &g1) {
        for (size_t i = 0; i < g0.size(); ++i) {
            for (size_t j = i + 1; j < g0.size(); ++j) {
                if (deg_diff(line_angle_deg(g0[i]), line_angle_deg(g0[j])) > cfg.houghOrientationTol) {
                    continue;
                }
                for (size_t k = 0; k < g1.size(); ++k) {
                    for (size_t l = k + 1; l < g1.size(); ++l) {
                        if (deg_diff(line_angle_deg(g1[k]), line_angle_deg(g1[l])) > cfg.houghOrientationTol) {
                            continue;
                        }
                        const double ortho = std::abs(deg_diff(line_angle_deg(g0[i]), line_angle_deg(g1[k])) - 90.0);
                        if (ortho > cfg.houghOrthogonalityTol) {
                            continue;
                        }
                        std::array<std::optional<Point2>, 4> ptsOpt {
                            intersect_lines(g0[i], g1[k]),
                            intersect_lines(g0[j], g1[k]),
                            intersect_lines(g0[j], g1[l]),
                            intersect_lines(g0[i], g1[l])
                        };
                        bool ok = true;
                        std::array<Point2, 4> pts {};
                        for (int idx = 0; idx < 4; ++idx) {
                            if (!ptsOpt[idx]) {
                                ok = false;
                                break;
                            }
                            pts[static_cast<size_t>(idx)] = *ptsOpt[idx];
                        }
                        if (!ok) {
                            continue;
                        }
                        auto ordered = order_quad(pts);
                        double area = std::abs(cv::contourArea(ordered));
                        if (area < 50.0) {
                            continue;
                        }
                        double score = quad_score(gray, pts, cfg);
                        if (score <= -1e8) {
                            continue;
                        }
                        candidates.push_back({pts, score, area});
                    }
                }
            }
        }
    };

    try_pairs(groups[0], groups[1]);
    try_pairs(groups[1], groups[0]);

    std::sort(candidates.begin(), candidates.end(), [](const auto &lhs, const auto &rhs) {
        return lhs.score > rhs.score;
    });
    return candidates;
}

bool quad_within_image(const std::array<Point2, 4> &quad, int rows, int cols, double marginRatio) {
    double margin = marginRatio * std::max(rows, cols);
    for (const auto &p : quad) {
        if (!inside_bounds(p, rows, cols, margin)) {
            return false;
        }
    }
    return true;
}

std::optional<std::array<Point2, 4>> detect_by_hough_search(const cv::Mat &gray, const DetectionConfig &cfg) {
    const char *stage = "gaussian_blur";
    try {
        const double sigma = cfg.houghGaussianSigma;
        cv::Mat blurred;
        if (std::isfinite(sigma) && sigma > 0.0) {
            cv::GaussianBlur(gray, blurred, cv::Size(), sigma);
        } else {
            blurred = gray.clone();
        }

        stage = "median_intensity";
        const double med = median_intensity(blurred);
        double lowThresh = std::max(cfg.houghCannyLowRatio * med, static_cast<double>(cfg.houghCannyLowMin));
        double highThresh = std::max(lowThresh * cfg.houghCannyHighRatio, lowThresh + 1.0);
        highThresh = std::clamp(highThresh, 0.0, 255.0);

    stage = "canny";
    cv::Mat edges;
    cv::Canny(blurred, edges, lowThresh, highThresh);
    const int edgeCount = cv::countNonZero(edges);

        if (cfg.houghDilateKernel > 0 && cfg.houghDilateIterations > 0) {
            stage = "dilate";
            const int size = std::max(1, cfg.houghDilateKernel);
            cv::Mat kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(size, size));
            cv::dilate(edges, edges, kernel, cv::Point(-1, -1), cfg.houghDilateIterations);
        }

        stage = "detect_segments";
        const auto segments = detect_segments(edges, cfg);
        if (segments.size() < 4) {
            Logger::warning(QStringLiteral("detect_by_hough_search: segments=%1 (<4) | edges=%2")
                                .arg(static_cast<int>(segments.size()))
                                .arg(edgeCount));
            return std::nullopt;
        }
        stage = "detect_quads";
        const auto quads = detect_quads_from_segments(gray, segments, cfg);
        if (quads.empty()) {
            Logger::warning(QStringLiteral("detect_by_hough_search: segments=%1 but quads=0 | edges=%2")
                                .arg(static_cast<int>(segments.size()))
                                .arg(edgeCount));
            return std::nullopt;
        }
        return quads.front().corners;
    } catch (const cv::Exception &ex) {
        Logger::warning(QStringLiteral("detect_by_hough_search exception[%1]: %2 | type=%3 | size=%4x%5")
                            .arg(QString::fromUtf8(stage))
                            .arg(QString::fromUtf8(ex.what()))
                            .arg(matTypeToString(gray.type()))
                            .arg(gray.cols)
                            .arg(gray.rows));
        return std::nullopt;
    } catch (const std::exception &ex) {
        Logger::warning(QStringLiteral("detect_by_hough_search std exception[%1]: %2 | type=%3 | size=%4x%5")
                            .arg(QString::fromUtf8(stage))
                            .arg(QString::fromUtf8(ex.what()))
                            .arg(matTypeToString(gray.type()))
                            .arg(gray.cols)
                            .arg(gray.rows));
        return std::nullopt;
    }
}

std::optional<std::array<Point2, 4>> detect_by_white_region(const cv::Mat &gray,
                                                            const DetectionConfig &cfg,
                                                            cv::Mat *maskDebug = nullptr) {
    const char *stage = "gaussian_blur";
    try {
        if (maskDebug) {
            maskDebug->release();
        }
        const double whiteSigma = cfg.whiteGaussianSigma;
        cv::Mat blurred;
        if (std::isfinite(whiteSigma) && whiteSigma > 0.0) {
            cv::GaussianBlur(gray, blurred, cv::Size(), whiteSigma);
        } else {
            blurred = gray.clone();
        }

        stage = "threshold";
        cv::Mat thresh;
        cv::threshold(blurred, thresh, 0, 255, cv::THRESH_BINARY + cv::THRESH_OTSU);

        if (cfg.whiteMorphKernel > 0) {
            stage = "morphology";
            const int k = std::max(1, cfg.whiteMorphKernel);
            cv::Mat kernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(k, k));
            cv::morphologyEx(thresh, thresh, cv::MORPH_CLOSE, kernel, cv::Point(-1, -1), std::max(1, cfg.whiteMorphIterations));
        }

        stage = "connected_components";
        cv::Mat labels;
        cv::Mat stats;
        cv::Mat centroids;
        const int num = cv::connectedComponentsWithStats(thresh, labels, stats, centroids, 8);
        if (num <= 1) {
            Logger::warning(QStringLiteral("detect_by_white_region: no foreground regions (num=%1)")
                                .arg(num));
            return std::nullopt;
        }

        const double totalArea = static_cast<double>(gray.total());
        const int H = gray.rows;
        const int W = gray.cols;
        const int borderMargin = std::max(3, static_cast<int>(0.01 * std::min(H, W)));

        struct CandidateRegion {
            double score;
            double area;
            int touches;
            int label;
        };

        std::vector<CandidateRegion> candidates;
        candidates.reserve(num - 1);

        for (int idx = 1; idx < num; ++idx) {
            const int x = stats.at<int>(idx, cv::CC_STAT_LEFT);
            const int y = stats.at<int>(idx, cv::CC_STAT_TOP);
            const int w = stats.at<int>(idx, cv::CC_STAT_WIDTH);
            const int h = stats.at<int>(idx, cv::CC_STAT_HEIGHT);
            const double area = static_cast<double>(stats.at<int>(idx, cv::CC_STAT_AREA));

            if (w < 8 || h < 8) {
                continue;
            }

            const double fillRatio = area / std::max(1.0, static_cast<double>(w * h));
            const bool touchLeft = x <= borderMargin;
            const bool touchTop = y <= borderMargin;
            const bool touchRight = (x + w) >= (W - borderMargin);
            const bool touchBottom = (y + h) >= (H - borderMargin);
            const int touchCount = static_cast<int>(touchLeft) + static_cast<int>(touchTop) +
                                   static_cast<int>(touchRight) + static_cast<int>(touchBottom);

            const double frameRatio = area / std::max(1.0, totalArea);
            const double fillFactor = 0.2 + 0.8 * std::clamp(fillRatio, 0.0, 1.0);
            const double borderFactor = 1.0 / (1.0 + 0.6 * touchCount);
            const double globalPenalty = std::max(0.2, 1.0 - std::max(0.0, frameRatio - 0.55) * 0.8);
            const double score = area * fillFactor * borderFactor * globalPenalty;

            candidates.push_back({score, area, touchCount, idx});
        }

        if (candidates.empty()) {
            int largestIdx = 1;
            double largestArea = static_cast<double>(stats.at<int>(1, cv::CC_STAT_AREA));
            for (int idx = 2; idx < num; ++idx) {
                double area = static_cast<double>(stats.at<int>(idx, cv::CC_STAT_AREA));
                if (area > largestArea) {
                    largestArea = area;
                    largestIdx = idx;
                }
            }
            candidates.push_back({largestArea, largestArea, 0, largestIdx});
        }

        std::vector<CandidateRegion> filtered;
        std::copy_if(candidates.begin(), candidates.end(), std::back_inserter(filtered), [](const CandidateRegion &c) {
            return c.touches <= 2;
        });
        const CandidateRegion best = filtered.empty()
                                         ? *std::max_element(candidates.begin(), candidates.end(), [](const CandidateRegion &a, const CandidateRegion &b) {
                                               return a.score < b.score;
                                           })
                                         : *std::max_element(filtered.begin(), filtered.end(), [](const CandidateRegion &a, const CandidateRegion &b) {
                                               return a.score < b.score;
                                           });

        stage = "inrange";
        cv::Mat mask;
        cv::inRange(labels, cv::Scalar(best.label), cv::Scalar(best.label), mask);
        if (maskDebug) {
            mask.copyTo(*maskDebug);
        }

        stage = "find_contours";
        std::vector<std::vector<cv::Point>> contours;
        cv::findContours(mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
        if (contours.empty()) {
            Logger::warning(QStringLiteral("detect_by_white_region: mask contours empty | label=%1")
                                .arg(best.label));
            return std::nullopt;
        }

        stage = "convex_hull";
        auto largest = std::max_element(contours.begin(), contours.end(), [](const auto &lhs, const auto &rhs) {
            return cv::contourArea(lhs) < cv::contourArea(rhs);
        });

        std::vector<cv::Point> hull;
        cv::convexHull(*largest, hull);
        double perimeter = cv::arcLength(hull, true);
        double eps = cfg.whiteApproxEpsRatio * perimeter;

        stage = "approx_poly";
        std::array<Point2, 4> quad {};
        bool success = false;
        for (int iter = 0; iter < cfg.areaIterations; ++iter) {
            std::vector<cv::Point> approx;
            cv::approxPolyDP(hull, approx, eps, true);
            if (approx.size() == 4) {
                for (int i = 0; i < 4; ++i) {
                    quad[static_cast<size_t>(i)] = Point2{static_cast<float>(approx[i].x), static_cast<float>(approx[i].y)};
                }
                success = true;
                break;
            }
            if (approx.size() > 4) {
                eps *= cfg.whiteApproxExpand;
            } else {
                eps *= cfg.whiteApproxShrink;
            }
        }

        if (!success) {
            stage = "min_area_rect";
            cv::RotatedRect rect = cv::minAreaRect(hull);
            cv::Point2f rectPts[4];
            rect.points(rectPts);
            for (int i = 0; i < 4; ++i) {
                quad[static_cast<size_t>(i)] = rectPts[i];
            }
        }

        stage = "order_quad";
        auto ordered = order_quad(quad);
        for (auto &pt : ordered) {
            pt.x = std::clamp(pt.x, 0.0F, static_cast<float>(gray.cols - 1));
            pt.y = std::clamp(pt.y, 0.0F, static_cast<float>(gray.rows - 1));
        }
        return std::array<Point2, 4>{ordered[0], ordered[1], ordered[2], ordered[3]};
    } catch (const cv::Exception &ex) {
        Logger::warning(QStringLiteral("detect_by_white_region exception[%1]: %2 | type=%3 | size=%4x%5")
                            .arg(QString::fromUtf8(stage))
                            .arg(QString::fromUtf8(ex.what()))
                            .arg(matTypeToString(gray.type()))
                            .arg(gray.cols)
                            .arg(gray.rows));
        return std::nullopt;
    } catch (const std::exception &ex) {
        Logger::warning(QStringLiteral("detect_by_white_region std exception[%1]: %2 | type=%3 | size=%4x%5")
                            .arg(QString::fromUtf8(stage))
                            .arg(QString::fromUtf8(ex.what()))
                            .arg(matTypeToString(gray.type()))
                            .arg(gray.cols)
                            .arg(gray.rows));
        return std::nullopt;
    }
}

std::optional<std::array<Point2, 4>> refine_quad_local(const cv::Mat &gray, const std::array<Point2, 4> &quad, const DetectionConfig &cfg) {
    cv::Mat quadMat(4, 2, CV_32F);
    for (int i = 0; i < 4; ++i) {
        quadMat.at<float>(i, 0) = quad[static_cast<size_t>(i)].x;
        quadMat.at<float>(i, 1) = quad[static_cast<size_t>(i)].y;
    }

    const float pad = static_cast<float>(std::max(cfg.quadExpandOffset * 1.5, 20.0));
    float minX = quadMat.at<float>(0, 0);
    float maxX = quadMat.at<float>(0, 0);
    float minY = quadMat.at<float>(0, 1);
    float maxY = quadMat.at<float>(0, 1);
    for (int i = 1; i < 4; ++i) {
        minX = std::min(minX, quadMat.at<float>(i, 0));
        maxX = std::max(maxX, quadMat.at<float>(i, 0));
        minY = std::min(minY, quadMat.at<float>(i, 1));
        maxY = std::max(maxY, quadMat.at<float>(i, 1));
    }

    cv::Rect roi;
    roi.x = std::max(0, static_cast<int>(std::floor(minX - pad)));
    roi.y = std::max(0, static_cast<int>(std::floor(minY - pad)));
    const int x2 = std::min(gray.cols, static_cast<int>(std::ceil(maxX + pad)));
    const int y2 = std::min(gray.rows, static_cast<int>(std::ceil(maxY + pad)));
    roi.width = std::max(0, x2 - roi.x);
    roi.height = std::max(0, y2 - roi.y);

    if (roi.width < 20 || roi.height < 20) {
        return std::nullopt;
    }

    cv::Mat local = gray(roi).clone();
    if (std::isfinite(cfg.houghGaussianSigma) && cfg.houghGaussianSigma > 0.0) {
        cv::GaussianBlur(local, local, cv::Size(), cfg.houghGaussianSigma);
    }

    const double med = median_intensity(local);
    double low = std::max(cfg.houghCannyLowRatio * med, static_cast<double>(cfg.houghCannyLowMin));
    double high = std::max(low * cfg.houghCannyHighRatio, low + 1.0);
    high = std::min(high, 255.0);

    cv::Mat edges;
    cv::Canny(local, edges, low, high);
    if (cfg.houghDilateKernel > 0) {
        cv::Mat kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(cfg.houghDilateKernel, cfg.houghDilateKernel));
        cv::dilate(edges, edges, kernel, cv::Point(-1, -1), cfg.houghDilateIterations + 1);
    }

    cv::Mat mask = cv::Mat::zeros(edges.size(), CV_8U);
    std::vector<cv::Point> localQuad(4);
    for (int i = 0; i < 4; ++i) {
        localQuad[static_cast<size_t>(i)] = cv::Point(static_cast<int>(std::round(quad[static_cast<size_t>(i)].x - roi.x)),
                                                      static_cast<int>(std::round(quad[static_cast<size_t>(i)].y - roi.y)));
    }
    cv::fillConvexPoly(mask, localQuad, 255);
    cv::Mat masked;
    cv::bitwise_and(edges, mask, masked);

    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(masked, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
    if (contours.empty()) {
        return std::nullopt;
    }

    auto bestContour = std::max_element(contours.begin(), contours.end(), [](const auto &a, const auto &b) {
        return cv::contourArea(a) < cv::contourArea(b);
    });
    if (cv::contourArea(*bestContour) < 50.0) {
        return std::nullopt;
    }

    double per = cv::arcLength(*bestContour, true);
    double epsilon = 0.01 * per;
    std::vector<cv::Point> approx;
    cv::approxPolyDP(*bestContour, approx, epsilon, true);
    int tries = 0;
    while (approx.size() > 4 && tries < 6) {
        epsilon *= 1.5;
        cv::approxPolyDP(*bestContour, approx, epsilon, true);
        ++tries;
    }
    if (approx.size() < 4) {
        cv::approxPolyDP(*bestContour, approx, 0.03 * per, true);
    }

    std::array<Point2, 4> refinedQuad {};
    if (approx.size() == 4) {
        for (int i = 0; i < 4; ++i) {
            refinedQuad[static_cast<size_t>(i)] = Point2{static_cast<float>(approx[i].x + roi.x), static_cast<float>(approx[i].y + roi.y)};
        }
    } else {
        cv::RotatedRect rect = cv::minAreaRect(*bestContour);
        cv::Point2f pts[4];
        rect.points(pts);
        for (int i = 0; i < 4; ++i) {
            refinedQuad[static_cast<size_t>(i)] = Point2{pts[i].x + roi.x, pts[i].y + roi.y};
        }
    }

    auto ordered = order_quad(refinedQuad);
    std::array<Point2, 4> result {ordered[0], ordered[1], ordered[2], ordered[3]};

    Point2 centerOld {0, 0};
    Point2 centerNew {0, 0};
    for (const auto &p : quad) {
        centerOld += p;
    }
    for (const auto &p : result) {
        centerNew += p;
    }
    centerOld *= 0.25F;
    centerNew *= 0.25F;
    if (cv::norm(centerNew - centerOld) > std::max(pad * 0.8F, 40.0F)) {
        return std::nullopt;
    }

    if (quad_score(gray, result, cfg) <= -1e8) {
        return std::nullopt;
    }
    return result;
}

std::optional<QuadCandidate> detect_quad(const cv::Mat &gray, const DetectionConfig &cfg, cv::Mat *whiteMaskDebug) {
    auto evaluate = [&](const std::array<Point2, 4> &quad) -> std::optional<QuadCandidate> {
        PointVec orderedVec = order_quad(quad);
        if (orderedVec.size() != 4) {
            return std::nullopt;
        }
        std::array<Point2, 4> ordered {
            orderedVec[0], orderedVec[1], orderedVec[2], orderedVec[3]
        };
        double score = quad_score(gray, ordered, cfg);
        if (score <= -1e8) {
            return std::nullopt;
        }
        double area = std::abs(cv::contourArea(orderedVec));
        return QuadCandidate{ordered, score, area};
    };

    auto consider = [&](const std::array<Point2, 4> &quad, std::optional<QuadCandidate> &best) {
        if (auto base = evaluate(quad)) {
            if (!best || base->score > best->score) {
                best = base;
            }
        }
        if (auto refined = refine_quad_local(gray, quad, cfg)) {
            if (auto refinedCandidate = evaluate(*refined)) {
                if (!best || refinedCandidate->score > best->score) {
                    best = refinedCandidate;
                }
            }
        }
    };

    std::optional<QuadCandidate> best;

    if (auto white = detect_by_white_region(gray, cfg, whiteMaskDebug)) {
        consider(*white, best);
    } else if (whiteMaskDebug) {
        whiteMaskDebug->release();
    }

    if (auto hough = detect_by_hough_search(gray, cfg)) {
        consider(*hough, best);
    }

    return best;
}

BlobSet detect_blobs(const cv::Mat &input, const DetectionConfig &cfg) {
    cv::Ptr<cv::SimpleBlobDetector> detector;
    {
        cv::SimpleBlobDetector::Params params;
        params.filterByColor = cfg.blobDark;
        params.blobColor = cfg.blobDark ? 0 : 255;
        params.filterByArea = true;
        params.minArea = cfg.blobMinArea;
        params.maxArea = cfg.blobMaxArea;
        params.filterByCircularity = true;
        params.minCircularity = cfg.blobMinCircularity;
        params.filterByConvexity = true;
        params.minConvexity = cfg.blobMinConvexity;
        params.filterByInertia = true;
        params.minInertiaRatio = cfg.blobMinInertia;
        params.minThreshold = cfg.blobMinThreshold;
        params.maxThreshold = cfg.blobMaxThreshold;
        params.thresholdStep = cfg.blobThresholdStep;
        params.minDistBetweenBlobs = cfg.blobMinDist;
        detector = cv::SimpleBlobDetector::create(params);
    }

    std::vector<cv::KeyPoint> keypoints;
    detector->detect(input, keypoints);

    BlobSet result;
    result.raw.reserve(keypoints.size());
    result.refined.resize(keypoints.size());
    for (const auto &kp : keypoints) {
        BlobCandidate candidate;
        candidate.keypoint = kp;
        candidate.index = static_cast<int>(result.raw.size());
        result.raw.push_back(candidate);
    }
    return result;
}

[[maybe_unused]] cv::Mat segment_component(const cv::Mat &gray, const DetectionConfig &cfg) {
    cv::Mat blurred;
    if (std::isfinite(cfg.whiteGaussianSigma) && cfg.whiteGaussianSigma > 0.0) {
        cv::GaussianBlur(gray, blurred, cv::Size(), cfg.whiteGaussianSigma);
    } else {
        blurred = gray.clone();
    }

    cv::Mat white;
    cv::morphologyEx(blurred, white, cv::MORPH_CLOSE, cv::getStructuringElement(cv::MORPH_RECT, cv::Size(cfg.whiteMorphKernel, cfg.whiteMorphKernel)),
                     cv::Point(-1, -1), cfg.whiteMorphIterations);

    cv::Mat dist;
    cv::distanceTransform(white, dist, cv::DIST_L2, 3);
    cv::normalize(dist, dist, 0, 255, cv::NORM_MINMAX);
    dist.convertTo(dist, CV_8U);

    cv::Mat thresh;
    cv::threshold(white, thresh, 0, 255, cv::THRESH_BINARY + cv::THRESH_OTSU);

    cv::Mat opened;
    cv::morphologyEx(thresh, opened, cv::MORPH_OPEN, cv::getStructuringElement(cv::MORPH_RECT, cfg.refineOpenKernel), cv::Point(-1, -1), 1);
    return opened;
}

std::optional<RefinedBlob> refine_blob(const cv::Mat &gray, const BlobCandidate &blob, const DetectionConfig &cfg) {
    const Point2 seed = blob.keypoint.pt;
    const double seedRadius = std::max(1.0f, blob.keypoint.size * 0.5f);
    auto make_default = [&](double score) {
        RefinedBlob fallback;
        fallback.center = seed;
        fallback.radius = seedRadius;
        fallback.area = CV_PI * seedRadius * seedRadius;
        fallback.score = score;
        fallback.sourceIndex = blob.index;
        return fallback;
    };

    double winSize = std::clamp(seedRadius * cfg.refineWinScale, cfg.refineWinMin, cfg.refineWinMax);
    cv::Rect roi(static_cast<int>(std::round(seed.x - winSize * 0.5)),
                 static_cast<int>(std::round(seed.y - winSize * 0.5)),
                 static_cast<int>(std::round(winSize)),
                 static_cast<int>(std::round(winSize)));
    roi &= cv::Rect(0, 0, gray.cols, gray.rows);
    if (roi.width <= 6 || roi.height <= 6) {
        return make_default(0.2);
    }

    cv::Mat patch = gray(roi).clone();
    if (patch.empty()) {
        return make_default(0.2);
    }

    cv::GaussianBlur(patch, patch, cv::Size(0, 0), 1.0);

    cv::Mat thresh;
    cv::threshold(patch, thresh, 0, 255, cv::THRESH_BINARY_INV + cv::THRESH_OTSU);

    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(thresh, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
    if (contours.empty()) {
        return make_default(0.3);
    }

    auto largestIt = std::max_element(contours.begin(), contours.end(), [](const auto &a, const auto &b) {
        return cv::contourArea(a) < cv::contourArea(b);
    });
    double area = std::max(0.0, cv::contourArea(*largestIt));
    if (area < 5.0) {
        return make_default(0.3);
    }

    cv::Moments m = cv::moments(*largestIt);
    if (std::abs(m.m00) < 1e-6) {
        return make_default(0.3);
    }

    Point2 localCenter(static_cast<float>(m.m10 / m.m00), static_cast<float>(m.m01 / m.m00));
    Point2 refined {localCenter.x + static_cast<float>(roi.x), localCenter.y + static_cast<float>(roi.y)};

    RefinedBlob result = make_default(0.4);
    const double shift = cv::norm(refined - seed);
    if (shift <= seedRadius * std::max(1.0, cfg.refineGate)) {
        result.center = refined;
        result.radius = std::sqrt(std::max(area / CV_PI, 1.0));
        result.area = std::max(area, 1.0);
        result.score = 1.0;
    }

    return result;
}

BlobSet refine_blobs(const cv::Mat &gray, BlobSet blobs, const DetectionConfig &cfg) {
    for (size_t i = 0; i < blobs.raw.size(); ++i) {
        blobs.refined[i] = refine_blob(gray, blobs.raw[i], cfg);
    }
    return blobs;
}

double blob_area(const RefinedBlob &blob)
{
    if (blob.area > 0.0) {
        return blob.area;
    }
    return CV_PI * blob.radius * blob.radius;
}

double median_value(std::vector<double> values)
{
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const size_t mid = values.size() / 2;
    if (values.size() % 2 == 0) {
        return 0.5 * (values[mid - 1] + values[mid]);
    }
    return values[mid];
}

std::vector<RefinedBlob> select_by_area(const std::vector<RefinedBlob> &items,
                                        int target,
                                        double relax,
                                        const DetectionConfig &cfg)
{
    if (target <= 0 || items.empty()) {
        return {};
    }
    if (static_cast<int>(items.size()) <= target) {
        return items;
    }

    std::vector<double> areas;
    areas.reserve(items.size());
    for (const auto &item : items) {
        areas.push_back(blob_area(item));
    }

    double med = median_value(areas);
    std::vector<double> deviations;
    deviations.reserve(areas.size());
    for (double a : areas) {
        deviations.push_back(std::abs(a - med));
    }
    double mad = median_value(deviations) + 1e-6;
    double lo = med - 2.5 * mad;
    double hi = med + 2.5 * mad;

    const double relaxFactor = (relax > 0.0 ? relax : cfg.areaRelaxDefault);

    for (int iter = 0; iter < cfg.areaIterations; ++iter) {
        std::vector<size_t> picked;
        picked.reserve(items.size());
        for (size_t i = 0; i < areas.size(); ++i) {
            if (areas[i] >= lo && areas[i] <= hi) {
                picked.push_back(i);
            }
        }
        if (static_cast<int>(picked.size()) >= target) {
            std::vector<double> pickedAreas;
            pickedAreas.reserve(picked.size());
            for (size_t idx : picked) {
                pickedAreas.push_back(areas[idx]);
            }
            const double pickedMedian = median_value(pickedAreas);
            std::sort(picked.begin(), picked.end(), [&](size_t a, size_t b) {
                const double da = std::abs(areas[a] - pickedMedian);
                const double db = std::abs(areas[b] - pickedMedian);
                if (std::abs(da - db) > 1e-6) {
                    return da < db;
                }
                return areas[a] > areas[b];
            });
            std::vector<RefinedBlob> result;
            result.reserve(static_cast<size_t>(target));
            for (int i = 0; i < target && i < static_cast<int>(picked.size()); ++i) {
                result.push_back(items[picked[static_cast<size_t>(i)]]);
            }
            return result;
        }

        const double width = hi - lo;
        lo -= width * relaxFactor * 0.5;
        hi += width * relaxFactor * 0.5;
    }

    std::vector<size_t> order(areas.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        const double da = std::abs(areas[a] - med);
        const double db = std::abs(areas[b] - med);
        if (std::abs(da - db) > 1e-6) {
            return da < db;
        }
        return areas[a] > areas[b];
    });

    std::vector<RefinedBlob> fallback;
    const int limit = std::min<int>(target, static_cast<int>(order.size()));
    fallback.reserve(static_cast<size_t>(limit));
    for (int i = 0; i < limit; ++i) {
        fallback.push_back(items[order[static_cast<size_t>(i)]]);
    }
    return fallback;
}

struct SizeClusterResult {
    std::vector<int> labels;
    int smallLabel {0};
    int bigLabel {1};
};

SizeClusterResult classify_blob_sizes(const std::vector<BlobCandidate> &blobs)
{
    SizeClusterResult result;
    const size_t count = blobs.size();
    result.labels.assign(count, 0);
    if (count < 2) {
        return result;
    }

    cv::Mat samples(static_cast<int>(count), 1, CV_32F);
    for (size_t i = 0; i < count; ++i) {
        samples.at<float>(static_cast<int>(i), 0) = blobs[i].keypoint.size;
    }

    cv::Mat labels;
    cv::Mat centers;
    const cv::TermCriteria criteria(cv::TermCriteria::EPS + cv::TermCriteria::MAX_ITER, 200, 1e-4);
    try {
        cv::kmeans(samples, 2, labels, criteria, 8, cv::KMEANS_PP_CENTERS, centers);
    } catch (const cv::Exception &) {
        return result;
    }

    std::array<int, 2> counts {0, 0};
    for (int i = 0; i < labels.rows; ++i) {
        int label = labels.at<int>(i, 0);
        if (label < 0 || label > 1) {
            label = 0;
        }
        result.labels[static_cast<size_t>(i)] = label;
        counts[static_cast<size_t>(label)]++;
    }

    if (counts[0] >= counts[1]) {
        result.smallLabel = 0;
        result.bigLabel = 1;
    } else {
        result.smallLabel = 1;
        result.bigLabel = 0;
    }

    return result;
}

NumberingResult number_circles(const std::vector<RefinedBlob> &smalls,
                               const std::vector<RefinedBlob> &bigs,
                               const cv::Size &rectSize,
                               const BoardSpec &spec)
{
    const int expected = static_cast<int>(spec.expectedCircleCount());
    if (static_cast<int>(smalls.size()) != expected) {
        return {false, {}, {}, "circle count mismatch"};
    }

    constexpr int kRows = 7;
    const std::array<int, kRows> expectedRowSizes {6, 6, 6, 5, 6, 6, 6};

    auto axes = axes_from_big4(bigs);
    if (!axes.valid) {
        axes.origin = Point2(static_cast<float>(rectSize.width) * 0.5F,
                             static_cast<float>(rectSize.height) * 0.5F);
        axes.xHat = cv::Vec2f(1.0F, 0.0F);
        axes.yHat = cv::Vec2f(0.0F, 1.0F);
    }

    std::vector<float> u(smalls.size(), 0.0F);
    std::vector<float> v(smalls.size(), 0.0F);
    for (size_t i = 0; i < smalls.size(); ++i) {
        const cv::Vec2f rel = smalls[i].center - axes.origin;
        u[i] = rel[0] * axes.xHat[0] + rel[1] * axes.xHat[1];
        v[i] = rel[0] * axes.yHat[0] + rel[1] * axes.yHat[1];
    }

    const auto km = kmeans_1d(v, kRows);
    if (!km.success) {
        Logger::warning(QStringLiteral("number_circles: kmeans failed, unable to cluster rows"));
        return {false, {}, {}, "kmeans_failed"};
    }

    std::vector<int> order(kRows);
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](int lhs, int rhs) {
        return km.centers[static_cast<size_t>(lhs)] < km.centers[static_cast<size_t>(rhs)];
    });
    std::vector<int> rank(kRows, 0);
    for (int i = 0; i < kRows; ++i) {
        rank[static_cast<size_t>(order[static_cast<size_t>(i)])] = i;
    }

    struct RowCluster {
        std::vector<int> indices;
        float center {0.0F};
    };

    std::vector<RowCluster> rows(kRows);
    for (int i = 0; i < kRows; ++i) {
        rows[static_cast<size_t>(i)].center = km.centers[static_cast<size_t>(order[static_cast<size_t>(i)])];
    }

    for (size_t i = 0; i < smalls.size(); ++i) {
        const int raw = km.labels[static_cast<size_t>(i)];
        if (raw < 0 || raw >= kRows) {
            Logger::warning(QStringLiteral("number_circles: row label out of range %1").arg(raw));
            return {false, {}, {}, "invalid_row_label"};
        }
        rows[static_cast<size_t>(rank[static_cast<size_t>(raw)])].indices.push_back(static_cast<int>(i));
    }

    auto sortRow = [&](RowCluster &row) {
        std::sort(row.indices.begin(), row.indices.end(), [&](int lhs, int rhs) {
            return u[static_cast<size_t>(lhs)] < u[static_cast<size_t>(rhs)];
        });
    };

    auto recomputeCenter = [&](RowCluster &row) {
        if (row.indices.empty()) {
            return;
        }
        double sum = 0.0;
        for (int idx : row.indices) {
            sum += v[static_cast<size_t>(idx)];
        }
        row.center = static_cast<float>(sum / static_cast<double>(row.indices.size()));
    };

    auto recomputeAllCenters = [&]() {
        for (auto &row : rows) {
            recomputeCenter(row);
        }
    };

    for (auto &row : rows) {
        sortRow(row);
    }
    recomputeAllCenters();

    struct MoveCandidate {
        int index {-1};
        size_t position {0};
        float cost {std::numeric_limits<float>::max()};
        [[nodiscard]] bool valid() const { return index >= 0; }
    };

    auto evaluateCandidate = [&](int donorIdx, int targetIdx) -> MoveCandidate {
        MoveCandidate best;
        const auto &indices = rows[static_cast<size_t>(donorIdx)].indices;
        if (indices.empty()) {
            return best;
        }
        for (size_t pos = 0; pos < indices.size(); ++pos) {
            const int idx = indices[pos];
            const float rowPenalty = static_cast<float>(std::abs(donorIdx - targetIdx)) * 1000.0F;
            const float centerPenalty = std::abs(v[static_cast<size_t>(idx)] - rows[static_cast<size_t>(targetIdx)].center);
            const float totalCost = rowPenalty + centerPenalty;
            if (!best.valid() || totalCost < best.cost) {
                best.index = idx;
                best.position = pos;
                best.cost = totalCost;
            }
        }
        return best;
    };

    for (int iter = 0; iter < 8; ++iter) {
        bool moved = false;

        for (int target = 0; target < kRows; ++target) {
            const int expectedCount = expectedRowSizes[static_cast<size_t>(target)];
            while (static_cast<int>(rows[static_cast<size_t>(target)].indices.size()) < expectedCount) {
                MoveCandidate best;
                int bestDonor = -1;
                for (int donor = 0; donor < kRows; ++donor) {
                    if (donor == target) {
                        continue;
                    }
                    if (static_cast<int>(rows[static_cast<size_t>(donor)].indices.size()) <= expectedRowSizes[static_cast<size_t>(donor)]) {
                        continue;
                    }
                    MoveCandidate candidate = evaluateCandidate(donor, target);
                    if (!candidate.valid()) {
                        continue;
                    }
                    if (!best.valid() || candidate.cost < best.cost) {
                        best = candidate;
                        bestDonor = donor;
                    }
                }
                if (bestDonor == -1) {
                    break;
                }
                auto &donorVec = rows[static_cast<size_t>(bestDonor)].indices;
                if (best.position < donorVec.size()) {
                    donorVec.erase(donorVec.begin() + static_cast<std::ptrdiff_t>(best.position));
                }
                rows[static_cast<size_t>(target)].indices.push_back(best.index);
                sortRow(rows[static_cast<size_t>(target)]);
                sortRow(rows[static_cast<size_t>(bestDonor)]);
                moved = true;
            }
        }

        for (int donor = 0; donor < kRows; ++donor) {
            const int allowed = expectedRowSizes[static_cast<size_t>(donor)];
            while (static_cast<int>(rows[static_cast<size_t>(donor)].indices.size()) > allowed) {
                MoveCandidate best;
                int bestTarget = -1;
                for (int target = 0; target < kRows; ++target) {
                    if (target == donor) {
                        continue;
                    }
                    if (static_cast<int>(rows[static_cast<size_t>(target)].indices.size()) >= expectedRowSizes[static_cast<size_t>(target)]) {
                        continue;
                    }
                    MoveCandidate candidate = evaluateCandidate(donor, target);
                    if (!candidate.valid()) {
                        continue;
                    }
                    if (!best.valid() || candidate.cost < best.cost) {
                        best = candidate;
                        bestTarget = target;
                    }
                }
                if (bestTarget == -1) {
                    break;
                }
                auto &donorVec = rows[static_cast<size_t>(donor)].indices;
                if (best.position < donorVec.size()) {
                    donorVec.erase(donorVec.begin() + static_cast<std::ptrdiff_t>(best.position));
                }
                rows[static_cast<size_t>(bestTarget)].indices.push_back(best.index);
                sortRow(rows[static_cast<size_t>(donor)]);
                sortRow(rows[static_cast<size_t>(bestTarget)]);
                moved = true;
            }
        }

        if (!moved) {
            break;
        }

        recomputeAllCenters();
    }

    for (auto &row : rows) {
        sortRow(row);
    }

    auto columnFor = [](int rowIndex, int orderIndex) {
        if (rowIndex == 3 && orderIndex >= 3) {
            return orderIndex + 1;
        }
        return orderIndex;
    };

    std::vector<Point2> ordered;
    ordered.reserve(expected);
    std::vector<cv::Vec2i> logical;
    logical.reserve(expected);
    std::vector<int> source;
    source.reserve(expected);
    QStringList rowSizeDebug;
    rowSizeDebug.reserve(kRows);

    int rowsWithFive = 0;
    for (int rowIdx = 0; rowIdx < kRows; ++rowIdx) {
        const auto &indices = rows[static_cast<size_t>(rowIdx)].indices;
        const bool expectFive = (rowIdx == 3);
        const int expectedCount = expectFive ? 5 : 6;
        const int actual = static_cast<int>(indices.size());
        rowSizeDebug << QString::number(actual);
        if (expectFive && actual == 5) {
            ++rowsWithFive;
        }
        if (actual != expectedCount) {
            Logger::warning(QStringLiteral("number_circles: row %1 count=%2 expected=%3 | rows=%4")
                                .arg(rowIdx)
                                .arg(actual)
                                .arg(expectedCount)
                                .arg(rowSizeDebug.join(QStringLiteral(","))));
            return {false, {}, {}, "row_size_mismatch"};
        }

        int colCounter = 0;
        for (int idx : indices) {
            const int col = columnFor(rowIdx, colCounter);
            ++colCounter;
        ordered.push_back(smalls[static_cast<size_t>(idx)].center);
        logical.emplace_back(rowIdx, col);
        source.push_back(idx);
        }
    }

    if (rowsWithFive != 1) {
        Logger::warning(QStringLiteral("number_circles: center row count anomaly %1")
                            .arg(rowSizeDebug.join(QStringLiteral(","))));
        return {false, {}, {}, "missing_center_row_not_unique"};
    }

    if (static_cast<int>(ordered.size()) != expected || logical.size() != ordered.size()) {
        Logger::warning(QStringLiteral("number_circles: ordered count mismatch result=%1 expected=%2 | rows=%3")
                            .arg(static_cast<int>(ordered.size()))
                            .arg(expected)
                            .arg(rowSizeDebug.join(QStringLiteral(","))));
        return {false, {}, {}, "ordered_size_mismatch"};
    }

    std::vector<int> permutation(ordered.size());
    std::iota(permutation.begin(), permutation.end(), 0);
    std::sort(permutation.begin(), permutation.end(), [&](int lhs, int rhs) {
        const auto &a = logical[static_cast<size_t>(lhs)];
        const auto &b = logical[static_cast<size_t>(rhs)];
        if (a[0] != b[0]) {
            return a[0] < b[0];
        }
        return a[1] < b[1];
    });

    std::vector<Point2> orderedSorted;
    orderedSorted.reserve(ordered.size());
    std::vector<cv::Vec2i> logicalSorted;
    logicalSorted.reserve(logical.size());
    std::vector<int> sourceSorted;
    sourceSorted.reserve(source.size());
    for (int idx : permutation) {
        orderedSorted.push_back(ordered[static_cast<size_t>(idx)]);
        logicalSorted.push_back(logical[static_cast<size_t>(idx)]);
        sourceSorted.push_back(source[static_cast<size_t>(idx)]);
    }

    NumberingResult result;
    result.success = true;
    result.orderedPoints = std::move(orderedSorted);
    result.logicalIndices = std::move(logicalSorted);
    result.sourceIndices = std::move(sourceSorted);
    return result;
}
} // namespace

BoardDetector::BoardDetector(const DetectionConfig &config) : m_cfg(sanitize_config(config)) {}

DetectionResult BoardDetector::detect(const cv::Mat &inputGray, const BoardSpec &spec, const std::string &name) const {
    DetectionResult result;
    result.name = name;

    const auto start = std::chrono::steady_clock::now();
    std::string stage = "initialize";
    cv::Mat gray;

    try {
    Logger::info(QStringLiteral("%1: input type=%2 | size=%3x%4")
                         .arg(QString::fromStdString(name))
                         .arg(matTypeToString(inputGray.type()))
                         .arg(inputGray.cols)
                         .arg(inputGray.rows));

        stage = "ensure_gray";
        gray = ensure_gray(inputGray);
        if (gray.empty()) {
            result.message = "Input image is empty";
            result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
            return result;
        }
        if (gray.type() != CV_8UC1) {
            gray.convertTo(gray, CV_8UC1);
        }
        result.resolution = gray.size();

        const std::uint64_t debugId = g_debugCounter.fetch_add(1, std::memory_order_relaxed);
        const std::string sanitizedName = sanitize_filename(name);
        std::filesystem::path debugDir = std::filesystem::temp_directory_path() / "calib_debug" /
                                         (sanitizedName + "_" + std::to_string(debugId));
        std::error_code makeDirEc;
        std::filesystem::create_directories(debugDir, makeDirEc);
        if (!makeDirEc) {
            result.debugDirectory = debugDir.string();
        } else {
            result.debugDirectory.clear();
        }

        const int debugMaxDim = 1600;
        cv::Mat originalColor = ensure_color_8u(gray);
        auto addDebugImage = [&](const std::string &label, const cv::Mat &image) {
            if (image.empty()) {
                return;
            }
            cv::Mat display = downscale_for_display(image, debugMaxDim);
            if (result.debugDirectory.empty()) {
                return;
            }
            const std::string slug = sanitize_filename(label);
            std::filesystem::path targetPath = std::filesystem::path(result.debugDirectory) /
                                             (slug + "_" + std::to_string(result.debugImages.size()) + ".png");
            if (!cv::imwrite(targetPath.string(), display)) {
                return;
            }
            DetectionDebugImage view;
            view.label = label;
            view.filePath = targetPath.string();
            result.debugImages.push_back(std::move(view));
        };
        auto drawQuadOverlay = [](cv::Mat &canvas, const std::array<Point2, 4> &corners, const cv::Scalar &color, int thickness) {
            std::vector<cv::Point> poly(4);
            for (int i = 0; i < 4; ++i) {
                poly[static_cast<size_t>(i)] = cv::Point(cvRound(corners[static_cast<size_t>(i)].x),
                                                        cvRound(corners[static_cast<size_t>(i)].y));
            }
            cv::polylines(canvas, poly, true, color, thickness, cv::LINE_AA);
        };

        addDebugImage("Input", originalColor);

    stage = "detect_quad/hough";
    cv::Mat whiteMaskDebug;
    auto quadOpt = detect_quad(gray, m_cfg, &whiteMaskDebug);
        if (!quadOpt) {
            if (!whiteMaskDebug.empty()) {
                cv::Mat maskColor;
                cv::applyColorMap(whiteMaskDebug, maskColor, cv::COLORMAP_JET);
                cv::Mat blended;
                cv::addWeighted(maskColor, 0.65, originalColor, 0.35, 0.0, blended);
                addDebugImage("White-region mask", blended);
            }
            result.message = "Failed to locate chessboard quadrilateral";
            result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
            return result;
        }
        const auto quad = *quadOpt;

        if (!whiteMaskDebug.empty()) {
            cv::Mat maskColor;
            cv::applyColorMap(whiteMaskDebug, maskColor, cv::COLORMAP_JET);
            cv::Mat blended;
            cv::addWeighted(maskColor, 0.65, originalColor, 0.35, 0.0, blended);
            addDebugImage("White-region mask", blended);
        }

        {
            cv::Mat quadOverlay = originalColor.clone();
            drawQuadOverlay(quadOverlay, quad.corners, cv::Scalar(0, 210, 255), 3);
            addDebugImage("Quad outline", quadOverlay);
        }

        stage = "validate_quad";
        if (!quad_within_image(quad.corners, gray.rows, gray.cols, m_cfg.quadMargin)) {
            result.message = "Chessboard quadrilateral is outside image bounds";
            result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
            return result;
        }

        stage = "expand_quad";
        auto expandedQuadOpt = expand_quad(quad.corners, m_cfg.quadExpandScale, m_cfg.quadExpandOffset);
        if (!expandedQuadOpt) {
            result.message = "Quad expansion failed";
            result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
            return result;
        }
        const auto expandedQuad = *expandedQuadOpt;

        stage = "warp_quad";
        WarpResult warp = warp_quad(gray, *expandedQuadOpt, m_cfg);
        if (warp.image.empty() || warp.homography.empty() || warp.homographyInv.empty()) {
            result.message = "Perspective warp failed";
            result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
            return result;
        }

        addDebugImage("Rectified board", ensure_color_8u(warp.image));

        stage = "preprocess_rect";
        cv::Mat rectPre = preprocess_rect(warp.image, m_cfg);
        cv::Mat rectPreColor = ensure_color_8u(rectPre);
        addDebugImage("Preprocessed", rectPreColor);

        stage = "detect_blobs";
        auto blobs = detect_blobs(rectPre, m_cfg);
        stage = "refine_blobs";
        blobs = refine_blobs(rectPre, std::move(blobs), m_cfg);

    Logger::info(QStringLiteral("%1: initial circle candidates = %2")
                         .arg(QString::fromStdString(name))
                         .arg(static_cast<int>(blobs.raw.size())));

        stage = "classify_blob_sizes";
        const auto clusters = classify_blob_sizes(blobs.raw);

        std::vector<RefinedBlob> smallCandidates;
        std::vector<RefinedBlob> bigCandidates;
        std::vector<RefinedBlob> allCandidates;
        smallCandidates.reserve(blobs.raw.size());
        bigCandidates.reserve(8);
        allCandidates.reserve(blobs.raw.size());

        for (size_t i = 0; i < blobs.raw.size(); ++i) {
            if (!blobs.refined[i].has_value()) {
                continue;
            }
            RefinedBlob blob = *blobs.refined[i];
            blob.sourceIndex = blobs.raw[i].index;
            allCandidates.push_back(blob);
            if (!clusters.labels.empty() && clusters.labels.size() == blobs.raw.size()) {
                if (clusters.labels[i] == clusters.bigLabel) {
                    bigCandidates.push_back(blob);
                } else {
                    smallCandidates.push_back(blob);
                }
            } else {
                smallCandidates.push_back(blob);
            }
        }

    Logger::info(QStringLiteral("%1: small candidates=%2, large candidates=%3, total=%4")
                         .arg(QString::fromStdString(name))
                         .arg(static_cast<int>(smallCandidates.size()))
                         .arg(static_cast<int>(bigCandidates.size()))
                         .arg(static_cast<int>(allCandidates.size())));

        if (allCandidates.size() >= 8 && (smallCandidates.size() < 30 || bigCandidates.size() < 2)) {
            stage = "reassign_big_candidates";
            std::vector<RefinedBlob> sortedAll = allCandidates;
            std::sort(sortedAll.begin(), sortedAll.end(), [](const RefinedBlob &a, const RefinedBlob &b) {
                return blob_area(a) > blob_area(b);
            });
            const size_t topCount = std::min<size_t>(6, sortedAll.size());
            std::vector<RefinedBlob> bigPool(sortedAll.begin(), sortedAll.begin() + topCount);
            std::vector<RefinedBlob> reassigned = select_by_area(bigPool, 4, m_cfg.areaRelaxReassignBig, m_cfg);
            std::unordered_set<int> bigIndices;
            for (const auto &b : reassigned) {
                bigIndices.insert(b.sourceIndex);
            }
            bigCandidates = reassigned;
            smallCandidates.clear();
            for (const auto &cand : allCandidates) {
                if (bigIndices.find(cand.sourceIndex) == bigIndices.end()) {
                    smallCandidates.push_back(cand);
                }
            }
        }

        stage = "select_by_area";
        const int expectedSmall = static_cast<int>(spec.expectedCircleCount());
        std::vector<RefinedBlob> selectedSmall = select_by_area(smallCandidates, expectedSmall, m_cfg.areaRelaxSmall, m_cfg);
        std::vector<RefinedBlob> selectedBig = select_by_area(bigCandidates, 4, m_cfg.areaRelaxBig, m_cfg);

        {
            cv::Mat selectionOverlay = rectPreColor.clone();
            for (const auto &blob : selectedSmall) {
                const cv::Point center(cvRound(blob.center.x), cvRound(blob.center.y));
                const int radius = std::max(2, static_cast<int>(std::round(blob.radius)));
                cv::circle(selectionOverlay, center, radius, cv::Scalar(80, 220, 120), 2, cv::LINE_AA);
            }
            for (const auto &blob : selectedBig) {
                const cv::Point center(cvRound(blob.center.x), cvRound(blob.center.y));
                const int radius = std::max(3, static_cast<int>(std::round(blob.radius * 1.2)));
                cv::circle(selectionOverlay, center, radius, cv::Scalar(40, 90, 240), 3, cv::LINE_AA);
            }
            addDebugImage("Selected circles (rectified)", selectionOverlay);
        }

    Logger::info(QStringLiteral("%1: selected small=%2, large=%3")
                         .arg(QString::fromStdString(name))
                         .arg(static_cast<int>(selectedSmall.size()))
                         .arg(static_cast<int>(selectedBig.size())));

        if (static_cast<int>(selectedSmall.size()) != expectedSmall) {
            Logger::warning(QStringLiteral("%1: insufficient small circles (expected %2, got %3)")
                                .arg(QString::fromStdString(name))
                                .arg(expectedSmall)
                                .arg(static_cast<int>(selectedSmall.size())));
            result.message = "Detected circle count mismatch";
            result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
            return result;
        }

    stage = "number_circles";
    auto numbering = number_circles(selectedSmall, selectedBig, warp.image.size(), spec);
        if (!numbering.success) {
            Logger::warning(QStringLiteral("%1: numbering failed: %2")
                                .arg(QString::fromStdString(name))
                                .arg(QString::fromStdString(numbering.message)));
            result.message = numbering.message;
            result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
            return result;
        }

        auto projectRadius = [&](const cv::Point2f &center, float radius) -> float {
            if (radius <= 0.0f) {
                return 0.0f;
            }
            if (warp.homographyInv.rows != 3 || warp.homographyInv.cols != 3) {
                return radius;
            }
            cv::Mat samples(5, 1, CV_32FC2);
            samples.at<cv::Point2f>(0, 0) = center;
            samples.at<cv::Point2f>(1, 0) = center + cv::Point2f(radius, 0.0f);
            samples.at<cv::Point2f>(2, 0) = center - cv::Point2f(radius, 0.0f);
            samples.at<cv::Point2f>(3, 0) = center + cv::Point2f(0.0f, radius);
            samples.at<cv::Point2f>(4, 0) = center - cv::Point2f(0.0f, radius);
            cv::perspectiveTransform(samples, samples, warp.homographyInv);
            const cv::Point2f transformedCenter = samples.at<cv::Point2f>(0, 0);
            double sum = 0.0;
            int count = 0;
            for (int i = 1; i < samples.rows; ++i) {
                sum += cv::norm(samples.at<cv::Point2f>(i, 0) - transformedCenter);
                ++count;
            }
            return count > 0 ? static_cast<float>(sum / static_cast<double>(count)) : radius;
        };

        stage = "back_project_points";
        if (warp.homographyInv.rows == 3 && warp.homographyInv.cols == 3) {
            cv::Mat pts(static_cast<int>(numbering.orderedPoints.size()), 1, CV_32FC2);
            for (int i = 0; i < pts.rows; ++i) {
                pts.at<cv::Point2f>(i, 0) = numbering.orderedPoints[static_cast<size_t>(i)];
            }
            cv::perspectiveTransform(pts, pts, warp.homographyInv);
            result.imagePoints.resize(numbering.orderedPoints.size());
            for (int i = 0; i < pts.rows; ++i) {
                result.imagePoints[static_cast<size_t>(i)] = pts.at<cv::Point2f>(i, 0);
            }

            if (!selectedBig.empty()) {
                cv::Mat bigPts(static_cast<int>(selectedBig.size()), 1, CV_32FC2);
                for (int i = 0; i < bigPts.rows; ++i) {
                    bigPts.at<cv::Point2f>(i, 0) = selectedBig[static_cast<size_t>(i)].center;
                }
                cv::perspectiveTransform(bigPts, bigPts, warp.homographyInv);
                result.bigCirclePoints.resize(selectedBig.size());
                for (int i = 0; i < bigPts.rows; ++i) {
                    result.bigCirclePoints[static_cast<size_t>(i)] = bigPts.at<cv::Point2f>(i, 0);
                }
                result.bigCircleRadiiPx.resize(selectedBig.size());
                for (size_t i = 0; i < selectedBig.size(); ++i) {
                    result.bigCircleRadiiPx[i] = projectRadius(selectedBig[i].center,
                                                               static_cast<float>(selectedBig[i].radius));
                }
            } else {
                result.bigCirclePoints.clear();
                result.bigCircleRadiiPx.clear();
            }
        } else {
            result.imagePoints = numbering.orderedPoints;
            result.bigCirclePoints.resize(selectedBig.size());
            result.bigCircleRadiiPx.resize(selectedBig.size());
            for (size_t i = 0; i < selectedBig.size(); ++i) {
                result.bigCirclePoints[i] = selectedBig[i].center;
                result.bigCircleRadiiPx[i] = static_cast<float>(selectedBig[i].radius);
            }
        }
        result.bigCircleCount = static_cast<int>(result.bigCirclePoints.size());
        result.logicalIndices = numbering.logicalIndices;
        result.circleRadiiPx.clear();
        result.circleRadiiPx.reserve(numbering.sourceIndices.size());
        for (size_t i = 0; i < numbering.sourceIndices.size(); ++i) {
            float storedRadius = 0.0f;
            const int idx = numbering.sourceIndices[i];
            if (idx >= 0 && idx < static_cast<int>(selectedSmall.size())) {
                const auto &blob = selectedSmall[static_cast<size_t>(idx)];
                storedRadius = projectRadius(blob.center, static_cast<float>(blob.radius));
            }
            result.circleRadiiPx.push_back(storedRadius);
        }
        if (!whiteMaskDebug.empty()) {
            result.whiteRegionMask = whiteMaskDebug.clone();
        } else {
            result.whiteRegionMask.release();
        }
        if (!warp.homography.empty()) {
            result.warpHomography = warp.homography.clone();
        } else {
            result.warpHomography.release();
        }
        if (!warp.homographyInv.empty()) {
            result.warpHomographyInv = warp.homographyInv.clone();
        } else {
            result.warpHomographyInv.release();
        }

        {
            cv::Mat warpOverlay = ensure_color_8u(warp.image);
            const std::array<cv::Scalar, 7> rowColors = {
                cv::Scalar(255, 206, 86),
                cv::Scalar(129, 212, 250),
                cv::Scalar(186, 104, 200),
                cv::Scalar(255, 167, 112),
                cv::Scalar(144, 238, 144),
                cv::Scalar(173, 190, 255),
                cv::Scalar(255, 221, 153)
            };

            const auto &logical = numbering.logicalIndices;
            for (size_t i = 0; i < numbering.orderedPoints.size(); ++i) {
                const cv::Point center(cvRound(numbering.orderedPoints[i].x), cvRound(numbering.orderedPoints[i].y));
                int rowIdx = 0;
                if (logical.size() == numbering.orderedPoints.size()) {
                    rowIdx = std::clamp(logical[static_cast<size_t>(i)][0], 0, static_cast<int>(rowColors.size() - 1));
                } else {
                    rowIdx = static_cast<int>(std::min<size_t>(rowColors.size() - 1, i / 6));
                }
                const cv::Scalar color = rowColors[static_cast<size_t>(rowIdx)];
                cv::circle(warpOverlay, center, 6, color, -1, cv::LINE_AA);
                cv::circle(warpOverlay, center, 10, color, 2, cv::LINE_AA);

                if (logical.size() == numbering.orderedPoints.size()) {
                    std::ostringstream stream;
                    stream << logical[static_cast<size_t>(i)][0] << ":" << logical[static_cast<size_t>(i)][1];
                    const std::string label = stream.str();
                    const cv::Point textPos = center + cv::Point(-18, -10);
                    cv::putText(warpOverlay, label, textPos, cv::FONT_HERSHEY_SIMPLEX, 0.42, cv::Scalar(20, 20, 20), 2, cv::LINE_AA);
                    cv::putText(warpOverlay, label, textPos, cv::FONT_HERSHEY_SIMPLEX, 0.42, cv::Scalar(245, 245, 245), 1, cv::LINE_AA);
                }
            }

            auto axesRect = axes_from_big4(selectedBig);
            if (!axesRect.valid) {
                axesRect.origin = Point2(static_cast<float>(warpOverlay.cols) * 0.6F,
                                         static_cast<float>(warpOverlay.rows) * 0.7F);
                axesRect.xHat = cv::Vec2f(1.0F, 0.0F);
                axesRect.yHat = cv::Vec2f(0.0F, 1.0F);
                axesRect.valid = true;
            }
            if (axesRect.valid) {
                const cv::Point origin(cvRound(axesRect.origin.x), cvRound(axesRect.origin.y));
                const int arrowLen = std::max(40, std::min(warpOverlay.cols, warpOverlay.rows) / 8);
                const cv::Point xEnd = origin + cv::Point(cvRound(axesRect.xHat[0] * arrowLen), cvRound(axesRect.xHat[1] * arrowLen));
                const cv::Point yEnd = origin + cv::Point(cvRound(axesRect.yHat[0] * arrowLen), cvRound(axesRect.yHat[1] * arrowLen));
                cv::arrowedLine(warpOverlay, origin, xEnd, cv::Scalar(64, 200, 255), 2, cv::LINE_AA, 0, 0.2);
                cv::arrowedLine(warpOverlay, origin, yEnd, cv::Scalar(255, 140, 90), 2, cv::LINE_AA, 0, 0.2);
                cv::putText(warpOverlay, "X", xEnd + cv::Point(4, -4), cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(15, 15, 15), 3, cv::LINE_AA);
                cv::putText(warpOverlay, "X", xEnd + cv::Point(4, -4), cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(220, 245, 255), 1, cv::LINE_AA);
                cv::putText(warpOverlay, "Y", yEnd + cv::Point(4, -4), cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(15, 15, 15), 3, cv::LINE_AA);
                cv::putText(warpOverlay, "Y", yEnd + cv::Point(4, -4), cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(255, 230, 210), 1, cv::LINE_AA);
            }

            for (const auto &blob : selectedBig) {
                const cv::Point center(cvRound(blob.center.x), cvRound(blob.center.y));
                const int radius = std::max(5, static_cast<int>(std::round(blob.radius * 1.5)));
                cv::circle(warpOverlay, center, radius, cv::Scalar(40, 90, 240), 3, cv::LINE_AA);
            }

            addDebugImage("Numbered grid", warpOverlay);
        }

        {
            cv::Mat detectionOverlay = originalColor.clone();
            drawQuadOverlay(detectionOverlay, expandedQuad, cv::Scalar(70, 100, 255), 1);
            drawQuadOverlay(detectionOverlay, quad.corners, cv::Scalar(0, 210, 255), 2);
            for (size_t i = 0; i < result.imagePoints.size(); ++i) {
                const cv::Point center(cvRound(result.imagePoints[i].x), cvRound(result.imagePoints[i].y));
                cv::circle(detectionOverlay, center, 4, cv::Scalar(80, 230, 150), -1, cv::LINE_AA);
                cv::circle(detectionOverlay, center, 7, cv::Scalar(80, 230, 150), 2, cv::LINE_AA);
                if (i % 8 == 0) {
                    const std::string idxText = std::to_string(i + 1);
                    cv::putText(detectionOverlay, idxText, center + cv::Point(6, -6), cv::FONT_HERSHEY_SIMPLEX, 0.4,
                                cv::Scalar(30, 40, 60), 2, cv::LINE_AA);
                    cv::putText(detectionOverlay, idxText, center + cv::Point(6, -6), cv::FONT_HERSHEY_SIMPLEX, 0.4,
                                cv::Scalar(250, 250, 250), 1, cv::LINE_AA);
                }
            }
            for (const auto &pt : result.bigCirclePoints) {
                const cv::Point center(cvRound(pt.x), cvRound(pt.y));
                cv::circle(detectionOverlay, center, 9, cv::Scalar(40, 90, 240), 2, cv::LINE_AA);
            }
            addDebugImage("Detection overlay", detectionOverlay);
        }

        stage = "build_object_points";
        const auto objectPoints = spec.buildObjectPoints(static_cast<int>(result.imagePoints.size()));
        result.objectPoints = objectPoints;
        result.success = true;
        result.message = "Detection succeeded";
        result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
        return result;
    } catch (const cv::Exception &ex) {
        const QString stageStr = QString::fromStdString(stage);
        const QString typeStr = gray.empty() ? QStringLiteral("empty") : matTypeToString(gray.type());
        const int cols = gray.empty() ? 0 : gray.cols;
        const int rows = gray.empty() ? 0 : gray.rows;
        Logger::error(QStringLiteral("%1: OpenCV exception @%2 -> %3 | input type=%4 | size=%5x%6")
                          .arg(QString::fromStdString(name))
                          .arg(stageStr)
                          .arg(QString::fromUtf8(ex.what()))
                          .arg(typeStr)
                          .arg(cols)
                          .arg(rows));
        result.success = false;
        result.message = std::string("native_detection_exception[") + stage + "]: " + ex.what();
        result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
        return result;
    } catch (const std::exception &ex) {
        const QString stageStr = QString::fromStdString(stage);
        const QString typeStr = gray.empty() ? QStringLiteral("empty") : matTypeToString(gray.type());
        const int cols = gray.empty() ? 0 : gray.cols;
        const int rows = gray.empty() ? 0 : gray.rows;
    Logger::error(QStringLiteral("%1: std exception @%2 -> %3 | input type=%4 | size=%5x%6")
                          .arg(QString::fromStdString(name))
                          .arg(stageStr)
                          .arg(QString::fromUtf8(ex.what()))
                          .arg(typeStr)
                          .arg(cols)
                          .arg(rows));
        result.success = false;
        result.message = std::string("native_detection_exception[") + stage + "]: " + ex.what();
        result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
        return result;
    } catch (...) {
        const QString stageStr = QString::fromStdString(stage);
        const QString typeStr = gray.empty() ? QStringLiteral("empty") : matTypeToString(gray.type());
        const int cols = gray.empty() ? 0 : gray.cols;
        const int rows = gray.empty() ? 0 : gray.rows;
    Logger::error(QStringLiteral("%1: unknown exception @%2 | input type=%3 | size=%4x%5")
                          .arg(QString::fromStdString(name))
                          .arg(stageStr)
                          .arg(typeStr)
                          .arg(cols)
                          .arg(rows));
        result.success = false;
        result.message = std::string("native_detection_exception[") + stage + "]: unknown";
        result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
        return result;
    }
}

} // namespace mycalib
