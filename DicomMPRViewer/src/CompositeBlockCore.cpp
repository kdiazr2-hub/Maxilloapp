#include "CompositeBlockCore.h"

#include <vtkAppendPolyData.h>
#include <vtkCellArray.h>
#include <vtkCellArrayIterator.h>
#include <vtkCellData.h>
#include <vtkCleanPolyData.h>
#include <vtkContourTriangulator.h>
#include <vtkIdList.h>
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
#include <vtkGenericCell.h>
#include <vtkPolyDataNormals.h>
#include <vtkStaticCellLocator.h>
#include <vtkStripper.h>
#include <vtkTransform.h>
#include <vtkTransformPolyDataFilter.h>
#include <vtkTriangleFilter.h>
#include <vtkUnsignedCharArray.h>
#include <vtkWindowedSincPolyDataFilter.h>

#include <QJsonArray>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <limits>
#include <set>
#include <thread>
#include <unordered_map>
#include <utility>

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

Vec3 jawAxisOf(CompositeJaw jaw)
{
    return jaw == CompositeJaw::Lower ? Vec3{0.0, 0.0, -1.0} : Vec3{0.0, 0.0, 1.0};
}

// Columns that hold a cusp, not the cheek. The most occlusal points are always
// cusps. So is a column that stands above the gingiva around it, even when that
// tooth sits further toward the bone than the rest of the arch: one plane from
// the single highest cusp is what slices those crowns. A flat vestibule does
// not stand above its neighbours, so it keeps the nearest tooth's cusp.
void markCrownColumns(std::vector<char>& tooth, const std::vector<double>& cellMin, int nu, int nv, double global)
{
    const int count = nu * nv;
    tooth.assign(static_cast<size_t>(count), 0);
    if (!(global < std::numeric_limits<double>::infinity()))
        return;
    for (int i = 0; i < count; ++i) {
        if (cellMin[static_cast<size_t>(i)] <= global + 6.0)
            tooth[static_cast<size_t>(i)] = 1;
    }
    auto at = [&](int iu, int iv) { return iv * nu + iu; };
    for (int iv = 0; iv < nv; ++iv) {
        for (int iu = 0; iu < nu; ++iu) {
            const double height = cellMin[static_cast<size_t>(at(iu, iv))];
            if (!(height < std::numeric_limits<double>::infinity()))
                continue;
            bool aboveGingiva = false;
            bool underAnotherCusp = false;
            for (int dv = -1; dv <= 1; ++dv) {
                for (int du = -1; du <= 1; ++du) {
                    if (du == 0 && dv == 0)
                        continue;
                    const int ju = iu + du;
                    const int jv = iv + dv;
                    if (ju < 0 || jv < 0 || ju >= nu || jv >= nv)
                        continue;
                    const double neighbor = cellMin[static_cast<size_t>(at(ju, jv))];
                    if (!(neighbor < std::numeric_limits<double>::infinity()))
                        continue;
                    if (neighbor > height + 0.8)
                        aboveGingiva = true;
                    if (neighbor + 0.3 < height)
                        underAnotherCusp = true;
                }
            }
            if (aboveGingiva && !underAnotherCusp)
                tooth[static_cast<size_t>(at(iu, iv))] = 1;
        }
    }
}

// A loop is the rim of the model when its two sides face each other across a few
// millimetres (a tooth socket, the cut top of a narrow wall). A horseshoe whose
// interior is the palate is much wider: filling that draws the V in the vault.
constexpr double kRimCloseMm = 9.0;

bool loopClosesOnItself(const std::vector<vtkIdType>& loop, vtkPoints* points, const Vec3& u, const Vec3& v)
{
    const size_t n = loop.size();
    if (n < 3)
        return false;
    std::vector<std::pair<double, double>> uv(n);
    std::vector<double> step(n, 0.0);
    double minU = std::numeric_limits<double>::max();
    double minV = minU;
    double maxU = -minU;
    double maxV = -minU;
    for (size_t i = 0; i < n; ++i) {
        double p[3] = {};
        points->GetPoint(loop[i], p);
        const Vec3 point{p[0], p[1], p[2]};
        uv[i] = {dot(point, u), dot(point, v)};
        minU = std::min(minU, uv[i].first);
        maxU = std::max(maxU, uv[i].first);
        minV = std::min(minV, uv[i].second);
        maxV = std::max(maxV, uv[i].second);
    }
    if (maxU - minU <= kRimCloseMm && maxV - minV <= kRimCloseMm)
        return true;
    double span = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const size_t j = (i + 1) % n;
        step[i] = std::hypot(uv[j].first - uv[i].first, uv[j].second - uv[i].second);
        span += step[i];
    }
    const size_t samples = std::min(n, static_cast<size_t>(48));
    double widest = 0.0;
    bool sawOpposite = false;
    for (size_t s = 0; s < samples; ++s) {
        const size_t i = s * n / samples;
        const size_t prev = (i + n - 1) % n;
        const size_t next = (i + 1) % n;
        double tx = uv[next].first - uv[prev].first;
        double ty = uv[next].second - uv[prev].second;
        const double tlen = std::hypot(tx, ty);
        if (tlen < 1e-9)
            continue;
        tx /= tlen;
        ty /= tlen;
        double along = 0.0;
        double best = std::numeric_limits<double>::infinity();
        for (size_t k = 1; k < n; ++k) {
            const size_t j = (i + k) % n;
            along += step[(j + n - 1) % n];
            if (std::min(along, span - along) < kRimCloseMm)
                continue;
            const double dx = uv[j].first - uv[i].first;
            const double dy = uv[j].second - uv[i].second;
            const double dist = std::hypot(dx, dy);
            if (dist < 1e-6)
                continue;
            // A point still travelling along the rim is not the other side of the wall.
            if (std::abs(dx * tx + dy * ty) / dist > 0.55)
                continue;
            best = std::min(best, dist);
        }
        if (best < std::numeric_limits<double>::infinity()) {
            sawOpposite = true;
            widest = std::max(widest, best);
        }
    }
    return sawOpposite && widest <= kRimCloseMm;
}

// Cap a planar opening only when it is the rim of the model. A fan from the
// middle of a horseshoe becomes the V across the palate, and it also seals the
// border the blend follows onto the bone, which leaves the dark slit.
void capPlanarOpening(vtkPolyData* mesh, const Vec3& axis, double cut)
{
    if (!mesh || mesh->GetNumberOfPolys() == 0)
        return;
    mesh->BuildCells();
    const vtkIdType nPoints = mesh->GetNumberOfPoints();
    std::unordered_map<std::uint64_t, int> edgeCount;
    auto keyOf = [](vtkIdType a, vtkIdType b) {
        if (a > b)
            std::swap(a, b);
        return (static_cast<std::uint64_t>(a) << 32) ^ static_cast<std::uint64_t>(b);
    };
    auto* polys = mesh->GetPolys();
    auto iter = vtk::TakeSmartPointer(polys->NewIterator());
    for (iter->GoToFirstCell(); !iter->IsDoneWithTraversal(); iter->GoToNextCell()) {
        vtkIdType npts = 0;
        const vtkIdType* ids = nullptr;
        iter->GetCurrentCell(npts, ids);
        for (vtkIdType k = 0; k < npts; ++k) {
            const vtkIdType a = ids[k];
            const vtkIdType b = ids[(k + 1) % npts];
            if (a == b)
                continue;
            ++edgeCount[keyOf(a, b)];
        }
    }
    std::vector<std::vector<vtkIdType>> adj(static_cast<size_t>(nPoints));
    std::set<std::pair<vtkIdType, vtkIdType>> unused;
    auto canon = [](vtkIdType a, vtkIdType b) {
        return a < b ? std::make_pair(a, b) : std::make_pair(b, a);
    };
    for (const auto& entry : edgeCount) {
        if (entry.second != 1)
            continue;
        const vtkIdType a = static_cast<vtkIdType>(entry.first >> 32);
        const vtkIdType b = static_cast<vtkIdType>(entry.first & 0xffffffffu);
        if (a < 0 || b < 0 || a >= nPoints || b >= nPoints)
            continue;
        adj[static_cast<size_t>(a)].push_back(b);
        adj[static_cast<size_t>(b)].push_back(a);
        unused.insert(canon(a, b));
    }
    Vec3 tangent = std::abs(axis[2]) < 0.9 ? Vec3{0.0, 0.0, 1.0} : Vec3{1.0, 0.0, 0.0};
    const Vec3 u = normalized(cross(tangent, axis), {1.0, 0.0, 0.0});
    const Vec3 v = cross(axis, u);
    auto* points = mesh->GetPoints();
    auto capped = vtkSmartPointer<vtkCellArray>::New();
    capped->DeepCopy(mesh->GetPolys());
    while (!unused.empty()) {
        const auto first = *unused.begin();
        unused.erase(unused.begin());
        std::vector<vtkIdType> loop{first.first, first.second};
        vtkIdType prev = first.first;
        vtkIdType cur = first.second;
        bool closed = false;
        const size_t walkLimit = unused.size() + 4;
        for (size_t guard = 0; guard < walkLimit; ++guard) {
            vtkIdType next = -1;
            for (vtkIdType neighbor : adj[static_cast<size_t>(cur)]) {
                if (neighbor == prev)
                    continue;
                if (neighbor == loop.front() && loop.size() >= 3) {
                    next = neighbor;
                    break;
                }
                if (unused.count(canon(cur, neighbor)) != 0) {
                    next = neighbor;
                    break;
                }
            }
            if (next < 0)
                break;
            unused.erase(canon(cur, next));
            if (next == loop.front()) {
                closed = true;
                break;
            }
            loop.push_back(next);
            prev = cur;
            cur = next;
        }
        if (!closed || loop.size() < 3)
            continue;
        double maxDev = 0.0;
        for (vtkIdType id : loop) {
            double p[3] = {};
            points->GetPoint(id, p);
            maxDev = std::max(maxDev, std::abs(dot({p[0], p[1], p[2]}, axis) - cut));
        }
        if (maxDev > 0.75)
            continue;
        double area = 0.0;
        auto coord = [&](vtkIdType id) {
            double p[3] = {};
            points->GetPoint(id, p);
            const Vec3 point{p[0], p[1], p[2]};
            return std::pair<double, double>{dot(point, u), dot(point, v)};
        };
        for (size_t i = 0; i < loop.size(); ++i) {
            const auto [u0, v0] = coord(loop[i]);
            const auto [u1, v1] = coord(loop[(i + 1) % loop.size()]);
            area += u0 * v1 - u1 * v0;
        }
        if (std::abs(area) < 1e-8 || !loopClosesOnItself(loop, points, u, v))
            continue;
        auto polygon = vtkSmartPointer<vtkIdList>::New();
        for (vtkIdType id : loop)
            polygon->InsertNextId(id);
        auto filled = vtkSmartPointer<vtkCellArray>::New();
        if (vtkContourTriangulator::TriangulatePolygon(polygon, points, filled) == 0)
            continue;
        auto tris = vtk::TakeSmartPointer(filled->NewIterator());
        for (tris->GoToFirstCell(); !tris->IsDoneWithTraversal(); tris->GoToNextCell()) {
            vtkIdType npts = 0;
            const vtkIdType* ids = nullptr;
            tris->GetCurrentCell(npts, ids);
            if (npts == 3)
                capped->InsertNextCell(3, ids);
        }
    }
    mesh->SetPolys(capped);
}
} // namespace

