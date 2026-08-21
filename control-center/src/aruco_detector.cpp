#include "logistics/control_center/aruco_detector.hpp"

#include <QHash>
#include <QPoint>
#include <QVector>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>

namespace logistics::control_center {
namespace {

constexpr std::array<quint16, 50> kDictionary4x4_50{
    0xB532, 0x0F9A, 0x332D, 0x9946, 0x549E, 0x79CD, 0x9E2E, 0xC4F2, 0xFEDA, 0xCF56, 0xF991, 0x11A7, 0x0EB7,
    0x2A0F, 0x24B1, 0x263E, 0x4665, 0x6600, 0x6C5E, 0x76AF, 0x868B, 0xB02B, 0xCCD5, 0xDD82, 0xFE47, 0x9471,
    0xACE4, 0xA554, 0x2123, 0x346F, 0x4415, 0x57B2, 0x9ECF, 0xF0CB, 0x08AE, 0x0929, 0x1875, 0x04FF, 0x0DF6,
    0x1C5A, 0x1718, 0x2A28, 0x328C, 0x38B2, 0x24E8, 0x2EEB, 0x2D3F, 0x4B64, 0x502E, 0x5013,
};

struct Candidate {
    std::array<QPointF, 4> corners;
    double area{ 0.0 };
};

quint16 RotateCodeClockwise(const quint16 code) {
    quint16 rotated = 0;
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            const auto source_bit = static_cast<quint16>((code >> (15 - (row * 4 + column))) & 1U);
            const int target_row = column;
            const int target_column = 3 - row;
            rotated |= static_cast<quint16>(source_bit << (15 - (target_row * 4 + target_column)));
        }
    }
    return rotated;
}

int DecodeMarker(quint16 code) {
    int best_id = -1;
    int best_distance = 2;
    for (int rotation = 0; rotation < 4; ++rotation) {
        for (std::size_t id = 0; id < kDictionary4x4_50.size(); ++id) {
            const int distance = std::popcount(static_cast<unsigned int>(code ^ kDictionary4x4_50[id]));
            if (distance < best_distance) {
                best_distance = distance;
                best_id = static_cast<int>(id);
            }
        }
        code = RotateCodeClockwise(code);
    }
    return best_distance <= 1 ? best_id : -1;
}

QByteArray AdaptiveThreshold(const QImage& grayscale) {
    const int width = grayscale.width();
    const int height = grayscale.height();
    QVector<quint64> integral((width + 1) * (height + 1));
    for (int y = 0; y < height; ++y) {
        quint64 row_sum = 0;
        const auto* scan_line = grayscale.constScanLine(y);
        for (int x = 0; x < width; ++x) {
            row_sum += scan_line[x];
            integral[(y + 1) * (width + 1) + x + 1] = integral[y * (width + 1) + x + 1] + row_sum;
        }
    }

    QByteArray black(width * height, Qt::Uninitialized);
    const int radius = std::clamp(std::min(width, height) / 40, 5, 20);
    for (int y = 0; y < height; ++y) {
        const int top = std::max(0, y - radius);
        const int bottom = std::min(height - 1, y + radius);
        const auto* scan_line = grayscale.constScanLine(y);
        for (int x = 0; x < width; ++x) {
            const int left = std::max(0, x - radius);
            const int right = std::min(width - 1, x + radius);
            const auto sum = integral[(bottom + 1) * (width + 1) + right + 1] -
                             integral[top * (width + 1) + right + 1] - integral[(bottom + 1) * (width + 1) + left] +
                             integral[top * (width + 1) + left];
            const int count = (right - left + 1) * (bottom - top + 1);
            black[y * width + x] = scan_line[x] < 96 || scan_line[x] + 7 < static_cast<int>(sum / count) ? 1 : 0;
        }
    }
    return black;
}

double QuadrilateralArea(const std::array<QPointF, 4>& corners) {
    double area = 0.0;
    for (int index = 0; index < 4; ++index) {
        const auto& current = corners[index];
        const auto& next = corners[(index + 1) % 4];
        area += current.x() * next.y() - next.x() * current.y();
    }
    return std::abs(area) * 0.5;
}

