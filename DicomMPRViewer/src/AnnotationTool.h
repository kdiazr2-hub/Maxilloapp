#pragma once

#include "MeasurementTool.h"

class AnnotationTool : public MeasurementTool
{
public:
    QString name() const override { return "Anotacion"; }
    int requiredPointCount() const override { return 1; }
    std::optional<Measurement> addPoint(const std::array<double, 3>& point,
                                        MeasurementViewOrientation view,
                                        int sliceIndex,
                                        QWidget* parent) override;
};
