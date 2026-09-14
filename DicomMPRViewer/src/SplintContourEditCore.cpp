#include "SplintContourEditCore.h"

#include <algorithm>
#include <cmath>

namespace SplintContourEditCore
{
namespace
{
constexpr double kPi = 3.14159265358979323846;

double segmentLength(const SplintPointUV& a, const SplintPointUV& b)
{
    return std::hypot(b[0] - a[0], b[1] - a[1]);
}
} // namespace

double Perimeter(const SplintContourUV& contour)
{
    double total = 0.0;
    for (size_t i = 0; i < contour.size(); ++i)
        total += segmentLength(contour[i], contour[(i + 1) % contour.size()]);
    return total;
}

double ClampInfluence(double percent)
{
    return std::clamp(percent, kMinInfluencePercent, kMaxInfluencePercent);
}

double InfluenceAfterDrag(double percent, double deltaYPixels)
{
    return ClampInfluence(percent - deltaYPixels * kInfluencePercentPerPixel);
}

SplintContourUV Densify(const SplintContourUV& contour, double maxSegmentMm)
{
    if (contour.size() < 2 || maxSegmentMm <= 0.0)
        return contour;
    SplintContourUV out;
    for (size_t i = 0; i < contour.size(); ++i) {
        const SplintPointUV& a = contour[i];
        const SplintPointUV& b = contour[(i + 1) % contour.size()];
        out.push_back(a);
        const int pieces = static_cast<int>(std::ceil(segmentLength(a, b) / maxSegmentMm));
        for (int k = 1; k < pieces; ++k) {
            const double t = static_cast<double>(k) / pieces;
            out.push_back({a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t});
        }
    }
    return out;
}

std::optional<Handle> FindHandle(const std::vector<SplintContourUV>& contours, const SplintPointUV& point,
                                 double maxDistanceMm)
{
    std::optional<Handle> best;
    for (size_t c = 0; c < contours.size(); ++c) {
        for (size_t v = 0; v < contours[c].size(); ++v) {
            const double d = segmentLength(point, contours[c][v]);
            if (d <= maxDistanceMm && (!best || d < best->distanceMm))
                best = Handle{static_cast<int>(c), static_cast<int>(v), d};
        }
    }
    return best;
}

double InfluenceWeight(double arcDistanceMm, double radiusMm)
{
    if (radiusMm <= 0.0)
        return arcDistanceMm <= 0.0 ? 1.0 : 0.0;
    if (arcDistanceMm >= radiusMm)
        return 0.0;
    return 0.5 * (1.0 + std::cos(kPi * arcDistanceMm / radiusMm));
}

SplintContourUV Drag(const SplintContourUV& contour, int vertex, const SplintPointUV& delta, double influencePercent)
{
    SplintContourUV out = contour;
    const int n = static_cast<int>(contour.size());
    if (vertex < 0 || vertex >= n)
        return out;

    const double perimeter = Perimeter(contour);
    const double radius = ClampInfluence(influencePercent) / 100.0 * perimeter;
    std::vector<double> arc(static_cast<size_t>(n), 0.0);
    for (int i = 1; i < n; ++i)
        arc[static_cast<size_t>(i)] = arc[static_cast<size_t>(i - 1)] +
                                      segmentLength(contour[static_cast<size_t>(i - 1)], contour[static_cast<size_t>(i)]);

    for (int i = 0; i < n; ++i) {
        const double forward = std::abs(arc[static_cast<size_t>(i)] - arc[static_cast<size_t>(vertex)]);
        const double distance = std::min(forward, perimeter - forward);
        const double w = i == vertex ? 1.0 : InfluenceWeight(distance, radius);
        out[static_cast<size_t>(i)][0] += w * delta[0];
        out[static_cast<size_t>(i)][1] += w * delta[1];
    }
    return out;
}
} // namespace SplintContourEditCore
