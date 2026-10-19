#pragma once

#include <vtkSmartPointer.h>

class vtkPolyData;

namespace ScanEraseCore {

// Drops every cell that has a vertex inside the sphere. The input mesh is left as it was.
vtkSmartPointer<vtkPolyData> Erase(vtkPolyData* mesh, const double center[3], double radiusMm);

// Drops every cell that has a vertex inside the screen circle. displayX/Y are parallel to the
// mesh points, in the same coordinates as the circle. A non-finite value is off screen and kept.
// Returns null when the circle covers no vertex, so the caller can leave the mesh alone.
vtkSmartPointer<vtkPolyData> EraseScreen(vtkPolyData* mesh, const float* displayX, const float* displayY,
                                         double centerX, double centerY, double radiusPx);

} // namespace ScanEraseCore
