#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// CollisionCore
//
// Intersection between two closed surfaces for REPOSICIÓN (ProPlan 4.6.1
// "Intersection volume" and "Highlight intersection"): both meshes are
// rasterised on the overlap of their bounds with vertical rays (exact
// ray–triangle crossings, inside by parity) and the voxels inside both are
// counted. The highlight is a copy of the first mesh whose cells lying inside
// the second one are coloured red. No Qt Widgets.
// ─────────────────────────────────────────────────────────────────────────────

#include <QString>
#include <vtkSmartPointer.h>
#include <vtkType.h>

#include <array>

class vtkPolyData;

struct IntersectionResult
{
    bool ok = false;
    QString error;
    double volumeMm3 = 0.0;
    vtkIdType voxels = 0;
    double spacingMm = 0.0;
    // Copy of the first mesh with RGB cell scalars "IntersectionColor".
    vtkSmartPointer<vtkPolyData> highlight;
    vtkIdType highlightedCells = 0;
};

namespace CollisionCore
{
inline constexpr const char* HighlightArrayName = "IntersectionColor";

IntersectionResult Intersection(vtkPolyData* first, vtkPolyData* second, double spacingMm = 0.5,
                                const std::array<unsigned char, 3>& baseColor = {226, 212, 190},
                                const std::array<unsigned char, 3>& hitColor = {230, 40, 40});
}
