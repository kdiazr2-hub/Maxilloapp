#include "CompositeBlockCore.h"
#include "MeshRepairCore.h"

#include <vtkAppendPolyData.h>
#include <vtkCellArray.h>
#include <vtkCellData.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkReverseSense.h>
#include <vtkSmartPointer.h>
#include <vtkSphereSource.h>
#include <vtkTriangleFilter.h>
#include <vtkUnsignedCharArray.h>

#include <cmath>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

vtkSmartPointer<vtkPolyData> sphere(double radius, double center[3], int resolution = 48)
{
    auto source = vtkSmartPointer<vtkSphereSource>::New();
    source->SetRadius(radius);
    source->SetCenter(center);
    source->SetThetaResolution(resolution);
    source->SetPhiResolution(resolution);
    auto tri = vtkSmartPointer<vtkTriangleFilter>::New();
    tri->SetInputConnection(source->GetOutputPort());
    tri->Update();
    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->DeepCopy(tri->GetOutput());
    return out;
}

vtkSmartPointer<vtkPolyData> unitSphere(int resolution = 48)
{
    double c[3] = {0.0, 0.0, 0.0};
    return sphere(10.0, c, resolution);
}

// STL-like copy: every triangle gets its own three points.
vtkSmartPointer<vtkPolyData> unmerged(vtkPolyData* mesh)
{
    auto points = vtkSmartPointer<vtkPoints>::New();
    auto polys = vtkSmartPointer<vtkCellArray>::New();
    vtkIdType npts = 0;
    const vtkIdType* ids = nullptr;
    auto cells = mesh->GetPolys();
    cells->InitTraversal();
    while (cells->GetNextCell(npts, ids)) {
        vtkIdType tri[3];
        for (int i = 0; i < 3; ++i)
            tri[i] = points->InsertNextPoint(mesh->GetPoint(ids[i]));
        polys->InsertNextCell(3, tri);
    }
    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->SetPoints(points);
    out->SetPolys(polys);
    return out;
}

vtkSmartPointer<vtkPolyData> withoutCells(vtkPolyData* mesh, vtkIdType first, vtkIdType count)
{
    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->DeepCopy(mesh);
    out->BuildLinks();
    for (vtkIdType c = first; c < first + count; ++c)
        out->DeleteCell(c);
    out->RemoveDeletedCells();
    return out;
}

void testClosedSphere()
{
    const auto mesh = unitSphere();
    const MeshCheck check = MeshRepairCore::Analyze(mesh);
    require(check.Closed() && check.Consistent() && check.OutwardNormals(), "sphere should be valid: " + check.Summary().toStdString());
    require(check.shells == 1, "sphere should have one shell");
    const double expected = 4.0 / 3.0 * 3.14159265358979 * 1000.0;
    require(std::abs(check.signedVolumeMm3 - expected) / expected < 0.03, "sphere volume is off");

    const MeshRepairResult repaired = MeshRepairCore::Repair(mesh);
    require(repaired.ok && !repaired.remeshed && !repaired.reversed, "valid sphere should pass unchanged");
}

void testUnmergedStlTopology()
{
    const auto raw = unmerged(unitSphere());
    const MeshCheck check = MeshRepairCore::Analyze(raw);
    require(check.Closed(), "Analyze must merge coincident STL points");
    const MeshRepairResult repaired = MeshRepairCore::Repair(raw);
    require(repaired.ok && !repaired.remeshed, "unmerged sphere should repair without remeshing");
    require(repaired.mesh->GetNumberOfPoints() < raw->GetNumberOfPoints(), "points were not merged");
}

void testReversedNormals()
{
    auto reverse = vtkSmartPointer<vtkReverseSense>::New();
    reverse->SetInputData(unitSphere());
    reverse->ReverseCellsOn();
    reverse->Update();
    const MeshCheck check = MeshRepairCore::Analyze(reverse->GetOutput());
    require(check.Closed() && !check.OutwardNormals(), "reversed sphere should report inward normals");
    const MeshRepairResult repaired = MeshRepairCore::Repair(reverse->GetOutput());
    require(repaired.ok && repaired.after.OutwardNormals(), "reversed normals were not fixed");
}

