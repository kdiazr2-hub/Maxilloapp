#include "AreaMeasurementTool.h"

std::optional<Measurement> AreaMeasurementTool::addPoint(
    const std::array<double, 3>& point,
    MeasurementViewOrientation view,
    int sliceIndex,
    QWidget*)
{
    if (!m_points.empty() && (m_view != view || m_sliceIndex != sliceIndex)) {
        reset();
    }

    appendPointOnSamePlane(point, view, sliceIndex);
    return std::nullopt;
}

bool AreaMeasurementTool::canFinish() const
{
    return m_points.size() >= 3;
}

std::optional<Measurement> AreaMeasurementTool::finish(QWidget*)
{
    if (!canFinish()) return std::nullopt;

    Measurement measurement = makeMeasurement(MeasurementType::Area);
    measurement.value = polygonAreaMm2(measurement.pointsPhysical,
                                       measurement.viewOrientation);
    measurement.areaMm2 = measurement.value;
    reset();
    return measurement;
}
