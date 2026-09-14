#pragma once

#include <array>
#include <string>
#include <vector>

enum class MeasurementType
{
    Distance,
    Angle,
    Annotation,
    Area,
    ROIStatistics,
    PerpendicularDistance
};

enum class MeasurementROIShape
{
    None,
    Circle,
    Rectangle
};

enum class MeasurementViewOrientation
{
    Axial,
    Coronal,
    Sagittal
};

struct Measurement
{
    std::string id;
    MeasurementType type = MeasurementType::Distance;
    MeasurementViewOrientation viewOrientation = MeasurementViewOrientation::Axial;
    MeasurementROIShape roiShape = MeasurementROIShape::None;
    int sliceIndex = 0;
    std::vector<std::array<double, 3>> pointsPhysical;
    double value = 0.0;
    double areaMm2 = 0.0;
    double huMean = 0.0;
    double huMin = 0.0;
    double huMax = 0.0;
    double huStdDev = 0.0;
    int sampleCount = 0;
    std::string text;
    bool visible = true;
    std::string createdAt;
};

const char* measurementTypeToString(MeasurementType type);
const char* measurementViewToString(MeasurementViewOrientation view);
const char* measurementROIShapeToString(MeasurementROIShape shape);
MeasurementType measurementTypeFromString(const std::string& value);
MeasurementViewOrientation measurementViewFromString(const std::string& value);
MeasurementROIShape measurementROIShapeFromString(const std::string& value);

double distanceMm(const std::array<double, 3>& a,
                  const std::array<double, 3>& b);
double angleDegrees(const std::array<double, 3>& p1,
                    const std::array<double, 3>& vertex,
                    const std::array<double, 3>& p3);
double polygonAreaMm2(const std::vector<std::array<double, 3>>& points,
                      MeasurementViewOrientation view);
std::array<double, 3> perpendicularProjection(const std::array<double, 3>& baseA,
                                              const std::array<double, 3>& baseB,
                                              const std::array<double, 3>& point);