void testInconsistentOrientation()
{
    auto mesh = vtkSmartPointer<vtkPolyData>::New();
    mesh->DeepCopy(unitSphere());
    // Flip a single triangle.
    vtkIdType npts = 0;
    const vtkIdType* ids = nullptr;
    mesh->GetCellPoints(10, npts, ids);
    const vtkIdType flipped[3] = {ids[0], ids[2], ids[1]};
    mesh->ReplaceCell(10, 3, flipped);
    const MeshCheck check = MeshRepairCore::Analyze(mesh);
    require(check.inconsistentEdges > 0, "flipped triangle not detected");
    const MeshRepairResult repaired = MeshRepairCore::Repair(mesh);
    require(repaired.ok, "flipped triangle not repaired: " + repaired.report.toStdString());
}

void testFloatingShellAndDegenerate()
{
    double c0[3] = {0.0, 0.0, 0.0};
    double c1[3] = {40.0, 0.0, 0.0};
    auto append = vtkSmartPointer<vtkAppendPolyData>::New();
    append->AddInputData(sphere(10.0, c0, 48));
    append->AddInputData(sphere(0.2, c1, 4)); // 16 triangles, below 1 % of the main shell
    append->Update();
    auto mesh = vtkSmartPointer<vtkPolyData>::New();
    mesh->DeepCopy(append->GetOutput());
    // Degenerate triangle on existing points.
    const vtkIdType degenerateIds[3] = {0, 0, 1};
    mesh->GetPolys()->InsertNextCell(3, degenerateIds);

    const MeshCheck check = MeshRepairCore::Analyze(mesh);
    require(check.shells >= 2 && check.degenerateTriangles >= 1, "shell/degenerate not detected");
    const MeshRepairResult repaired = MeshRepairCore::Repair(mesh);
    require(repaired.ok, "floating shell repair failed: " + repaired.report.toStdString());
    require(repaired.removedShells == 1 && repaired.after.shells == 1, "small shell not removed");
    require(repaired.removedDegenerate >= 1, "degenerate triangle not removed");
}

void testOpenSurfaceRemesh()
{
    const auto open = withoutCells(unitSphere(), 200, 2);
    const MeshCheck check = MeshRepairCore::Analyze(open);
    require(!check.Closed() && check.boundaryEdges > 0, "hole not detected");

    MeshRepairOptions noRemesh;
    noRemesh.allowVoxelRemesh = false;
    const MeshRepairResult kept = MeshRepairCore::Repair(open, noRemesh);
    require(!kept.ok && !kept.remeshed, "open mesh must fail validation without remeshing");

    MeshRepairOptions remesh;
    remesh.voxelSpacingMm = 0.3;
    remesh.closingVoxels = 3;
    const MeshRepairResult closed = MeshRepairCore::Repair(open, remesh);
    require(closed.remeshed && closed.ok, "remesh did not close the surface: " + closed.report.toStdString());
}

void testKeepsCompositeParts()
{
    auto tagged = CompositeBlockCore::TagPart(unmerged(unitSphere()), CompositeBlockCore::DentalPart);
    const MeshRepairResult repaired = MeshRepairCore::Repair(tagged);
    require(repaired.ok, "tagged sphere failed repair");
    require(CompositeBlockCore::HasParts(repaired.mesh), "repair dropped the CompositePart cell array");
}
} // namespace

int main()
{
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"closed sphere", testClosedSphere},
        {"unmerged STL topology", testUnmergedStlTopology},
        {"reversed normals", testReversedNormals},
        {"inconsistent orientation", testInconsistentOrientation},
        {"floating shell and degenerate", testFloatingShellAndDegenerate},
        {"open surface remesh", testOpenSurfaceRemesh},
        {"keeps composite parts", testKeepsCompositeParts},
    };
    int failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
            ++failures;
        }
    }
    return failures == 0 ? 0 : 1;
}
