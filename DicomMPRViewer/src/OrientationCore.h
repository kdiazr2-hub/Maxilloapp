#pragma once

#include <array>

#include <vtkMatrix4x4.h>
#include <vtkSmartPointer.h>

namespace OrientationCore {

// Orden guardado del paso Orientación, milímetros, DICOM LPS, Z+ superior:
// 0 porion derecho, 1 porion izquierdo, 2 orbital derecho, 3 orbital izquierdo.
struct FrankfurtPoints {
    std::array<double, 3> porionRight{};
    std::array<double, 3> porionLeft{};
    std::array<double, 3> orbitalRight{};
    std::array<double, 3> orbitalLeft{};
};

struct FrankfurtAlignment {
    bool valid = false;
    vtkSmartPointer<vtkMatrix4x4> level; // identidad si !valid
    vtkSmartPointer<vtkMatrix4x4> yaw;
    double levelDegrees = 0;
    double yawDegrees = 0;
    std::array<double, 3> levelAxis{}; // eje de la nivelación, el que hoy se loguea
};

// level luego yaw, ambas alrededor de pivot. Matrices identidad cuando !valid.
FrankfurtAlignment AlignFrankfurt(const FrankfurtPoints& points, const std::array<double, 3>& pivot);

}
