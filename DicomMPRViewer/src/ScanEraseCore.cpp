#include "ScanEraseCore.h"

#include <vtkCellArray.h>
#include <vtkCleanPolyData.h>
#include <vtkIdList.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkStaticPointLocator.h>

#include <cmath>
#include <unordered_set>

namespace ScanEraseCore {

vtkSmartPointer<vtkPolyData> Erase(vtkPolyData* mesh, const double center[3], double radiusMm)
{
    if (!mesh || !mesh->GetPolys() || mesh->GetNumberOfPolys() == 0 || radiusMm <= 0.0)
        return nullptr;
    auto locator = vtkSmartPointer<vtkStaticPointLocator>::New();
    locator->SetDataSet(mesh);
    locator->BuildLocator();
    auto inside = vtkSmartPointer<vtkIdList>::New();
    locator->FindPointsWithinRadius(radiusMm, center, inside);
    if (inside->GetNumberOfIds() == 0) {
        auto same = vtkSmartPointer<vtkPolyData>::New();
        same->DeepCopy(mesh);
        return same;
    }
    std::unordered_set<vtkIdType> drop;
    drop.reserve(static_cast<size_t>(inside->GetNumberOfIds()));
    for (vtkIdType i = 0; i < inside->GetNumberOfIds(); ++i)
        drop.insert(inside->GetId(i));

    auto polys = vtkSmartPointer<vtkCellArray>::New();
    vtkIdType npts = 0;
    const vtkIdType* ids = nullptr;
    for (mesh->GetPolys()->InitTraversal(); mesh->GetPolys()->GetNextCell(npts, ids);) {
        bool hit = false;
        for (vtkIdType k = 0; k < npts && !hit; ++k)
            hit = drop.count(ids[k]) != 0;
        if (!hit)
            polys->InsertNextCell(npts, ids);
    }
    auto kept = vtkSmartPointer<vtkPolyData>::New();
    kept->SetPoints(mesh->GetPoints());
    kept->SetPolys(polys);
    auto clean = vtkSmartPointer<vtkCleanPolyData>::New();
    clean->SetInputData(kept);
    clean->PointMergingOff();
    clean->Update();
    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->DeepCopy(clean->GetOutput());
    return out;
}

vtkSmartPointer<vtkPolyData> EraseScreen(vtkPolyData* mesh, const float* displayX, const float* displayY,
                                         double centerX, double centerY, double radiusPx)
{
    if (!mesh || !mesh->GetPolys() || mesh->GetNumberOfPolys() == 0 || !displayX || !displayY || radiusPx <= 0.0)
        return nullptr;
    const vtkIdType n = mesh->GetNumberOfPoints();
    const double radius2 = radiusPx * radiusPx;
    std::unordered_set<vtkIdType> drop;
    for (vtkIdType i = 0; i < n; ++i) {
        if (!std::isfinite(displayX[i]) || !std::isfinite(displayY[i]))
            continue;
        const double dx = static_cast<double>(displayX[i]) - centerX;
        const double dy = static_cast<double>(displayY[i]) - centerY;
        if (dx * dx + dy * dy <= radius2)
            drop.insert(i);
    }
    if (drop.empty())
        return nullptr;

    auto polys = vtkSmartPointer<vtkCellArray>::New();
    vtkIdType npts = 0;
    const vtkIdType* ids = nullptr;
    for (mesh->GetPolys()->InitTraversal(); mesh->GetPolys()->GetNextCell(npts, ids);) {
        bool hit = false;
        for (vtkIdType k = 0; k < npts && !hit; ++k)
            hit = drop.count(ids[k]) != 0;
        if (!hit)
            polys->InsertNextCell(npts, ids);
    }
    auto kept = vtkSmartPointer<vtkPolyData>::New();
    kept->SetPoints(mesh->GetPoints());
    kept->SetPolys(polys);
    auto clean = vtkSmartPointer<vtkCleanPolyData>::New();
    clean->SetInputData(kept);
    clean->PointMergingOff();
    clean->Update();
    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->DeepCopy(clean->GetOutput());
    return out;
}

} // namespace ScanEraseCore
