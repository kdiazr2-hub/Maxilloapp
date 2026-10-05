#include "GuideEngraveCore.h"

#include <vtkCleanPolyData.h>
#include <vtkFlyingEdges3D.h>
#include <vtkIdList.h>
#include <vtkImageData.h>
#include <vtkPolyData.h>
#include <vtkTransform.h>
#include <vtkTransformPolyDataFilter.h>
#include <vtkTriangleFilter.h>
#include <vtkVectorText.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace
{
using Vec3 = std::array<double, 3>;
Vec3 scale(const Vec3& a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
Vec3 sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
double dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec3 cross(const Vec3& a, const Vec3& b)
{
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
Vec3 unit(const Vec3& a, const Vec3& fallback)
{
    const double n = std::sqrt(dot(a, a));
    return n > 1e-12 ? scale(a, 1.0 / n) : fallback;
}

// The glyphs, flat, scaled to `heightMm` and centred on the origin.
vtkSmartPointer<vtkPolyData> flatText(const QString& text, double heightMm)
{
    auto glyphs = vtkSmartPointer<vtkVectorText>::New();
    glyphs->SetText(text.toUtf8().constData());
    auto clean = vtkSmartPointer<vtkCleanPolyData>::New(); // shared edges become shared points
    clean->SetInputConnection(glyphs->GetOutputPort());
    clean->Update();
    double b[6];
    clean->GetOutput()->GetBounds(b);
    // vtkVectorText's capitals and digits are about one unit tall.
    const double unitHeight = std::max(1e-6, b[3] - b[2]);
    const double factor = heightMm / unitHeight;
    auto transform = vtkSmartPointer<vtkTransform>::New();
    transform->PostMultiply();
    transform->Translate(-0.5 * (b[0] + b[1]), -0.5 * (b[2] + b[3]), 0.0);
    transform->Scale(factor, factor, 1.0);
    auto place = vtkSmartPointer<vtkTransformPolyDataFilter>::New();
    place->SetInputConnection(clean->GetOutputPort());
    place->SetTransform(transform);
    place->Update();
    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->DeepCopy(place->GetOutput());
    return out;
}
} // namespace

namespace GuideEngraveCore
{
vtkSmartPointer<vtkPolyData> TextSolid(const QString& text, double heightMm, double reliefMm, double sinkMm)
{
    // The glyphs' triangles overlap, so extruding them does not close. They are rasterised instead on a fine
    // grid, stacked from −sink to +relief, and contoured: always a closed solid.
    auto triangles = vtkSmartPointer<vtkTriangleFilter>::New();
    triangles->SetInputData(flatText(text, heightMm));
    triangles->Update();
    vtkPolyData* flat = triangles->GetOutput();
    auto out = vtkSmartPointer<vtkPolyData>::New();
    if (flat->GetNumberOfPolys() == 0)
        return out;
    double b[6];
    flat->GetBounds(b);
    const double h = std::max(0.02, std::min(0.08, 0.03 * heightMm));
    const double dz = std::min(0.1, 0.25 * (sinkMm + reliefMm));
    const int nx = static_cast<int>(std::ceil((b[1] - b[0]) / h)) + 3;
    const int ny = static_cast<int>(std::ceil((b[3] - b[2]) / h)) + 3;
    const int nz = static_cast<int>(std::ceil((sinkMm + reliefMm) / dz)) + 3;
    const double x0 = b[0] - h, y0 = b[2] - h, z0 = -sinkMm - dz;
    std::vector<unsigned char> inside(static_cast<size_t>(nx) * ny, 0);
    auto ids = vtkSmartPointer<vtkIdList>::New();
    for (vtkIdType cell = 0; cell < flat->GetNumberOfCells(); ++cell) {
        flat->GetCellPoints(cell, ids);
        if (ids->GetNumberOfIds() != 3)
            continue;
        double p[3][3];
        for (int k = 0; k < 3; ++k)
            flat->GetPoint(ids->GetId(k), p[k]);
        const double minX = std::min({p[0][0], p[1][0], p[2][0]}), maxX = std::max({p[0][0], p[1][0], p[2][0]});
        const double minY = std::min({p[0][1], p[1][1], p[2][1]}), maxY = std::max({p[0][1], p[1][1], p[2][1]});
        const double area = (p[1][0] - p[0][0]) * (p[2][1] - p[0][1]) - (p[2][0] - p[0][0]) * (p[1][1] - p[0][1]);
        if (std::abs(area) < 1e-12)
            continue;
        for (int j = std::max(0, static_cast<int>((minY - y0) / h)); j < ny && y0 + j * h <= maxY; ++j)
            for (int i = std::max(0, static_cast<int>((minX - x0) / h)); i < nx && x0 + i * h <= maxX; ++i) {
                const double x = x0 + i * h, y = y0 + j * h;
                const double w0 = ((p[1][0] - x) * (p[2][1] - y) - (p[2][0] - x) * (p[1][1] - y)) / area;
                const double w1 = ((p[2][0] - x) * (p[0][1] - y) - (p[0][0] - x) * (p[2][1] - y)) / area;
                const double w2 = 1.0 - w0 - w1;
                if (w0 >= -1e-9 && w1 >= -1e-9 && w2 >= -1e-9)
                    inside[static_cast<size_t>(j) * nx + i] = 1;
            }
    }
    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetDimensions(nx, ny, nz);
    image->SetSpacing(h, h, dz);
    image->SetOrigin(x0, y0, z0);
    image->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    auto* voxels = static_cast<unsigned char*>(image->GetScalarPointer());
    for (int k = 0; k < nz; ++k)
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i)
                voxels[(static_cast<size_t>(k) * ny + j) * nx + i] =
                    (k > 0 && k < nz - 1 && inside[static_cast<size_t>(j) * nx + i]) ? 255 : 0;
    auto contour = vtkSmartPointer<vtkFlyingEdges3D>::New();
    contour->SetInputData(image);
    contour->SetValue(0, 127.5);
    contour->ComputeNormalsOff();
    contour->Update();
    auto clean = vtkSmartPointer<vtkCleanPolyData>::New();
    clean->SetInputConnection(contour->GetOutputPort());
    clean->Update();
    // The contour sits half a slice inside the stack's ends: put the faces at −sink and +relief exactly.
    auto stretch = vtkSmartPointer<vtkTransform>::New();
    stretch->PostMultiply();
    double cb[6];
    clean->GetOutput()->GetBounds(cb);
    const double span = std::max(1e-6, cb[5] - cb[4]);
    stretch->Translate(0.0, 0.0, -cb[4]);
    stretch->Scale(1.0, 1.0, (sinkMm + reliefMm) / span);
    stretch->Translate(0.0, 0.0, -sinkMm);
    auto place = vtkSmartPointer<vtkTransformPolyDataFilter>::New();
    place->SetInputConnection(clean->GetOutputPort());
    place->SetTransform(stretch);
    place->Update();
    out->DeepCopy(place->GetOutput());
    return out;
}

double TextWidth(const QString& text, double heightMm)
{
    const auto flat = flatText(text, heightMm);
    if (!flat || flat->GetNumberOfPoints() == 0)
        return 0.0;
    double b[6];
    flat->GetBounds(b);
    return b[1] - b[0];
}

GuideFigure TextFigure(const QString& text, const std::array<double, 3>& center, const std::array<double, 3>& readingAxis,
                       const std::array<double, 3>& outward, double heightMm, double reliefMm)
{
    const Vec3 z = unit(outward, {0.0, 0.0, 1.0});
    const Vec3 x = unit(sub(readingAxis, scale(z, dot(readingAxis, z))), {1.0, 0.0, 0.0});
    const Vec3 y = cross(z, x);
    GuideFigure figure;
    figure.shape = GuideFigureShape::Mesh;
    figure.operation = GuideFigureOperation::Add;
    figure.mesh = TextSolid(text, heightMm, reliefMm);
    // Row-major local → world, columns the frame's axes.
    figure.matrix = {x[0], y[0], z[0], center[0], x[1], y[1], z[1], center[1], x[2], y[2], z[2], center[2],
                     0.0,  0.0,  0.0,  1.0};
    return figure;
}
}
