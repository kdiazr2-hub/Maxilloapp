#include "ROIStatisticsTool.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>

#include <vtkImageData.h>

static void planeAxesForView(MeasurementViewOrientation view,
                             int& horizontalAxis,
                             int& verticalAxis,
                             int& normalAxis)
{
    switch (view) {
        case MeasurementViewOrientation::Axial:
            horizontalAxis = 0; verticalAxis = 1; normalAxis = 2; break;
        case MeasurementViewOrientation::Coronal:
            horizontalAxis = 0; verticalAxis = 2; normalAxis = 1; break;
        case MeasurementViewOrientation::Sagittal:
            horizontalAxis = 1; verticalAxis = 2; normalAxis = 0; break;
    }
}

static int clampIndex(int value, const int extent[6], int axis)
{
    return std::clamp(value, extent[axis * 2], extent[axis * 2 + 1]);
}

ROIStatisticsTool::ROIStatisticsTool(MeasurementROIShape shape)
    : m_shape(shape)
{
}

QString ROIStatisticsTool::name() const
{
    return m_shape == MeasurementROIShape::Circle ? "ROI circular" : "ROI rectangular";
}

std::optional<Measurement> ROIStatisticsTool::addPoint(
    const std::array<double, 3>& point,
    MeasurementViewOrientation view,
    int sliceIndex,
    QWidget*)
{
    appendPointOnSamePlane(point, view, sliceIndex);
    if (pendingPointCount() < requiredPointCount()) return std::nullopt;

    Measurement measurement = buildMeasurement();
    reset();
    return measurement;
}

Measurement ROIStatisticsTool::buildMeasurement() const
{
    Measurement measurement = makeMeasurement(MeasurementType::ROIStatistics);
    measurement.roiShape = m_shape;

    int h = 0;
    int v = 1;
    int n = 2;
    planeAxesForView(measurement.viewOrientation, h, v, n);

    const auto& a = measurement.pointsPhysical[0];
    const auto& b = measurement.pointsPhysical[1];
    if (m_shape == MeasurementROIShape::Circle) {
        const double dx = b[h] - a[h];
        const double dy = b[v] - a[v];
        const double radius = std::sqrt(dx * dx + dy * dy);
        measurement.areaMm2 = 3.14159265358979323846 * radius * radius;
    } else {
        measurement.areaMm2 = std::abs((b[h] - a[h]) * (b[v] - a[v]));
    }
    measurement.value = measurement.areaMm2;

    calculateStatistics(measurement);

    std::ostringstream ss;
    ss.setf(std::ios::fixed);
    ss.precision(1);
    ss << "Area " << measurement.areaMm2 << " mm2";
    if (measurement.sampleCount > 0) {
        ss << " | HU mean " << measurement.huMean
           << " min " << measurement.huMin
           << " max " << measurement.huMax
           << " sd " << measurement.huStdDev;
    }
    measurement.text = ss.str();
    return measurement;
}

void ROIStatisticsTool::calculateStatistics(Measurement& measurement) const
{
    if (!m_imageData || measurement.pointsPhysical.size() < 2) return;

    int extent[6] = {};
    double origin[3] = {};
    double spacing[3] = {1.0, 1.0, 1.0};
    m_imageData->GetExtent(extent);
    m_imageData->GetOrigin(origin);
    m_imageData->GetSpacing(spacing);

    int h = 0;
    int v = 1;
    int n = 2;
    planeAxesForView(measurement.viewOrientation, h, v, n);

    const auto& a = measurement.pointsPhysical[0];
    const auto& b = measurement.pointsPhysical[1];
    const int normalIndex = clampIndex(measurement.sliceIndex, extent, n);

    auto toIndex = [&](int axis, double value) {
        const double step = spacing[axis] == 0.0 ? 1.0 : spacing[axis];
        return static_cast<int>(std::lround((value - origin[axis]) / step));
    };
    auto toPhysical = [&](int axis, int index) {
        return origin[axis] + static_cast<double>(index) * spacing[axis];
    };

    double minH = std::min(a[h], b[h]);
    double maxH = std::max(a[h], b[h]);
    double minV = std::min(a[v], b[v]);
    double maxV = std::max(a[v], b[v]);
    if (m_shape == MeasurementROIShape::Circle) {
        const double radius = std::sqrt((b[h] - a[h]) * (b[h] - a[h]) +
                                        (b[v] - a[v]) * (b[v] - a[v]));
        minH = a[h] - radius;
        maxH = a[h] + radius;
        minV = a[v] - radius;
        maxV = a[v] + radius;
    }

    const int h0 = clampIndex(toIndex(h, minH), extent, h);
    const int h1 = clampIndex(toIndex(h, maxH), extent, h);
    const int v0 = clampIndex(toIndex(v, minV), extent, v);
    const int v1 = clampIndex(toIndex(v, maxV), extent, v);

    int count = 0;
    double mean = 0.0;
    double m2 = 0.0;
    double minValue = std::numeric_limits<double>::max();
    double maxValue = std::numeric_limits<double>::lowest();

    for (int hi = std::min(h0, h1); hi <= std::max(h0, h1); ++hi) {
        for (int vi = std::min(v0, v1); vi <= std::max(v0, v1); ++vi) {
            const double ph = toPhysical(h, hi);
            const double pv = toPhysical(v, vi);

            bool inside = ph >= minH && ph <= maxH && pv >= minV && pv <= maxV;
            if (m_shape == MeasurementROIShape::Circle) {
                const double dx = ph - a[h];
                const double dy = pv - a[v];
                const double radius2 = (b[h] - a[h]) * (b[h] - a[h]) +
                                       (b[v] - a[v]) * (b[v] - a[v]);
                inside = (dx * dx + dy * dy) <= radius2;
            }
            if (!inside) continue;

            int idx[3] = {};
            idx[h] = hi;
            idx[v] = vi;
            idx[n] = normalIndex;
            const double hu = m_imageData->GetScalarComponentAsDouble(
                idx[0], idx[1], idx[2], 0);

            ++count;
            const double delta = hu - mean;
            mean += delta / static_cast<double>(count);
            const double delta2 = hu - mean;
            m2 += delta * delta2;
            minValue = std::min(minValue, hu);
            maxValue = std::max(maxValue, hu);
        }
    }

    measurement.sampleCount = count;
    if (count == 0) return;

    measurement.huMean = mean;
    measurement.huMin = minValue;
    measurement.huMax = maxValue;
    measurement.huStdDev = count > 1 ? std::sqrt(m2 / static_cast<double>(count - 1)) : 0.0;
}
