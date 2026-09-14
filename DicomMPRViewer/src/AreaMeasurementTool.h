#pragma once

#include "MeasurementTool.h"

class AreaMeasurementTool : public MeasurementTool
{
public:
    QString name() const override { return "Area"; }
    int requiredPointCount() const override { return 3; }
    std::optional<Measurement> addPoint(const std::array<double, 3>& point,
                                        MeasurementViewOrientation view,
                                        int sliceIndex,
                                        QWidget* parent) override;
    bool canFinish() const override;
    std::optional<Measurement> finish(QWidget* parent) override;
};
