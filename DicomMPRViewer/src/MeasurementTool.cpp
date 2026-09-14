#include "MeasurementTool.h"

#include <QDateTime>

void MeasurementTool::reset()
{
    m_points.clear();
}

void MeasurementTool::appendPointOnSamePlane(const std::array<double, 3>& point,
                                             MeasurementViewOrientation view,
                                             int sliceIndex)
{
    if (m_points.empty() || m_view != view || m_sliceIndex != sliceIndex) {
        m_points.clear();
        m_view = view;
        m_sliceIndex = sliceIndex;
    }
    m_points.push_back(point);
}

Measurement MeasurementTool::makeMeasurement(MeasurementType type) const
{
    Measurement measurement;
    measurement.type = type;
    measurement.viewOrientation = m_view;
    measurement.sliceIndex = m_sliceIndex;
    measurement.pointsPhysical = m_points;
    measurement.visible = true;
    measurement.createdAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toStdString();
    return measurement;
}