QList<Candidate> FindCandidates(const QByteArray& black, const int width, const int height) {
    QByteArray visited(width * height, 0);
    QVector<int> queue;
    QList<Candidate> candidates;
    constexpr std::array<QPoint, 4> neighbors{ QPoint(-1, 0), QPoint(1, 0), QPoint(0, -1), QPoint(0, 1) };

    for (int start = 0; start < width * height; ++start) {
        if (!black[start] || visited[start]) {
            continue;
        }
        queue.clear();
        queue.append(start);
        visited[start] = 1;
        int cursor = 0;
        int min_x = width;
        int max_x = 0;
        int min_y = height;
        int max_y = 0;
        int pixel_count = 0;
        std::array<QPoint, 4> corners{};
        std::array<int, 4> scores{ std::numeric_limits<int>::max(), std::numeric_limits<int>::min(),
                                   std::numeric_limits<int>::min(), std::numeric_limits<int>::max() };

        while (cursor < queue.size()) {
            const int position = queue[cursor++];
            const int x = position % width;
            const int y = position / width;
            ++pixel_count;
            min_x = std::min(min_x, x);
            max_x = std::max(max_x, x);
            min_y = std::min(min_y, y);
            max_y = std::max(max_y, y);
            const int sum = x + y;
            const int difference = x - y;
            if (sum < scores[0]) {
                scores[0] = sum;
                corners[0] = QPoint(x, y);
            }
            if (difference > scores[1]) {
                scores[1] = difference;
                corners[1] = QPoint(x, y);
            }
            if (sum > scores[2]) {
                scores[2] = sum;
                corners[2] = QPoint(x, y);
            }
            if (difference < scores[3]) {
                scores[3] = difference;
                corners[3] = QPoint(x, y);
            }
            for (const auto& neighbor : neighbors) {
                const int next_x = x + neighbor.x();
                const int next_y = y + neighbor.y();
                if (next_x < 0 || next_x >= width || next_y < 0 || next_y >= height) {
                    continue;
                }
                const int next = next_y * width + next_x;
                if (black[next] && !visited[next]) {
                    visited[next] = 1;
                    queue.append(next);
                }
            }
        }

        const int candidate_width = max_x - min_x + 1;
        const int candidate_height = max_y - min_y + 1;
        if (pixel_count < 100 || candidate_width < 18 || candidate_height < 18 || min_x == 0 || min_y == 0 ||
            max_x == width - 1 || max_y == height - 1 || candidate_width > candidate_height * 3 ||
            candidate_height > candidate_width * 3) {
            continue;
        }
        Candidate candidate{ .corners = { corners[0], corners[1], corners[2], corners[3] } };
        candidate.area = QuadrilateralArea(candidate.corners);
        if (candidate.area >= 200.0) {
            candidates.append(candidate);
        }
    }
    return candidates;
}

bool SolveHomography(const std::array<QPointF, 4>& corners, std::array<double, 8>& result) {
    std::array<std::array<double, 9>, 8> matrix{};
    constexpr std::array<QPointF, 4> source{ QPointF(0, 0), QPointF(1, 0), QPointF(1, 1), QPointF(0, 1) };
    for (int index = 0; index < 4; ++index) {
        const double u = source[index].x();
        const double v = source[index].y();
        const double x = corners[index].x();
        const double y = corners[index].y();
        matrix[index * 2] = { u, v, 1, 0, 0, 0, -u * x, -v * x, x };
        matrix[index * 2 + 1] = { 0, 0, 0, u, v, 1, -u * y, -v * y, y };
    }
    for (int column = 0; column < 8; ++column) {
        int pivot = column;
        for (int row = column + 1; row < 8; ++row) {
            if (std::abs(matrix[row][column]) > std::abs(matrix[pivot][column])) {
                pivot = row;
            }
        }
        if (std::abs(matrix[pivot][column]) < 1e-9) {
            return false;
        }
        std::swap(matrix[pivot], matrix[column]);
        const double divisor = matrix[column][column];
        for (int entry = column; entry < 9; ++entry) {
            matrix[column][entry] /= divisor;
        }
        for (int row = 0; row < 8; ++row) {
            if (row == column) {
                continue;
            }
            const double factor = matrix[row][column];
            for (int entry = column; entry < 9; ++entry) {
                matrix[row][entry] -= factor * matrix[column][entry];
            }
        }
    }
    for (int index = 0; index < 8; ++index) {
        result[index] = matrix[index][8];
    }
    return true;
}

