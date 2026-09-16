#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// WrapCore
//
// 3-matic's Wrap (Thickness group): one closed envelope over a set of surfaces,
// however open or scattered they are. Same four steps the composite's voxel
// union has always used — rasterise the shells, close small gaps, fill the
// interior from outside, contour — but on ImplicitCore's grid, so the two
// parameters are real millimetres instead of voxel counts, the closing is an
// exact distance rather than a number of dilation passes, and the result comes
// out smoothed and repaired.
//
// `CompositeBlockCore::VoxelUnion` stays where it is: MeshRepairCore's remesh
// calls it, and routing that through a wrap that repairs its own output would
// loop back into the repair.
// No Qt Widgets.
// ─────────────────────────────────────────────────────────────────────────────

#include <QString>
#include <vtkSmartPointer.h>

#include <atomic>
#include <vector>

class vtkPolyData;

struct WrapParams
{
    // Holes, slits and gaps narrower than this are bridged; it also decides how much the envelope
    // bulges over concavities, exactly as in 3-matic.
    double gapClosingMm = 1.5;
    // Voxel size: the smallest feature that survives, and what the cost is paid on.
    double smallestDetailMm = 0.3;
    int smoothingIterations = 15; // windowed sinc on the result; 0 leaves the raw contour
};

struct WrapResult
{
    bool ok = false;
    QString error;
    QString report;
    vtkSmartPointer<vtkPolyData> mesh;
    double spacingMm = 0.0; // the grid actually used (coarsened if the volume would be too large)
};

namespace WrapCore
{
WrapResult Wrap(const std::vector<vtkPolyData*>& meshes, const WrapParams& params = {},
                const std::atomic<bool>* cancel = nullptr);
}
