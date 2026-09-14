#pragma once

// Synthetic dental arches shared by the splint tests: 12 box teeth on a U whose
// apex (anterior) points toward +y. Upper crowns span z in [1, 9], lower crowns
// z in [-9, -1]; guide points sit on the buccal side of teeth 0, 5 and 11.

#include "SplintHeightmapGenerator.h"

#include <vtkAppendPolyData.h>
#include <vtkCellArray.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkSmartPointer.h>
#include <vtkTriangleFilter.h>

#include <array>
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

namespace splinttest
{
inline constexpr double kPi = 3.14159265358979323846;
inline constexpr int kTeeth = 12;
inline constexpr double kArchRx = 24.0;
inline constexpr double kArchRy = 20.0;

inline void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

struct Box
{
    double x0, x1, y0, y1, z0, z1;
};

// Axis-aligned box with outward normals; open faces model intraoral scans
// that end at the gingiva.
inline vtkSmartPointer<vtkPolyData> boxMesh(const Box& b, bool openTop, bool openBottom)
{
    auto points = vtkSmartPointer<vtkPoints>::New();
    for (int k = 0; k < 8; ++k)
        points->InsertNextPoint((k & 1) ? b.x1 : b.x0, (k & 2) ? b.y1 : b.y0, (k & 4) ? b.z1 : b.z0);
    auto polys = vtkSmartPointer<vtkCellArray>::New();
    auto quad = [&](vtkIdType a, vtkIdType c, vtkIdType d, vtkIdType e) {
        const vtkIdType ids[4] = {a, c, d, e};
        polys->InsertNextCell(4, ids);
    };
    if (!openBottom) quad(0, 2, 3, 1);
    if (!openTop) quad(4, 5, 7, 6);
    quad(0, 1, 5, 4);
    quad(2, 6, 7, 3);
    quad(0, 4, 6, 2);
    quad(1, 3, 7, 5);
    auto mesh = vtkSmartPointer<vtkPolyData>::New();
    mesh->SetPoints(points);
    mesh->SetPolys(polys);
    auto tri = vtkSmartPointer<vtkTriangleFilter>::New();
    tri->SetInputData(mesh);
    tri->Update();
    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->DeepCopy(tri->GetOutput());
    return out;
}

inline std::array<double, 2> toothCenter(int k)
{
    const double t = kPi * (k + 0.5) / kTeeth;
    return {kArchRx * std::cos(t), kArchRy * std::sin(t)};
}

struct ArchOptions
{
    bool openScan = false;
    bool mushroomUpper = false;
    double toothHalf = 2.5;
};

inline vtkSmartPointer<vtkPolyData> upperTeeth(const ArchOptions& options)
{
    auto append = vtkSmartPointer<vtkAppendPolyData>::New();
    for (int k = 0; k < kTeeth; ++k) {
        const auto c = toothCenter(k);
        if (options.mushroomUpper) {
            append->AddInputData(boxMesh({c[0] - 3, c[0] + 3, c[1] - 3, c[1] + 3, 1, 3}, false, false));
            append->AddInputData(boxMesh({c[0] - 1, c[0] + 1, c[1] - 1, c[1] + 1, 3, 9}, options.openScan, false));
        } else {
            const double s = options.toothHalf;
            append->AddInputData(boxMesh({c[0] - s, c[0] + s, c[1] - s, c[1] + s, 1, 9}, options.openScan, false));
        }
    }
    append->Update();
    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->DeepCopy(append->GetOutput());
    return out;
}

inline vtkSmartPointer<vtkPolyData> lowerTeeth(const ArchOptions& options)
{
    auto append = vtkSmartPointer<vtkAppendPolyData>::New();
    for (int k = 0; k < kTeeth; ++k) {
        const auto c = toothCenter(k);
        const double s = options.toothHalf;
        append->AddInputData(boxMesh({c[0] - s, c[0] + s, c[1] - s, c[1] + s, -9, -1}, false, options.openScan));
    }
    append->Update();
    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->DeepCopy(append->GetOutput());
    return out;
}

inline std::vector<SplintPoint3> guidePoints(double z)
{
    std::vector<SplintPoint3> points;
    for (int k : {0, 5, 11}) {
        const auto c = toothCenter(k);
        const double len = std::hypot(c[0], c[1]);
        points.push_back({c[0] + 2.8 * c[0] / len, c[1] + 2.8 * c[1] / len, z});
    }
    return points;
}

struct Scene
{
    vtkSmartPointer<vtkPolyData> upper;
    vtkSmartPointer<vtkPolyData> lower;
    SplintHeightmapInputs inputs;
};

inline Scene makeScene(const ArchOptions& options = {}, double upperZ = 5.0, double lowerZ = -5.0)
{
    Scene scene;
    scene.upper = upperTeeth(options);
    scene.lower = lowerTeeth(options);
    scene.inputs.upperTeeth = scene.upper;
    scene.inputs.lowerTeeth = scene.lower;
    scene.inputs.upperPoints = guidePoints(upperZ);
    scene.inputs.lowerPoints = guidePoints(lowerZ);
    scene.inputs.params.gridResolutionMm = 0.4;
    return scene;
}
} // namespace splinttest
