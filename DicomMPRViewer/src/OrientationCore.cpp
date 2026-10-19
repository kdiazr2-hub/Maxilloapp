#include "OrientationCore.h"

#include <algorithm>
#include <cmath>

#include <vtkTransform.h>

namespace {

// M_PI as used by the previous MainWindow implementation (float normal, double yaw).
constexpr double kPi = 3.14159265358979323846;

// Stand-in for QVector3D: the Frankfurt normal is computed in float.
struct Vec3 {
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
};

Vec3 fromPoint(const std::array<double, 3>& p)
{
    return {static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2])};
}

float length(const Vec3& v)
{
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

vtkSmartPointer<vtkMatrix4x4> identityMatrix()
{
    auto matrix = vtkSmartPointer<vtkMatrix4x4>::New();
    matrix->Identity();
    return matrix;
}

vtkSmartPointer<vtkMatrix4x4> matrixOf(vtkTransform* transform)
{
    auto matrix = vtkSmartPointer<vtkMatrix4x4>::New();
    matrix->DeepCopy(transform->GetMatrix());
    return matrix;
}

// Same cast back to float as QVector3D(out[0], out[1], out[2]) before the yaw vector.
Vec3 transformPoint(vtkMatrix4x4* matrix, const std::array<double, 3>& p)
{
    const double in[4] = {p[0], p[1], p[2], 1.0};
    double out[4] = {};
    matrix->MultiplyPoint(in, out);
    return {static_cast<float>(out[0]), static_cast<float>(out[1]), static_cast<float>(out[2])};
}

}

namespace OrientationCore {

FrankfurtAlignment AlignFrankfurt(const FrankfurtPoints& points, const std::array<double, 3>& pivot)
{
    FrankfurtAlignment result;
    result.level = identityMatrix();
    result.yaw = identityMatrix();

    // Newell on the convex quad: Porion D → Orbital D → Orbital I → Porion I.
    // Not the stored order (Porion D, Porion I, Orbital D, Orbital I).
    const Vec3 poly[4] = {
        fromPoint(points.porionRight),
        fromPoint(points.orbitalRight),
        fromPoint(points.orbitalLeft),
        fromPoint(points.porionLeft),
    };
    Vec3 normal;
    for (int i = 0; i < 4; ++i) {
        const Vec3& curr = poly[i];
        const Vec3& next = poly[(i + 1) % 4];
        normal.x += (curr.y - next.y) * (curr.z + next.z);
        normal.y += (curr.z - next.z) * (curr.x + next.x);
        normal.z += (curr.x - next.x) * (curr.y + next.y);
    }
    const double normalLengthSquared = static_cast<double>(normal.x) * normal.x
                                      + static_cast<double>(normal.y) * normal.y
                                      + static_cast<double>(normal.z) * normal.z;
    if (normalLengthSquared < 1e-12)
        return result;

    const float normalLength = length(normal);
    normal.x /= normalLength;
    normal.y /= normalLength;
    normal.z /= normalLength;

    const Vec3 target{0.f, 0.f, 1.f};
    const float cosA = normal.x * target.x + normal.y * target.y + normal.z * target.z;
    const float angle = static_cast<float>(std::acos(std::clamp(static_cast<double>(cosA), -1.0, 1.0)));

    Vec3 axis{
        normal.y * target.z - normal.z * target.y,
        normal.z * target.x - normal.x * target.z,
        normal.x * target.y - normal.y * target.x};
    const float axisLen = length(axis);
    if (axisLen < 1e-6f) {
        axis = (cosA < 0.f) ? Vec3{1.f, 0.f, 0.f} : Vec3{0.f, 0.f, 1.f};
    } else {
        axis.x /= axisLen;
        axis.y /= axisLen;
        axis.z /= axisLen;
    }

    const float angleDeg = angle * 180.0f / static_cast<float>(kPi);
    auto levelTransform = vtkSmartPointer<vtkTransform>::New();
    levelTransform->Translate(pivot[0], pivot[1], pivot[2]);
    levelTransform->RotateWXYZ(angleDeg, axis.x, axis.y, axis.z);
    levelTransform->Translate(-pivot[0], -pivot[1], -pivot[2]);
    result.level = matrixOf(levelTransform);

    const Vec3 leveledLeft = transformPoint(result.level, points.porionLeft);
    const Vec3 leveledRight = transformPoint(result.level, points.porionRight);
    Vec3 leftRight{leveledLeft.x - leveledRight.x, leveledLeft.y - leveledRight.y, 0.f};
    double yawDeg = 0.0;
    if (length(leftRight) >= 1e-6f) {
        const float leftRightLength = length(leftRight);
        leftRight.x /= leftRightLength;
        leftRight.y /= leftRightLength;
        double yawRad = std::atan2(static_cast<double>(leftRight.y),
                                   static_cast<double>(leftRight.x));
        if (yawRad > kPi * 0.5)
            yawRad -= kPi;
        else if (yawRad < -kPi * 0.5)
            yawRad += kPi;
        yawDeg = -yawRad * 180.0 / kPi;
    }

    auto yawTransform = vtkSmartPointer<vtkTransform>::New();
    yawTransform->Translate(pivot[0], pivot[1], pivot[2]);
    yawTransform->RotateZ(yawDeg);
    yawTransform->Translate(-pivot[0], -pivot[1], -pivot[2]);

    result.valid = true;
    result.yaw = matrixOf(yawTransform);
    result.levelDegrees = static_cast<double>(angleDeg);
    result.yawDegrees = yawDeg;
    result.levelAxis = {static_cast<double>(axis.x),
                        static_cast<double>(axis.y),
                        static_cast<double>(axis.z)};
    return result;
}

}
