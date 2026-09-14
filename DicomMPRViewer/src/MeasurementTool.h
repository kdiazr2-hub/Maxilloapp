#pragma once

#include <array>
#include <optional>
#include <vector>

#include <QString>

#include "Measurement.h"

class QWidget;
class vtkImageData;

class MeasurementTool
{
public:
    virtual ~MeasurementTool() = default;

    virtual QString name() const = 0;
    virtual int requiredPointCount() const = 0;
    virtual void setImageData(vtkImageData*) {}
    virtual std::optional<Measurement> addPoint(const std::array<double, 3>& point,
                                                MeasurementViewOrientation view,
                                                int sliceIndex,
                                                QWidget* parent) = 0;
    virtual bool canFinish() const { return false; }
    virtual std::optional<Measurement> finish(QWidget*) { return std::nullopt; }

    void reset();
    int pendingPointCount() const { return static_cast<int>(m_points.size()); }
    const std::vector<std::array<double, 3>>& pendingPoints() const { return m_points; }
    MeasurementViewOrientation pendingView() const { return m_view; }
    int pendingSliceIndex() const { return m_sliceIndex; }

protected:
    void appendPointOnSamePlane(const std::array<double, 3>& point,
                                MeasurementViewOrientation view,
                                int sliceIndex);
    Measurement makeMeasurement(MeasurementType type) const;

    std::vector<std::array<double, 3>> m_points;
    MeasurementViewOrientation m_view = MeasurementViewOrientation::Axial;
    int m_sliceIndex = 0;
};
