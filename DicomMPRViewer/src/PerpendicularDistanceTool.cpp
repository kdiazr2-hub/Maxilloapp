#include "PerpendicularDistanceTool.h"

std::optional<Measurement> PerpendicularDistanceTool::addPoint(
    const std::array<double, 3>& point,
    MeasurementViewOrientation view,
    int sliceIndex,
    QWidget*)
{
    appendPointOnSamePlane(point, view, sliceIndex);
    if (pendingPointCount() < requiredPointCount()) return std::nullopt;

    Measurement measurement = makeMeasurement(MeasurementType::PerpendicularDistance);
    const auto projection = perpendicularProjection(m_points[0], m_points[1], m_points[2]);
    measurement.pointsPhysical.push_back(projection);
    measurement.value = distanceMm(m_points[2], projection);
    reset();
    return measurement;
}