namespace CompositeBlockCore
{
bool BlockFacesJaw(const CompositeCutBlock& block, CompositeJaw jaw)
{
    return jaw == CompositeJaw::Auto || (jaw == CompositeJaw::Upper) == (block.axisZ[2] > 0.0);
}

CompositeCutBlock InitialBlock(vtkPolyData* dentalScan, vtkPolyData* bone, double thicknessMm, QString* error,
                               CompositeJaw jaw)
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
    if (jaw != CompositeJaw::Auto) {
        // Upper jaw: the bone is superior to the teeth; lower jaw: inferior ("below the points" is then
        // everything from the points up to the cusps).
        if ((jaw == CompositeJaw::Upper) != (z[2] > 0.0))
            z = mul(z, -1.0);
    } else if (bone && bone->GetNumberOfPoints() > 0 && dot(sub(sampledCentroid(bone), centroid), z) < 0.0) {
        z = mul(z, -1.0);
    }
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
    std::vector<size_t> hull;                   // convex outline of the points (CCW), whatever their order
    double occlusal = 0.0;                      // block Z of the occlusal face (beyond the cusps)
    // Arch span of the points seen from their centroid (open end = largest angular gap).
    bool spanLimited = false;
    double spanCenter[2] = {0.0, 0.0};
    double spanStart = 0.0;
    double spanWidth = 0.0;
};

// Bone is replaced up to the line of the points plus this margin, so no CT crown edge remains.
constexpr double kContourBoneMarginMm = 0.5;
// Crowns reach beyond the gingival points in the occlusal view (bulges, proclined incisors, brackets):
// the outline is widened by this much, still far from other bone below that level (mastoids).
constexpr double kContourHullMarginMm = 10.0;
// The replaced region ends this far past the first and last points along the arch (bone behind the marked
// teeth: mandibular rami, tuberosity) and past the block's occlusal face (bone rising above the crowns).
constexpr double kContourSpanMarginMm = 3.0;
constexpr double kContourOcclusalMarginMm = 1.0;
constexpr double kContourPi = 3.14159265358979323846;