QPointF MapPoint(const std::array<double, 8>& homography, const double u, const double v) {
    const double divisor = homography[6] * u + homography[7] * v + 1.0;
    return { (homography[0] * u + homography[1] * v + homography[2]) / divisor,
             (homography[3] * u + homography[4] * v + homography[5]) / divisor };
}

int DecodeCandidate(const Candidate& candidate, const QByteArray& black, const int width, const int height) {
    std::array<double, 8> homography{};
    if (!SolveHomography(candidate.corners, homography)) {
        return -1;
    }
    std::array<bool, 36> cells{};
    for (int row = 0; row < 6; ++row) {
        for (int column = 0; column < 6; ++column) {
            int black_samples = 0;
            for (const double offset_y : { -0.16, 0.0, 0.16 }) {
                for (const double offset_x : { -0.16, 0.0, 0.16 }) {
                    const auto point =
                        MapPoint(homography, (column + 0.5 + offset_x) / 6.0, (row + 0.5 + offset_y) / 6.0);
                    const int x = qRound(point.x());
                    const int y = qRound(point.y());
                    if (x >= 0 && x < width && y >= 0 && y < height && black[y * width + x]) {
                        ++black_samples;
                    }
                }
            }
            cells[row * 6 + column] = black_samples >= 5;
        }
    }
    int black_border_cells = 0;
    for (int row = 0; row < 6; ++row) {
        for (int column = 0; column < 6; ++column) {
            if ((row == 0 || row == 5 || column == 0 || column == 5) && cells[row * 6 + column]) {
                ++black_border_cells;
            }
        }
    }
    if (black_border_cells < 18) {
        return -1;
    }
    quint16 code = 0;
    for (int row = 1; row <= 4; ++row) {
        for (int column = 1; column <= 4; ++column) {
            code = static_cast<quint16>((code << 1) | (cells[row * 6 + column] ? 0 : 1));
        }
    }
    return DecodeMarker(code);
}

}  // namespace

QList<ArucoMarkerRegion> DetectAruco4x4Markers(const QImage& image, const double danger_margin) {
    if (image.isNull()) {
        return {};
    }
    auto grayscale = image.convertToFormat(QImage::Format_Grayscale8);
    constexpr int kMaximumDetectionWidth = 960;
    if (grayscale.width() > kMaximumDetectionWidth) {
        grayscale = grayscale.scaledToWidth(kMaximumDetectionWidth, Qt::FastTransformation);
    }
    const auto black = AdaptiveThreshold(grayscale);
    QHash<int, ArucoMarkerRegion> detected;
    for (const auto& candidate : FindCandidates(black, grayscale.width(), grayscale.height())) {
        const int marker_id = DecodeCandidate(candidate, black, grayscale.width(), grayscale.height());
        if (marker_id < 0) {
            continue;
        }
        QPolygonF normalized_corners;
        for (const auto& corner : candidate.corners) {
            normalized_corners.append(QPointF(corner.x() / grayscale.width(), corner.y() / grayscale.height()));
        }
        auto danger_rect = normalized_corners.boundingRect();
        danger_rect.adjust(-danger_margin, -danger_margin, danger_margin, danger_margin);
        danger_rect = danger_rect.intersected(QRectF(0, 0, 1, 1));
        const ArucoMarkerRegion marker{ .marker_id = marker_id,
                                        .corners = normalized_corners,
                                        .danger_rect = danger_rect };
        if (!detected.contains(marker_id) ||
            detected[marker_id].danger_rect.width() * detected[marker_id].danger_rect.height() <
                danger_rect.width() * danger_rect.height()) {
            detected.insert(marker_id, marker);
        }
    }
    return detected.values();
}

ArucoDetectorWorker::ArucoDetectorWorker(const double danger_margin, QObject* parent)
    : QObject(parent), danger_margin_(std::clamp(danger_margin, 0.0, 0.5)) {}

void ArucoDetectorWorker::detect(const QImage& image) {
    emit markersDetected(DetectAruco4x4Markers(image, danger_margin_));
}

}  // namespace logistics::control_center
