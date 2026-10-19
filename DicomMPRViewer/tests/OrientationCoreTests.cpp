#include "OrientationCore.h"

#include <cmath>
#include <iostream>
#include <string>

#include <vtkMatrix4x4.h>

namespace {

constexpr double kPi = 3.14159265358979323846;

int fail(const std::string& message)
{
    std::cerr << "FAIL: " << message << "\n";
    return 1;
}

bool nearIdentity(vtkMatrix4x4* matrix, double tolerance)
{
    if (!matrix)
        return false;
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            const double expected = (row == column) ? 1.0 : 0.0;
            if (std::abs(matrix->GetElement(row, column) - expected) > tolerance)
                return false;
        }
    }
    return true;
}

std::array<double, 3> rotateX(const std::array<double, 3>& p, double degrees)
{
    const double a = degrees * kPi / 180.0;
    const double c = std::cos(a);
    const double s = std::sin(a);
    return {p[0], p[1] * c - p[2] * s, p[1] * s + p[2] * c};
}

std::array<double, 3> apply(vtkMatrix4x4* matrix, const std::array<double, 3>& p, double w)
{
    const double in[4] = {p[0], p[1], p[2], w};
    double out[4] = {};
    matrix->MultiplyPoint(in, out);
    return {out[0], out[1], out[2]};
}

// Newell normal of the same convex quad the core uses, in double.
std::array<double, 3> newellNormal(const OrientationCore::FrankfurtPoints& points)
{
    const std::array<double, 3> poly[4] = {
        points.porionRight, points.orbitalRight, points.orbitalLeft, points.porionLeft};
    double n[3] = {};
    for (int i = 0; i < 4; ++i) {
        const auto& curr = poly[i];
        const auto& next = poly[(i + 1) % 4];
        n[0] += (curr[1] - next[1]) * (curr[2] + next[2]);
        n[1] += (curr[2] - next[2]) * (curr[0] + next[0]);
        n[2] += (curr[0] - next[0]) * (curr[1] + next[1]);
    }
    const double len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    return {n[0] / len, n[1] / len, n[2] / len};
}

OrientationCore::FrankfurtPoints horizontalPorionsOnX()
{
    OrientationCore::FrankfurtPoints points;
    points.porionRight = {-40.0, 0.0, 0.0};
    points.porionLeft = {40.0, 0.0, 0.0};
    points.orbitalRight = {-30.0, -80.0, 0.0};
    points.orbitalLeft = {30.0, -80.0, 0.0};
    return points;
}

}

int main()
{
    const std::array<double, 3> origin{0.0, 0.0, 0.0};

    const auto levelled = OrientationCore::AlignFrankfurt(horizontalPorionsOnX(), origin);
    if (!levelled.valid)
        return fail("A horizontal Frankfurt plane was rejected.");
    if (std::abs(levelled.levelDegrees) >= 1e-3 || std::abs(levelled.yawDegrees) >= 1e-3)
        return fail("A plane already on Z with porions on X must not level or yaw.");
    if (!nearIdentity(levelled.level, 1e-4) || !nearIdentity(levelled.yaw, 1e-4))
        return fail("Level and yaw of an already aligned plane must be identity.");

    OrientationCore::FrankfurtPoints tilted = horizontalPorionsOnX();
    tilted.porionRight = rotateX(tilted.porionRight, 20.0);
    tilted.porionLeft = rotateX(tilted.porionLeft, 20.0);
    tilted.orbitalRight = rotateX(tilted.orbitalRight, 20.0);
    tilted.orbitalLeft = rotateX(tilted.orbitalLeft, 20.0);
    const auto raised = OrientationCore::AlignFrankfurt(tilted, origin);
    if (!raised.valid || !raised.level)
        return fail("A plane tilted 20 degrees about X was rejected.");
    const auto originalNormal = newellNormal(tilted);
    const auto levelledNormal = apply(raised.level, originalNormal, 0.0);
    if (!(levelledNormal[2] > 0.999
          && std::abs(levelledNormal[0]) < 1e-3
          && std::abs(levelledNormal[1]) < 1e-3))
        return fail("Level must take the tilted plane normal to +Z.");

    OrientationCore::FrankfurtPoints alongY;
    alongY.porionRight = {0.0, -40.0, 0.0};
    alongY.porionLeft = {0.0, 40.0, 0.0};
    alongY.orbitalRight = {-30.0, -80.0, 0.0};
    alongY.orbitalLeft = {30.0, -80.0, 0.0};
    const auto yawed = OrientationCore::AlignFrankfurt(alongY, origin);
    if (!yawed.valid || !yawed.level || !yawed.yaw)
        return fail("Porions lying on Y were rejected.");
    if (std::abs(std::abs(yawed.yawDegrees) - 90.0) > 1e-3)
        return fail("Porions on Y must yaw by about 90 degrees.");
    const auto left = apply(yawed.yaw, apply(yawed.level, alongY.porionLeft, 1.0), 1.0);
    const auto right = apply(yawed.yaw, apply(yawed.level, alongY.porionRight, 1.0), 1.0);
    const std::array<double, 3> leftRight{left[0] - right[0], left[1] - right[1], left[2] - right[2]};
    if (!(std::abs(leftRight[0]) > std::abs(leftRight[1])
          && std::abs(leftRight[0]) > std::abs(leftRight[2])
          && std::abs(leftRight[2]) < 1e-3))
        return fail("After level then yaw, porion left minus right must lie on X.");

    OrientationCore::FrankfurtPoints collapsed;
    collapsed.porionRight = {1.0, 2.0, 3.0};
    collapsed.porionLeft = collapsed.porionRight;
    collapsed.orbitalRight = collapsed.porionRight;
    collapsed.orbitalLeft = collapsed.porionRight;
    const auto degenerate = OrientationCore::AlignFrankfurt(collapsed, origin);
    if (degenerate.valid)
        return fail("Four coincident points must not define a Frankfurt plane.");
    if (!nearIdentity(degenerate.level, 0.0) || !nearIdentity(degenerate.yaw, 0.0))
        return fail("A rejected alignment must return identity matrices.");

    return 0;
}
