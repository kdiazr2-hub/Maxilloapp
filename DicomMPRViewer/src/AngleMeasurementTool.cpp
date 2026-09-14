#include "AngleMeasurementTool.h"

std::optional<Measurement> AngleMeasurementTool::addPoint(
    const std::array<double, 3>& point,
    MeasurementViewOrientation view,
    int sliceIndex,
    QWidget*)
{
    appendPointOnSamePlane(point, view, sliceIndex);
    if (pendingPointCount() < requiredPointCount()) return std::nullopt;

    Measurement measurement = makeMeasurement(MeasurementType::Angle);
    measurement.value = angleDegrees(m_points[0], m_points[1], m_points[2]);
    reset();
    return measurement;
}
