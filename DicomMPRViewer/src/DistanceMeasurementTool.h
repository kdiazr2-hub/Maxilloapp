#pragma once

#include "MeasurementTool.h"

class DistanceMeasurementTool : public MeasurementTool
{
public:
    QString name() const override { return "Distancia"; }
    int requiredPointCount() const override { return 2; }
    std::optional<Measurement> addPoint(const std::array<double, 3>& point,
                                        MeasurementViewOrientation view,
                                        int sliceIndex,
                                        QWidget* parent) override;
};
