#pragma once

#include "MeasurementTool.h"

class PerpendicularDistanceTool : public MeasurementTool
{
public:
    QString name() const override { return "Perpendicular"; }
    int requiredPointCount() const override { return 3; }
    std::optional<Measurement> addPoint(const std::array<double, 3>& point,
                                        MeasurementViewOrientation view,
                                        int sliceIndex,
                                        QWidget* parent) override;
};
