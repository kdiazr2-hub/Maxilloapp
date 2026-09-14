#include "DistanceMeasurementTool.h"

std::optional<Measurement> DistanceMeasurementTool::addPoint(
    const std::array<double, 3>& point,
    MeasurementViewOrientation view,
    int sliceIndex,
    QWidget*)
{
    appendPointOnSamePlane(point, view, sliceIndex);
    if (pendingPointCount() < requiredPointCount()) return std::nullopt;

    Measurement measurement = makeMeasurement(MeasurementType::Distance);
    measurement.value = distanceMm(m_points[0], m_points[1]);
    reset();
    return measurement;
}
