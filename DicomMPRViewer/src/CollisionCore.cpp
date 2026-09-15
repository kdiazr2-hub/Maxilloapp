#include "CollisionCore.h"

#include <vtkCellArray.h>
#include <vtkCellArrayIterator.h>
#include <vtkCellData.h>
#include <vtkIdList.h>
#include <vtkPolyData.h>
#include <vtkStaticCellLocator.h>
#include <vtkTriangleFilter.h>
#include <vtkUnsignedCharArray.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace
{
constexpr double kMaxColumns = 4.0e6;

vtkSmartPointer<vtkPolyData> triangles(vtkPolyData* mesh)
{
    auto filter = vtkSmartPointer<vtkTriangleFilter>::New();
    filter->SetInputData(mesh);
    filter->PassVertsOff();
    filter->PassLinesOff();
    filter->Update();
    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->DeepCopy(filter->GetOutput());
    return out;
}

// Sorted z of the crossings of the vertical line (x, y) with the triangles.
struct ColumnCaster
{
    vtkSmartPointer<vtkPolyData> mesh;
    vtkSmartPointer<vtkStaticCellLocator> locator;
    double zLow = 0.0;
    double zHigh = 0.0;

    explicit ColumnCaster(vtkPolyData* input)
        : mesh(triangles(input))
    {
        locator = vtkSmartPointer<vtkStaticCellLocator>::New();
        locator->SetDataSet(mesh);
        locator->BuildLocator();
        double b[6] = {};
        mesh->GetBounds(b);
        zLow = b[4] - 1.0;
        zHigh = b[5] + 1.0;
    }

    std::vector<double> crossings(double x, double y) const
    {
        std::vector<double> zs;
        auto ids = vtkSmartPointer<vtkIdList>::New();
        double p0[3] = {x, y, zLow};
        double p1[3] = {x, y, zHigh};
        locator->FindCellsAlongLine(p0, p1, 1e-6, ids);
        double a[3], b[3], c[3];
        for (vtkIdType k = 0; k < ids->GetNumberOfIds(); ++k) {
            vtkIdType npts = 0;
            const vtkIdType* pts = nullptr;
            mesh->GetCellPoints(ids->GetId(k), npts, pts);
            if (npts != 3)
                continue;
            mesh->GetPoint(pts[0], a);
            mesh->GetPoint(pts[1], b);
            mesh->GetPoint(pts[2], c);
            const double det = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1]);
            if (std::abs(det) < 1e-14)
                continue; // triangle seen edge-on
            const double l1 = ((b[1] - c[1]) * (x - c[0]) + (c[0] - b[0]) * (y - c[1])) / det;
            const double l2 = ((c[1] - a[1]) * (x - c[0]) + (a[0] - c[0]) * (y - c[1])) / det;
            const double l3 = 1.0 - l1 - l2;
            if (l1 < 0.0 || l2 < 0.0 || l3 < 0.0)
                continue;
            zs.push_back(l1 * a[2] + l2 * b[2] + l3 * c[2]);
        }
        std::sort(zs.begin(), zs.end());
        if (zs.size() % 2 == 1)
            zs.pop_back(); // open surface: ignore the unpaired crossing
        return zs;
    }
};

bool insideIntervals(const std::vector<double>& zs, double z)
{
    for (size_t i = 0; i + 1 < zs.size(); i += 2)
        if (z >= zs[i] && z <= zs[i + 1])
            return true;
    return false;
}
} // namespace