// Convex hull (monotone chain) as indices into the points, counter-clockwise.
std::vector<size_t> convexHull(const std::vector<std::array<double, 2>>& points)
{
    std::vector<size_t> order(points.size());
    for (size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    if (order.size() < 3)
        return order;
    std::sort(order.begin(), order.end(), [&points](size_t a, size_t b) {
        return points[a][0] < points[b][0] || (points[a][0] == points[b][0] && points[a][1] < points[b][1]);
    });
    const auto turn = [&points](size_t o, size_t a, size_t b) {
        return (points[a][0] - points[o][0]) * (points[b][1] - points[o][1]) -
               (points[a][1] - points[o][1]) * (points[b][0] - points[o][0]);
    };
    std::vector<size_t> hull(2 * order.size());
    size_t k = 0;
    for (size_t i : order) {
        while (k >= 2 && turn(hull[k - 2], hull[k - 1], i) <= 0.0)
            --k;
        hull[k++] = i;
    }
    for (size_t j = order.size() - 1, lower = k + 1; j-- > 0;) {
        while (k >= lower && turn(hull[k - 2], hull[k - 1], order[j]) <= 0.0)
            --k;
        hull[k++] = order[j];
    }
    hull.resize(k - 1);
    return hull;
}

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
    frame.hull = convexHull(frame.polygon);
    if (frame.polygon.size() >= 3) {
        double cu = 0.0, cv = 0.0;
        for (const auto& p : frame.polygon) {
            cu += p[0] / static_cast<double>(frame.polygon.size());
            cv += p[1] / static_cast<double>(frame.polygon.size());
        }
        std::vector<double> angles;
        for (const auto& p : frame.polygon)
            angles.push_back(std::atan2(p[1] - cv, p[0] - cu));
        std::sort(angles.begin(), angles.end());
        double gap = angles.front() + 2.0 * kContourPi - angles.back();
        double start = angles.front();
        for (size_t i = 1; i < angles.size(); ++i)
            if (angles[i] - angles[i - 1] > gap) {
                gap = angles[i] - angles[i - 1];
                start = angles[i];
            }
        if (gap >= kContourPi / 3.0) { // an open arch; points all around leave no end to limit
            frame.spanLimited = true;
            frame.spanCenter[0] = cu;
            frame.spanCenter[1] = cv;
            frame.spanStart = start;
            frame.spanWidth = 2.0 * kContourPi - gap;
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

// Signed distance to the convex outline of the points seen along the occlusal axis (negative inside).
double hullField(const ContourFrame& frame, double x, double y)
{
    const size_t n = frame.hull.size();
    if (n < 3)
        return std::numeric_limits<double>::max();
    double best = std::numeric_limits<double>::max();
    bool inside = true;
    for (size_t i = 0; i < n; ++i) {
        const auto& a = frame.polygon[frame.hull[i]];
        const auto& b = frame.polygon[frame.hull[(i + 1) % n]];
        const double ex = b[0] - a[0], ey = b[1] - a[1];
        if (ex * (y - a[1]) - ey * (x - a[0]) < 0.0)
            inside = false;
        const double len2 = ex * ex + ey * ey;
        const double t = len2 > 0.0 ? std::clamp(((x - a[0]) * ex + (y - a[1]) * ey) / len2, 0.0, 1.0) : 0.0;
        best = std::min(best, std::hypot(x - (a[0] + t * ex), y - (a[1] + t * ey)));
    }
    return inside ? -best : best;
}

// Signed field of the intersection of two regions (negative inside both).
double intersectFields(double a, double b)
{
    if (a <= 0.0 && b <= 0.0)
        return std::max(a, b);
    return std::hypot(std::max(a, 0.0), std::max(b, 0.0));
}

// Distance along the arch (arc length at this radius) to the nearest end of the points' span:
// negative within the span, positive past its first or last point.
double spanField(const ContourFrame& frame, double x, double y)
{
    if (!frame.spanLimited)
        return -1e9;
    const double du = x - frame.spanCenter[0];
    const double dv = y - frame.spanCenter[1];
    const double radius = std::hypot(du, dv);
    double offset = std::atan2(dv, du) - frame.spanStart;
    while (offset < 0.0)
        offset += 2.0 * kContourPi;
    while (offset >= 2.0 * kContourPi)
        offset -= 2.0 * kContourPi;
    if (offset <= frame.spanWidth)
        return -std::min(offset, frame.spanWidth - offset) * radius;
    return std::min(offset - frame.spanWidth, 2.0 * kContourPi - offset) * radius;
}

// Height of the scan border line above (x, y). Use the closest marked segment
// instead of radial interpolation so the boundary remains straight between
// consecutive points.
double lineHeight(const ContourFrame& frame, double x, double y)
{
    if (frame.polygon.empty() || frame.height.empty())
        return 0.0;
    if (frame.polygon.size() == 1)
        return frame.height.front();
    double bestDistance2 = std::numeric_limits<double>::max();
    double bestHeight = frame.height.front();
    for (size_t i = 1; i < frame.polygon.size(); ++i) {
        const auto& a = frame.polygon[i - 1];
        const auto& b = frame.polygon[i];
        const double dx = b[0] - a[0];
        const double dy = b[1] - a[1];
        const double length2 = dx * dx + dy * dy;
        const double t = length2 > 1e-12
            ? std::clamp(((x - a[0]) * dx + (y - a[1]) * dy) / length2, 0.0, 1.0)
            : 0.0;
        const double px = a[0] + t * dx;
        const double py = a[1] + t * dy;
        const double distance2 = (x - px) * (x - px) + (y - py) * (y - py);
        if (distance2 < bestDistance2) {
            bestDistance2 = distance2;
            bestHeight = frame.height[i - 1] + t * (frame.height[i] - frame.height[i - 1]);
        }
    }
    return bestHeight;
}

// Everything below the line of the points (toward the occlusal side) within their widened outline;
// lift raises the line (the bone is replaced slightly above the scan border).
double belowLineField(const ContourFrame& frame, const Vec3& d, const CompositeCutBlock& block, double lift)
{
    const double x = dot(d, block.axisX);
    const double y = dot(d, block.axisY);
    const double z = dot(d, block.axisZ);
    const double lateral = intersectFields(hullField(frame, x, y) - kContourHullMarginMm,
                                           spanField(frame, x, y) - kContourSpanMarginMm);
    const double vertical = intersectFields(z - (lineHeight(frame, x, y) + lift),
                                            frame.occlusal - kContourOcclusalMarginMm - z);
    return intersectFields(lateral, vertical);
}

// Bone replaced by the scan.
double contourFieldLocal(const ContourFrame& frame, const Vec3& d, const CompositeCutBlock& block)
{
    return belowLineField(frame, d, block, kContourBoneMarginMm);
}

// The scan border is a few long edges. A band hung on those edges is the row of big triangles.
// Split only the open border, so the new points stay on the scan and the crowns are untouched.
vtkSmartPointer<vtkPolyData> splitLongBoundaryEdges(vtkPolyData* input, double maxEdgeMm)
{
    auto mesh = vtkSmartPointer<vtkPolyData>::New();
    mesh->DeepCopy(input);
    const double max2 = maxEdgeMm * maxEdgeMm;
    for (int pass = 0; pass < 6; ++pass) {
        auto* points = mesh->GetPoints();
        if (!points)
            return mesh;
        struct EdgeUse
        {
            int count = 0;
        };
        std::unordered_map<std::uint64_t, EdgeUse> uses;
        const auto keyOf = [](vtkIdType a, vtkIdType b) {
            return (static_cast<std::uint64_t>(std::min(a, b)) << 32) | static_cast<std::uint64_t>(std::max(a, b));
        };
        std::vector<std::array<vtkIdType, 3>> tris;
        auto it = vtk::TakeSmartPointer(mesh->GetPolys()->NewIterator());
        for (it->GoToFirstCell(); !it->IsDoneWithTraversal(); it->GoToNextCell()) {
            vtkIdType npts = 0;
            const vtkIdType* ids = nullptr;
            it->GetCurrentCell(npts, ids);
            if (npts != 3)
                continue;
            tris.push_back({ids[0], ids[1], ids[2]});
            for (int e = 0; e < 3; ++e)
                ++uses[keyOf(ids[e], ids[(e + 1) % 3])].count;
        }
        std::unordered_map<std::uint64_t, vtkIdType> mids;
        for (const auto& entry : uses) {
            if (entry.second.count != 1)
                continue;
            const vtkIdType a = static_cast<vtkIdType>(entry.first >> 32);
            const vtkIdType b = static_cast<vtkIdType>(entry.first & 0xffffffffu);
            double pa[3] = {};
            double pb[3] = {};
            points->GetPoint(a, pa);
            points->GetPoint(b, pb);
            const double dx = pb[0] - pa[0];
            const double dy = pb[1] - pa[1];
            const double dz = pb[2] - pa[2];
            if (dx * dx + dy * dy + dz * dz <= max2)
                continue;
            mids[entry.first] = points->InsertNextPoint(0.5 * (pa[0] + pb[0]), 0.5 * (pa[1] + pb[1]), 0.5 * (pa[2] + pb[2]));
        }
        if (mids.empty())
            break;
        auto polys = vtkSmartPointer<vtkCellArray>::New();
        const auto mid = [&](vtkIdType a, vtkIdType b) -> vtkIdType {
            const auto found = mids.find(keyOf(a, b));
            return found == mids.end() ? -1 : found->second;
        };
        for (const auto& t : tris) {
            const vtkIdType m01 = mid(t[0], t[1]);
            const vtkIdType m12 = mid(t[1], t[2]);
            const vtkIdType m20 = mid(t[2], t[0]);
            const int splits = (m01 >= 0) + (m12 >= 0) + (m20 >= 0);
            if (splits == 0) {
                polys->InsertNextCell(3, t.data());
            } else if (splits == 3) {
                const vtkIdType a[3] = {t[0], m01, m20};
                const vtkIdType b[3] = {m01, t[1], m12};
                const vtkIdType c[3] = {m20, m12, t[2]};
                const vtkIdType d[3] = {m01, m12, m20};
                polys->InsertNextCell(3, a);
                polys->InsertNextCell(3, b);
                polys->InsertNextCell(3, c);
                polys->InsertNextCell(3, d);
            } else {
                std::array<vtkIdType, 3> tv = t;
                std::array<vtkIdType, 3> tm = {m01, m12, m20};
                int r = 0;
                while (tm[static_cast<size_t>(r)] < 0)
                    ++r;
                const vtkIdType v0 = tv[static_cast<size_t>(r)];
                const vtkIdType v1 = tv[static_cast<size_t>((r + 1) % 3)];
                const vtkIdType v2 = tv[static_cast<size_t>((r + 2) % 3)];
                const vtkIdType s01 = tm[static_cast<size_t>(r)];
                const vtkIdType s12 = tm[static_cast<size_t>((r + 1) % 3)];
                const vtkIdType s20 = tm[static_cast<size_t>((r + 2) % 3)];
                if (splits == 1) {
                    const vtkIdType a[3] = {v0, s01, v2};
                    const vtkIdType b[3] = {s01, v1, v2};
                    polys->InsertNextCell(3, a);
                    polys->InsertNextCell(3, b);
                } else if (s12 >= 0) {
                    const vtkIdType a[3] = {v0, s01, v2};
                    const vtkIdType b[3] = {s01, v1, s12};
                    const vtkIdType c[3] = {s01, s12, v2};
                    polys->InsertNextCell(3, a);
                    polys->InsertNextCell(3, b);
                    polys->InsertNextCell(3, c);
                } else {
                    const vtkIdType a[3] = {v0, s01, s20};
                    const vtkIdType b[3] = {s01, v1, v2};
                    const vtkIdType c[3] = {s20, s01, v2};
                    polys->InsertNextCell(3, a);
                    polys->InsertNextCell(3, b);
                    polys->InsertNextCell(3, c);
                }
            }
        }
        mesh->SetPolys(polys);
    }
    return mesh;
}

// Curved blend from the scan's open border toward the bone (axisZ: up on the maxilla, down on
// the mandible). It leaves along the wall and settles on the bone instead of ending in a cut edge.
constexpr int kBlendRings = 4;
constexpr double kBlendRollMm = 0.7;
constexpr double kBlendSearchMm = 8.0;
constexpr double kBlendLipMm = 2.5;
constexpr double kBlendMaxMm = 6.0;

vtkSmartPointer<vtkPolyData> contourSkirt(vtkPolyData* dental, const ContourFrame& frame, const CompositeCutBlock& block,
                                          vtkPolyData* bone)
{
    vtkSmartPointer<vtkPolyData> refinedBorder;
    if (frame.polygon.empty() && dental && dental->GetNumberOfPolys() > 0) {
        refinedBorder = splitLongBoundaryEdges(dental, 0.8);
        dental = refinedBorder;
    }
    struct Edge
    {
        int count = 0;
        vtkIdType a = 0;
        vtkIdType b = 0;
        vtkIdType opposite = 0;
    };
    std::unordered_map<std::uint64_t, Edge> edges;
    for (vtkIdType c = 0; c < dental->GetNumberOfCells(); ++c) {
        vtkIdType count = 0;
        const vtkIdType* ids = nullptr;
        dental->GetCellPoints(c, count, ids);
        for (vtkIdType i = 0; i < count; ++i) {
            const vtkIdType a = ids[i];
            const vtkIdType b = ids[(i + 1) % count];
            const std::uint64_t key =
                (static_cast<std::uint64_t>(std::min(a, b)) << 32) | static_cast<std::uint64_t>(std::max(a, b));
            Edge& edge = edges[key];
            if (edge.count++ == 0) {
                edge.a = a;
                edge.b = b;
                for (vtkIdType k = 0; k < count; ++k) {
                    if (ids[k] != a && ids[k] != b)
                        edge.opposite = ids[k];
                }
            }
        }
    }

    struct RimEdge
    {
        vtkIdType a = 0;
        vtkIdType b = 0;
        vtkIdType opposite = 0;
        Vec3 inwardA{};
        Vec3 inwardB{};
    };
    std::vector<RimEdge> rim;
    double rimTop = -std::numeric_limits<double>::max();
    auto heightOf = [&](const double p[3]) {
        return dot(sub({p[0], p[1], p[2]}, block.center), block.axisZ);
    };
    for (const auto& entry : edges) {
        const Edge& edge = entry.second;
        if (edge.count != 1)
            continue;
        double pa[3], pb[3], pc[3];
        dental->GetPoint(edge.a, pa);
        dental->GetPoint(edge.b, pb);
        dental->GetPoint(edge.opposite, pc);
        const Vec3 along = sub({pb[0], pb[1], pb[2]}, {pa[0], pa[1], pa[2]});
        const double span = length(along);
        if (span < 1e-9)
            continue;
        const double za = heightOf(pa);
        const double zb = heightOf(pb);
        const auto nearLine = [&](const double p[3], double z) {
            const Vec3 d = sub({p[0], p[1], p[2]}, block.center);
            return std::abs(z - lineHeight(frame, dot(d, block.axisX), dot(d, block.axisY))) < 1.0;
        };
        const bool gingival = frame.polygon.empty();
        const bool onLine = !gingival && nearLine(pa, za) && nearLine(pb, zb);
        const bool shallow = std::abs(dot(along, block.axisZ)) / span < 0.75;
        // The gingival wall is often steep. Those edges are the rim that has to meet the crest.
        if (!onLine && !shallow && !gingival)
            continue;
        Vec3 inward{};
        const Vec3 n = cross(along, sub({pc[0], pc[1], pc[2]}, {pa[0], pa[1], pa[2]}));
        const double nlen = length(n);
        if (nlen > 1e-12) {
            inward = sub(mul(n, -1.0 / nlen), mul(block.axisZ, dot(mul(n, -1.0 / nlen), block.axisZ)));
            const double ilen = length(inward);
            inward = ilen > 1e-8 ? mul(inward, 1.0 / ilen) : Vec3{};
        }
        rim.push_back({edge.a, edge.b, edge.opposite, inward, inward});
        rimTop = std::max(rimTop, std::max(za, zb));
    }

    vtkSmartPointer<vtkStaticCellLocator> boneLocator;
    if (bone && bone->GetNumberOfCells() > 0) {
        boneLocator = vtkSmartPointer<vtkStaticCellLocator>::New();
        boneLocator->SetDataSet(bone);
        boneLocator->BuildLocator();
    }
    const bool gingivalRim = frame.polygon.empty();
    Vec3 archCenter{};
    if (dental->GetNumberOfPoints() > 0) {
        double acc[3] = {};
        const vtkIdType count = dental->GetNumberOfPoints();
        double q[3] = {};
        for (vtkIdType i = 0; i < count; ++i) {
            dental->GetPoint(i, q);
            acc[0] += q[0];
            acc[1] += q[1];
            acc[2] += q[2];
        }
        const double inv = 1.0 / static_cast<double>(count);
        archCenter = {acc[0] * inv, acc[1] * inv, acc[2] * inv};
    }
    // The open border has to land on the bone. A closest-point search from the rim stops short
    // when the crest is beside the gingiva, and the black slit is what remains. A ray out of the
    // wall, sunk a little into the surface, overlaps the bone so the two shells cannot leave a gap.
    // A wall that opens toward the arch is the palate: a long ray there is the V-shaped lid.
    bool landed = false;
    auto travelOf = [&](const double p[3], const Vec3& openDir) -> Vec3 {
        landed = false;
        const Vec3 lip = mul(block.axisZ, gingivalRim ? 1.5 : kBlendLipMm);
        if (!boneLocator)
            return lip;
        auto consider = [&](const Vec3& origin, double radius, double reach, double& bestDist, Vec3& best, bool& found) {
            thread_local vtkSmartPointer<vtkGenericCell> cell = vtkSmartPointer<vtkGenericCell>::New();
            double at[3] = {origin[0], origin[1], origin[2]};
            double closest[3] = {};
            vtkIdType cellId = -1;
            int subId = 0;
            double dist2 = 0.0;
            if (boneLocator->FindClosestPointWithinRadius(at, radius, closest, cell, cellId, subId, dist2) == 0)
                return;
            const Vec3 toward = sub({closest[0], closest[1], closest[2]}, {p[0], p[1], p[2]});
            const double dist = length(toward);
            const double alongJaw = dot(toward, block.axisZ);
            if (dist < 1e-4 || dist > reach || alongJaw < -0.35 * dist)
                return;
            if (dist < bestDist) {
                bestDist = dist;
                best = toward;
                found = true;
            }
        };
        if (!gingivalRim) {
            double bestDist = std::numeric_limits<double>::max();
            Vec3 best{};
            bool found = false;
            for (double station : {0.0, 2.0, 4.0})
                consider(add({p[0], p[1], p[2]}, mul(block.axisZ, station)), kBlendSearchMm, kBlendMaxMm, bestDist, best,
                         found);
            return found ? best : lip;
        }
        Vec3 fromCenter = sub({p[0], p[1], p[2]}, archCenter);
        fromCenter = sub(fromCenter, mul(block.axisZ, dot(fromCenter, block.axisZ)));
        const Vec3 away = normalized(fromCenter, {1.0, 0.0, 0.0});
        const bool towardArch = length(openDir) > 1e-8 && dot(openDir, away) < 0.0;
        auto ray = [&](const Vec3& dir, double reach, Vec3& out) -> bool {
            const double len = length(dir);
            if (len < 1e-6)
                return false;
            const Vec3 d = mul(dir, 1.0 / len);
            double p0[3] = {p[0] - d[0] * 0.5, p[1] - d[1] * 0.5, p[2] - d[2] * 0.5};
            double p1[3] = {p[0] + d[0] * reach, p[1] + d[1] * reach, p[2] + d[2] * reach};
            thread_local vtkSmartPointer<vtkGenericCell> cell = vtkSmartPointer<vtkGenericCell>::New();
            double t = 0.0;
            double x[3] = {};
            double pc[3] = {};
            int subId = 0;
            vtkIdType cellId = -1;
            if (boneLocator->IntersectWithLine(p0, p1, 0.05, t, x, pc, subId, cellId, cell) == 0)
                return false;
            Vec3 travel = sub({x[0], x[1], x[2]}, {p[0], p[1], p[2]});
            constexpr double kSinkMm = 0.8;
            travel = add(travel, mul(d, kSinkMm));
            const double alongJaw = dot(travel, block.axisZ);
            if (alongJaw < -4.0)
                return false;
            const Vec3 horizontal = sub(travel, mul(block.axisZ, alongJaw));
            if (dot(horizontal, away) < -6.0 && alongJaw < 3.0)
                return false;
            out = travel;
            return true;
        };
        if (towardArch) {
            Vec3 hit;
            if (ray(block.axisZ, 5.0, hit)) {
                landed = true;
                return hit;
            }
            return lip;
        }
        const Vec3 dirs[] = {
            length(openDir) > 1e-8 ? add(block.axisZ, openDir) : block.axisZ,
            length(openDir) > 1e-8 ? add(mul(block.axisZ, 0.4), openDir) : away,
            block.axisZ,
            length(openDir) > 1e-8 ? openDir : away,
            add(block.axisZ, away),
            add(mul(block.axisZ, 0.35), away),
        };
        double bestDist = std::numeric_limits<double>::max();
        Vec3 best{};
        bool found = false;
        for (const Vec3& dir : dirs) {
            Vec3 hit;
            if (!ray(dir, 16.0, hit))
                continue;
            const double dist = length(hit);
            if (dist < bestDist) {
                bestDist = dist;
                best = hit;
                found = true;
            }
        }
        if (found) {
            landed = true;
            return best;
        }
        double nearDist = std::numeric_limits<double>::max();
        Vec3 near{};
        bool foundNear = false;
        consider({p[0], p[1], p[2]}, 14.0, 14.0, nearDist, near, foundNear);
        if (!foundNear)
            return lip;
        const double alongJaw = dot(near, block.axisZ);
        const Vec3 horizontal = sub(near, mul(block.axisZ, alongJaw));
        if (dot(horizontal, away) < -6.0 && alongJaw < 3.0)
            return lip;
        landed = true;
        return add(near, mul(normalized(near, block.axisZ), 0.8));
    };
    auto openDirOf = [&](const double p[3], vtkIdType opposite) {
        double o[3] = {};
        dental->GetPoint(opposite, o);
        Vec3 across = sub({o[0], o[1], o[2]}, {p[0], p[1], p[2]});
        across = sub(across, mul(block.axisZ, dot(across, block.axisZ)));
        const double len = length(across);
        return len > 1e-8 ? mul(across, -1.0 / len) : Vec3{};
    };

    std::unordered_map<vtkIdType, Vec3> travel;
    std::unordered_map<vtkIdType, Vec3> inwardAt;
    std::unordered_map<vtkIdType, double> nearHit;
    std::vector<RimEdge> used;
    auto awayAt = [&](const double p[3]) {
        Vec3 fromCenter = sub({p[0], p[1], p[2]}, archCenter);
        fromCenter = sub(fromCenter, mul(block.axisZ, dot(fromCenter, block.axisZ)));
        return normalized(fromCenter, {1.0, 0.0, 0.0});
    };
    for (const RimEdge& edge : rim) {
        double pa[3], pb[3];
        dental->GetPoint(edge.a, pa);
        dental->GetPoint(edge.b, pb);
        const double za = heightOf(pa);
        const double zb = heightOf(pb);
        const bool apical = za >= rimTop - 12.0 && zb >= rimTop - 12.0;
        const auto nearLine = [&](const double p[3], double z) {
            const Vec3 d = sub({p[0], p[1], p[2]}, block.center);
            return std::abs(z - lineHeight(frame, dot(d, block.axisX), dot(d, block.axisY))) < 1.0;
        };
        // Every gingival edge, including the molars that sit well below the highest point.
        // Leaving those out is the black slit under the posterior teeth.
        if (!frame.polygon.empty() && !apical && !(nearLine(pa, za) && nearLine(pb, zb)))
            continue;
        if (gingivalRim) {
            // The palatal opening faces the arch. A sheet there is the lid across the palate.
            const Vec3 openA = openDirOf(pa, edge.opposite);
            const Vec3 openB = openDirOf(pb, edge.opposite);
            const bool palatalA = length(openA) > 1e-8 && dot(openA, awayAt(pa)) < 0.0;
            const bool palatalB = length(openB) > 1e-8 && dot(openB, awayAt(pb)) < 0.0;
            if (palatalA && palatalB)
                continue;
        }
        used.push_back(edge);
        const auto noteBorder = [&](vtkIdType id, const double p[3]) {
            if (!gingivalRim) {
                if (!travel.count(id))
                    travel[id] = travelOf(p, openDirOf(p, edge.opposite));
                return;
            }
            if (nearHit.count(id) || !boneLocator)
                return;
            double p0[3] = {p[0] - block.axisZ[0] * 0.3, p[1] - block.axisZ[1] * 0.3, p[2] - block.axisZ[2] * 0.3};
            double p1[3] = {p[0] + block.axisZ[0] * 8.0, p[1] + block.axisZ[1] * 8.0, p[2] + block.axisZ[2] * 8.0};
            thread_local vtkSmartPointer<vtkGenericCell> cell = vtkSmartPointer<vtkGenericCell>::New();
            double t = 0.0;
            double x[3] = {};
            double pc[3] = {};
            int subId = 0;
            vtkIdType cellId = -1;
            if (boneLocator->IntersectWithLine(p0, p1, 0.05, t, x, pc, subId, cellId, cell) == 0)
                return;
            const double dx = x[0] - p[0];
            const double dy = x[1] - p[1];
            const double dz = x[2] - p[2];
            const double hit = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (hit >= 0.15 && hit <= 8.0)
                nearHit[id] = hit;
        };
        noteBorder(edge.a, pa);
        noteBorder(edge.b, pb);
        inwardAt[edge.a] = add(inwardAt[edge.a], edge.inwardA);
        inwardAt[edge.b] = add(inwardAt[edge.b], edge.inwardB);
    }
    for (auto& entry : inwardAt) {
        const double ilen = length(entry.second);
        entry.second = ilen > 1e-8 ? mul(entry.second, 1.0 / ilen) : Vec3{};
    }
    // On the mandible the crest is beside the teeth, so a straight ray hits on one
    // vertex and misses the next. Extruding only the hits is the row of spikes.
    // Every border point takes the same length as its neighbours, and the sheet stays one piece.
    if (frame.polygon.empty()) {
        std::unordered_map<vtkIdType, std::vector<vtkIdType>> neighbors;
        std::unordered_map<vtkIdType, double> depth;
        for (const RimEdge& edge : used) {
            neighbors[edge.a].push_back(edge.b);
            neighbors[edge.b].push_back(edge.a);
        }
        // A hole around a bracket is its own little border. Extruding it makes a spike.
        // The gingival margin is the long border; the short loops stay as they are.
        {
            std::unordered_map<vtkIdType, int> component;
            int nextComponent = 0;
            std::vector<int> componentEdges;
            for (const RimEdge& edge : used) {
                if (component.count(edge.a))
                    continue;
                const int id = nextComponent++;
                componentEdges.push_back(0);
                std::vector<vtkIdType> stack{edge.a};
                component[edge.a] = id;
                while (!stack.empty()) {
                    const vtkIdType current = stack.back();
                    stack.pop_back();
                    for (vtkIdType neighbor : neighbors[current]) {
                        if (component.count(neighbor))
                            continue;
                        component[neighbor] = id;
                        stack.push_back(neighbor);
                    }
                }
            }
            for (const RimEdge& edge : used) {
                const auto found = component.find(edge.a);
                if (found != component.end())
                    ++componentEdges[static_cast<size_t>(found->second)];
            }
            std::vector<RimEdge> longBorder;
            longBorder.reserve(used.size());
            for (const RimEdge& edge : used) {
                const auto found = component.find(edge.a);
                if (found != component.end() && componentEdges[static_cast<size_t>(found->second)] >= 40)
                    longBorder.push_back(edge);
            }
            if (!longBorder.empty())
                used.swap(longBorder);
            neighbors.clear();
            for (const RimEdge& edge : used) {
                neighbors[edge.a].push_back(edge.b);
                neighbors[edge.b].push_back(edge.a);
            }
        }
        for (const auto& entry : nearHit)
            depth[entry.first] = std::min(8.0, entry.second + 2.0);
        for (int pass = 0; pass < 12; ++pass) {
            auto extra = depth;
            for (const auto& entry : neighbors) {
                if (depth.count(entry.first))
                    continue;
                double acc = 0.0;
                int count = 0;
                for (vtkIdType neighbor : entry.second) {
                    const auto other = depth.find(neighbor);
                    if (other == depth.end())
                        continue;
                    acc += other->second;
                    ++count;
                }
                if (count > 0)
                    extra[entry.first] = acc / count;
            }
            depth = std::move(extra);
        }
        for (int pass = 0; pass < 10; ++pass) {
            auto next = depth;
            for (const auto& entry : depth) {
                const auto found = neighbors.find(entry.first);
                if (found == neighbors.end() || found->second.empty())
                    continue;
                double acc = 0.0;
                int count = 0;
                for (vtkIdType neighbor : found->second) {
                    const auto other = depth.find(neighbor);
                    if (other == depth.end())
                        continue;
                    acc += other->second;
                    ++count;
                }
                if (count == 0)
                    continue;
                next[entry.first] = entry.second * 0.3 + (acc / count) * 0.7;
            }
            depth = std::move(next);
        }
        for (const auto& entry : depth)
            travel[entry.first] = mul(block.axisZ, std::clamp(entry.second, 2.0, 7.0));
        std::vector<RimEdge> kept;
        kept.reserve(used.size());
        for (const RimEdge& edge : used) {
            if (travel.count(edge.a) && travel.count(edge.b))
                kept.push_back(edge);
        }
        used.swap(kept);
    }

    auto points = vtkSmartPointer<vtkPoints>::New();
    auto polys = vtkSmartPointer<vtkCellArray>::New();
    struct RingKey
    {
        vtkIdType id = 0;
        int ring = 0;
        bool operator==(const RingKey& other) const { return id == other.id && ring == other.ring; }
    };
    struct RingHash
    {
        std::size_t operator()(const RingKey& key) const
        {
            return (static_cast<std::size_t>(key.id) << 3) ^ static_cast<std::size_t>(key.ring);
        }
    };
    std::unordered_map<RingKey, vtkIdType, RingHash> ringPoint;
    auto at = [&](vtkIdType id, int ring) {
        const RingKey key{id, ring};
        const auto found = ringPoint.find(key);
        if (found != ringPoint.end())
            return found->second;
        double p[3];
        dental->GetPoint(id, p);
        const int rings = gingivalRim ? 8 : kBlendRings;
        const double t = static_cast<double>(ring) / rings;
        const double settle = gingivalRim ? t : 1.0 - (1.0 - t) * (1.0 - t);
        const double bulge = 4.0 * t * (1.0 - t);
        const Vec3 offset = travel.count(id) ? travel[id] : mul(block.axisZ, gingivalRim ? 0.0 : kBlendLipMm);
        const Vec3 bow = (!gingivalRim && inwardAt.count(id)) ? mul(inwardAt[id], kBlendRollMm * bulge) : Vec3{};
        const Vec3 moved = add({p[0], p[1], p[2]}, add(mul(offset, settle), bow));
        const vtkIdType pid = points->InsertNextPoint(moved.data());
        ringPoint.emplace(key, pid);
        return pid;
    };
    const int rings = gingivalRim ? 8 : kBlendRings;
    for (const RimEdge& edge : used) {
        for (int ring = 0; ring < rings; ++ring) {
            const vtkIdType a0 = at(edge.a, ring);
            const vtkIdType b0 = at(edge.b, ring);
            const vtkIdType a1 = at(edge.a, ring + 1);
            const vtkIdType b1 = at(edge.b, ring + 1);
            const vtkIdType first[3] = {b0, a0, a1};
            const vtkIdType second[3] = {b0, a1, b1};
            polys->InsertNextCell(3, first);
            polys->InsertNextCell(3, second);
        }
    }
    auto skirt = vtkSmartPointer<vtkPolyData>::New();
    skirt->SetPoints(points);
    skirt->SetPolys(polys);
    if (!gingivalRim || skirt->GetNumberOfPolys() == 0)
        return skirt;
    // Twelve rings are still long facets on a coarse scan. Split every edge once, then
    // smooth the middle. The rim that touches the scan stays put.
    auto finePoints = vtkSmartPointer<vtkPoints>::New();
    finePoints->DeepCopy(skirt->GetPoints());
    std::unordered_map<std::uint64_t, vtkIdType> midpoint;
    auto midOf = [&](vtkIdType a, vtkIdType b) {
        const std::uint64_t key = (static_cast<std::uint64_t>(std::min(a, b)) << 32) | static_cast<std::uint64_t>(std::max(a, b));
        const auto found = midpoint.find(key);
        if (found != midpoint.end())
            return found->second;
        double pa[3] = {};
        double pb[3] = {};
        finePoints->GetPoint(a, pa);
        finePoints->GetPoint(b, pb);
        const vtkIdType id = finePoints->InsertNextPoint(0.5 * (pa[0] + pb[0]), 0.5 * (pa[1] + pb[1]), 0.5 * (pa[2] + pb[2]));
        midpoint.emplace(key, id);
        return id;
    };
    auto finePolys = vtkSmartPointer<vtkCellArray>::New();
    auto skirtCells = vtk::TakeSmartPointer(skirt->GetPolys()->NewIterator());
    for (skirtCells->GoToFirstCell(); !skirtCells->IsDoneWithTraversal(); skirtCells->GoToNextCell()) {
        vtkIdType npts = 0;
        const vtkIdType* ids = nullptr;
        skirtCells->GetCurrentCell(npts, ids);
        if (npts != 3)
            continue;
        const vtkIdType ab = midOf(ids[0], ids[1]);
        const vtkIdType bc = midOf(ids[1], ids[2]);
        const vtkIdType ca = midOf(ids[2], ids[0]);
        const vtkIdType t0[3] = {ids[0], ab, ca};
        const vtkIdType t1[3] = {ab, ids[1], bc};
        const vtkIdType t2[3] = {ca, bc, ids[2]};
        const vtkIdType t3[3] = {ab, bc, ca};
        finePolys->InsertNextCell(3, t0);
        finePolys->InsertNextCell(3, t1);
        finePolys->InsertNextCell(3, t2);
        finePolys->InsertNextCell(3, t3);
    }
    auto fine = vtkSmartPointer<vtkPolyData>::New();
    fine->SetPoints(finePoints);
    fine->SetPolys(finePolys);
    return fine;
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
    std::vector<std::array<double, 2>> outline;
    for (size_t i : frame.hull)
        outline.push_back(frame.polygon[i]);
    if (outline.size() < 3 || polygonArea(outline) < 1.0) {
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
    return belowLineField(frame, sub({point[0], point[1], point[2]}, block.center), block, 0.0);
}

vtkSmartPointer<vtkPolyData> ContourWallMesh(const CompositeCutBlock& block, const CompositeContour& contour)
{
    const ContourFrame frame = contourFrame(block, contour);
    auto points = vtkSmartPointer<vtkPoints>::New();
    auto polys = vtkSmartPointer<vtkCellArray>::New();
    const auto world = [&](const std::array<double, 2>& uv, double z) {
        return add(block.center, add(mul(block.axisX, uv[0]), add(mul(block.axisY, uv[1]), mul(block.axisZ, z))));
    };
    const size_t n = frame.hull.size();
    // Around the outline, from the occlusal face up to each point: where the scan replaces the CT teeth.
    for (size_t i : frame.hull) {
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
    // Around the outline of the points, at their heights on the scan border.
    for (size_t i : frame.hull) {
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
    ContourFrame frame = contourFrame(block, contour);
    // The occlusal limit follows the scan's own cusps, whatever the block thickness.
    for (vtkIdType i = 0; i < dentalScan->GetNumberOfPoints(); ++i) {
        double p[3];
        dentalScan->GetPoint(i, p);
        frame.occlusal = std::min(frame.occlusal, dot(sub({p[0], p[1], p[2]}, block.center), block.axisZ));
    }
    // Scan kept and bone replaced below the line of the points, within their outline (any point order).
    const auto scanField = [&frame, &block](const Vec3& p) { return belowLineField(frame, sub(p, block.center), block, 0.0); };

    // Region around the outline walls, where long edges are refined before clipping.
    const double top = *std::max_element(frame.height.begin(), frame.height.end()) + kContourBoneMarginMm;
    Vec3 lo{std::numeric_limits<double>::max(), std::numeric_limits<double>::max(), std::numeric_limits<double>::max()};
    Vec3 hi = mul(lo, -1.0);
    for (const auto& uv : frame.polygon)
        for (double z : {std::min(frame.occlusal, top), top}) {
            const Vec3 p = add(block.center, add(mul(block.axisX, uv[0]), add(mul(block.axisY, uv[1]), mul(block.axisZ, z))));
            for (size_t k = 0; k < 3; ++k) {
                lo[k] = std::min(lo[k], p[k] - 2.0 - kContourHullMarginMm);
                hi[k] = std::max(hi[k], p[k] + 2.0 + kContourHullMarginMm);
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
    const auto dentalCut = clipByField(refineNear(dentalScan, scanLo, scanHi, kMaxEdgeMm), scanField, true);
    vtkSmartPointer<vtkPolyData> dentalWithSkirt = dentalCut;
    const auto skirt = contourSkirt(dentalCut, frame, block, bone);
    if (skirt->GetNumberOfPolys() > 0) {
        auto append = vtkSmartPointer<vtkAppendPolyData>::New();
        append->AddInputData(dentalCut);
        append->AddInputData(skirt);
        append->Update();
        dentalWithSkirt = append->GetOutput();
    }
    const auto dentalPart = TagPart(dentalWithSkirt, DentalPart);
    // Bone below the line is replaced only next to the scan itself. The blend is left out of this
    // query so it can sit on the bone past the line instead of carving a trench around its tip.
    // The field is evaluated in parallel: static cell locator queries with one generic cell per thread.
    dentalCut->BuildCells();
    auto scanLocator = vtkSmartPointer<vtkStaticCellLocator>::New();
    scanLocator->SetDataSet(dentalCut);
    scanLocator->BuildLocator();
    const auto boneField = [&frame, &block, &scanLocator](const Vec3& p) {
        const double region = contourFieldLocal(frame, sub(p, block.center), block);
        if (region > 0.0)
            return region;
        thread_local vtkSmartPointer<vtkGenericCell> cell = vtkSmartPointer<vtkGenericCell>::New();
        double x[3] = {p[0], p[1], p[2]};
        double closest[3] = {};
        vtkIdType cellId = -1;
        int subId = 0;
        double dist2 = 0.0;
        const bool found = scanLocator->FindClosestPointWithinRadius(x, 2.0 * ContourNearScanMm, closest, cell,
                                                                     cellId, subId, dist2) != 0;
        const double nearScan = found ? std::sqrt(dist2) - ContourNearScanMm : ContourNearScanMm;
        return intersectFields(region, nearScan);
    };
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
                                   "(%3 triángulos conservados), escaneo dental %4 triángulos, borde fundido hacia el hueso.")
                        .arg(frame.polygon.size()).arg(polygonArea(frame.polygon), 0, 'f', 0)
                        .arg(result.boneCells).arg(result.dentalCells);
    result.ok = true;
    return result;
}

CompositeBlockResult CreateGingivalComposite(vtkPolyData* bone, vtkPolyData* dentalScan, const CompositeCutBlock& block)
{
    CompositeBlockResult result;
    if (!block.valid) {
        result.error = QStringLiteral("No hay una orientación del escaneo para recortar los dientes.");
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

    constexpr double kCellMm = 2.0;
    const vtkIdType n = dentalScan->GetNumberOfPoints();
    std::vector<double> hx(static_cast<size_t>(n));
    std::vector<double> hy(static_cast<size_t>(n));
    std::vector<double> hz(static_cast<size_t>(n));
    double minX = std::numeric_limits<double>::max();
    double minY = minX;
    double maxX = -minX;
    double maxY = maxX;
    for (vtkIdType i = 0; i < n; ++i) {
        double p[3] = {};
        dentalScan->GetPoint(i, p);
        const Vec3 d = sub({p[0], p[1], p[2]}, block.center);
        hx[static_cast<size_t>(i)] = dot(d, block.axisX);
        hy[static_cast<size_t>(i)] = dot(d, block.axisY);
        hz[static_cast<size_t>(i)] = dot(d, block.axisZ);
        minX = std::min(minX, hx[static_cast<size_t>(i)]);
        minY = std::min(minY, hy[static_cast<size_t>(i)]);
        maxX = std::max(maxX, hx[static_cast<size_t>(i)]);
        maxY = std::max(maxY, hy[static_cast<size_t>(i)]);
    }
    const int nu = std::max(1, static_cast<int>(std::ceil((maxX - minX) / kCellMm)) + 1);
    const int nv = std::max(1, static_cast<int>(std::ceil((maxY - minY) / kCellMm)) + 1);
    std::vector<double> cellMin(static_cast<size_t>(nu * nv), std::numeric_limits<double>::infinity());
    auto cellAt = [&](double x, double y) {
        const int iu = std::clamp(static_cast<int>(std::floor((x - minX) / kCellMm)), 0, nu - 1);
        const int iv = std::clamp(static_cast<int>(std::floor((y - minY) / kCellMm)), 0, nv - 1);
        return iv * nu + iu;
    };
    for (vtkIdType i = 0; i < n; ++i) {
        double& slot = cellMin[static_cast<size_t>(cellAt(hx[static_cast<size_t>(i)], hy[static_cast<size_t>(i)]))];
        slot = std::min(slot, hz[static_cast<size_t>(i)]);
    }
    // The occlusal height comes from the crowns. A vestibule in front of the teeth used to take
    // its own top as the reference and then keep 12 mm of that wall, which is the white sheet
    // down the chin.
    double global = std::numeric_limits<double>::infinity();
    for (double z : cellMin)
        global = std::min(global, z);
    std::vector<char> tooth;
    markCrownColumns(tooth, cellMin, nu, nv, global);
    std::vector<double> occlusal(cellMin.size(), global);
    constexpr int kCrownReach = 8;
    for (int iv = 0; iv < nv; ++iv) {
        for (int iu = 0; iu < nu; ++iu) {
            double best = std::numeric_limits<double>::infinity();
            int bestCells = 1 << 20;
            for (int dv = -kCrownReach; dv <= kCrownReach; ++dv) {
                for (int du = -kCrownReach; du <= kCrownReach; ++du) {
                    const int jv = iv + dv;
                    const int ju = iu + du;
                    if (jv < 0 || ju < 0 || jv >= nv || ju >= nu)
                        continue;
                    const int index = jv * nu + ju;
                    if (!tooth[static_cast<size_t>(index)])
                        continue;
                    const int cells = dv * dv + du * du;
                    if (cells < bestCells || (cells == bestCells && cellMin[static_cast<size_t>(index)] < best)) {
                        bestCells = cells;
                        best = cellMin[static_cast<size_t>(index)];
                    }
                }
            }
            if (best < std::numeric_limits<double>::infinity())
                occlusal[static_cast<size_t>(iv * nu + iu)] = best;
        }
    }
    const auto limitAt = [&](double x, double y) {
        const int index = cellAt(x, y);
        const double z = occlusal[static_cast<size_t>(index)];
        return (z < std::numeric_limits<double>::infinity() ? z : global) + ToothAndGingivaMm;
    };
    const auto bandField = [&](const Vec3& p) {
        const Vec3 d = sub(p, block.center);
        return dot(d, block.axisZ) - limitAt(dot(d, block.axisX), dot(d, block.axisY));
    };

    double scanBounds[6] = {};
    dentalScan->GetBounds(scanBounds);
    Vec3 lo{scanBounds[0] - 2.0, scanBounds[2] - 2.0, scanBounds[4] - 2.0};
    Vec3 hi{scanBounds[1] + 2.0, scanBounds[3] + 2.0, scanBounds[5] + 2.0};
    const auto dentalCut = clipByField(refineNear(dentalScan, lo, hi, 1.0), bandField, true);
    const auto dentalPart = TagPart(dentalCut, DentalPart);
    // The scan replaces the CT crowns under the teeth, up to the gingival margin.
    // Bone beside that footprint stays, including the crest the border sinks into:
    // cutting the crest is the empty slit. A cheek column is not a crown, and a point
    // outside the scan is never clamped onto a tooth column.
    const auto underScan = [&](double x, double y) {
        if (x < minX || y < minY || x > maxX || y > maxY)
            return false;
        return tooth[static_cast<size_t>(cellAt(x, y))] != 0;
    };
    const auto boneField = [&](const Vec3& p) {
        const Vec3 d = sub(p, block.center);
        const double x = dot(d, block.axisX);
        const double y = dot(d, block.axisY);
        if (dot(d, block.axisZ) >= limitAt(x, y))
            return 1.0;
        return underScan(x, y) ? -1.0 : 1.0;
    };
    for (size_t k = 0; k < 3; ++k) {
        lo[k] -= ToothAndGingivaMm;
        hi[k] += ToothAndGingivaMm;
    }
    const auto bonePart = TagPart(clipByField(refineNear(bone, lo, hi, 1.0), boneField, false), BonePart);
    // The bone and the scan stay as they are. Only a short smoothed band joins them,
    // so the skull is not remeshed into a thick skin.
    vtkSmartPointer<vtkPolyData> dentalWithSkirt = dentalPart;
    const auto skirt = contourSkirt(dentalCut, ContourFrame{}, block, bone);
    if (skirt->GetNumberOfPolys() > 0) {
        auto append = vtkSmartPointer<vtkAppendPolyData>::New();
        append->AddInputData(dentalPart);
        append->AddInputData(skirt);
        append->Update();
        dentalWithSkirt = TagPart(append->GetOutput(), DentalPart);
    }
    const auto composite = mergeTaggedParts(bonePart, dentalWithSkirt);
    result.boneCells = bonePart->GetNumberOfPolys();
    result.dentalCells = dentalWithSkirt->GetNumberOfPolys();
    if (result.dentalCells == 0) {
        result.error = QStringLiteral("No quedó escaneo dental: el recorte de dientes y encía salió vacío.");
        return result;
    }
    if (result.boneCells == 0) {
        result.error = QStringLiteral("El escaneo cubre todo el hueso.");
        return result;
    }
    result.composite = composite;
    result.report = QStringLiteral("Dientes y encía, sin puntos (%1 mm desde la cara oclusal): hueso %2 triángulos, "
                                   "escaneo %3 triángulos, borde fundido hacia el hueso.")
                        .arg(ToothAndGingivaMm, 0, 'f', 0)
                        .arg(result.boneCells)
                        .arg(result.dentalCells);
    result.ok = true;
    return result;
}

namespace
{
// The three clicks are the gingival line, not a slicing plane. A plane through points
// that sit on the facial wall cuts a hole through that wall. The line is the arch curve
// through the points, and the teeth are whatever is on its occlusal side.
struct ArchSample
{
    Vec3 point{};
    double x = 0.0;
    double y = 0.0;
    double height = 0.0;
};

QString archCurve(const CompositeCutBlock& block, const CompositeContour& points, std::vector<ArchSample>& curve)
{
    curve.clear();
    if (!block.valid)
        return QStringLiteral("No hay una orientación del escaneo para la línea de los dientes.");
    if (static_cast<int>(points.size()) != 3)
        return QStringLiteral("Marque tres puntos: posterior derecho, posterior izquierdo y anterior.");
    int anterior = 0;
    for (int i = 1; i < 3; ++i)
        if (points[static_cast<size_t>(i)][1] < points[static_cast<size_t>(anterior)][1])
            anterior = i;
    const int otherA = (anterior + 1) % 3;
    const int otherB = (anterior + 2) % 3;
    const int right = points[static_cast<size_t>(otherA)][0] < points[static_cast<size_t>(otherB)][0] ? otherA : otherB;
    const int left = otherA + otherB - right;
    const Vec3 p0 = points[static_cast<size_t>(right)];
    const Vec3 mid = points[static_cast<size_t>(anterior)];
    const Vec3 p2 = points[static_cast<size_t>(left)];
    if (length(sub(p0, mid)) + length(sub(mid, p2)) < 8.0)
        return QStringLiteral("Los tres puntos están demasiado juntos: sepárelos a lo largo de la arcada.");
    const auto place = [&](const Vec3& p) {
        const Vec3 d = sub(p, block.center);
        return ArchSample{p, dot(d, block.axisX), dot(d, block.axisY), dot(d, block.axisZ)};
    };
    const ArchSample rightS = place(p0);
    const ArchSample frontS = place(mid);
    const ArchSample leftS = place(p2);
    // Circle in the arch plane. A straight join of the three clicks cuts through the bone and
    // never wraps the teeth; the circle is the horseshoe, right molar to left molar.
    const double d = 2.0 * (rightS.x * (frontS.y - leftS.y) + frontS.x * (leftS.y - rightS.y) +
                            leftS.x * (rightS.y - frontS.y));
    const double rr = rightS.x * rightS.x + rightS.y * rightS.y;
    const double ff = frontS.x * frontS.x + frontS.y * frontS.y;
    const double ll = leftS.x * leftS.x + leftS.y * leftS.y;
    if (std::abs(d) < 1e-3)
        return QStringLiteral("Los tres puntos están en línea recta: adelante el punto anterior, por delante de los incisivos.");
    const double ux = (rr * (frontS.y - leftS.y) + ff * (leftS.y - rightS.y) + ll * (rightS.y - frontS.y)) / d;
    const double uy = (rr * (leftS.x - frontS.x) + ff * (rightS.x - leftS.x) + ll * (frontS.x - rightS.x)) / d;
    const double radius = std::hypot(rightS.x - ux, rightS.y - uy);
    if (radius < 8.0 || radius > 90.0)
        return QStringLiteral("Esos tres puntos no describen la arcada. El anterior va por delante de los incisivos.");
    const auto angleOf = [&](double x, double y) { return std::atan2(y - uy, x - ux); };
    const double a0 = angleOf(rightS.x, rightS.y);
    const double a1 = angleOf(frontS.x, frontS.y);
    const double a2 = angleOf(leftS.x, leftS.y);
    constexpr double kPi = 3.14159265358979323846;
    const auto ccw = [&](double from, double to) {
        double sweep = to - from;
        while (sweep < 0.0)
            sweep += 2.0 * kPi;
        while (sweep >= 2.0 * kPi)
            sweep -= 2.0 * kPi;
        return sweep;
    };
    // The mandible block points axisZ downward, so counterclockwise in that plane is the
    // other way around the teeth. Take the shorter of the two directions that still pass
    // through the anterior point; the long one is almost a full turn and used to be rejected.
    const double ccwFront = ccw(a0, a1);
    const double ccwLeft = ccw(a1, a2);
    const double cwFront = ccw(a1, a0);
    const double cwLeft = ccw(a2, a1);
    const bool useCcw = ccwFront + ccwLeft <= cwFront + cwLeft;
    const double toFront = useCcw ? ccwFront : cwFront;
    const double toLeft = useCcw ? ccwLeft : cwLeft;
    const double across = toFront + toLeft;
    const double sweepSign = useCcw ? 1.0 : -1.0;
    if (toFront < 0.2 || toLeft < 0.2 || across > 2.0 * kPi - 0.05)
        return QStringLiteral("Los tres puntos no rodean la arcada. El anterior es el que queda por delante de los incisivos.");
    // Just labial to the clicks, and further forward only at the incisors, so the line crosses
    // in front of the scan teeth. It continues past both molars along the same circle.
    constexpr double kBuccalMm = 1.8;
    constexpr double kFrontExtraMm = 4.0;
    constexpr double kEndMarginMm = 14.0;
    const double endAng = kEndMarginMm / radius;
    const double total = across + 2.0 * endAng;
    const int steps = std::max(32, static_cast<int>(std::ceil(total * radius / 1.2)));
    curve.reserve(static_cast<size_t>(steps) + 1);
    for (int i = 0; i <= steps; ++i) {
        const double t = -endAng + total * (static_cast<double>(i) / static_cast<double>(steps));
        double height = rightS.height;
        if (t >= across)
            height = leftS.height;
        else if (t >= toFront)
            height = frontS.height + (leftS.height - frontS.height) * ((t - toFront) / toLeft);
        else if (t >= 0.0)
            height = rightS.height + (frontS.height - rightS.height) * (t / toFront);
        const double fromFront = std::abs(t - toFront);
        const double half = std::max(toFront, toLeft);
        const double fade = std::max(0.0, 1.0 - fromFront / half);
        const double radial = radius + kBuccalMm + kFrontExtraMm * fade * fade;
        const double theta = a0 + sweepSign * t;
        const double x = ux + radial * std::cos(theta);
        const double y = uy + radial * std::sin(theta);
        const Vec3 p = add(add(add(block.center, mul(block.axisX, x)), mul(block.axisY, y)), mul(block.axisZ, height));
        curve.push_back({p, x, y, height});
    }
    return {};
}

// Negative inside the horseshoe: the arc plus the chord that closes it behind the molars.
// The chords of a triangle leave the teeth out in the segments, which is the empty interior.
double archFootprint(const CompositeCutBlock& block, const std::vector<ArchSample>& curve, double x, double y)
{
    constexpr double kArchMarginMm = 1.5;
    const size_t n = curve.size();
    if (n < 3)
        return 1.0;
    bool inside = false;
    double best = std::numeric_limits<double>::max();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const double ax = curve[j].x;
        const double ay = curve[j].y;
        const double bx = curve[i].x;
        const double by = curve[i].y;
        if ((ay > y) != (by > y)) {
            const double xInt = ax + (bx - ax) * (y - ay) / (by - ay);
            if (xInt > x)
                inside = !inside;
        }
        const double ex = bx - ax;
        const double ey = by - ay;
        const double len2 = ex * ex + ey * ey;
        const double t = len2 > 1e-12 ? std::clamp(((x - ax) * ex + (y - ay) * ey) / len2, 0.0, 1.0) : 0.0;
        best = std::min(best, std::hypot(x - (ax + t * ex), y - (ay + t * ey)));
    }
    (void)block;
    return (inside ? -best : best) - kArchMarginMm;
}

double archLimit(const std::vector<ArchSample>& curve, double x, double y)
{
    double best = std::numeric_limits<double>::max();
    double height = curve.front().height;
    for (const ArchSample& sample : curve) {
        const double d2 = (x - sample.x) * (x - sample.x) + (y - sample.y) * (y - sample.y);
        if (d2 < best) {
            best = d2;
            height = sample.height;
        }
    }
    return height;
}

void addTube(vtkPoints* points, vtkCellArray* polys, const Vec3& a, const Vec3& b, const Vec3& axisZ)
{
    const Vec3 tangent = normalized(sub(b, a), {1.0, 0.0, 0.0});
    Vec3 side = cross(axisZ, tangent);
    if (length(side) < 1e-6)
        side = cross({1.0, 0.0, 0.0}, tangent);
    side = normalized(side, {1.0, 0.0, 0.0});
    const Vec3 up = normalized(cross(tangent, side), axisZ);
    constexpr double kRadius = 0.55;
    Vec3 corner[4] = {add(side, up), add(mul(side, -1.0), up), add(mul(side, -1.0), mul(up, -1.0)),
                      add(side, mul(up, -1.0))};
    const vtkIdType base = points->GetNumberOfPoints();
    for (int i = 0; i < 4; ++i) {
        points->InsertNextPoint(add(a, mul(corner[i], kRadius)).data());
        points->InsertNextPoint(add(b, mul(corner[i], kRadius)).data());
    }
    for (int i = 0; i < 4; ++i) {
        const int j = (i + 1) % 4;
        const vtkIdType q0[3] = {base + 2 * i, base + 2 * i + 1, base + 2 * j + 1};
        const vtkIdType q1[3] = {base + 2 * i, base + 2 * j + 1, base + 2 * j};
        polys->InsertNextCell(3, q0);
        polys->InsertNextCell(3, q1);
    }
}

} // namespace

vtkSmartPointer<vtkPolyData> ArchRibbon(vtkPolyData* bone, const CompositeCutBlock& block, const CompositeContour& points)
{
    (void)bone;
    auto mesh = vtkSmartPointer<vtkPolyData>::New();
    std::vector<ArchSample> curve;
    if (!archCurve(block, points, curve).isEmpty() || curve.size() < 2)
        return mesh;
    auto pts = vtkSmartPointer<vtkPoints>::New();
    auto polys = vtkSmartPointer<vtkCellArray>::New();
    Vec3 hub{0.0, 0.0, 0.0};
    for (const ArchSample& sample : curve)
        hub = add(hub, sample.point);
    hub = mul(hub, 1.0 / static_cast<double>(curve.size()));
    const Vec3 proud = mul(block.axisZ, -0.4);
    const vtkIdType base = pts->GetNumberOfPoints();
    for (const ArchSample& sample : curve)
        pts->InsertNextPoint(add(sample.point, proud).data());
    const vtkIdType hubId = pts->InsertNextPoint(add(hub, proud).data());
    for (size_t i = 0; i < curve.size(); ++i) {
        const vtkIdType a = base + static_cast<vtkIdType>(i);
        const vtkIdType b = base + static_cast<vtkIdType>((i + 1) % curve.size());
        const vtkIdType tri[3] = {a, b, hubId};
        const vtkIdType back[3] = {hubId, b, a};
        polys->InsertNextCell(3, tri);
        polys->InsertNextCell(3, back);
    }
    std::vector<Vec3> drawn;
    drawn.reserve(curve.size());
    for (const ArchSample& sample : curve) {
        Vec3 outward = sub(sample.point, hub);
        outward = sub(outward, mul(block.axisZ, dot(outward, block.axisZ)));
        outward = normalized(outward, mul(block.axisZ, -1.0));
        drawn.push_back(add(sample.point, mul(outward, 0.6)));
    }
    for (size_t i = 1; i < drawn.size(); ++i)
        addTube(pts, polys, drawn[i - 1], drawn[i], block.axisZ);
    mesh->SetPoints(pts);
    mesh->SetPolys(polys);
    return mesh;
}

CompositeBlockResult CreateArchComposite(vtkPolyData* bone, vtkPolyData* dentalScan, const CompositeCutBlock& block,
                                        const CompositeContour& points)
{
    CompositeBlockResult result;
    std::vector<ArchSample> curve;
    const QString curveError = archCurve(block, points, curve);
    if (!curveError.isEmpty()) {
        result.error = curveError;
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
    // Height along the jaw: smaller is the tooth side. The scan crosses the line by a
    // couple of millimetres and covers the bone edge, so the junction needs no skirt.
    constexpr double kOverlapMm = 2.0;
    const auto place = [&](const Vec3& p, double& x, double& y, double& height) {
        const Vec3 d = sub(p, block.center);
        x = dot(d, block.axisX);
        y = dot(d, block.axisY);
        height = dot(d, block.axisZ);
    };
    const auto scanField = [&](const Vec3& p) {
        double x = 0.0, y = 0.0, height = 0.0;
        place(p, x, y, height);
        return std::max(height - (archLimit(curve, x, y) + kOverlapMm), archFootprint(block, curve, x, y));
    };
    const auto boneField = [&](const Vec3& p) {
        double x = 0.0, y = 0.0, height = 0.0;
        place(p, x, y, height);
        if (archFootprint(block, curve, x, y) > 0.0)
            return 1.0;
        return height - archLimit(curve, x, y);
    };
    double bounds[6] = {};
    dentalScan->GetBounds(bounds);
    Vec3 lo{bounds[0], bounds[2], bounds[4]};
    Vec3 hi{bounds[1], bounds[3], bounds[5]};
    for (const auto& p : points)
        for (int k = 0; k < 3; ++k) {
            lo[k] = std::min(lo[k], p[k]);
            hi[k] = std::max(hi[k], p[k]);
        }
    for (const ArchSample& sample : curve)
        for (int k = 0; k < 3; ++k) {
            lo[k] = std::min(lo[k], sample.point[k]);
            hi[k] = std::max(hi[k], sample.point[k]);
        }
    for (int k = 0; k < 3; ++k) {
        lo[k] -= 8.0;
        hi[k] += 8.0;
    }
    const auto dentalCut = clipByField(refineNear(dentalScan, lo, hi, 1.0), scanField, true);
    const auto dentalPart = TagPart(dentalCut, DentalPart);
    const auto bonePart = TagPart(clipByField(refineNear(bone, lo, hi, 1.0), boneField, false), BonePart);
    result.boneCells = bonePart->GetNumberOfPolys();
    result.dentalCells = dentalPart->GetNumberOfPolys();
    if (result.dentalCells == 0) {
        result.error = QStringLiteral("No quedó escaneo de este lado de la línea. Baje los tres puntos hacia los dientes.");
        return result;
    }
    if (result.boneCells == 0) {
        result.error = QStringLiteral("La línea cubre todo el hueso: acérquela a los dientes.");
        return result;
    }
    result.composite = mergeTaggedParts(bonePart, dentalPart);
    result.report = QStringLiteral("Tres puntos: los dientes del TAC de este lado de la línea se reemplazaron por el escaneo "
                                   "(hueso %1 triángulos, escaneo %2 triángulos).")
                        .arg(result.boneCells)
                        .arg(result.dentalCells);
    result.ok = true;
    return result;
}

vtkSmartPointer<vtkPolyData> BoxScan(vtkPolyData* scan, CompositeJaw jaw, QString* error)
{
    if (!scan || scan->GetNumberOfPolys() == 0) {
        if (error)
            *error = QStringLiteral("No hay un escaneo para encofrar.");
        return nullptr;
    }
    const Vec3 axis = jawAxisOf(jaw);
    const vtkIdType n = scan->GetNumberOfPoints();
    double occlusal = std::numeric_limits<double>::infinity();
    double boneward = -occlusal;
    double p[3] = {};
    for (vtkIdType i = 0; i < n; ++i) {
        scan->GetPoint(i, p);
        const double height = dot({p[0], p[1], p[2]}, axis);
        occlusal = std::min(occlusal, height);
        boneward = std::max(boneward, height);
    }
    if (!(occlusal < boneward)) {
        if (error)
            *error = QStringLiteral("El escaneo no tiene altura para encofrar.");
        return nullptr;
    }
    // One plane from the single highest cusp cuts the shorter teeth through the crown.
    // Each part of the arch keeps the band from its own cusp, the same 12 mm the
    // maxilla already shows: the whole tooth and a little gingiva.
    Vec3 tangent = std::abs(axis[2]) < 0.9 ? Vec3{0.0, 0.0, 1.0} : Vec3{1.0, 0.0, 0.0};
    const Vec3 axisX = normalized(cross(tangent, axis), {1.0, 0.0, 0.0});
    const Vec3 axisY = cross(axis, axisX);
    constexpr double kCellMm = 2.0;
    double minX = std::numeric_limits<double>::max();
    double minY = minX;
    double maxX = -minX;
    double maxY = -minY;
    std::vector<double> hx(static_cast<size_t>(n));
    std::vector<double> hy(hx.size());
    std::vector<double> hz(hx.size());
    for (vtkIdType i = 0; i < n; ++i) {
        scan->GetPoint(i, p);
        hx[static_cast<size_t>(i)] = dot({p[0], p[1], p[2]}, axisX);
        hy[static_cast<size_t>(i)] = dot({p[0], p[1], p[2]}, axisY);
        hz[static_cast<size_t>(i)] = dot({p[0], p[1], p[2]}, axis);
        minX = std::min(minX, hx[static_cast<size_t>(i)]);
        minY = std::min(minY, hy[static_cast<size_t>(i)]);
        maxX = std::max(maxX, hx[static_cast<size_t>(i)]);
        maxY = std::max(maxY, hy[static_cast<size_t>(i)]);
    }
    const int nu = std::max(1, static_cast<int>(std::ceil((maxX - minX) / kCellMm)) + 1);
    const int nv = std::max(1, static_cast<int>(std::ceil((maxY - minY) / kCellMm)) + 1);
    std::vector<double> cellMin(static_cast<size_t>(nu * nv), std::numeric_limits<double>::infinity());
    auto cellIndex = [&](double x, double y) {
        const int iu = std::clamp(static_cast<int>(std::floor((x - minX) / kCellMm)), 0, nu - 1);
        const int iv = std::clamp(static_cast<int>(std::floor((y - minY) / kCellMm)), 0, nv - 1);
        return iv * nu + iu;
    };
    for (vtkIdType i = 0; i < n; ++i) {
        const int index = cellIndex(hx[static_cast<size_t>(i)], hy[static_cast<size_t>(i)]);
        cellMin[static_cast<size_t>(index)] = std::min(cellMin[static_cast<size_t>(index)], hz[static_cast<size_t>(i)]);
    }
    std::vector<char> tooth;
    markCrownColumns(tooth, cellMin, nu, nv, occlusal);
    std::vector<double> localOcclusal(cellMin.size(), occlusal);
    constexpr int kCrownReach = 8;
    for (int iv = 0; iv < nv; ++iv) {
        for (int iu = 0; iu < nu; ++iu) {
            double best = std::numeric_limits<double>::infinity();
            int bestCells = 1 << 20;
            for (int dv = -kCrownReach; dv <= kCrownReach; ++dv) {
                for (int du = -kCrownReach; du <= kCrownReach; ++du) {
                    const int jv = iv + dv;
                    const int ju = iu + du;
                    if (jv < 0 || ju < 0 || jv >= nv || ju >= nu)
                        continue;
                    const int index = jv * nu + ju;
                    if (!tooth[static_cast<size_t>(index)])
                        continue;
                    const int cells = dv * dv + du * du;
                    if (cells < bestCells || (cells == bestCells && cellMin[static_cast<size_t>(index)] < best)) {
                        bestCells = cells;
                        best = cellMin[static_cast<size_t>(index)];
                    }
                }
            }
            if (best < std::numeric_limits<double>::infinity())
                localOcclusal[static_cast<size_t>(iv * nu + iu)] = best;
        }
    }
    const auto limitAt = [&](double x, double y) {
        const int index = cellIndex(x, y);
        return localOcclusal[static_cast<size_t>(index)] + ToothAndGingivaMm;
    };
    const auto band = [&](const Vec3& point) {
        return dot(point, axis) - limitAt(dot(point, axisX), dot(point, axisY));
    };
    const double cut = occlusal + ToothAndGingivaMm;
    auto boxed = clipByField(triangulated(scan), band, true);
    if (boxed->GetNumberOfPolys() == 0) {
        if (error)
            *error = QStringLiteral("El encofrado no dejó superficie.");
        return nullptr;
    }
    capPlanarOpening(boxed, axis, cut);
    return boxed;
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
                            const std::atomic<bool>* cancel, bool fillSolids)
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

    // Without a solid fill, erode the dilation back. Surfaces closer than the
    // closing stay joined; a cavity that opens onto the border (sinus, palate) stays empty.
    if (!fillSolids) {
        for (int iter = 0; iter < closingVoxels; ++iter)
            shell = morph(shell, false);
        shell = morph(shell, true);
    }

    // A solid fill would turn a closed jaw into a blob and pack the sinus. The
    // gingival join only wants the skin, with the gap between scan and bone closed.
    std::vector<uint8_t> solid;
    if (!fillSolids) {
        solid = std::move(shell);
    } else {
        solid.assign(total, 1);
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
    }
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
    smooth->SetNumberOfIterations(fillSolids ? 20 : 40);
    smooth->SetPassBand(fillSolids ? 0.1 : 0.05);
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
