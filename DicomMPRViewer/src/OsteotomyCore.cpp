#include "OsteotomyCore.h"

#include <QJsonArray>

#include "CompositeBlockCore.h"

#include <vtkCellArray.h>
#include <vtkCellArrayIterator.h>
#include <vtkCellData.h>

#include <numeric>
#include <unordered_map>

#include <vtkCleanPolyData.h>
#include <vtkClipPolyData.h>
#include <vtkDoubleArray.h>
#include <vtkFlyingEdges3D.h>
#include <vtkFloatArray.h>
#include <vtkImageData.h>
#include <vtkLandmarkTransform.h>
#include <vtkMath.h>
#include <vtkMatrix4x4.h>
#include <vtkPoints.h>
#include <vtkPointData.h>
#include <vtkPolyData.h>
#include <vtkPolyDataNormals.h>
#include <vtkSMPTools.h>
#include <vtkTransform.h>
#include <vtkTransformPolyDataFilter.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace
{
using Vec3 = OstPoint3;
using Vec2 = std::array<double, 2>;

Vec3 sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec3 add(const Vec3& a, const Vec3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
Vec3 mul(const Vec3& a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
double dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec3 cross(const Vec3& a, const Vec3& b)
{
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
double norm(const Vec3& a) { return std::sqrt(dot(a, a)); }
bool normalize(Vec3& v)
{
    const double length = norm(v);
    if (length < 1e-9)
        return false;
    v = mul(v, 1.0 / length);
    return true;
}
// Component of v perpendicular to unit axis.
Vec3 reject(const Vec3& v, const Vec3& axis) { return sub(v, mul(axis, dot(v, axis))); }

// ProPlan frontal view: the cutting path is drawn in the patient's lateral–vertical plane and swept
// antero-posteriorly, so it passes through every landmark at its own height. Oriented models have +Z
// superior; the depth axis is horizontal and perpendicular to the right → left direction.
Vec3 frontalDepth(const Vec3& rightToLeft)
{
    const Vec3 up{0.0, 0.0, 1.0};
    Vec3 depth = cross(up, reject(rightToLeft, up));
    if (!normalize(depth))
        depth = {0.0, 1.0, 0.0};
    return depth;
}

constexpr double kMinPathSpanMm = 5.0;
constexpr int kMaxGuideCells = 180;

// ── Path evaluation ───────────────────────────────────────────────────────────

struct PathFrame
{
    bool valid = false;
    QString error;
    Vec3 origin{};
    Vec3 lateral{}; // s: right → left
    Vec3 up{};      // t: positive side
    Vec3 depth{};   // u: sweep
    std::vector<Vec2> poly; // extended polyline in (s, t)
    std::vector<Vec2> segmentNormal;
    std::vector<Vec2> vertexNormal;
    Vec2 startDirection{}; // outward direction of the first ray
    Vec2 endDirection{};
    double halfWidth = 0.0;
    double centerDepth = 0.0;
};

Vec2 leftNormal(const Vec2& d) { return {-d[1], d[0]}; }

PathFrame pathFrame(const OsteotomyPath& path)
{
    PathFrame f;
    if (path.points.size() < 2) {
        f.error = QStringLiteral("La trayectoria de corte necesita al menos 2 puntos.");
        return f;
    }
    f.depth = path.depthAxis;
    if (!normalize(f.depth)) {
        f.error = QStringLiteral("La dirección de profundidad del corte no es válida.");
        return f;
    }
    Vec3 mean{};
    for (const Vec3& p : path.points)
        mean = add(mean, p);
    f.origin = mul(mean, 1.0 / static_cast<double>(path.points.size()));

    // Lateral axis: the widest spread of the points seen along the depth axis,
    // oriented from the first point to the last.
    Vec3 lateral{};
    double best = 0.0;
    for (size_t i = 0; i < path.points.size(); ++i)
        for (size_t j = i + 1; j < path.points.size(); ++j) {
            const Vec3 d = reject(sub(path.points[j], path.points[i]), f.depth);
            if (norm(d) > best) {
                best = norm(d);
                lateral = d;
            }
        }
    if (best < kMinPathSpanMm) {
        f.error = QStringLiteral("Los puntos de la osteotomía están demasiado juntos.");
        return f;
    }
    if (dot(lateral, sub(path.points.back(), path.points.front())) < 0.0)
        lateral = mul(lateral, -1.0);
    normalize(lateral);
    f.lateral = lateral;
    f.up = cross(f.depth, f.lateral);
    normalize(f.up);
    if (dot(f.up, path.upAxis) < 0.0)
        f.up = mul(f.up, -1.0);

    std::vector<Vec2> pts;
    double minDepth = std::numeric_limits<double>::max();
    double maxDepth = -minDepth;
    for (const Vec3& p : path.points) {
        const Vec3 d = sub(p, f.origin);
        pts.push_back({dot(d, f.lateral), dot(d, f.up)});
        minDepth = std::min(minDepth, dot(d, f.depth));
        maxDepth = std::max(maxDepth, dot(d, f.depth));
    }
    std::sort(pts.begin(), pts.end(), [](const Vec2& a, const Vec2& b) { return a[0] < b[0]; });
    std::vector<Vec2> unique;
    for (const Vec2& p : pts)
        if (unique.empty() || std::hypot(p[0] - unique.back()[0], p[1] - unique.back()[1]) > 1e-6)
            unique.push_back(p);
    if (unique.size() < 2) {
        f.error = QStringLiteral("Los puntos de la osteotomía coinciden.");
        return f;
    }

    const auto direction = [](const Vec2& from, const Vec2& to) {
        const double length = std::hypot(to[0] - from[0], to[1] - from[1]);
        return Vec2{(to[0] - from[0]) / length, (to[1] - from[1]) / length};
    };
    const Vec2 first = direction(unique[0], unique[1]);
    const Vec2 last = direction(unique[unique.size() - 2], unique.back());
    const double extStart = std::max(0.0, path.extensionStartMm);
    const double extEnd = std::max(0.0, path.extensionEndMm);
    f.poly.push_back({unique.front()[0] - first[0] * extStart, unique.front()[1] - first[1] * extStart});
    for (const Vec2& p : unique)
        f.poly.push_back(p);
    f.poly.push_back({unique.back()[0] + last[0] * extEnd, unique.back()[1] + last[1] * extEnd});
    // Drop zero-length extension segments.
    std::vector<Vec2> cleaned;
    for (const Vec2& p : f.poly)
        if (cleaned.empty() || std::hypot(p[0] - cleaned.back()[0], p[1] - cleaned.back()[1]) > 1e-6)
            cleaned.push_back(p);
    f.poly = cleaned;
    f.startDirection = {-first[0], -first[1]};
    f.endDirection = last;

    for (size_t i = 0; i + 1 < f.poly.size(); ++i)
        f.segmentNormal.push_back(leftNormal(direction(f.poly[i], f.poly[i + 1])));
    f.vertexNormal.resize(f.poly.size());
    for (size_t v = 0; v < f.poly.size(); ++v) {
        Vec2 n{0.0, 0.0};
        if (v > 0) {
            n[0] += f.segmentNormal[v - 1][0];
            n[1] += f.segmentNormal[v - 1][1];
        }
        if (v < f.segmentNormal.size()) {
            n[0] += f.segmentNormal[v][0];
            n[1] += f.segmentNormal[v][1];
        }
        if (v == 0) {
            n[0] += f.segmentNormal.front()[0];
            n[1] += f.segmentNormal.front()[1];
        }
        if (v == f.poly.size() - 1) {
            n[0] += f.segmentNormal.back()[0];
            n[1] += f.segmentNormal.back()[1];
        }
        f.vertexNormal[v] = n;
    }
    f.halfWidth = 0.5 * std::max(1.0, path.widthMm);
    f.centerDepth = 0.5 * (minDepth + maxDepth);
    f.valid = true;
    return f;
}

double pathField2D(const PathFrame& f, double s, double t)
{
    double bestDistance = std::numeric_limits<double>::max();
    double sign = 1.0;
    const auto consider = [&](double distance, const Vec2& closest, const Vec2& normal) {
        if (distance < bestDistance) {
            bestDistance = distance;
            sign = (s - closest[0]) * normal[0] + (t - closest[1]) * normal[1] >= 0.0 ? 1.0 : -1.0;
        }
    };
    const size_t n = f.poly.size();
    for (size_t i = 0; i + 1 < n; ++i) {
        const Vec2& a = f.poly[i];
        const Vec2& b = f.poly[i + 1];
        const double dx = b[0] - a[0];
        const double dy = b[1] - a[1];
        const double length2 = dx * dx + dy * dy;
        const double param = ((s - a[0]) * dx + (t - a[1]) * dy) / length2;
        if (param > 0.0 && param < 1.0) {
            const Vec2 c{a[0] + param * dx, a[1] + param * dy};
            consider(std::hypot(s - c[0], t - c[1]), c, f.segmentNormal[i]);
        }
    }
    for (size_t v = 0; v < n; ++v)
        consider(std::hypot(s - f.poly[v][0], t - f.poly[v][1]), f.poly[v], f.vertexNormal[v]);
    // Infinite rays beyond the extended ends keep the bone fully separated.
    const auto ray = [&](const Vec2& origin, const Vec2& direction, const Vec2& normal) {
        const double param = (s - origin[0]) * direction[0] + (t - origin[1]) * direction[1];
        if (param <= 0.0)
            return;
        const Vec2 c{origin[0] + param * direction[0], origin[1] + param * direction[1]};
        consider(std::hypot(s - c[0], t - c[1]), c, normal);
    };
    ray(f.poly.front(), f.startDirection, f.segmentNormal.front());
    ray(f.poly.back(), f.endDirection, f.segmentNormal.back());
    return sign * bestDistance;
}

double pathFieldWorld(const PathFrame& f, const Vec3& p)
{
    const Vec3 d = sub(p, f.origin);
    return pathField2D(f, dot(d, f.lateral), dot(d, f.up));
}

// ── Split ────────────────────────────────────────────────────────────────────

vtkSmartPointer<vtkPolyData> finishPiece(vtkPolyData* clipped)
{
    auto piece = vtkSmartPointer<vtkPolyData>::New();
    piece->ShallowCopy(clipped);
    piece->GetPointData()->RemoveArray(OsteotomyCore::FieldArrayName);
    auto clean = vtkSmartPointer<vtkCleanPolyData>::New();
    clean->SetInputData(piece);
    clean->Update();
    auto normals = vtkSmartPointer<vtkPolyDataNormals>::New();
    normals->SetInputData(clean->GetOutput());
    normals->ConsistencyOn();
    normals->SplittingOff();
    normals->Update();
    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->DeepCopy(normals->GetOutput());
    return out;
}

// Bone below the path that is not connected to the segment (e.g. the mastoid
// processes of a composite that includes the skull base) must not be cut:
// only the connected parts below the path that hold teeth or lie mostly inside
// the guide extent are cut; the rest is pushed to the remaining side.
void restrictToSegment(vtkPolyData* mesh, vtkDoubleArray* values, double halfKerf, const PathFrame& frame)
{
    const vtkIdType n = mesh->GetNumberOfPoints();
    if (n == 0 || frame.poly.empty())
        return;
    std::vector<vtkIdType> parent(static_cast<size_t>(n));
    std::iota(parent.begin(), parent.end(), vtkIdType{0});
    const auto root = [&parent](vtkIdType i) {
        while (parent[static_cast<size_t>(i)] != i) {
            parent[static_cast<size_t>(i)] = parent[static_cast<size_t>(parent[static_cast<size_t>(i)])];
            i = parent[static_cast<size_t>(i)];
        }
        return i;
    };
    const auto below = [values, halfKerf](vtkIdType i) { return values->GetValue(i) < halfKerf; };

    vtkDataArray* parts = mesh->GetCellData()->GetArray(CompositeBlockCore::PartArrayName);
    std::vector<char> dentalPoint(static_cast<size_t>(n), 0);
    const vtkIdType offset = mesh->GetNumberOfVerts() + mesh->GetNumberOfLines();
    vtkIdType cellIndex = 0;
    auto it = vtk::TakeSmartPointer(mesh->GetPolys()->NewIterator());
    for (it->GoToFirstCell(); !it->IsDoneWithTraversal(); it->GoToNextCell(), ++cellIndex) {
        vtkIdType npts = 0;
        const vtkIdType* ids = nullptr;
        it->GetCurrentCell(npts, ids);
        const bool dental = parts && std::lround(parts->GetTuple1(offset + cellIndex)) == CompositeBlockCore::DentalPart;
        for (vtkIdType k = 0; k < npts; ++k) {
            const vtkIdType a = ids[k];
            const vtkIdType b = ids[(k + 1) % npts];
            if (below(a) && below(b)) {
                const vtkIdType ra = root(a);
                const vtkIdType rb = root(b);
                if (ra != rb)
                    parent[static_cast<size_t>(ra)] = rb;
            }
            if (dental)
                dentalPoint[static_cast<size_t>(a)] = 1;
        }
    }

    const double minS = frame.poly.front()[0];
    const double maxS = frame.poly.back()[0];
    std::unordered_map<vtkIdType, std::array<vtkIdType, 3>> components; // total, inside guide, dental
    Vec3 p{};
    for (vtkIdType i = 0; i < n; ++i) {
        if (!below(i))
            continue;
        auto& c = components[root(i)];
        ++c[0];
        mesh->GetPoint(i, p.data());
        const Vec3 d = sub(p, frame.origin);
        const double s = dot(d, frame.lateral);
        const double u = dot(d, frame.depth);
        if (s >= minS && s <= maxS && std::abs(u - frame.centerDepth) <= frame.halfWidth)
            ++c[1];
        if (dentalPoint[static_cast<size_t>(i)])
            ++c[2];
    }
    bool anyKept = false;
    std::unordered_map<vtkIdType, bool> keep;
    for (const auto& [id, c] : components) {
        const bool kept = c[2] > 0 || 2 * c[1] >= c[0];
        keep[id] = kept;
        anyKept = anyKept || kept;
    }
    if (!anyKept)
        return;
    for (vtkIdType i = 0; i < n; ++i)
        if (below(i) && !keep[root(i)])
            values->SetValue(i, 1.0e3);
}

OsteotomySplitResult splitByField(vtkPolyData* mesh, const std::function<double(const Vec3&)>& field, double thicknessMm,
                                  const std::function<void(vtkPolyData*, vtkDoubleArray*, double)>& adjust = {})
{
    OsteotomySplitResult result;
    if (!mesh || mesh->GetNumberOfCells() == 0) {
        result.error = QStringLiteral("No hay hueso que cortar.");
        return result;
    }
    const vtkIdType count = mesh->GetNumberOfPoints();
    auto values = vtkSmartPointer<vtkDoubleArray>::New();
    values->SetName(OsteotomyCore::FieldArrayName);
    values->SetNumberOfTuples(count);
    vtkSMPTools::For(0, count, [&](vtkIdType begin, vtkIdType end) {
        Vec3 p{};
        for (vtkIdType i = begin; i < end; ++i) {
            mesh->GetPoint(i, p.data());
            values->SetValue(i, field(p));
        }
    });
    const double halfKerf = 0.5 * std::clamp(thicknessMm, 0.0, 10.0);
    if (adjust)
        adjust(mesh, values, halfKerf);
    auto work = vtkSmartPointer<vtkPolyData>::New();
    work->ShallowCopy(mesh);
    work->GetPointData()->AddArray(values);
    work->GetPointData()->SetActiveScalars(OsteotomyCore::FieldArrayName);

    const auto clip = [&](double value, bool insideOut) {
        auto clipper = vtkSmartPointer<vtkClipPolyData>::New();
        clipper->SetInputData(work);
        clipper->SetValue(value);
        clipper->SetInsideOut(insideOut);
        clipper->GenerateClipScalarsOff();
        clipper->Update();
        return finishPiece(clipper->GetOutput());
    };
    result.positive = clip(halfKerf, false);
    result.negative = clip(-halfKerf, true);
    if (!result.positive || result.positive->GetNumberOfCells() == 0 ||
        !result.negative || result.negative->GetNumberOfCells() == 0) {
        result.error = QStringLiteral("El corte no atraviesa el hueso: ajuste la trayectoria.");
        return result;
    }
    result.ok = true;
    return result;
}

// ── Guide slab ───────────────────────────────────────────────────────────────

// Closed surface |field| = half thickness inside the box [min, max] of the
// local frame (origin + x, y, z axes).
vtkSmartPointer<vtkPolyData> slabMesh(const std::function<double(const Vec3&)>& field, const Vec3& origin,
                                      const Vec3& axisX, const Vec3& axisY, const Vec3& axisZ, const Vec3& minimum,
                                      const Vec3& maximum, double thicknessMm)
{
    Vec3 extent = sub(maximum, minimum);
    const double largest = std::max({extent[0], extent[1], extent[2], 1.0});
    const double h = std::clamp(largest / kMaxGuideCells, 0.35, 1.5);
    const double half = std::max(0.5 * thicknessMm, h);
    constexpr int pad = 1;
    std::array<int, 3> dims{};
    for (int a = 0; a < 3; ++a)
        dims[static_cast<size_t>(a)] = static_cast<int>(std::ceil(extent[static_cast<size_t>(a)] / h)) + 1 + 2 * pad;

    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetDimensions(dims[0], dims[1], dims[2]);
    image->SetSpacing(h, h, h);
    image->SetOrigin(minimum[0] - pad * h, minimum[1] - pad * h, minimum[2] - pad * h);
    image->AllocateScalars(VTK_FLOAT, 1);
    auto* scalars = static_cast<float*>(image->GetScalarPointer());
    const vtkIdType sliceSize = static_cast<vtkIdType>(dims[0]) * dims[1];
    vtkSMPTools::For(0, static_cast<vtkIdType>(dims[2]), [&](vtkIdType kBegin, vtkIdType kEnd) {
        for (vtkIdType k = kBegin; k < kEnd; ++k)
            for (int j = 0; j < dims[1]; ++j)
                for (int i = 0; i < dims[0]; ++i) {
                    const vtkIdType idx = k * sliceSize + static_cast<vtkIdType>(j) * dims[0] + i;
                    const bool border = i < pad || j < pad || k < pad || i >= dims[0] - pad || j >= dims[1] - pad ||
                                        k >= dims[2] - pad;
                    if (border) {
                        scalars[idx] = 1.0f;
                        continue;
                    }
                    const double lx = minimum[0] + (i - pad) * h;
                    const double ly = minimum[1] + (j - pad) * h;
                    const double lz = static_cast<double>(minimum[2] + (k - pad) * h);
                    const Vec3 world = add(origin, add(mul(axisX, lx), add(mul(axisY, ly), mul(axisZ, lz))));
                    scalars[idx] = static_cast<float>(std::abs(field(world)) - half);
                }
    });

    auto surface = vtkSmartPointer<vtkFlyingEdges3D>::New();
    surface->SetInputData(image);
    surface->SetValue(0, 0.0);
    surface->ComputeNormalsOff();
    surface->ComputeGradientsOff();
    surface->ComputeScalarsOff();
    surface->Update();
    if (surface->GetOutput()->GetNumberOfPolys() == 0)
        return nullptr;

    auto matrix = vtkSmartPointer<vtkMatrix4x4>::New();
    matrix->Identity();
    for (int r = 0; r < 3; ++r) {
        matrix->SetElement(r, 0, axisX[static_cast<size_t>(r)]);
        matrix->SetElement(r, 1, axisY[static_cast<size_t>(r)]);
        matrix->SetElement(r, 2, axisZ[static_cast<size_t>(r)]);
        matrix->SetElement(r, 3, origin[static_cast<size_t>(r)]);
    }
    auto transform = vtkSmartPointer<vtkTransform>::New();
    transform->SetMatrix(matrix);
    auto toWorld = vtkSmartPointer<vtkTransformPolyDataFilter>::New();
    toWorld->SetInputConnection(surface->GetOutputPort());
    toWorld->SetTransform(transform);
    auto normals = vtkSmartPointer<vtkPolyDataNormals>::New();
    normals->SetInputConnection(toWorld->GetOutputPort());
    normals->ConsistencyOn();
    normals->AutoOrientNormalsOn();
    normals->SplittingOff();
    normals->Update();
    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->DeepCopy(normals->GetOutput());
    return out;
}

Vec3 transformPoint(vtkMatrix4x4* m, const Vec3& p)
{
    double in[4] = {p[0], p[1], p[2], 1.0};
    double out[4] = {};
    m->MultiplyPoint(in, out);
    return {out[0], out[1], out[2]};
}

Vec3 transformVector(vtkMatrix4x4* m, const Vec3& v)
{
    double in[4] = {v[0], v[1], v[2], 0.0};
    double out[4] = {};
    m->MultiplyPoint(in, out);
    return {out[0], out[1], out[2]};
}
} // namespace

namespace OsteotomyCore
{
QString TypeName(OsteotomyType type)
{
    switch (type) {
    case OsteotomyType::LeFortI: return QStringLiteral("Le Fort I");
    case OsteotomyType::Bsso: return QStringLiteral("BSSO (sagital bilateral)");
    case OsteotomyType::Genioplasty: return QStringLiteral("Genioplastia");
    }
    return QString();
}

std::vector<OsteotomyLandmark> Landmarks(OsteotomyType type)
{
    switch (type) {
    case OsteotomyType::LeFortI:
        return {
            {QStringLiteral("Apertura piriforme derecha"), QStringLiteral("Borde lateral de la apertura piriforme, a la altura del corte.")},
            {QStringLiteral("Apertura piriforme izquierda"), QStringLiteral("Borde lateral de la apertura piriforme, a la altura del corte.")},
            {QStringLiteral("Pilar cigomático-maxilar derecho"), QStringLiteral("Punto del pilar por donde pasa el corte hacia posterior.")},
            {QStringLiteral("Pilar cigomático-maxilar izquierdo"), QStringLiteral("Punto del pilar por donde pasa el corte hacia posterior.")},
        };
    case OsteotomyType::Bsso:
        return {
            {QStringLiteral("Rama derecha: sobre la língula"), QStringLiteral("Cara medial de la rama, altura del corte horizontal.")},
            {QStringLiteral("Línea oblicua derecha"), QStringLiteral("Cresta oblicua externa detrás del último molar (inicio del corte sagital).")},
            {QStringLiteral("Cortical vestibular derecha"), QStringLiteral("Cortical vestibular a nivel del corte vertical (1.er–2.º molar).")},
            {QStringLiteral("Rama izquierda: sobre la língula"), QStringLiteral("Cara medial de la rama, altura del corte horizontal.")},
            {QStringLiteral("Línea oblicua izquierda"), QStringLiteral("Cresta oblicua externa detrás del último molar (inicio del corte sagital).")},
            {QStringLiteral("Cortical vestibular izquierda"), QStringLiteral("Cortical vestibular a nivel del corte vertical (1.er–2.º molar).")},
        };
    case OsteotomyType::Genioplasty:
        return {
            {QStringLiteral("Borde basal derecho"), QStringLiteral("Salida del corte en el borde inferior derecho, por detrás del agujero mentoniano.")},
            {QStringLiteral("Bajo el canino derecho"), QStringLiteral("Punto del corte bajo el ápice del canino derecho y el agujero mentoniano.")},
            {QStringLiteral("Bajo el canino izquierdo"), QStringLiteral("Punto del corte bajo el ápice del canino izquierdo y el agujero mentoniano.")},
            {QStringLiteral("Borde basal izquierdo"), QStringLiteral("Salida del corte en el borde inferior izquierdo, por detrás del agujero mentoniano.")},
        };
    }
    return {};
}

namespace
{
QJsonArray pointJson(const OstPoint3& p) { return QJsonArray{p[0], p[1], p[2]}; }

OstPoint3 pointFromJson(const QJsonArray& a, const OstPoint3& fallback)
{
    return a.size() == 3 ? OstPoint3{a[0].toDouble(), a[1].toDouble(), a[2].toDouble()} : fallback;
}
} // namespace

QJsonObject PathToJson(const OsteotomyPath& path)
{
    QJsonArray points;
    for (const OstPoint3& p : path.points)
        points.append(pointJson(p));
    return QJsonObject{{QStringLiteral("points"), points},
                       {QStringLiteral("depthAxis"), pointJson(path.depthAxis)},
                       {QStringLiteral("upAxis"), pointJson(path.upAxis)},
                       {QStringLiteral("widthMm"), path.widthMm},
                       {QStringLiteral("thicknessMm"), path.thicknessMm},
                       {QStringLiteral("extensionStartMm"), path.extensionStartMm},
                       {QStringLiteral("extensionEndMm"), path.extensionEndMm}};
}

OsteotomyPath PathFromJson(const QJsonObject& object)
{
    OsteotomyPath path;
    for (const QJsonValue& value : object.value(QStringLiteral("points")).toArray())
        path.points.push_back(pointFromJson(value.toArray(), {0.0, 0.0, 0.0}));
    path.depthAxis = pointFromJson(object.value(QStringLiteral("depthAxis")).toArray(), path.depthAxis);
    path.upAxis = pointFromJson(object.value(QStringLiteral("upAxis")).toArray(), path.upAxis);
    path.widthMm = object.value(QStringLiteral("widthMm")).toDouble(path.widthMm);
    path.thicknessMm = object.value(QStringLiteral("thicknessMm")).toDouble(path.thicknessMm);
    path.extensionStartMm = object.value(QStringLiteral("extensionStartMm")).toDouble(path.extensionStartMm);
    path.extensionEndMm = object.value(QStringLiteral("extensionEndMm")).toDouble(path.extensionEndMm);
    return CheckedPath(path);
}

OsteotomyPath CheckedPath(OsteotomyPath path)
{
    const PathFrame frame = pathFrame(path);
    path.valid = frame.valid;
    path.error = frame.error;
    return path;
}

OsteotomyPath LeFortPath(const std::array<OstPoint3, 4>& l, double widthMm, double thicknessMm,
                         double extensionRightMm, double extensionLeftMm)
{
    OsteotomyPath path;
    // Frontal view: the path keeps each landmark's height (a chevron or stepped Le Fort I stays so).
    // Right side first: pilar R, piriform R, piriform L, pilar L (sorted again by the frame).
    path.points = {l[2], l[0], l[1], l[3]};
    path.depthAxis = frontalDepth(sub(add(l[1], l[3]), add(l[0], l[2])));
    path.upAxis = {0.0, 0.0, 1.0};
    path.widthMm = widthMm;
    path.thicknessMm = thicknessMm;
    path.extensionStartMm = extensionRightMm;
    path.extensionEndMm = extensionLeftMm;
    return CheckedPath(path);
}

OsteotomyPath GenioPath(const std::array<OstPoint3, 4>& l, double widthMm, double thicknessMm,
                        double extensionRightMm, double extensionLeftMm)
{
    OsteotomyPath path;
    // Frontal-view path like Le Fort I: basal exit R, under canine R, under canine L, basal exit L.
    // The chin is below the path; the body lateral to the descending ends is not cut.
    path.points = {l[0], l[1], l[2], l[3]};
    path.depthAxis = frontalDepth(sub(add(l[2], l[3]), add(l[0], l[1])));
    path.upAxis = {0.0, 0.0, 1.0};
    path.widthMm = widthMm;
    path.thicknessMm = thicknessMm;
    path.extensionStartMm = extensionRightMm;
    path.extensionEndMm = extensionLeftMm;
    return CheckedPath(path);
}

OsteotomyPath TransformPath(const OsteotomyPath& path, vtkMatrix4x4* matrix)
{
    if (!matrix)
        return path;
    OsteotomyPath out = path;
    for (Vec3& p : out.points)
        p = transformPoint(matrix, p);
    Vec3 depth = transformVector(matrix, path.depthAxis);
    const double scale = norm(depth) / std::max(1e-9, norm(path.depthAxis));
    out.depthAxis = depth;
    out.upAxis = transformVector(matrix, path.upAxis);
    normalize(out.depthAxis);
    normalize(out.upAxis);
    if (std::isfinite(scale) && scale > 0.0)
        out.widthMm = path.widthMm * scale;
    return CheckedPath(out);
}

BssoPlan TransformBsso(const BssoPlan& plan, vtkMatrix4x4* matrix, bool rightSide, bool leftSide)
{
    if (!matrix)
        return plan;
    BssoPlan out = plan;
    const auto move = [matrix](BssoSideLandmarks& side) {
        side.ramus = transformPoint(matrix, side.ramus);
        side.oblique = transformPoint(matrix, side.oblique);
        side.body = transformPoint(matrix, side.body);
    };
    if (rightSide)
        move(out.right);
    if (leftSide)
        move(out.left);
    // The cranial axis is shared: it only follows a transform applied to both sides.
    if (rightSide && leftSide) {
        out.upAxis = transformVector(matrix, plan.upAxis);
        normalize(out.upAxis);
    }
    return out;
}

struct PreparedPathField
{
    PathFrame frame;
};

std::shared_ptr<const PreparedPathField> PreparePathField(const OsteotomyPath& path, QString* error)
{
    auto prepared = std::make_shared<PreparedPathField>();
    prepared->frame = pathFrame(path);
    if (!prepared->frame.valid) {
        if (error)
            *error = prepared->frame.error;
        return nullptr;
    }
    return prepared;
}

double FieldAt(const PreparedPathField& prepared, const OstPoint3& point)
{
    return prepared.frame.valid ? pathFieldWorld(prepared.frame, point) : 0.0;
}

double PathField(const OsteotomyPath& path, const OstPoint3& point)
{
    const auto prepared = PreparePathField(path);
    return prepared ? FieldAt(*prepared, point) : 0.0;
}

BssoSidePlanes BssoPlanes(const BssoPlan& plan, bool leftSide)
{
    BssoSidePlanes planes;
    Vec3 up = plan.upAxis;
    if (!normalize(up)) {
        planes.error = QStringLiteral("Dirección craneal no válida.");
        return planes;
    }
    const BssoSideLandmarks& side = leftSide ? plan.left : plan.right;
    const BssoSideLandmarks& other = leftSide ? plan.right : plan.left;

    Vec3 towardOther = reject(sub(other.ramus, side.ramus), up);
    if (norm(towardOther) < 10.0) {
        planes.error = QStringLiteral("Las ramas derecha e izquierda están demasiado juntas.");
        return planes;
    }
    normalize(towardOther);
    Vec3 anterior = reject(sub(side.body, side.ramus), up);
    anterior = reject(anterior, towardOther);
    if (!normalize(anterior)) {
        planes.error = QStringLiteral("El punto vestibular debe estar por delante de la rama.");
        return planes;
    }
    planes.anterior = anterior;
    planes.medial = towardOther;

    const Vec3 middle = mul(add(side.ramus, other.ramus), 0.5);
    planes.midline = {middle, towardOther};
    planes.horizontal = {side.ramus, mul(up, -1.0)};
    planes.vertical = {side.body, anterior};
    planes.ramusFront = {side.oblique, anterior};

    Vec3 sagittalNormal = cross(sub(side.body, side.oblique), anterior);
    if (norm(sagittalNormal) < 0.2 * norm(sub(side.body, side.oblique)))
        sagittalNormal = towardOther;
    normalize(sagittalNormal);
    if (dot(sagittalNormal, towardOther) < 0.0)
        sagittalNormal = mul(sagittalNormal, -1.0);
    planes.sagittal = {side.oblique, sagittalNormal};

    if (dot(sub(side.body, side.oblique), anterior) < 0.0) {
        planes.error = QStringLiteral("El punto vestibular debe estar por delante de la línea oblicua.");
        return planes;
    }
    planes.valid = true;
    return planes;
}

double BssoField(const BssoSidePlanes& p, const OstPoint3& x)
{
    const auto signedDistance = [&x](const OsteotomyPlane& plane) { return dot(sub(x, plane.origin), plane.normal); };
    const double lateralPosterior = std::max(signedDistance(p.sagittal), signedDistance(p.vertical));
    const double upperRamus = std::max(signedDistance(p.horizontal), signedDistance(p.ramusFront));
    return std::max(signedDistance(p.midline), std::min(lateralPosterior, upperRamus));
}

OsteotomySplitResult SplitByPath(vtkPolyData* mesh, const OsteotomyPath& path)
{
    const PathFrame frame = pathFrame(path);
    if (!frame.valid) {
        OsteotomySplitResult result;
        result.error = frame.error;
        return result;
    }
    return splitByField(mesh, [&frame](const Vec3& p) { return pathFieldWorld(frame, p); }, path.thicknessMm,
                        [&frame](vtkPolyData* m, vtkDoubleArray* values, double halfKerf) {
                            restrictToSegment(m, values, halfKerf, frame);
                        });
}

OsteotomySplitResult SplitBssoSide(vtkPolyData* mesh, const BssoPlan& plan, bool leftSide)
{
    const BssoSidePlanes planes = BssoPlanes(plan, leftSide);
    if (!planes.valid) {
        OsteotomySplitResult result;
        result.error = planes.error;
        return result;
    }
    return splitByField(mesh, [&planes](const Vec3& p) { return BssoField(planes, p); }, plan.thicknessMm);
}

BssoSplitResult SplitBsso(vtkPolyData* mesh, const BssoPlan& plan)
{
    BssoSplitResult result;
    const OsteotomySplitResult right = SplitBssoSide(mesh, plan, false);
    if (!right.ok) {
        result.error = QStringLiteral("Lado derecho: %1").arg(right.error);
        return result;
    }
    const OsteotomySplitResult left = SplitBssoSide(right.positive, plan, true);
    if (!left.ok) {
        result.error = QStringLiteral("Lado izquierdo: %1").arg(left.error);
        return result;
    }
    result.proximalRight = right.negative;
    result.proximalLeft = left.negative;
    result.distal = left.positive;
    result.ok = true;
    return result;
}

vtkSmartPointer<vtkPolyData> PathGuideMesh(const OsteotomyPath& path)
{
    const PathFrame frame = pathFrame(path);
    if (!frame.valid)
        return nullptr;
    double minS = std::numeric_limits<double>::max(), maxS = -minS, minT = minS, maxT = -minS;
    for (const Vec2& p : frame.poly) {
        minS = std::min(minS, p[0]);
        maxS = std::max(maxS, p[0]);
        minT = std::min(minT, p[1]);
        maxT = std::max(maxT, p[1]);
    }
    const double margin = std::max(2.0, path.thicknessMm);
    return slabMesh([&frame](const Vec3& p) { return pathFieldWorld(frame, p); }, frame.origin, frame.lateral, frame.up,
                    frame.depth, {minS, minT - margin, frame.centerDepth - frame.halfWidth},
                    {maxS, maxT + margin, frame.centerDepth + frame.halfWidth}, path.thicknessMm);
}

vtkSmartPointer<vtkPolyData> BssoGuideMesh(const BssoPlan& plan, bool leftSide)
{
    const BssoSidePlanes planes = BssoPlanes(plan, leftSide);
    if (!planes.valid)
        return nullptr;
    const BssoSideLandmarks& side = leftSide ? plan.left : plan.right;
    Vec3 up = plan.upAxis;
    normalize(up);
    const Vec3 x = planes.anterior;
    const Vec3 y = planes.medial;
    const Vec3 z = up;
    const Vec3 origin = side.ramus;
    Vec3 minimum{std::numeric_limits<double>::max(), std::numeric_limits<double>::max(), std::numeric_limits<double>::max()};
    Vec3 maximum = mul(minimum, -1.0);
    for (const Vec3& p : {side.ramus, side.oblique, side.body}) {
        const Vec3 d = sub(p, origin);
        const Vec3 local{dot(d, x), dot(d, y), dot(d, z)};
        for (size_t a = 0; a < 3; ++a) {
            minimum[a] = std::min(minimum[a], local[a]);
            maximum[a] = std::max(maximum[a], local[a]);
        }
    }
    minimum[0] -= plan.posteriorExtensionMm;
    maximum[0] += 3.0;
    minimum[1] -= plan.mediolateralExtensionMm;
    maximum[1] += plan.mediolateralExtensionMm;
    minimum[2] -= plan.inferiorExtensionMm;
    maximum[2] += 15.0;
    return slabMesh([&planes](const Vec3& p) { return BssoField(planes, p); }, origin, x, y, z, minimum, maximum,
                    plan.thicknessMm);
}

SegmentReference CaptureSegmentReference(vtkPolyData* mesh, const std::vector<OstPoint3>& landmarks, int samples)
{
    SegmentReference reference;
    reference.landmarks = landmarks;
    if (!mesh || mesh->GetNumberOfPoints() < 4 || samples < 4)
        return reference;
    const vtkIdType count = mesh->GetNumberOfPoints();
    const vtkIdType sampled = std::min<vtkIdType>(count, samples);
    for (vtkIdType i = 0; i < sampled; ++i) {
        const vtkIdType id = (i * count) / sampled;
        double p[3];
        mesh->GetPoint(id, p);
        reference.ids.push_back(id);
        reference.points.push_back({p[0], p[1], p[2]});
    }
    return reference;
}

LandmarkMovement MeasureLandmarkMovement(const SegmentReference& reference, vtkPolyData* current, double maxRmsMm)
{
    LandmarkMovement movement;
    if (!current || reference.ids.size() < 4 || reference.ids.size() != reference.points.size())
        return movement;
    auto source = vtkSmartPointer<vtkPoints>::New();
    auto target = vtkSmartPointer<vtkPoints>::New();
    for (size_t i = 0; i < reference.ids.size(); ++i) {
        const vtkIdType id = reference.ids[i];
        if (id < 0 || id >= current->GetNumberOfPoints())
            return movement;
        double p[3];
        current->GetPoint(id, p);
        source->InsertNextPoint(reference.points[i].data());
        target->InsertNextPoint(p);
    }
    auto fit = vtkSmartPointer<vtkLandmarkTransform>::New();
    fit->SetSourceLandmarks(source);
    fit->SetTargetLandmarks(target);
    fit->SetModeToRigidBody();
    fit->Update();

    double sum = 0.0;
    for (vtkIdType i = 0; i < source->GetNumberOfPoints(); ++i) {
        double moved[3];
        fit->TransformPoint(source->GetPoint(i), moved);
        const double* expected = target->GetPoint(i);
        sum += vtkMath::Distance2BetweenPoints(moved, expected);
    }
    movement.rmsMm = std::sqrt(sum / static_cast<double>(source->GetNumberOfPoints()));
    if (movement.rmsMm > maxRmsMm)
        return movement;
    for (const OstPoint3& landmark : reference.landmarks) {
        double moved[3];
        fit->TransformPoint(landmark.data(), moved);
        movement.displacements.push_back({moved[0] - landmark[0], moved[1] - landmark[1], moved[2] - landmark[2]});
    }
    movement.valid = true;
    return movement;
}
} // namespace OsteotomyCore
