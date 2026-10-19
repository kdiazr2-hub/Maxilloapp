#include "ScanEraseCore.h"

#include <vtkCellArray.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace
{
void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

vtkSmartPointer<vtkPolyData> quadAt(double x, double y, double z)
{
    auto points = vtkSmartPointer<vtkPoints>::New();
    points->InsertNextPoint(x, y, z);
    points->InsertNextPoint(x + 2.0, y, z);
    points->InsertNextPoint(x + 2.0, y + 2.0, z);
    points->InsertNextPoint(x, y + 2.0, z);
    auto polys = vtkSmartPointer<vtkCellArray>::New();
    vtkIdType a[3] = {0, 1, 2};
    vtkIdType b[3] = {0, 2, 3};
    polys->InsertNextCell(3, a);
    polys->InsertNextCell(3, b);
    auto mesh = vtkSmartPointer<vtkPolyData>::New();
    mesh->SetPoints(points);
    mesh->SetPolys(polys);
    return mesh;
}

vtkSmartPointer<vtkPolyData> twoPatches()
{
    auto left = quadAt(0.0, 0.0, 0.0);
    auto right = quadAt(30.0, 0.0, 0.0);
    auto points = vtkSmartPointer<vtkPoints>::New();
    auto polys = vtkSmartPointer<vtkCellArray>::New();
    for (vtkPolyData* patch : {left.Get(), right.Get()}) {
        const vtkIdType base = points->GetNumberOfPoints();
        for (vtkIdType i = 0; i < patch->GetNumberOfPoints(); ++i) {
            double p[3] = {};
            patch->GetPoint(i, p);
            points->InsertNextPoint(p);
        }
        vtkIdType npts = 0;
        const vtkIdType* ids = nullptr;
        for (patch->GetPolys()->InitTraversal(); patch->GetPolys()->GetNextCell(npts, ids);) {
            vtkIdType shifted[3] = {ids[0] + base, ids[1] + base, ids[2] + base};
            polys->InsertNextCell(3, shifted);
        }
    }
    auto mesh = vtkSmartPointer<vtkPolyData>::New();
    mesh->SetPoints(points);
    mesh->SetPolys(polys);
    return mesh;
}
} // namespace

int main()
{
    try {
        const auto mesh = twoPatches();
        const vtkIdType before = mesh->GetNumberOfPolys();
        const double center[3] = {1.0, 1.0, 0.0};
        const auto erased = ScanEraseCore::Erase(mesh, center, 3.0);
        require(erased && erased->GetNumberOfPolys() == before / 2, "the brush did not remove only the patch under it");
        require(mesh->GetNumberOfPolys() == before, "erasing changed the original scan");
        double far = 0.0;
        bool keptFar = false;
        for (vtkIdType i = 0; i < erased->GetNumberOfPoints(); ++i) {
            double p[3] = {};
            erased->GetPoint(i, p);
            keptFar = keptFar || p[0] > 20.0;
            far = std::max(far, p[0]);
        }
        require(keptFar && far > 20.0, "the patch outside the circle was removed");
        const double miss[3] = {0.0, 0.0, 50.0};
        const auto untouched = ScanEraseCore::Erase(mesh, miss, 2.0);
        require(untouched && untouched->GetNumberOfPolys() == before, "a miss erased the scan");
        float displayX[8] = {10.f, 12.f, 12.f, 10.f, 80.f, 82.f, 82.f, 80.f};
        float displayY[8] = {10.f, 10.f, 12.f, 12.f, 10.f, 10.f, 12.f, 12.f};
        const auto screen = ScanEraseCore::EraseScreen(mesh, displayX, displayY, 11.0, 11.0, 5.0);
        require(screen && screen->GetNumberOfPolys() == before / 2, "the screen circle did not remove only the patch under it");
        require(ScanEraseCore::EraseScreen(mesh, displayX, displayY, 200.0, 200.0, 5.0) == nullptr,
                "a screen circle over empty space rebuilt the scan");
        std::cout << "PASS scan erase\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
