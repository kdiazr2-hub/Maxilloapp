#pragma once

#include "MeasurementTool.h"

class vtkImageData;

class ROIStatisticsTool : public MeasurementTool
{
public:
    explicit ROIStatisticsTool(MeasurementROIShape shape);

    QString name() const override;
    int requiredPointCount() const override { return 2; }
    void setImageData(vtkImageData* imageData) override { m_imageData = imageData; }
    std::optional<Measurement> addPoint(const std::array<double, 3>& point,
                                        MeasurementViewOrientation view,
                                        int sliceIndex,
                                        QWidget* parent) override;

private:
    Measurement buildMeasurement() const;
    void calculateStatistics(Measurement& measurement) const;

    MeasurementROIShape m_shape = MeasurementROIShape::Rectangle;
    vtkImageData* m_imageData = nullptr;
};
