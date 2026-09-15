#include "CompositeBlockCore.h"

#include <vtkAppendPolyData.h>
#include <vtkCellArray.h>
#include <vtkCellArrayIterator.h>
#include <vtkCellData.h>
#include <vtkCleanPolyData.h>
#include <vtkClipPolyData.h>
#include <vtkCubeSource.h>
#include <vtkCutter.h>
#include <vtkDoubleArray.h>
#include <vtkPointData.h>
#include <vtkFlyingEdges3D.h>
#include <vtkImageData.h>
#include <vtkMath.h>
#include <vtkMatrix4x4.h>
#include <vtkPlane.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataNormals.h>
#include <vtkStripper.h>
#include <vtkTransform.h>
#include <vtkTransformPolyDataFilter.h>
#include <vtkTriangleFilter.h>
#include <vtkUnsignedCharArray.h>
#include <vtkWindowedSincPolyDataFilter.h>

#include <QJsonArray>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <thread>
#include <unordered_map>

namespace
{
using Vec3 = std::array<double, 3>;

Vec3 sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec3 add(const Vec3& a, const Vec3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec3 mul(const Vec3& a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
double dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec3 cross(const Vec3& a, const Vec3& b)
{
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
double length(const Vec3& v) { return std::sqrt(dot(v, v)); }
Vec3 normalized(const Vec3& v, const Vec3& fallback)
{
    const double len = length(v);
    return len > 1e-12 ? mul(v, 1.0 / len) : fallback;
}

void parallelFor(int count, const std::function<void(int, int)>& body)
{
    if (count <= 0)
        return;
    const int threads = std::min(static_cast<int>(std::max(1u, std::thread::hardware_concurrency())), count);
    if (threads <= 1) {
        body(0, count);
        return;
    }
    const int chunk = (count + threads - 1) / threads;
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t) {
        const int begin = t * chunk;
        const int end = std::min(count, begin + chunk);
        if (begin < end)
            pool.emplace_back(body, begin, end);
    }
    for (auto& thread : pool)
        thread.join();
}

vtkSmartPointer<vtkPolyData> triangulated(vtkPolyData* mesh)
{
    auto tri = vtkSmartPointer<vtkTriangleFilter>::New();
    tri->SetInputData(mesh);
    tri->PassVertsOff();
    tri->PassLinesOff();
    tri->Update();
    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->DeepCopy(tri->GetOutput());
    return out;
}

Vec3 sampledCentroid(vtkPolyData* mesh)
{
    Vec3 c{};
    const vtkIdType n = mesh->GetNumberOfPoints();
    const vtkIdType step = std::max<vtkIdType>(1, n / 50000);
    vtkIdType count = 0;
    double p[3] = {};
    for (vtkIdType i = 0; i < n; i += step, ++count) {
        mesh->GetPoint(i, p);
        c = add(c, {p[0], p[1], p[2]});
    }
    return count > 0 ? mul(c, 1.0 / static_cast<double>(count)) : c;
}

QJsonArray vecJson(const Vec3& v)
{
    return QJsonArray{v[0], v[1], v[2]};
}

bool vecFrom(const QJsonValue& value, Vec3& out)
{
    const QJsonArray a = value.toArray();
    if (a.size() != 3)
        return false;
    for (int i = 0; i < 3; ++i) {
        if (!a[i].isDouble())
            return false;
        out[static_cast<size_t>(i)] = a[i].toDouble();
    }
    return true;
}
} // namespace

namespace CompositeBlockCore
{
CompositeCutBlock InitialBlock(vtkPolyData* dentalScan, vtkPolyData* bone, double thicknessMm, QString* error)
{
    CompositeCutBlock block;
    if (!dentalScan || dentalScan->GetNumberOfPoints() < 10) {
        if (error)
            *error = QStringLiteral("El escaneo dental registrado no tiene suficientes puntos.");
        return block;
    }

    const Vec3 centroid = sampledCentroid(dentalScan);
    const vtkIdType n = dentalScan->GetNumberOfPoints();
    const vtkIdType step = std::max<vtkIdType>(1, n / 50000);
    double c0[3] = {}, c1[3] = {}, c2[3] = {};
    double* cov[3] = {c0, c1, c2};
    double p[3] = {};
    for (vtkIdType i = 0; i < n; i += step) {
        dentalScan->GetPoint(i, p);
        const Vec3 d = sub({p[0], p[1], p[2]}, centroid);
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                cov[r][c] += d[static_cast<size_t>(r)] * d[static_cast<size_t>(c)];
    }
    double eigenvalues[3] = {};
    double e0[3] = {}, e1[3] = {}, e2[3] = {};
    double* eigenvectors[3] = {e0, e1, e2};
    vtkMath::Jacobi(cov, eigenvalues, eigenvectors); // decreasing, vectors in columns

    Vec3 z = normalized({eigenvectors[0][2], eigenvectors[1][2], eigenvectors[2][2]}, {0.0, 0.0, 1.0});
    if (bone && bone->GetNumberOfPoints() > 0 && dot(sub(sampledCentroid(bone), centroid), z) < 0.0)
        z = mul(z, -1.0);
    const Vec3 major{eigenvectors[0][0], eigenvectors[1][0], eigenvectors[2][0]};
    const Vec3 x = normalized(sub(major, mul(z, dot(major, z))), {1.0, 0.0, 0.0});
    const Vec3 y = cross(z, x);

    double minX = std::numeric_limits<double>::max(), maxX = -minX;
    double minY = minX, maxY = -minX, minZ = minX;
    for (vtkIdType i = 0; i < n; ++i) {
        dentalScan->GetPoint(i, p);
        const Vec3 d = sub({p[0], p[1], p[2]}, centroid);
        minX = std::min(minX, dot(d, x));
        maxX = std::max(maxX, dot(d, x));
        minY = std::min(minY, dot(d, y));
        maxY = std::max(maxY, dot(d, y));
        minZ = std::min(minZ, dot(d, z));
    }

    constexpr double kSideMarginMm = 2.0;
    constexpr double kOcclusalMarginMm = 1.0;
    const double thickness = std::max(1.0, thicknessMm);
    const double bottom = minZ - kOcclusalMarginMm;
    const double top = bottom + thickness;
    block.center = add(centroid, add(mul(x, 0.5 * (minX + maxX)), add(mul(y, 0.5 * (minY + maxY)), mul(z, 0.5 * (bottom + top)))));
    block.axisX = x;
    block.axisY = y;
    block.axisZ = z;
    block.sizeMm = {maxX - minX + 2.0 * kSideMarginMm, maxY - minY + 2.0 * kSideMarginMm, thickness};
    block.valid = true;
    return block;
}

vtkSmartPointer<vtkMatrix4x4> BlockLocalToWorld(const CompositeCutBlock& block)
{
    auto m = vtkSmartPointer<vtkMatrix4x4>::New();
    m->Identity();
    for (int r = 0; r < 3; ++r) {
        m->SetElement(r, 0, block.axisX[static_cast<size_t>(r)]);
        m->SetElement(r, 1, block.axisY[static_cast<size_t>(r)]);
        m->SetElement(r, 2, block.axisZ[static_cast<size_t>(r)]);
        m->SetElement(r, 3, block.center[static_cast<size_t>(r)]);
    }
    return m;
}

vtkSmartPointer<vtkPolyData> BlockMesh(const CompositeCutBlock& block)
{
    auto cube = vtkSmartPointer<vtkCubeSource>::New();
    cube->SetXLength(block.sizeMm[0]);
    cube->SetYLength(block.sizeMm[1]);
    cube->SetZLength(block.sizeMm[2]);
    auto transform = vtkSmartPointer<vtkTransform>::New();
    transform->SetMatrix(BlockLocalToWorld(block));
    auto filter = vtkSmartPointer<vtkTransformPolyDataFilter>::New();
    filter->SetInputConnection(cube->GetOutputPort());
    filter->SetTransform(transform);
    filter->Update();
    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->DeepCopy(filter->GetOutput());
    return out;
}

CompositeCutBlock TransformBlock(const CompositeCutBlock& block, vtkMatrix4x4* matrix)
{
    if (!matrix || !block.valid)
        return block;
    const auto linear = [matrix](const Vec3& v) {
        Vec3 out{};
        for (int r = 0; r < 3; ++r)
            out[static_cast<size_t>(r)] = matrix->GetElement(r, 0) * v[0] + matrix->GetElement(r, 1) * v[1] +
                                          matrix->GetElement(r, 2) * v[2];
        return out;
    };
    const Vec3 ex = linear(mul(block.axisX, block.sizeMm[0]));
    const Vec3 ey = linear(mul(block.axisY, block.sizeMm[1]));
    const Vec3 ez = linear(mul(block.axisZ, block.sizeMm[2]));
    const double lx = length(ex), ly = length(ey), lz = length(ez);
    if (lx < 1e-9 || ly < 1e-9 || lz < 1e-9)
        return block;

    CompositeCutBlock out = block;
    double c[4] = {block.center[0], block.center[1], block.center[2], 1.0};
    double r[4] = {};
    matrix->MultiplyPoint(c, r);
    const double w = std::abs(r[3]) > 1e-12 ? r[3] : 1.0;
    out.center = {r[0] / w, r[1] / w, r[2] / w};
    out.axisX = mul(ex, 1.0 / lx);
    out.axisY = normalized(sub(ey, mul(out.axisX, dot(out.axisX, ey))), block.axisY);
    out.axisZ = cross(out.axisX, out.axisY);
    out.sizeMm = {lx, ly, lz};
    return out;
}

CompositeCutBlock WithSize(const CompositeCutBlock& block, double widthMm, double lengthMm, double thicknessMm)
{
    CompositeCutBlock out = block;
    out.sizeMm = {std::max(1.0, widthMm), std::max(1.0, lengthMm), std::max(1.0, thicknessMm)};
    return out;
}

bool Contains(const CompositeCutBlock& block, const double point[3], double toleranceMm)
{
    const Vec3 d = sub({point[0], point[1], point[2]}, block.center);
    return std::abs(dot(d, block.axisX)) <= 0.5 * block.sizeMm[0] + toleranceMm &&
           std::abs(dot(d, block.axisY)) <= 0.5 * block.sizeMm[1] + toleranceMm &&
           std::abs(dot(d, block.axisZ)) <= 0.5 * block.sizeMm[2] + toleranceMm;
}

bool HasParts(vtkPolyData* mesh)
{
    return mesh && mesh->GetCellData()->GetArray(PartArrayName) != nullptr;
}

vtkSmartPointer<vtkPolyData> TagPart(vtkPolyData* mesh, unsigned char part)
{
    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->DeepCopy(mesh);
    auto tags = vtkSmartPointer<vtkUnsignedCharArray>::New();
    tags->SetName(PartArrayName);
    tags->SetNumberOfTuples(out->GetNumberOfCells());
    tags->FillValue(part);
    out->GetCellData()->AddArray(tags);
    return out;
}

CompositeBlockResult CreateBlockComposite(vtkPolyData* bone, vtkPolyData* dentalScan, const CompositeCutBlock& block)
{
    CompositeBlockResult result;
    if (!block.valid) {
        result.error = QStringLiteral("El bloque de corte no es válido.");
        return result;
    }
    if (!bone || bone->GetNumberOfPolys() == 0) {
        result.error = QStringLiteral("Falta la malla de hueso del TAC.");
        return result;
    }
    if (!dentalScan || dentalScan->GetNumberOfPolys() == 0) {
        result.error = QStringLiteral("Falta el escaneo dental registrado.");
        return result;
    }

    // Exact cut with the six face planes (an implicit box is only evaluated at
    // vertices, so large triangles crossing the block would survive).
    struct FacePlane
    {
        Vec3 origin;
        Vec3 outward;
    };
    std::vector<FacePlane> faces;
    const std::array<Vec3, 3> axes{block.axisX, block.axisY, block.axisZ};
    for (size_t a = 0; a < 3; ++a) {
        const Vec3 half = mul(axes[a], 0.5 * block.sizeMm[a]);
        faces.push_back({add(block.center, half), axes[a]});
        faces.push_back({sub(block.center, half), mul(axes[a], -1.0)});
    }
    // Returns {outside, inside} of one face plane.
    const auto split = [](vtkPolyData* mesh, const FacePlane& face) {
        auto plane = vtkSmartPointer<vtkPlane>::New();
        plane->SetOrigin(face.origin[0], face.origin[1], face.origin[2]);
        plane->SetNormal(face.outward[0], face.outward[1], face.outward[2]);
        auto clipper = vtkSmartPointer<vtkClipPolyData>::New();
        clipper->SetInputData(mesh);
        clipper->SetClipFunction(plane);
        clipper->GenerateClippedOutputOn();
        clipper->Update();
        auto outside = vtkSmartPointer<vtkPolyData>::New();
        outside->DeepCopy(clipper->GetOutput());
        auto inside = vtkSmartPointer<vtkPolyData>::New();
        inside->DeepCopy(clipper->GetClippedOutput());
        return std::pair{outside, inside};
    };

    vtkSmartPointer<vtkPolyData> dentalInside = triangulated(dentalScan);
    for (const FacePlane& face : faces)
        dentalInside = split(dentalInside, face).second;

    auto boneOutside = vtkSmartPointer<vtkAppendPolyData>::New();
    vtkSmartPointer<vtkPolyData> boneRemainder = triangulated(bone);
    for (const FacePlane& face : faces) {
        auto [outside, inside] = split(boneRemainder, face);
        if (outside->GetNumberOfPolys() > 0)
            boneOutside->AddInputData(outside);
        boneRemainder = inside;
    }
    auto boneKept = vtkSmartPointer<vtkPolyData>::New();
    if (boneOutside->GetNumberOfInputConnections(0) > 0) {
        boneOutside->Update();
        boneKept->DeepCopy(boneOutside->GetOutput());
    }

    const auto bonePart = TagPart(boneKept, BonePart);
    const auto dentalPart = TagPart(dentalInside, DentalPart);
    result.boneCells = bonePart->GetNumberOfPolys();
    result.dentalCells = dentalPart->GetNumberOfPolys();
    if (result.dentalCells == 0) {
        result.error = QStringLiteral("El bloque no contiene el escaneo dental: ajústelo sobre los dientes.");
        return result;
    }
    if (result.boneCells == 0) {
        result.error = QStringLiteral("El bloque cubre todo el hueso: reduzca su tamaño.");
        return result;
    }

    auto append = vtkSmartPointer<vtkAppendPolyData>::New();
    append->AddInputData(bonePart);
    append->AddInputData(dentalPart);
    auto clean = vtkSmartPointer<vtkCleanPolyData>::New();
    clean->SetInputConnection(append->GetOutputPort());
    auto normals = vtkSmartPointer<vtkPolyDataNormals>::New();
    normals->SetInputConnection(clean->GetOutputPort());
    normals->ComputePointNormalsOn();
    normals->ComputeCellNormalsOff();
    normals->SplittingOff();
    normals->ConsistencyOn();
    normals->Update();

    result.composite = vtkSmartPointer<vtkPolyData>::New();
    result.composite->DeepCopy(normals->GetOutput());
    result.report = QStringLiteral("Bloque de %1 × %2 × %3 mm: hueso conservado %4 triángulos, escaneo dental %5 triángulos.")
                        .arg(block.sizeMm[0], 0, 'f', 1).arg(block.sizeMm[1], 0, 'f', 1).arg(block.sizeMm[2], 0, 'f', 1)
                        .arg(result.boneCells).arg(result.dentalCells);
    result.ok = true;
    return result;
}

namespace
{
struct ContourFrame
{
    std::vector<std::array<double, 2>> polygon; // block X–Y coordinates
    std::vector<double> height;                 // block Z of each point: the scan border line
    double occlusal = 0.0;                      // block Z of the occlusal face (beyond the cusps)
};

// Bone is replaced up to the line of the points plus this margin, so no CT crown edge remains.
constexpr double kContourBoneMarginMm = 0.5;

ContourFrame contourFrame(const CompositeCutBlock& block, const CompositeContour& contour)
{
    ContourFrame frame;
    frame.occlusal = -0.5 * block.sizeMm[2];
    for (const auto& p : contour) {
        const Vec3 d = sub(p, block.center);
        const std::array<double, 2> uv{dot(d, block.axisX), dot(d, block.axisY)};
        if (frame.polygon.empty() || std::hypot(uv[0] - frame.polygon.back()[0], uv[1] - frame.polygon.back()[1]) > 1e-6) {
            frame.polygon.push_back(uv);
            frame.height.push_back(dot(d, block.axisZ));
        }
    }
    return frame;
}

double polygonArea(const std::vector<std::array<double, 2>>& polygon)
{
    double area = 0.0;
    for (size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++)
        area += polygon[j][0] * polygon[i][1] - polygon[i][0] * polygon[j][1];
    return 0.5 * std::abs(area);
}

// Signed distance to the outline seen along the occlusal axis (negative inside).
double planarField(const ContourFrame& frame, double x, double y)
{
    double best = std::numeric_limits<double>::max();
    bool inside = false;
    const auto& poly = frame.polygon;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        const auto& a = poly[i];
        const auto& b = poly[j];
        if ((a[1] > y) != (b[1] > y) && x < (b[0] - a[0]) * (y - a[1]) / (b[1] - a[1]) + a[0])
            inside = !inside;
        const double ex = b[0] - a[0], ey = b[1] - a[1];
        const double len2 = ex * ex + ey * ey;
        const double t = len2 > 0.0 ? std::clamp(((x - a[0]) * ex + (y - a[1]) * ey) / len2, 0.0, 1.0) : 0.0;
        best = std::min(best, std::hypot(x - (a[0] + t * ex), y - (a[1] + t * ey)));
    }
    return inside ? -best : best;
}

// Height of the scan border line above (x, y): inverse-distance weighting of the points.
double lineHeight(const ContourFrame& frame, double x, double y)
{
    double weights = 0.0;
    double sum = 0.0;
    for (size_t i = 0; i < frame.polygon.size(); ++i) {
        const double dx = x - frame.polygon[i][0];
        const double dy = y - frame.polygon[i][1];
        const double d2 = dx * dx + dy * dy;
        if (d2 < 1e-12)
            return frame.height[i];
        weights += 1.0 / d2;
        sum += frame.height[i] / d2;
    }
    return weights > 0.0 ? sum / weights : 0.0;
}

// Bone replaced by the scan: inside the outline, from the occlusal side up to the line of the points.
double contourFieldLocal(const ContourFrame& frame, const Vec3& d, const CompositeCutBlock& block)
{
    const double x = dot(d, block.axisX);
    const double y = dot(d, block.axisY);
    const double z = dot(d, block.axisZ);
    const double planar = planarField(frame, x, y);
    const double vertical = z - (lineHeight(frame, x, y) + kContourBoneMarginMm);
    if (planar <= 0.0 && vertical <= 0.0)
        return std::max(planar, vertical);
    return std::hypot(std::max(planar, 0.0), std::max(vertical, 0.0));
}

// Conforming midpoint refinement of triangle edges longer than maxEdgeMm that
// touch the box [lo, hi]: clipping by a curved field only interpolates along
// edges, so long edges crossing the contour must be split first.
vtkSmartPointer<vtkPolyData> refineNear(vtkPolyData* input, const Vec3& lo, const Vec3& hi, double maxEdgeMm)
{
    auto mesh = triangulated(input);
    auto points = vtkSmartPointer<vtkPoints>::New();
    points->DeepCopy(mesh->GetPoints());
    std::vector<std::array<vtkIdType, 3>> tris;
    tris.reserve(static_cast<size_t>(mesh->GetNumberOfPolys()));
    {
        auto it = vtk::TakeSmartPointer(mesh->GetPolys()->NewIterator());
        for (it->GoToFirstCell(); !it->IsDoneWithTraversal(); it->GoToNextCell()) {
            vtkIdType npts = 0;
            const vtkIdType* ids = nullptr;
            it->GetCurrentCell(npts, ids);
            if (npts == 3)
                tris.push_back({ids[0], ids[1], ids[2]});
        }
    }
    const double max2 = maxEdgeMm * maxEdgeMm;
    const auto touches = [&](const Vec3& a, const Vec3& b) {
        for (size_t k = 0; k < 3; ++k)
            if (std::max(a[k], b[k]) < lo[k] || std::min(a[k], b[k]) > hi[k])
                return false;
        return true;
    };
    for (int pass = 0; pass < 12; ++pass) {
        std::unordered_map<unsigned long long, vtkIdType> midpoints;
        const auto key = [](vtkIdType a, vtkIdType b) {
            return (static_cast<unsigned long long>(std::min(a, b)) << 32) ^ static_cast<unsigned long long>(std::max(a, b));
        };
        const auto midpoint = [&](vtkIdType a, vtkIdType b) -> vtkIdType {
            const auto found = midpoints.find(key(a, b));
            return found == midpoints.end() ? -1 : found->second;
        };
        Vec3 pa{}, pb{};
        for (const auto& t : tris) {
            for (int e = 0; e < 3; ++e) {
                const vtkIdType a = t[static_cast<size_t>(e)];
                const vtkIdType b = t[static_cast<size_t>((e + 1) % 3)];
                if (midpoints.count(key(a, b)))
                    continue;
                points->GetPoint(a, pa.data());
                points->GetPoint(b, pb.data());
                const Vec3 d = sub(pb, pa);
                if (dot(d, d) > max2 && touches(pa, pb))
                    midpoints[key(a, b)] = points->InsertNextPoint(mul(add(pa, pb), 0.5).data());
            }
        }
        if (midpoints.empty())
            break;
        std::vector<std::array<vtkIdType, 3>> next;
        next.reserve(tris.size() * 2);
        for (const auto& t : tris) {
            std::array<vtkIdType, 3> m{midpoint(t[0], t[1]), midpoint(t[1], t[2]), midpoint(t[2], t[0])};
            const int splits = (m[0] >= 0) + (m[1] >= 0) + (m[2] >= 0);
            if (splits == 0) {
                next.push_back(t);
            } else if (splits == 3) {
                next.push_back({t[0], m[0], m[2]});
                next.push_back({m[0], t[1], m[1]});
                next.push_back({m[2], m[1], t[2]});
                next.push_back({m[0], m[1], m[2]});
            } else {
                // Rotate so that edge 0 (v0-v1) is split.
                int r = 0;
                while (m[static_cast<size_t>(r)] < 0)
                    ++r;
                const vtkIdType v0 = t[static_cast<size_t>(r)], v1 = t[static_cast<size_t>((r + 1) % 3)],
                                v2 = t[static_cast<size_t>((r + 2) % 3)];
                const vtkIdType m01 = m[static_cast<size_t>(r)];
                const vtkIdType m12 = m[static_cast<size_t>((r + 1) % 3)];
                const vtkIdType m20 = m[static_cast<size_t>((r + 2) % 3)];
                if (splits == 1) {
                    next.push_back({v0, m01, v2});
                    next.push_back({m01, v1, v2});
                } else if (m12 >= 0) {
                    next.push_back({v0, m01, v2});
                    next.push_back({m01, v1, m12});
                    next.push_back({m01, m12, v2});
                } else { // m20
                    next.push_back({v0, m01, m20});
                    next.push_back({m01, v1, v2});
                    next.push_back({m20, m01, v2});
                }
            }
        }
        tris.swap(next);
    }
    auto polys = vtkSmartPointer<vtkCellArray>::New();
    for (const auto& t : tris)
        polys->InsertNextCell(3, t.data());
    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->SetPoints(points);
    out->SetPolys(polys);
    return out;
}

// Mesh part where the field is below (inside) or above (outside) zero.
vtkSmartPointer<vtkPolyData> clipByField(vtkPolyData* mesh, const std::function<double(const Vec3&)>& field, bool inside)
{
    auto work = vtkSmartPointer<vtkPolyData>::New();
    work->ShallowCopy(mesh);
    const vtkIdType n = work->GetNumberOfPoints();
    auto values = vtkSmartPointer<vtkDoubleArray>::New();
    values->SetName("CompositeContourField");
    values->SetNumberOfTuples(n);
    parallelFor(static_cast<int>(n), [&](int begin, int end) {
        double p[3] = {};
        for (int i = begin; i < end; ++i) {
            work->GetPoint(i, p);
            values->SetValue(i, field({p[0], p[1], p[2]}));
        }
    });
    work->GetPointData()->AddArray(values);
    work->GetPointData()->SetActiveScalars("CompositeContourField");
    auto clipper = vtkSmartPointer<vtkClipPolyData>::New();
    clipper->SetInputData(work);
    clipper->SetValue(0.0);
    clipper->SetInsideOut(inside);
    clipper->GenerateClipScalarsOff();
    clipper->Update();
    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->DeepCopy(clipper->GetOutput());
    out->GetPointData()->RemoveArray("CompositeContourField");
    return out;
}

vtkSmartPointer<vtkPolyData> mergeTaggedParts(vtkPolyData* bonePart, vtkPolyData* dentalPart)
{
    auto append = vtkSmartPointer<vtkAppendPolyData>::New();
    append->AddInputData(bonePart);
    append->AddInputData(dentalPart);
    auto clean = vtkSmartPointer<vtkCleanPolyData>::New();
    clean->SetInputConnection(append->GetOutputPort());
    auto normals = vtkSmartPointer<vtkPolyDataNormals>::New();
    normals->SetInputConnection(clean->GetOutputPort());
    normals->ComputePointNormalsOn();
    normals->ComputeCellNormalsOff();
    normals->SplittingOff();
    normals->ConsistencyOn();
    normals->Update();
    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->DeepCopy(normals->GetOutput());
    return out;
}
} // namespace

bool ContourValid(const CompositeCutBlock& block, const CompositeContour& contour, QString* error)
{
    if (!block.valid) {
        if (error)
            *error = QStringLiteral("Falta el marco del escaneo para el contorno.");
        return false;
    }
    const ContourFrame frame = contourFrame(block, contour);
    if (static_cast<int>(frame.polygon.size()) < MinContourPoints) {
        if (error)
            *error = QStringLiteral("Marque al menos %1 puntos alrededor del escaneo.").arg(MinContourPoints);
        return false;
    }
    if (polygonArea(frame.polygon) < 1.0) {
        if (error)
            *error = QStringLiteral("Los puntos del contorno están alineados: rodee el escaneo.");
        return false;
    }
    return true;
}

double ContourField(const CompositeCutBlock& block, const CompositeContour& contour, const double point[3])
{
    const ContourFrame frame = contourFrame(block, contour);
    if (frame.polygon.size() < 3)
        return std::numeric_limits<double>::max();
    return contourFieldLocal(frame, sub({point[0], point[1], point[2]}, block.center), block);
}

double ContourScanField(const CompositeCutBlock& block, const CompositeContour& contour, const double point[3])
{
    const ContourFrame frame = contourFrame(block, contour);
    if (frame.polygon.size() < 3)
        return std::numeric_limits<double>::max();
    const Vec3 d = sub({point[0], point[1], point[2]}, block.center);
    return planarField(frame, dot(d, block.axisX), dot(d, block.axisY));
}

vtkSmartPointer<vtkPolyData> ContourWallMesh(const CompositeCutBlock& block, const CompositeContour& contour)
{
    const ContourFrame frame = contourFrame(block, contour);
    auto points = vtkSmartPointer<vtkPoints>::New();
    auto polys = vtkSmartPointer<vtkCellArray>::New();
    const auto world = [&](const std::array<double, 2>& uv, double z) {
        return add(block.center, add(mul(block.axisX, uv[0]), add(mul(block.axisY, uv[1]), mul(block.axisZ, z))));
    };
    const size_t n = frame.polygon.size();
    // From the occlusal face up to each point: where the scan replaces the CT teeth.
    for (size_t i = 0; i < frame.polygon.size(); ++i) {
        points->InsertNextPoint(world(frame.polygon[i], frame.occlusal).data());
        points->InsertNextPoint(world(frame.polygon[i], frame.height[i]).data());
    }
    if (n >= 2) {
        for (size_t i = 0; i < n; ++i) {
            const vtkIdType a = static_cast<vtkIdType>(2 * i);
            const vtkIdType b = static_cast<vtkIdType>(2 * ((i + 1) % n));
            const vtkIdType quad[4] = {a, b, b + 1, a + 1};
            if (n > 2 || i == 0)
                polys->InsertNextCell(4, quad);
        }
    }
    auto mesh = vtkSmartPointer<vtkPolyData>::New();
    mesh->SetPoints(points);
    mesh->SetPolys(polys);
    return mesh;
}

vtkSmartPointer<vtkPolyData> ContourPolyline(const CompositeCutBlock& block, const CompositeContour& contour)
{
    const ContourFrame frame = contourFrame(block, contour);
    auto points = vtkSmartPointer<vtkPoints>::New();
    auto lines = vtkSmartPointer<vtkCellArray>::New();
    std::vector<vtkIdType> ids;
    // Through the points themselves, so the outline follows the scan border.
    for (size_t i = 0; i < frame.polygon.size(); ++i) {
        const auto& uv = frame.polygon[i];
        const Vec3 p = add(block.center, add(mul(block.axisX, uv[0]), add(mul(block.axisY, uv[1]), mul(block.axisZ, frame.height[i]))));
        ids.push_back(points->InsertNextPoint(p.data()));
    }
    if (ids.size() >= 2) {
        if (ids.size() >= 3)
            ids.push_back(ids.front());
        lines->InsertNextCell(static_cast<vtkIdType>(ids.size()), ids.data());
    }
    auto polyline = vtkSmartPointer<vtkPolyData>::New();
    polyline->SetPoints(points);
    polyline->SetLines(lines);
    return polyline;
}

CompositeBlockResult CreateContourComposite(vtkPolyData* bone, vtkPolyData* dentalScan, const CompositeCutBlock& block,
                                            const CompositeContour& contour)
{
    CompositeBlockResult result;
    if (!ContourValid(block, contour, &result.error))
        return result;
    if (!bone || bone->GetNumberOfPolys() == 0) {
        result.error = QStringLiteral("Falta la malla de hueso del TAC.");
        return result;
    }
    if (!dentalScan || dentalScan->GetNumberOfPolys() == 0) {
        result.error = QStringLiteral("Falta el escaneo dental registrado.");
        return result;
    }
    const ContourFrame frame = contourFrame(block, contour);
    // Scan: everything inside the outline, at any height. Bone: inside the outline up to the line of the points.
    const auto boneField = [&frame, &block](const Vec3& p) { return contourFieldLocal(frame, sub(p, block.center), block); };
    const auto scanField = [&frame, &block](const Vec3& p) {
        const Vec3 d = sub(p, block.center);
        return planarField(frame, dot(d, block.axisX), dot(d, block.axisY));
    };

    // Region around the outline walls, where long edges are refined before clipping.
    const double top = *std::max_element(frame.height.begin(), frame.height.end()) + kContourBoneMarginMm;
    Vec3 lo{std::numeric_limits<double>::max(), std::numeric_limits<double>::max(), std::numeric_limits<double>::max()};
    Vec3 hi = mul(lo, -1.0);
    for (const auto& uv : frame.polygon)
        for (double z : {std::min(frame.occlusal, top), top}) {
            const Vec3 p = add(block.center, add(mul(block.axisX, uv[0]), add(mul(block.axisY, uv[1]), mul(block.axisZ, z))));
            for (size_t k = 0; k < 3; ++k) {
                lo[k] = std::min(lo[k], p[k] - 2.0);
                hi[k] = std::max(hi[k], p[k] + 2.0);
            }
        }
    Vec3 scanLo = lo;
    Vec3 scanHi = hi;
    double scanBounds[6];
    dentalScan->GetBounds(scanBounds);
    for (size_t k = 0; k < 3; ++k) {
        scanLo[k] = std::min(scanLo[k], scanBounds[2 * k] - 2.0);
        scanHi[k] = std::max(scanHi[k], scanBounds[2 * k + 1] + 2.0);
    }
    constexpr double kMaxEdgeMm = 1.0;
    const auto dentalPart = TagPart(clipByField(refineNear(dentalScan, scanLo, scanHi, kMaxEdgeMm), scanField, true), DentalPart);
    const auto bonePart = TagPart(clipByField(refineNear(bone, lo, hi, kMaxEdgeMm), boneField, false), BonePart);
    result.boneCells = bonePart->GetNumberOfPolys();
    result.dentalCells = dentalPart->GetNumberOfPolys();
    if (result.dentalCells == 0) {
        result.error = QStringLiteral("El contorno no contiene el escaneo dental: rodee los dientes con los puntos.");
        return result;
    }
    if (result.boneCells == 0) {
        result.error = QStringLiteral("El contorno cubre todo el hueso: acérquelo a los dientes.");
        return result;
    }
    result.composite = mergeTaggedParts(bonePart, dentalPart);
    result.report = QStringLiteral("Contorno de %1 puntos (%2 mm²): hueso sustituido hasta la línea de los puntos "
                                   "(%3 triángulos conservados), escaneo dental %4 triángulos.")
                        .arg(frame.polygon.size()).arg(polygonArea(frame.polygon), 0, 'f', 0)
                        .arg(result.boneCells).arg(result.dentalCells);
    result.ok = true;
    return result;
}

QJsonArray ContourToJson(const CompositeContour& contour)
{
    QJsonArray array;
    for (const auto& p : contour)
        array.append(vecJson(p));
    return array;
}

CompositeContour ContourFromJson(const QJsonArray& array)
{
    CompositeContour contour;
    for (const QJsonValue& value : array) {
        Vec3 p{};
        if (vecFrom(value, p))
            contour.push_back(p);
    }
    return contour;
}

vtkSmartPointer<vtkPolyData> ExtractPart(vtkPolyData* mesh, unsigned char part)
{
    if (!HasParts(mesh))
        return nullptr;
    vtkDataArray* parts = mesh->GetCellData()->GetArray(PartArrayName);
    auto polys = vtkSmartPointer<vtkCellArray>::New();
    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->SetPoints(mesh->GetPoints());
    out->GetCellData()->CopyAllocate(mesh->GetCellData());

    const vtkIdType offset = mesh->GetNumberOfVerts() + mesh->GetNumberOfLines();
    vtkIdType polyIndex = 0;
    vtkIdType kept = 0;
    auto it = vtk::TakeSmartPointer(mesh->GetPolys()->NewIterator());
    for (it->GoToFirstCell(); !it->IsDoneWithTraversal(); it->GoToNextCell(), ++polyIndex) {
        const vtkIdType cellId = offset + polyIndex;
        if (std::lround(parts->GetTuple1(cellId)) != part)
            continue;
        vtkIdType npts = 0;
        const vtkIdType* ids = nullptr;
        it->GetCurrentCell(npts, ids);
        polys->InsertNextCell(npts, ids);
        out->GetCellData()->CopyData(mesh->GetCellData(), cellId, kept++);
    }
    if (kept == 0)
        return nullptr;
    out->SetPolys(polys);

    auto clean = vtkSmartPointer<vtkCleanPolyData>::New();
    clean->SetInputData(out);
    clean->Update();
    auto result = vtkSmartPointer<vtkPolyData>::New();
    result->DeepCopy(clean->GetOutput());
    return result;
}

vtkSmartPointer<vtkPolyData> PartColoredCopy(vtkPolyData* mesh, const std::array<unsigned char, 3>& boneColor,
                                             const std::array<unsigned char, 3>& dentalColor)
{
    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->DeepCopy(mesh);
    vtkDataArray* parts = out->GetCellData()->GetArray(PartArrayName);
    if (!parts)
        return out;
    auto colors = vtkSmartPointer<vtkUnsignedCharArray>::New();
    colors->SetName("PartColor");
    colors->SetNumberOfComponents(3);
    colors->SetNumberOfTuples(out->GetNumberOfCells());
    for (vtkIdType i = 0; i < out->GetNumberOfCells(); ++i)
        colors->SetTypedTuple(i, std::lround(parts->GetTuple1(i)) == DentalPart ? dentalColor.data() : boneColor.data());
    out->GetCellData()->SetScalars(colors);
    return out;
}

vtkSmartPointer<vtkPolyData> SliceContour(vtkPolyData* mesh, int axis, double position)
{
    auto empty = vtkSmartPointer<vtkPolyData>::New();
    if (!mesh || mesh->GetNumberOfPolys() == 0 || axis < 0 || axis > 2)
        return empty;
    double bounds[6] = {};
    mesh->GetBounds(bounds);
    if (position < bounds[2 * axis] || position > bounds[2 * axis + 1])
        return empty;

    auto plane = vtkSmartPointer<vtkPlane>::New();
    double origin[3] = {0.0, 0.0, 0.0};
    double normal[3] = {0.0, 0.0, 0.0};
    origin[axis] = position;
    normal[axis] = 1.0;
    plane->SetOrigin(origin);
    plane->SetNormal(normal);
    auto cutter = vtkSmartPointer<vtkCutter>::New();
    cutter->SetInputData(mesh);
    cutter->SetCutFunction(plane);
    auto stripper = vtkSmartPointer<vtkStripper>::New();
    stripper->SetInputConnection(cutter->GetOutputPort());
    stripper->JoinContiguousSegmentsOn();
    stripper->Update();
    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->DeepCopy(stripper->GetOutput());
    return out;
}

VoxelUnionResult VoxelUnion(const std::vector<vtkPolyData*>& meshes, double spacingMm, int closingVoxels,
                            const std::atomic<bool>* cancel)
{
    VoxelUnionResult result;
    const auto cancelled = [cancel] { return cancel && cancel->load(); };
    double bounds[6] = {std::numeric_limits<double>::max(), -std::numeric_limits<double>::max(),
                        std::numeric_limits<double>::max(), -std::numeric_limits<double>::max(),
                        std::numeric_limits<double>::max(), -std::numeric_limits<double>::max()};
    bool any = false;
    for (vtkPolyData* mesh : meshes) {
        if (!mesh || mesh->GetNumberOfPolys() == 0)
            continue;
        double b[6] = {};
        mesh->GetBounds(b);
        for (int a = 0; a < 3; ++a) {
            bounds[2 * a] = std::min(bounds[2 * a], b[2 * a]);
            bounds[2 * a + 1] = std::max(bounds[2 * a + 1], b[2 * a + 1]);
        }
        any = true;
    }
    if (!any) {
        result.error = QStringLiteral("No hay mallas para unir.");
        return result;
    }

    closingVoxels = std::clamp(closingVoxels, 0, 10);
    double h = std::max(0.05, spacingMm);
    std::array<int, 3> n{};
    double pad = 0.0;
    const auto computeGrid = [&] {
        pad = (closingVoxels + 2) * h;
        for (int a = 0; a < 3; ++a)
            n[static_cast<size_t>(a)] = static_cast<int>(std::ceil((bounds[2 * a + 1] - bounds[2 * a] + 2.0 * pad) / h)) + 1;
    };
    computeGrid();
    constexpr double kMaxVoxels = 40.0e6;
    const double requested = static_cast<double>(n[0]) * n[1] * n[2];
    if (requested > kMaxVoxels) {
        h *= std::cbrt(requested / kMaxVoxels) * 1.01;
        computeGrid();
    }
    const size_t nx = static_cast<size_t>(n[0]), ny = static_cast<size_t>(n[1]), nz = static_cast<size_t>(n[2]);
    const size_t total = nx * ny * nz;
    const double ox = bounds[0] - pad, oy = bounds[2] - pad, oz = bounds[4] - pad;
    const auto index = [nx, ny](size_t i, size_t j, size_t k) { return i + nx * (j + ny * k); };

    // Shells: sample every triangle at half-voxel steps.
    std::vector<uint8_t> shell(total, 0);
    for (vtkPolyData* mesh : meshes) {
        if (!mesh || mesh->GetNumberOfPolys() == 0)
            continue;
        const auto tri = triangulated(mesh);
        auto it = vtk::TakeSmartPointer(tri->GetPolys()->NewIterator());
        vtkIdType counter = 0;
        double a[3] = {}, b[3] = {}, c[3] = {};
        for (it->GoToFirstCell(); !it->IsDoneWithTraversal(); it->GoToNextCell()) {
            if ((++counter % 20000) == 0 && cancelled()) {
                result.error = QStringLiteral("Cálculo cancelado.");
                return result;
            }
            vtkIdType npts = 0;
            const vtkIdType* ids = nullptr;
            it->GetCurrentCell(npts, ids);
            if (npts != 3)
                continue;
            tri->GetPoint(ids[0], a);
            tri->GetPoint(ids[1], b);
            tri->GetPoint(ids[2], c);
            const double edge = std::max({std::sqrt(vtkMath::Distance2BetweenPoints(a, b)),
                                          std::sqrt(vtkMath::Distance2BetweenPoints(b, c)),
                                          std::sqrt(vtkMath::Distance2BetweenPoints(c, a))});
            const int steps = std::max(1, static_cast<int>(std::ceil(edge / (0.5 * h))));
            for (int s = 0; s <= steps; ++s) {
                for (int t = 0; s + t <= steps; ++t) {
                    const double u = static_cast<double>(s) / steps;
                    const double v = static_cast<double>(t) / steps;
                    const double px = a[0] + (b[0] - a[0]) * u + (c[0] - a[0]) * v;
                    const double py = a[1] + (b[1] - a[1]) * u + (c[1] - a[1]) * v;
                    const double pz = a[2] + (b[2] - a[2]) * u + (c[2] - a[2]) * v;
                    const long i = std::lround((px - ox) / h);
                    const long j = std::lround((py - oy) / h);
                    const long k = std::lround((pz - oz) / h);
                    if (i >= 0 && j >= 0 && k >= 0 && i < n[0] && j < n[1] && k < n[2])
                        shell[index(static_cast<size_t>(i), static_cast<size_t>(j), static_cast<size_t>(k))] = 1;
                }
            }
        }
    }

    const auto morph = [&](const std::vector<uint8_t>& src, bool dilate) {
        std::vector<uint8_t> dst(src);
        parallelFor(n[2], [&](int kBegin, int kEnd) {
            for (size_t k = static_cast<size_t>(kBegin); k < static_cast<size_t>(kEnd); ++k)
                for (size_t j = 0; j < ny; ++j)
                    for (size_t i = 0; i < nx; ++i) {
                        const size_t idx = index(i, j, k);
                        if (dilate == (src[idx] != 0))
                            continue;
                        const bool border = i == 0 || j == 0 || k == 0 || i + 1 == nx || j + 1 == ny || k + 1 == nz;
                        bool neighbour = false;
                        if (border) {
                            neighbour = !dilate; // outside the grid counts as empty
                        }
                        if (!neighbour && i > 0) neighbour = (src[idx - 1] != 0) == dilate;
                        if (!neighbour && i + 1 < nx) neighbour = (src[idx + 1] != 0) == dilate;
                        if (!neighbour && j > 0) neighbour = (src[idx - nx] != 0) == dilate;
                        if (!neighbour && j + 1 < ny) neighbour = (src[idx + nx] != 0) == dilate;
                        if (!neighbour && k > 0) neighbour = (src[idx - nx * ny] != 0) == dilate;
                        if (!neighbour && k + 1 < nz) neighbour = (src[idx + nx * ny] != 0) == dilate;
                        if (neighbour)
                            dst[idx] = dilate ? 1 : 0;
                    }
        });
        return dst;
    };

    for (int iter = 0; iter < closingVoxels; ++iter)
        shell = morph(shell, true);
    if (cancelled()) {
        result.error = QStringLiteral("Cálculo cancelado.");
        return result;
    }

    // Everything not reachable from the border through empty voxels is solid.
    std::vector<uint8_t> solid(total, 1);
    std::vector<uint32_t> stack;
    const auto push = [&](size_t idx) {
        if (solid[idx] && !shell[idx]) {
            solid[idx] = 0;
            stack.push_back(static_cast<uint32_t>(idx));
        }
    };
    for (size_t k = 0; k < nz; ++k)
        for (size_t j = 0; j < ny; ++j)
            for (size_t i = 0; i < nx; ++i)
                if (i == 0 || j == 0 || k == 0 || i + 1 == nx || j + 1 == ny || k + 1 == nz)
                    push(index(i, j, k));
    while (!stack.empty()) {
        const size_t idx = stack.back();
        stack.pop_back();
        const size_t i = idx % nx;
        const size_t j = (idx / nx) % ny;
        const size_t k = idx / (nx * ny);
        if (i > 0) push(idx - 1);
        if (i + 1 < nx) push(idx + 1);
        if (j > 0) push(idx - nx);
        if (j + 1 < ny) push(idx + nx);
        if (k > 0) push(idx - nx * ny);
        if (k + 1 < nz) push(idx + nx * ny);
    }
    std::vector<uint8_t>().swap(shell);
    for (int iter = 0; iter < closingVoxels; ++iter)
        solid = morph(solid, false);
    if (cancelled()) {
        result.error = QStringLiteral("Cálculo cancelado.");
        return result;
    }

    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetDimensions(n[0], n[1], n[2]);
    image->SetOrigin(ox, oy, oz);
    image->SetSpacing(h, h, h);
    image->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    auto* voxels = static_cast<unsigned char*>(image->GetScalarPointer());
    size_t filled = 0;
    for (size_t idx = 0; idx < total; ++idx) {
        voxels[idx] = solid[idx] ? 255 : 0;
        filled += solid[idx];
    }
    std::vector<uint8_t>().swap(solid);
    if (filled == 0) {
        result.error = QStringLiteral("La unión por vóxeles quedó vacía.");
        return result;
    }

    auto surface = vtkSmartPointer<vtkFlyingEdges3D>::New();
    surface->SetInputData(image);
    surface->SetValue(0, 127.5);
    surface->ComputeNormalsOff();
    surface->ComputeGradientsOff();
    surface->ComputeScalarsOff();
    auto smooth = vtkSmartPointer<vtkWindowedSincPolyDataFilter>::New();
    smooth->SetInputConnection(surface->GetOutputPort());
    smooth->SetNumberOfIterations(20);
    smooth->SetPassBand(0.1);
    smooth->BoundarySmoothingOff();
    smooth->FeatureEdgeSmoothingOff();
    smooth->NonManifoldSmoothingOn();
    smooth->NormalizeCoordinatesOn();
    auto normals = vtkSmartPointer<vtkPolyDataNormals>::New();
    normals->SetInputConnection(smooth->GetOutputPort());
    normals->ConsistencyOn();
    normals->AutoOrientNormalsOn();
    normals->SplittingOff();
    normals->Update();

    result.mesh = vtkSmartPointer<vtkPolyData>::New();
    result.mesh->DeepCopy(normals->GetOutput());
    result.spacingMm = h;
    result.report = QStringLiteral("Unión por vóxeles a %1 mm (%2 × %3 × %4 vóxeles): %5 triángulos.")
                        .arg(h, 0, 'f', 2).arg(n[0]).arg(n[1]).arg(n[2]).arg(result.mesh->GetNumberOfPolys());
    result.ok = result.mesh->GetNumberOfPolys() > 0;
    if (!result.ok)
        result.error = QStringLiteral("La unión por vóxeles no generó superficie.");
    return result;
}

QJsonObject BlockToJson(const CompositeCutBlock& block)
{
    QJsonObject o;
    o[QStringLiteral("center")] = vecJson(block.center);
    o[QStringLiteral("axisX")] = vecJson(block.axisX);
    o[QStringLiteral("axisY")] = vecJson(block.axisY);
    o[QStringLiteral("axisZ")] = vecJson(block.axisZ);
    o[QStringLiteral("sizeMm")] = vecJson(block.sizeMm);
    o[QStringLiteral("valid")] = block.valid;
    return o;
}

CompositeCutBlock BlockFromJson(const QJsonObject& o)
{
    CompositeCutBlock block;
    const bool ok = vecFrom(o.value(QStringLiteral("center")), block.center) &&
                    vecFrom(o.value(QStringLiteral("axisX")), block.axisX) &&
                    vecFrom(o.value(QStringLiteral("axisY")), block.axisY) &&
                    vecFrom(o.value(QStringLiteral("axisZ")), block.axisZ) &&
                    vecFrom(o.value(QStringLiteral("sizeMm")), block.sizeMm);
    block.valid = ok && o.value(QStringLiteral("valid")).toBool(false);
    if (!block.valid)
        return CompositeCutBlock{};
    return block;
}
} // namespace CompositeBlockCore
