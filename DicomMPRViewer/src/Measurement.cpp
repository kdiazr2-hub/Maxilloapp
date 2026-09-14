#include "Measurement.h"

#include <algorithm>
#include <cmath>

const char* measurementTypeToString(MeasurementType type)
{
    switch (type) {
        case MeasurementType::Distance:              return "distance";
        case MeasurementType::Angle:                 return "angle";
        case MeasurementType::Annotation:            return "annotation";
        case MeasurementType::Area:                  return "area";
        case MeasurementType::ROIStatistics:         return "roi_statistics";
        case MeasurementType::PerpendicularDistance: return "perpendicular_distance";
    }
    return "distance";
}

const char* measurementViewToString(MeasurementViewOrientation view)
{
    switch (view) {
        case MeasurementViewOrientation::Axial:    return "axial";
        case MeasurementViewOrientation::Coronal:  return "coronal";
        case MeasurementViewOrientation::Sagittal: return "sagittal";
    }
    return "axial";
}

const char* measurementROIShapeToString(MeasurementROIShape shape)
{
    switch (shape) {
        case MeasurementROIShape::None:      return "none";
        case MeasurementROIShape::Circle:    return "circle";
        case MeasurementROIShape::Rectangle: return "rectangle";
    }
    return "none";
}

MeasurementType measurementTypeFromString(const std::string& value)
{
    if (value == "angle") return MeasurementType::Angle;
    if (value == "annotation") return MeasurementType::Annotation;
    if (value == "area") return MeasurementType::Area;
    if (value == "roi_statistics") return MeasurementType::ROIStatistics;
    if (value == "perpendicular_distance") return MeasurementType::PerpendicularDistance;
    return MeasurementType::Distance;
}

MeasurementViewOrientation measurementViewFromString(const std::string& value)
{
    if (value == "coronal") return MeasurementViewOrientation::Coronal;
    if (value == "sagittal") return MeasurementViewOrientation::Sagittal;
    return MeasurementViewOrientation::Axial;
}

MeasurementROIShape measurementROIShapeFromString(const std::string& value)
{
    if (value == "circle") return MeasurementROIShape::Circle;
    if (value == "rectangle") return MeasurementROIShape::Rectangle;
    return MeasurementROIShape::None;
}

double distanceMm(const std::array<double, 3>& a,
                  const std::array<double, 3>& b)
{
    const double dx = a[0] - b[0];
    const double dy = a[1] - b[1];
    const double dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

double angleDegrees(const std::array<double, 3>& p1,
                    const std::array<double, 3>& vertex,
                    const std::array<double, 3>& p3)
{
    const std::array<double, 3> v1 = {
        p1[0] - vertex[0],
        p1[1] - vertex[1],
        p1[2] - vertex[2]
    };
    const std::array<double, 3> v2 = {
        p3[0] - vertex[0],
        p3[1] - vertex[1],
        p3[2] - vertex[2]
    };

    const double dot = v1[0] * v2[0] + v1[1] * v2[1] + v1[2] * v2[2];
    const double len1 = std::sqrt(v1[0] * v1[0] + v1[1] * v1[1] + v1[2] * v1[2]);
    const double len2 = std::sqrt(v2[0] * v2[0] + v2[1] * v2[1] + v2[2] * v2[2]);
    if (len1 <= 1e-9 || len2 <= 1e-9) return 0.0;

    const double cosTheta = std::clamp(dot / (len1 * len2), -1.0, 1.0);
    return std::acos(cosTheta) * 180.0 / 3.14159265358979323846;
}

static void planeAxesForView(MeasurementViewOrientation view,
                             int& horizontalAxis,
                             int& verticalAxis)
{
    switch (view) {
        case MeasurementViewOrientation::Axial:
            horizontalAxis = 0;
            verticalAxis = 1;
            break;
        case MeasurementViewOrientation::Coronal:
            horizontalAxis = 0;
            verticalAxis = 2;
            break;
        case MeasurementViewOrientation::Sagittal:
            horizontalAxis = 1;
            verticalAxis = 2;
            break;
    }
}

double polygonAreaMm2(const std::vector<std::array<double, 3>>& points,
                      MeasurementViewOrientation view)
{
    if (points.size() < 3) return 0.0;

    int h = 0;
    int v = 1;
    planeAxesForView(view, h, v);

    double sum = 0.0;
    for (size_t i = 0; i < points.size(); ++i) {
        const auto& a = points[i];
        const auto& b = points[(i + 1) % points.size()];
        sum += a[h] * b[v] - b[h] * a[v];
    }
    return std::abs(sum) * 0.5;
}

std::array<double, 3> perpendicularProjection(const std::array<double, 3>& baseA,
                                              const std::array<double, 3>& baseB,
                                              const std::array<double, 3>& point)
{
    const std::array<double, 3> ab = {
        baseB[0] - baseA[0],
        baseB[1] - baseA[1],
        baseB[2] - baseA[2]
    };
    const std::array<double, 3> ap = {
        point[0] - baseA[0],
        point[1] - baseA[1],
        point[2] - baseA[2]
    };
    const double denom = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
    if (denom <= 1e-9) return baseA;

    const double t = (ap[0] * ab[0] + ap[1] * ab[1] + ap[2] * ab[2]) / denom;
    return {
        baseA[0] + t * ab[0],
        baseA[1] + t * ab[1],
        baseA[2] + t * ab[2]
    };
}
