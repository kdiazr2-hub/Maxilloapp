#include "MeshRepairCore.h"

#include "CompositeBlockCore.h"

#include <QStringList>

#include <vtkCellArray.h>
#include <vtkCellArrayIterator.h>
#include <vtkCleanPolyData.h>
#include <vtkIdList.h>
#include <vtkIdTypeArray.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataConnectivityFilter.h>
#include <vtkPolyDataNormals.h>
#include <vtkReverseSense.h>
#include <vtkTriangleFilter.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>
#include <vector>

namespace
{
using Vec3 = std::array<double, 3>;

Vec3 sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec3 cross(const Vec3& a, const Vec3& b)
{
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
double dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

constexpr double kDegenerateAreaMm2 = 1e-10;

vtkSmartPointer<vtkPolyData> merged(vtkPolyData* mesh)
{
    auto clean = vtkSmartPointer<vtkCleanPolyData>::New();
    clean->SetInputData(mesh);
    clean->PointMergingOn();
    clean->SetTolerance(0.0);
    clean->ConvertPolysToLinesOff();
    clean->ConvertLinesToPointsOff();
    clean->ConvertStripsToPolysOn();
    clean->Update();
    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->ShallowCopy(clean->GetOutput());
    return out;
}

vtkSmartPointer<vtkPolyData> copyOf(vtkPolyData* mesh)
{
    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->DeepCopy(mesh);
    return out;
}

bool degenerate(vtkPolyData* mesh, vtkIdType npts, const vtkIdType* ids)
{
    if (npts != 3 || ids[0] == ids[1] || ids[1] == ids[2] || ids[0] == ids[2])
        return true;
    Vec3 a{}, b{}, c{};
    mesh->GetPoint(ids[0], a.data());
    mesh->GetPoint(ids[1], b.data());
    mesh->GetPoint(ids[2], c.data());
    const Vec3 n = cross(sub(b, a), sub(c, a));
    return 0.5 * std::sqrt(dot(n, n)) < kDegenerateAreaMm2;
}

int shellCount(vtkPolyData* mesh)
{
    if (!mesh || mesh->GetNumberOfPolys() == 0)
        return 0;
    auto connectivity = vtkSmartPointer<vtkPolyDataConnectivityFilter>::New();
    connectivity->SetInputData(mesh);
    connectivity->SetExtractionModeToAllRegions();
    connectivity->Update();
    return connectivity->GetNumberOfExtractedRegions();
}
} // namespace

QString MeshCheck::Summary() const
{
    QString text = QStringLiteral("%1 triángulos, %2 cáscara(s)").arg(triangles).arg(shells);
    text += Closed() ? QStringLiteral(", cerrada")
                     : QStringLiteral(", abierta (%1 bordes libres, %2 no-manifold)").arg(boundaryEdges).arg(nonManifoldEdges);
    if (inconsistentEdges > 0)
        text += QStringLiteral(", %1 aristas con orientación incoherente").arg(inconsistentEdges);
    if (degenerateTriangles > 0)
        text += QStringLiteral(", %1 triángulos degenerados").arg(degenerateTriangles);
    if (otherCells > 0)
        text += QStringLiteral(", %1 celdas no triangulares").arg(otherCells);
    if (Closed())
        text += OutwardNormals() ? QStringLiteral(", normales hacia afuera, volumen %1 mm³").arg(signedVolumeMm3, 0, 'f', 1)
                                 : QStringLiteral(", normales invertidas");
    return text;
}

namespace MeshRepairCore
{
MeshCheck Analyze(vtkPolyData* input)
{
    MeshCheck check;
    if (!input || input->GetNumberOfCells() == 0)
        return check;
    const auto mesh = merged(input);
    check.points = mesh->GetNumberOfPoints();
    check.otherCells = mesh->GetNumberOfVerts() + mesh->GetNumberOfLines() + mesh->GetNumberOfStrips();
    // Merging drops triangles that collapse (repeated points): count them as degenerate.
    check.degenerateTriangles = std::max<vtkIdType>(0, input->GetNumberOfPolys() - mesh->GetNumberOfPolys());

    struct EdgeUse
    {
        int count = 0;
        int direction = 0; // +1 for low→high, -1 for high→low
    };
    std::unordered_map<unsigned long long, EdgeUse> edges;
    edges.reserve(static_cast<size_t>(mesh->GetNumberOfPolys()) * 2);
    const auto numPoints = static_cast<unsigned long long>(std::max<vtkIdType>(1, mesh->GetNumberOfPoints()));

    auto iter = vtk::TakeSmartPointer(mesh->GetPolys()->NewIterator());
    for (iter->GoToFirstCell(); !iter->IsDoneWithTraversal(); iter->GoToNextCell()) {
        vtkIdType npts = 0;
        const vtkIdType* ids = nullptr;
        iter->GetCurrentCell(npts, ids);
        if (npts != 3) {
            ++check.otherCells;
            continue;
        }
        ++check.triangles;
        if (degenerate(mesh, npts, ids)) {
            ++check.degenerateTriangles;
        }
        Vec3 a{}, b{}, c{};
        mesh->GetPoint(ids[0], a.data());
        mesh->GetPoint(ids[1], b.data());
        mesh->GetPoint(ids[2], c.data());
        const Vec3 n = cross(sub(b, a), sub(c, a));
        check.areaMm2 += 0.5 * std::sqrt(dot(n, n));
        check.signedVolumeMm3 += dot(a, cross(b, c)) / 6.0;
        for (int e = 0; e < 3; ++e) {
            const vtkIdType p = ids[e];
            const vtkIdType q = ids[(e + 1) % 3];
            if (p == q)
                continue;
            const auto lo = static_cast<unsigned long long>(std::min(p, q));
            const auto hi = static_cast<unsigned long long>(std::max(p, q));
            EdgeUse& use = edges[lo * numPoints + hi];
            ++use.count;
            use.direction += p < q ? 1 : -1;
        }
    }
    for (const auto& [key, use] : edges) {
        if (use.count == 1)
            ++check.boundaryEdges;
        else if (use.count > 2)
            ++check.nonManifoldEdges;
        else if (use.direction != 0)
            ++check.inconsistentEdges;
    }
    check.shells = shellCount(mesh);
    return check;
}

MeshRepairResult Repair(vtkPolyData* input, const MeshRepairOptions& options)
{
    MeshRepairResult result;
    result.before = Analyze(input);
    if (!input || result.before.triangles == 0) {
        result.report = QStringLiteral("Malla vacía: no hay triángulos que reparar.");
        return result;
    }

    // Triangles only, coincident points merged.
    auto triangles = vtkSmartPointer<vtkTriangleFilter>::New();
    triangles->SetInputData(input);
    triangles->PassLinesOff();
    triangles->PassVertsOff();
    triangles->Update();
    auto mesh = merged(triangles->GetOutput());
    result.removedDegenerate = std::max<vtkIdType>(0, triangles->GetOutput()->GetNumberOfPolys() - mesh->GetNumberOfPolys());

    // Degenerate triangles (cell data stays aligned through DeleteCell).
    mesh->BuildLinks();
    auto polys = mesh->GetPolys();
    const vtkIdType firstPoly = mesh->GetNumberOfVerts() + mesh->GetNumberOfLines();
    for (vtkIdType cell = 0; cell < polys->GetNumberOfCells(); ++cell) {
        vtkIdType npts = 0;
        const vtkIdType* ids = nullptr;
        mesh->GetCellPoints(firstPoly + cell, npts, ids);
        if (degenerate(mesh, npts, ids)) {
            mesh->DeleteCell(firstPoly + cell);
            ++result.removedDegenerate;
        }
    }
    if (result.removedDegenerate > 0)
        mesh->RemoveDeletedCells();
    mesh = merged(mesh);

    // Tiny floating shells.
    auto connectivity = vtkSmartPointer<vtkPolyDataConnectivityFilter>::New();
    connectivity->SetInputData(mesh);
    connectivity->SetExtractionModeToAllRegions();
    connectivity->Update();
    const int regions = connectivity->GetNumberOfExtractedRegions();
    if (regions > 1) {
        vtkIdTypeArray* sizes = connectivity->GetRegionSizes();
        vtkIdType largest = 0;
        for (int r = 0; r < regions; ++r)
            largest = std::max(largest, sizes->GetValue(r));
        const auto keepThreshold = static_cast<vtkIdType>(std::ceil(options.minShellFraction * static_cast<double>(largest)));
        auto keep = vtkSmartPointer<vtkPolyDataConnectivityFilter>::New();
        keep->SetInputData(mesh);
        keep->SetExtractionModeToSpecifiedRegions();
        for (int r = 0; r < regions; ++r) {
            if (sizes->GetValue(r) >= keepThreshold)
                keep->AddSpecifiedRegion(r);
            else
                ++result.removedShells;
        }
        if (result.removedShells > 0) {
            keep->Update();
            mesh = merged(keep->GetOutput());
        }
    }

    // Consistent orientation (outward when closed).
    MeshCheck check = Analyze(mesh);
    auto normals = vtkSmartPointer<vtkPolyDataNormals>::New();
    normals->SetInputData(mesh);
    normals->ConsistencyOn();
    normals->SplittingOff();
    normals->ComputePointNormalsOn();
    normals->ComputeCellNormalsOff();
    normals->SetAutoOrientNormals(check.Closed());
    normals->Update();
    mesh = copyOf(normals->GetOutput());
    check = Analyze(mesh);

    if (!check.Closed() && options.allowVoxelRemesh) {
        const VoxelUnionResult united = CompositeBlockCore::VoxelUnion({mesh.GetPointer()}, options.voxelSpacingMm,
                                                                       options.closingVoxels);
        if (united.ok && united.mesh && united.mesh->GetNumberOfPolys() > 0) {
            mesh = copyOf(united.mesh);
            result.remeshed = true;
            check = Analyze(mesh);
        }
    }

    if (check.Closed() && !check.OutwardNormals()) {
        auto reverse = vtkSmartPointer<vtkReverseSense>::New();
        reverse->SetInputData(mesh);
        reverse->ReverseCellsOn();
        reverse->ReverseNormalsOn();
        reverse->Update();
        mesh = copyOf(reverse->GetOutput());
        result.reversed = true;
        check = Analyze(mesh);
    }

    result.mesh = mesh;
    result.after = check;
    result.ok = check.Valid();

    QStringList actions;
    if (result.removedDegenerate > 0)
        actions << QStringLiteral("%1 triángulos degenerados eliminados").arg(result.removedDegenerate);
    if (result.removedShells > 0)
        actions << QStringLiteral("%1 cáscara(s) pequeñas eliminadas").arg(result.removedShells);
    if (result.reversed)
        actions << QStringLiteral("normales invertidas corregidas");
    if (result.remeshed)
        actions << QStringLiteral("superficie reconstruida por vóxeles de %1 mm para cerrarla").arg(options.voxelSpacingMm, 0, 'f', 2);
    result.report = QStringLiteral("Reparación STL: %1. Antes: %2. Después: %3.")
                        .arg(actions.isEmpty() ? QStringLiteral("sin cambios necesarios") : actions.join(QStringLiteral(", ")),
                             result.before.Summary(), result.after.Summary());
    return result;
}
} // namespace MeshRepairCore