namespace CollisionCore
{
IntersectionResult Intersection(vtkPolyData* first, vtkPolyData* second, double spacingMm,
                                const std::array<unsigned char, 3>& baseColor,
                                const std::array<unsigned char, 3>& hitColor)
{
    IntersectionResult result;
    if (!first || !second || first->GetNumberOfCells() == 0 || second->GetNumberOfCells() == 0) {
        result.error = QStringLiteral("Faltan las mallas para el análisis de intersección.");
        return result;
    }
    double ba[6] = {}, bb[6] = {};
    first->GetBounds(ba);
    second->GetBounds(bb);
    const std::array<double, 3> lo{std::max(ba[0], bb[0]), std::max(ba[2], bb[2]), std::max(ba[4], bb[4])};
    const std::array<double, 3> hi{std::min(ba[1], bb[1]), std::min(ba[3], bb[3]), std::min(ba[5], bb[5])};

    const ColumnCaster casterA(first);
    result.highlight = vtkSmartPointer<vtkPolyData>::New();
    result.highlight->DeepCopy(casterA.mesh);
    auto colors = vtkSmartPointer<vtkUnsignedCharArray>::New();
    colors->SetName(HighlightArrayName);
    colors->SetNumberOfComponents(3);
    colors->SetNumberOfTuples(result.highlight->GetNumberOfCells());
    for (vtkIdType i = 0; i < result.highlight->GetNumberOfCells(); ++i)
        colors->SetTypedTuple(i, baseColor.data());
    result.highlight->GetCellData()->SetScalars(colors);

    double h = std::max(0.05, spacingMm);
    if (lo[0] >= hi[0] || lo[1] >= hi[1] || lo[2] >= hi[2]) {
        result.spacingMm = h;
        result.ok = true; // no overlap of the bounds
        return result;
    }
    const double columns = std::ceil((hi[0] - lo[0]) / h) * std::ceil((hi[1] - lo[1]) / h);
    if (columns > kMaxColumns)
        h *= std::sqrt(columns / kMaxColumns);
    result.spacingMm = h;
    const int nx = std::max(1, static_cast<int>(std::ceil((hi[0] - lo[0]) / h)));
    const int ny = std::max(1, static_cast<int>(std::ceil((hi[1] - lo[1]) / h)));
    const int nz = std::max(1, static_cast<int>(std::ceil((hi[2] - lo[2]) / h)));

    const ColumnCaster casterB(second);
    // Tiny offsets keep the rays off shared edges and vertices of regular meshes.
    const double jitterX = 0.000731 * h;
    const double jitterY = 0.000419 * h;
    std::vector<std::vector<double>> columnsB(static_cast<size_t>(nx) * static_cast<size_t>(ny));
    for (int j = 0; j < ny; ++j) {
        for (int i = 0; i < nx; ++i) {
            const double x = lo[0] + (i + 0.5) * h + jitterX;
            const double y = lo[1] + (j + 0.5) * h + jitterY;
            auto zb = casterB.crossings(x, y);
            if (!zb.empty()) {
                const auto za = casterA.crossings(x, y);
                for (int k = 0; k < nz; ++k) {
                    const double z = lo[2] + (k + 0.5) * h;
                    if (insideIntervals(za, z) && insideIntervals(zb, z))
                        ++result.voxels;
                }
            }
            columnsB[static_cast<size_t>(j) * nx + i] = std::move(zb);
        }
    }
    result.volumeMm3 = static_cast<double>(result.voxels) * h * h * h;

    // Highlight: cells of the first mesh whose centroid lies inside the second.
    vtkPolyData* mesh = result.highlight;
    const vtkIdType offset = mesh->GetNumberOfVerts() + mesh->GetNumberOfLines();
    vtkIdType cell = 0;
    auto it = vtk::TakeSmartPointer(mesh->GetPolys()->NewIterator());
    double p[3];
    for (it->GoToFirstCell(); !it->IsDoneWithTraversal(); it->GoToNextCell(), ++cell) {
        vtkIdType npts = 0;
        const vtkIdType* ids = nullptr;
        it->GetCurrentCell(npts, ids);
        double c[3] = {0.0, 0.0, 0.0};
        for (vtkIdType k = 0; k < npts; ++k) {
            mesh->GetPoint(ids[k], p);
            for (int a = 0; a < 3; ++a)
                c[a] += p[a] / static_cast<double>(npts);
        }
        if (c[0] < lo[0] || c[0] > hi[0] || c[1] < lo[1] || c[1] > hi[1] || c[2] < lo[2] || c[2] > hi[2])
            continue;
        const int i = std::clamp(static_cast<int>((c[0] - lo[0]) / h), 0, nx - 1);
        const int j = std::clamp(static_cast<int>((c[1] - lo[1]) / h), 0, ny - 1);
        if (insideIntervals(columnsB[static_cast<size_t>(j) * nx + i], c[2])) {
            colors->SetTypedTuple(offset + cell, hitColor.data());
            ++result.highlightedCells;
        }
    }
    result.ok = true;
    return result;
}
} // namespace CollisionCore
