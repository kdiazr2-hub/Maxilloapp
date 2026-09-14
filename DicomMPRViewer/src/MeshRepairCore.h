#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// MeshRepairCore
//
// STL checking and fixing before export (ProPlan "Fix STL"): merged topology,
// boundary / non-manifold / inconsistently oriented edges, degenerate
// triangles, shells and signed volume. Repair merges points, drops degenerate
// triangles and tiny floating shells, makes orientation consistent and outward
// and, when the surface is still open, rebuilds it as a closed voxel surface.
// No Qt Widgets.
// ─────────────────────────────────────────────────────────────────────────────

#include <QString>
#include <vtkSmartPointer.h>
#include <vtkType.h>

class vtkPolyData;

struct MeshCheck
{
    vtkIdType points = 0;           // after merging coincident points
    vtkIdType triangles = 0;
    vtkIdType otherCells = 0;       // polygons that are not triangles, lines, vertices
    vtkIdType boundaryEdges = 0;
    vtkIdType nonManifoldEdges = 0;
    vtkIdType inconsistentEdges = 0; // shared by two triangles traversing it the same way
    vtkIdType degenerateTriangles = 0;
    int shells = 0;
    double signedVolumeMm3 = 0.0;
    double areaMm2 = 0.0;

    bool Closed() const { return triangles > 0 && boundaryEdges == 0 && nonManifoldEdges == 0; }
    bool Consistent() const { return inconsistentEdges == 0; }
    bool OutwardNormals() const { return signedVolumeMm3 > 0.0; }
    bool Valid() const
    {
        return Closed() && Consistent() && OutwardNormals() && degenerateTriangles == 0 && otherCells == 0;
    }
    QString Summary() const; // Spanish, one line
};

struct MeshRepairOptions
{
    double minShellFraction = 0.01; // shells with fewer triangles than this fraction of the largest are dropped
    bool allowVoxelRemesh = true;
    double voxelSpacingMm = 0.2;
    int closingVoxels = 2;
};

struct MeshRepairResult
{
    bool ok = false; // result passes MeshCheck::Valid()
    vtkSmartPointer<vtkPolyData> mesh;
    MeshCheck before;
    MeshCheck after;
    vtkIdType removedDegenerate = 0;
    int removedShells = 0;
    bool reversed = false;
    bool remeshed = false;
    QString report;
};

namespace MeshRepairCore
{
MeshCheck Analyze(vtkPolyData* mesh);
MeshRepairResult Repair(vtkPolyData* mesh, const MeshRepairOptions& options = {});
}
